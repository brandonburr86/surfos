/*
SurfOS Framebuffer Console
--------------------
File: framebuffer.c Date: 10/5/26 (USB boot)
--------------------
The console's screen when the boot loader left a linear framebuffer (GRUB on UEFI)
instead of VGA text mode. kernel/console.c keeps writing its char/attribute cells
into a buffer in RAM; this draws the cells that changed, so scrolling and console
switches repaint from that buffer and never read the framebuffer back. The cursor is
drawn in software as an underline. 10/5/26: the grid is whatever fb_setup() made of
the mode (120x33 on a 1920x1080 panel). It starts as the top left 80x25 of that, drawn
from the console's static buffer; once the kernel heap works, framebuffer_grow() takes
the whole grid (kernel/console.c, init_console_grid()).

Console output also comes from interrupt handlers, so drawing is guarded by a busy
flag: whoever finds the screen busy marks it dirty and the drawer goes round again.
The drawer cannot be preempted while it holds the flag (a task killed in the middle
of drawing would otherwise keep it, and the screen would never change again), and a
panic, or a task dying of an exception while it draws, takes the screen back.
Nothing here allocates memory before framebuffer_grow(), so the screen works before
the kernel heap does.
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/irq.h>
#include <surfos/cpu.h>
#include <surfos/task.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <sys/framebuffer.h>
#include <blibc_common.h>

#define SHOWN_CURSOR 0x10000
#define SHOWN_UNKNOWN 0xFFFFFFFF
#define NO_CURSOR 0xFFFFFFFF

static struct fb_surface fbs;               /* fbs.cols x fbs.rows: the grid drawn now */
static bool ready;
static u32 full_cols, full_rows;            /* the grid fb_setup() found room for */
static u32 ncells;                          /* fbs.cols * fbs.rows */
static const u8 *cells;                     /* the console's video buffer */
static u32 shown_boot[80 * 25];             /* char | attr << 8 | SHOWN_CURSOR, as drawn: 80x25 at first, */
static u32 *shown = shown_boot;             /* then the whole grid's, from the kernel heap */
static u32 cursor = NO_CURSOR;              /* cell index; off screen until set */
static volatile int busy, dirty;
static surf_task *owner;                    /* the task that holds busy (or was interrupted holding it) */
static const char *why_not = "the boot loader passed no framebuffer";

int framebuffer_init(void) {
    struct fb_mode m;
    u64 bytes = 0;
    const char *why;
    u8 *virt;
    u32 i;

    if(!bootinfo.fb.present) return -1;
    m.addr = bootinfo.fb.addr;
    m.width = bootinfo.fb.width;
    m.height = bootinfo.fb.height;
    m.pitch = bootinfo.fb.pitch;
    m.bpp = bootinfo.fb.bpp;
    m.type = bootinfo.fb.type;
    m.red_pos = bootinfo.fb.red_pos;     m.red_size = bootinfo.fb.red_size;
    m.green_pos = bootinfo.fb.green_pos; m.green_size = bootinfo.fb.green_size;
    m.blue_pos = bootinfo.fb.blue_pos;   m.blue_size = bootinfo.fb.blue_size;
    why = fb_check_mode(&m, &bytes);
    if(why) { why_not = why; return -1; }
    virt = (u8 *)vmm_map_large(m.addr, bytes, VMM_WC, &why);
    if(!virt) { why_not = why; return -1; }

    fb_setup(&fbs, virt, &m);
    full_cols = fbs.cols;
    full_rows = fbs.rows;
    fbs.cols = 80;                                            /* the console's 80x25 to begin with, */
    fbs.rows = 25;                                            /* where the whole grid will start */
    ncells = fbs.cols * fbs.rows;
    fb_fill(&fbs, 0, 0, m.width, m.height, fbs.palette[0]);   /* whatever GRUB left goes, margins too */
    for(i = 0; i < ncells; i++) shown[i] = 0x0700;            /* a cleared cell: NUL on light grey */
    ready = true;
    return 0;
}

void framebuffer_grid(u_int *cols, u_int *rows) {
    *cols = full_cols;
    *rows = full_rows;
}

/* The whole grid, drawn from a buffer of that size (init_console_grid() fills it). Every
   cell is drawn again with the next present. Nobody else draws yet at that point of the
   boot, but the screen must not be busy. */
int framebuffer_grow(const u8 *buffer) {
    u32 i, n = full_cols * full_rows, *all;
    u_long flags;
    if(!ready || busy) return -1;
    all = (u32 *)kalloc(n * sizeof(u32));
    if(!all) return -1;
    for(i = 0; i < n; i++) all[i] = SHOWN_UNKNOWN;
    flags = irq_save();
    shown = all;
    cells = buffer;
    fbs.cols = full_cols;
    fbs.rows = full_rows;
    ncells = n;
    cursor = NO_CURSOR;
    irq_restore(flags);
    return 0;
}

bool framebuffer_active(void) {
    return ready;
}

void framebuffer_bind(const u8 *buffer) {
    cells = buffer;
}

/**** drawing ****/

static void draw(u32 i) {
    u32 want = cells[i * 2] | ((u32)cells[i * 2 + 1] << 8) | (i == cursor ? SHOWN_CURSOR : 0);
    if(want == shown[i]) return;
    fb_draw_glyph(&fbs, i % fbs.cols, i / fbs.cols, cells[i * 2], cells[i * 2 + 1], i == cursor);
    shown[i] = want;
}

/* true when this caller owns the screen; otherwise the owner will repaint */
static bool begin(void) {
    u_long flags;
    if(!ready || !cells) return false;
    flags = irq_save();
    if(busy) {
        dirty = 1;
        irq_restore(flags);
        return false;
    }
    busy = 1;                                   /* dirty stays: set only by a dead owner (task_died) */
    owner = curTask;
    kcritical_enter();                          /* no preemption until end(): nobody can kill us holding it */
    irq_restore(flags);
    return true;
}

/* release the screen, first catching up with whatever changed while it was busy */
static void end(void) {
    for(;;) {
        u_long flags = irq_save();
        u32 i;
        if(!dirty) {
            busy = 0;
            owner = NULL;
            irq_restore(flags);
            kcritical_leave();
            return;
        }
        dirty = 0;
        irq_restore(flags);
        for(i = 0; i < ncells; i++) draw(i);
    }
}

/* The kernel is going down (panic, or an exception it cannot survive): whoever was
   drawing will never finish, so the screen is taken over and redrawn as it is now.
   Interrupts are off; a fault in here must not come back here. */
void framebuffer_panic(void) {
    static bool inside;
    u32 i;
    if(!ready || !cells || inside) return;
    inside = true;
    busy = 0;
    dirty = 0;
    owner = NULL;
    for(i = 0; i < ncells; i++) shown[i] = SHOWN_UNKNOWN;
    for(i = 0; i < ncells; i++) draw(i);
    inside = false;
}

/* A task is being killed by an exception: if it was drawing, give the screen back.
   The next drawer catches up with everything (dirty), nothing is drawn from here. */
void framebuffer_task_died(surf_task *t) {
    u_long flags = irq_save();
    if(busy && owner == t) {
        busy = 0;
        owner = NULL;
        dirty = 1;
    }
    irq_restore(flags);
}

void framebuffer_draw_cell(u32 col, u32 row, u8 ch, u8 color) {
    if(col >= fbs.cols || row >= fbs.rows || !begin()) return;
    draw(row * fbs.cols + col);                 /* ch and color are already in the buffer */
    end();
}

void framebuffer_present(void) {
    u32 i;
    if(!begin()) return;
    for(i = 0; i < ncells; i++) draw(i);
    end();
}

void framebuffer_set_cursor(u32 col, u32 row) {
    u32 old, now = (col < fbs.cols && row < fbs.rows) ? row * fbs.cols + col : NO_CURSOR;
    if(now == cursor || !begin()) {
        if(now != cursor) cursor = now;         /* the owner's next pass draws it */
        return;
    }
    old = cursor;
    cursor = now;
    if(old != NO_CURSOR) draw(old);
    if(now != NO_CURSOR) draw(now);
    end();
}

const char *framebuffer_describe(char *buf, size_t size) {
    if(!ready) {
        snprintf(buf, size, "none (%s)", why_not);
        return buf;
    }
    snprintf(buf, size, "framebuffer %ux%u, %u bpp at 0x%llx, %ux%u text at scale %u, %s", fbs.mode.width,
             fbs.mode.height, fbs.mode.bpp, fbs.mode.addr, fbs.cols, fbs.rows, fbs.scale,
             cpu.pat_wc ? "write-combining" : "uncached");
    return buf;
}
