/*
SurfOS riscv32 Screen
--------------------
File: fbcon.c   Date: 10/15/26 (SurfCPU)
--------------------
kernel/console.c draws on a "framebuffer" through the interface of
include/sys/framebuffer.h when the boot loader left one (the UEFI laptop): the console's
cells live in RAM and the driver puts the ones that change on the screen. The riscv32
port uses that same path for its displays, which are text displays: the Arty's 80x30
text RAM (boards/arty_a7/textcon.c) gets the cells as cells, in its 16-colour palette,
scrolling in hardware. A board without a display (QEMU virt, an Arty bitstream built
without video) still gets a RAM grid, so the console never touches 0xB8000 or the VGA
ports; what is printed then reaches the serial mirror only.

Graphics sessions (the desktop's 2D drawing, docs/GRAPHICS.md) need a pixel
framebuffer: there is none, so they are refused.
*/

#include <surfos/types.h>
#include <sys/framebuffer.h>
#include <riscv/board.h>
#include <blibc_common.h>

static const struct board_display *disp;
static const u8 *cells;                 /* the buffer the screen shows (kernel/console.c's conVideo) */
static u32 cols = 80, rows = 25;        /* that buffer's grid: 80x25 until framebuffer_grow() */

int framebuffer_init(void) {
    disp = board_display();
    if(disp && disp->init) disp->init();
    return 0;                           /* a RAM grid in every case (above) */
}

void framebuffer_bind(const u8 *c) {
    cells = c;
    cols = 80;
    rows = 25;
}

void framebuffer_grid(u_int *c, u_int *r) {
    *c = disp ? disp->cols : 80;
    *r = disp ? disp->rows : 25;
}

int framebuffer_grow(const u8 *c) {
    if(!disp) return -1;
    cells = c;
    cols = disp->cols;
    rows = disp->rows;
    return 0;
}

void framebuffer_draw_cell(u32 col, u32 row, u8 ch, u8 color) {
    if(disp && col < cols && row < rows) disp->draw_cell(col, row, ch, color);
}

void framebuffer_present(void) {
    if(disp && cells) disp->present(cells, cols, rows);
}

/* kernel/console.c hides the cursor by putting it below the last row */
void framebuffer_set_cursor(u32 col, u32 row) {
    if(disp) disp->set_cursor(col, row, col < cols && row < rows);
}

bool framebuffer_active(void) {
    return disp != NULL;
}

const char *framebuffer_describe(char *buf, size_t size) {
    if(disp) return disp->describe(buf, size);
    strlcpy(buf, "none: this board has no display (the serial console is the screen)", size);
    return buf;
}

void framebuffer_panic(void) {
}

void framebuffer_task_died(struct surf_task *t) {
    (void)t;
}

void framebuffer_console_switched(void) {
}

int framebuffer_graphics_begin(void) {
    return -1;
}

bool framebuffer_graphics_owned(void) {
    return false;
}

int framebuffer_graphics_draw(void (*draw)(const struct fb_surface *, void *), void *arg) {
    (void)draw;
    (void)arg;
    return -1;
}

void framebuffer_graphics_end(void) {
}
