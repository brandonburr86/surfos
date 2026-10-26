/*
SurfOS Console I/O
--------------------
File: console.c Date: Prior to 4/23/04
--------------------
(C)2004 Brandon Burr

10/5/26: the text grid is set at boot (a framebuffer gets as many cells as it holds),
and the consoles draw the ANSI escape sequences that include/surfos/tty.h lists.
10/6/26: a cell's byte is a code page 437 character (driver/cp437.c), and what is
printed is UTF-8: kputch() decodes it, per writer, so ▸ ≋ … and café show on the screen.
The pager (TTY_PAGE in tty.h) lives here too: kputch() counts the rows and asks --More--.
10/6/26: the pseudo-terminals' consoles (kernel/pty.c) are streams: kputch() hands what is
printed to the master side instead of drawing it, the writer's colour becomes an ANSI SGR
sequence, and the pager counts the rows and columns of the size PTY_SETSIZE gave and asks
its question in the stream (see "stream consoles" below).
10/25/26 (track A, A6; audit C9): F1-F4 on a framebuffer keep what scrolls off their top, and
Shift+PgUp shows it ("scrollback" below).
*/

#include <asm/io.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/keyboard.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/tty.h>
#include <surfos/multiboot.h>
#include <sys/serial.h>
#include <sys/framebuffer.h>
#include <surfos/klog.h>

#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <surfos/irq.h>

#include <blibc_common.h>
#include <stdarg.h>

/* Initial consoles */
surf_console conArray[NUM_TTYS]; //array of available consoles (10/6/26: F1-F4, then the pseudo-terminals')
surf_console conVideo; //the console mapped to the actual video memory
surf_console *conActive; //the pointer to the active console
/********************/

/* The screen (10/5/26, USB boot): VGA text memory at 0xB8000, or, when the boot loader
   left a framebuffer, a buffer in RAM that driver/framebuffer.c draws from. A UEFI boot
   without a usable framebuffer has no screen at all (VIDEO_NONE): 0xB8000 and the CRTC
   ports are never touched there, only the serial port shows anything. */
enum { VIDEO_VGA, VIDEO_FB, VIDEO_NONE };
static int video = VIDEO_VGA;
static bool mirror_serial = true;

/* The text grid (10/5/26): 80x25 as in 2004, in each console's memBuf, until a framebuffer
   that holds more cells of the same letters (1920x1080: 120x33) gets its whole grid in
   init_console_grid(). */
u_int conColumns = 80, conLines = 25;

/* the bytes of one screenful in the grid: cells x 2 (character and attribute) */
#define SCREEN_BYTES (COLUMNS * LINES * 2)

static void page_show(surf_console *con);       /* 10/6/26: the pager's question (below) */
static void page_unshow(surf_console *con);
static int page_descends(struct surf_task *t, struct surf_task *owner);
static void stream_clear(surf_console *con);    /* 10/6/26: stream consoles (below) */
static void alt_gone(surf_console *con);        /* 10/25/26 (A8): the alternate screen (below) */
static void stream_csi(surf_console *con, int final);
static int stream_how(void);
static bool panicking;                          /* 10/6/26: console_panic() was called: the report goes to the screen */
#define PTY_OUT_LIT(con, lit, how) pty_out(con, lit, sizeof(lit) - 1, how)     /* 10/6/26: a sequence of our own */

/* 10/6/26: the size the pager counts: the grid's, or a stream console's own */
static u_int con_cols(surf_console *con) { return con->stream ? con->cols : COLUMNS; }
static u_int con_rows(surf_console *con) { return con->stream ? con->rows : LINES; }

void init_console(void) {
    int i=0;
    if(NUM_CONSOLES<1) return;

    //init the video console
    if(framebuffer_init() == 0) video = VIDEO_FB;
    else if(bootinfo.efi) video = VIDEO_NONE;
    conVideo.vmem = video == VIDEO_VGA ? (u_char*)VMEM_ADDR : conVideo.memBuf; //normally 0xB8000
    if(video == VIDEO_FB) framebuffer_bind(conVideo.memBuf);
    conVideo.txtColor=TEAL_TXT; //for posterity
    clearConsole((surf_console*)&conVideo);
    setPoint((surf_point*)&conVideo.loc,0,0);

    //init the virtual consoles
    for(i=0;i<NUM_CONSOLES;i++) {
        conArray[i].vmem = conArray[i].memBuf;//( CONSOLE_BASE + (i*VMEM_SIZE) );
      conArray[i].txtColor=WHITE_TXT;
      clearConsole((surf_console*)&conArray[i]);
        setPoint((surf_point*)&conArray[i].loc,0,0);
    }

    for(i = NUM_CONSOLES; i < NUM_TTYS; i++) console_stream_reset(&conArray[i]); //10/6/26: the pseudo-terminals'

    conActive = (surf_console*)&conArray[0]; //set active console to first.*/
    if(video == VIDEO_FB) framebuffer_present();

    return;
}

/* 10/5/26: more text, same letters. A framebuffer that holds more than 80x25 cells gets
   its whole grid here, with one buffer from the kernel heap for the four consoles and the
   screen (static buffers of the largest grid would make the kernel image hundreds of KB
   bigger). The heap is touched only once the page fault handler exists, so this runs
   after init_interrupt(); the boot messages so far are in the kernel log and are drawn
   again on the bigger grid. VGA text mode, and a mode that holds just 80x25, keep the
   static buffers. Without memory the screen stays at 80x25. */
static bool scrollback_ok;                      /* 10/25/26 (A6): the grid is final: scrollback rings may be made */

void init_console_grid(void) {
    u_int cols, rows, bytes, i;
    u_char *mem;
    u_long flags;
    if(video != VIDEO_FB) return;
    framebuffer_grid(&cols, &rows);
    bytes = cols * rows * 2;
    mem = cols == COLUMNS && rows == LINES ? NULL : (u_char *)kalloc((NUM_CONSOLES + 1) * bytes);
    if(!mem) {                  /* 80x25 already, or no memory for more: the grid stays (10/25/26: and is final) */
        scrollback_ok = true;
        return;
    }
    for(i = 0; i < (NUM_CONSOLES + 1) * bytes; i += 2) {   /* cleared cells, as clearConsole() leaves them */
        mem[i] = 0;
        mem[i + 1] = 7;
    }
    if(framebuffer_grow(mem + NUM_CONSOLES * bytes) != 0) {
        kfree(mem);
        scrollback_ok = true;
        return;
    }
    flags = irq_save();
    conColumns = cols;
    conLines = rows;
    conVideo.vmem = mem + NUM_CONSOLES * bytes;
    setPoint(&conVideo.loc, 0, 0);
    for(i = 0; i < NUM_CONSOLES; i++) {
        conArray[i].vmem = mem + i * bytes;
        setPoint(&conArray[i].loc, 0, 0);
        conArray[i].wrap = 0;
    }
    scrollback_ok = true;       //10/25/26: the rings get rows of this grid, the boot replayed below among them
    irq_restore(flags);
    syncVideoConsole(true);     //the whole screen, blank
    console_replay_log();       //and the boot so far (the serial port has it already)
}

/* what the screen is, for bootinfo and drivers */
const char *console_describe(char *buf, size_t size) {
    if(video == VIDEO_FB) return framebuffer_describe(buf, size);
    if(video == VIDEO_VGA) {
        strlcpy(buf, "VGA text mode at 0xB8000, 80x25 text", size);
        return buf;
    }
    framebuffer_describe(buf, size);            /* "none (why)" */
    return buf;
}

/* Show the boot messages printed before there was a screen (they are in the kernel
   log; the serial port had them as they happened, so it does not get them twice). */
void console_replay_log(void) {
    mirror_serial = false;
    klog_dump();
    mirror_serial = true;
}

/* 10/25/26 (track A, A3): what the framebuffer owes (framebuffer_present_lazy()), now: a reader is about
   to wait for a key (tty.c), so the prompt or the pager's question shows at once */
void console_flush(void) {
    if(video == VIDEO_FB) framebuffer_flush();
}

void syncVideoConsole(bool memcopy) {
    surf_console *con = conActive;
    if(!con || con->view) return;       /* 10/25/26 (A6): a view back keeps the screen */
    KCRIT_ENTER
    conVideo.loc=con->loc;
    conVideo.txtColor=con->txtColor;
    setpos(con);
    if(memcopy) memcpy(conVideo.vmem,con->vmem,SCREEN_BYTES);
    KCRIT_LEAVE
    if(memcopy && video == VIDEO_FB) framebuffer_present_lazy();   /* 10/25/26: a scroll, a clear: batched */
}

bool isConsoleActive(surf_console *con) {
    return con == conActive ? true:false;
}

bool isConsoleValid(surf_console *con) {
    if(!con) return false;
    if(!con->vmem) return false;
    if(con->loc.x >= COLUMNS || con->loc.y >= LINES) return false;
    return true;

}

/**** scrollback (10/25/26, track A, A6; audit C9) ****/

/*
    F1-F4 on a framebuffer keep the rows that go off the top of the screen in a ring of their
    own, made from the kernel heap when the first row goes (SCROLLBACK_BYTES: about 1,600 rows of
    160 columns, 2 MB for the four), once the grid is final (init_console_grid()). ESC[2J and
    clearConsole() put the screen's rows in it too, up to the last that shows anything, so clear
    and Ctrl+L lose nothing. Shift+PgUp and Shift+PgDn move the view back and forth a screenful
    less a row, Shift+Home to the oldest row, Shift+End back to the live screen: keyboard.c gives
    them here before the tty, so they work at the pager's question as well. A view back shows the
    ring's rows, then the console's own, with " -N " (the rows back) in inverse video at the top
    right and no cursor. What is printed meanwhile goes into the console's cells as ever, not onto
    the screen (shows_live()), and a row that goes into the ring moves the view along, so the
    screen keeps what it shows. Any other key, a console switch or a panic shows the live screen
    again; under a graphics session (the desktop) the keys are the session's. VGA text mode and
    the pseudo-terminals keep no rows.
*/
#define SCROLLBACK_BYTES (512 * 1024)
#define SCROLLBACK_SPARE 512                    /* 10/25/26 (the review): frames left free after a ring is made */
#define VIEW_TAG_ATTR 0x70                      /* " -N ": black on light grey, as the pager's question */

/* is con on the screen as it is now (the active console, not a view back)? */
static bool shows_live(surf_console *con) {
    return con == conActive && !con->view;
}

/* row k of con's ring, 0 the oldest */
static u_char *sb_row(surf_console *con, u_int k) {
    return con->sb + ((con->sb_next + con->sb_cap - con->sb_count + k) % con->sb_cap) * COLUMNS * 2;
}

/* one row of COLUMNS cells into con's ring, made with its first row; a view back stays on the rows
   it shows. Interrupt handlers print too: with interrupts off. */
static void sb_push(surf_console *con, const u_char *row) {
    u_long flags;
    if(!scrollback_ok || con < conArray || con >= conArray + NUM_CONSOLES || con->stream || con->alt_on) return;
    flags = irq_save();
    if(!con->sb && !con->sb_none && !panicking) {       /* never into the heap from a panic's report */
        con->sb_cap = SCROLLBACK_BYTES / (COLUMNS * 2);
        con->sb = (u_char *)kalloc(con->sb_cap * COLUMNS * 2);
        if(!con->sb) con->sb_none = 1;          /* no memory: no scrollback, and no asking again */
    }
    if(con->sb) {
        memcpy(con->sb + con->sb_next * COLUMNS * 2, row, COLUMNS * 2);
        con->sb_next = (con->sb_next + 1) % con->sb_cap;
        if(con->sb_count < con->sb_cap) con->sb_count++;
        if(con->view && con->view < con->sb_count) con->view++;
    }
    irq_restore(flags);
}

/* the screen's rows up to the last that shows anything, into the ring (ESC[2J, clearConsole()) */
static void sb_push_screen(surf_console *con) {
    int last = -1;
    u_int r, c;
    if(!scrollback_ok || con->alt_on) return;           /* 10/25/26 (A8): not the alternate screen */
    for(r = 0; r < LINES; r++)
        for(c = 0; c < COLUMNS; c++) {
            u_char ch = con->vmem[(r * COLUMNS + c) * 2];
            if(ch && ch != ' ') {
                last = (int)r;
                break;
            }
        }
    for(r = 0; (int)r <= last; r++) sb_push(con, con->vmem + r * COLUMNS * 2);
}

/* The screen shows con, the active console: as it is now, or its view back (the ring's rows, then
   its own, the tag, no cursor); the fbflush thread presents it. Interrupts off. */
static void view_draw(surf_console *con) {
    u_int r, row = COLUMNS * 2;
    char tag[16];
    u_int n, i;
    if(!con->view) {
        memcpy(conVideo.vmem, con->vmem, SCREEN_BYTES);
        conVideo.loc = con->loc;
        setpos(con);
        framebuffer_present_soon();
        return;
    }
    for(r = 0; r < LINES; r++) {
        u_int k = con->sb_count - con->view + r;
        memcpy(conVideo.vmem + r * row, k < con->sb_count ? sb_row(con, k) : con->vmem + (k - con->sb_count) * row, row);
    }
    n = (u_int)snprintf(tag, sizeof(tag), " -%u ", con->view);
    if(n > COLUMNS) n = COLUMNS;
    for(i = 0; i < n; i++) {
        conVideo.vmem[(COLUMNS - n + i) * 2] = (u_char)tag[i];
        conVideo.vmem[(COLUMNS - n + i) * 2 + 1] = VIEW_TAG_ATTR;
    }
    framebuffer_set_cursor(0, LINES);           /* below the last row: no cursor */
    framebuffer_present_soon();
}

/* Shift+PgUp, PgDn, Home, End (keyboard.c): 1 when the key was the view's, moved or not */
int console_scroll_key(int key) {
    surf_console *con = conActive;
    u_int page = LINES > 1 ? LINES - 1 : 1, view;
    u_long flags;
    if(video != VIDEO_FB || !con || framebuffer_graphics_on()) return 0;
    if(key != PGUP && key != PGDN && key != HOME && key != END) return 0;
    flags = irq_save();
    view = con->view;
    if(key == PGUP) view = view + page < con->sb_count ? view + page : con->sb_count;
    else if(key == PGDN) view = view > page ? view - page : 0;
    else view = key == HOME ? con->sb_count : 0;
    if(view != con->view) {
        con->view = view;
        view_draw(con);
    }
    irq_restore(flags);
    return 1;
}

/* any other key (keyboard.c): the live screen again */
void console_view_live(void) {
    u_long flags = irq_save();
    if(conActive && conActive->view) {
        conActive->view = 0;
        view_draw(conActive);
    }
    irq_restore(flags);
}

void switchConsole(u_int conNum) {
    if(conNum < 0 || conNum >= NUM_CONSOLES) return;
    if(isConsoleValid((surf_console*)&conArray[conNum]) == false) return;
    KCRIT_ENTER
    if(conActive) conActive->view = 0;  //10/25/26 (A6): a console comes back live
    conActive = &conArray[conNum];
    framebuffer_console_switched();
    KCRIT_LEAVE
}

void clearConsole(surf_console *con) { //re-initializes a console
  int i;
  if(!con) return;
  if(con->stream) { stream_clear(con); return; } //10/6/26: a pseudo-terminal's: ESC[2J ESC[H to the far terminal
  sb_push_screen(con); //10/25/26 (A6): what it showed stays in the scrollback
  for (i=0; i< SCREEN_BYTES; i+=2) {
        *(con->vmem + i) = 0;
       *(con->vmem + i + 1) = 7;
  }
  con->loc.x = 0;
  con->loc.y = 0;
  con->wrap = 0;
  con->page_rows = 0; //10/6/26: an empty screen for the pager to fill
  con->page_fresh = 1;
  con->page_shown = 0;
  console_reset_ansi(con); //10/5/26: no escape sequence half read, plain colours, the cursor shown
  setpos(con);
  if(isConsoleActive(con)) {
      syncVideoConsole(true);
      serial_console_clear();
  }
  if(con->page_asker && !con->page_cancel) { //10/6/26: a task at the pager's question: it comes back at the top
      u_long flags = irq_save();
      page_show(con);
      irq_restore(flags);
  }
}

void clearScreen() {
    clearConsole(conActive);
}

void setConTextColor(surf_console *con, TEXTCOLOR color) { //sets the text color of a console
    if(con) {
        con->txtColor = color;
    }
}

void setPoint(surf_point *pt, u_int x, u_int y) { //sets the effective x and y of the console
    if(!pt) return;
    pt->x = x;
    pt->y = y;
}

void sscroll(surf_console *con) { //scrolls the console
    int i,j;
    u_char *vmem;
    if(!con) return;
    vmem=con->vmem;
    sb_push(con, vmem); //10/25/26 (A6): the top row into the scrollback

    for(i=COLUMNS*2,j=0;i<SCREEN_BYTES;i++,j++) { //shift the console up
        *(vmem+j) = *(vmem+i);
    }

    for(;j<SCREEN_BYTES;j+=2) { //clean the last line
        *(vmem+j)=0; //char byte
        *(vmem+j+1)=7; //attrib byte
    }

    syncVideoConsole(true);
    return;
}


int tX,tY;
inline void setpos(surf_console *con) { //Sets the hardware cursor correctly for the console
    u_int pos;
    if(!con) return;
    if(!shows_live(con)) return; //10/25/26 (A6): not in a view back either
    tX=con->loc.x;
    tY=con->loc.y;
    if(con->cursor_off) { tX = 0; tY = LINES; } //ESC[?25l: just below the last line, where it never shows
    if(video == VIDEO_FB) { framebuffer_set_cursor(tX, tY); return; } //a drawn cursor; no CRTC on a framebuffer
    if(video == VIDEO_NONE) return;
    /* 10/5/26: the CRTC cursor location, low byte (register 0xF) then high byte (0xE). The
       2004 code did the same sum in inline assembly, with the width fixed at 80. */
    pos = tY * COLUMNS + tX;
    KCRIT_ENTER
    outb(VIDPORT, 0x0F);
    outb(VIDPORT + 1, pos & 0xFF);
    outb(VIDPORT, 0x0E);
    outb(VIDPORT + 1, (pos >> 8) & 0xFF);
    KCRIT_LEAVE
}

/* write one character cell, at (x, y) (10/6/26: the pager's question) or at the console's
   cursor, without moving the cursor */
static void cell_at(surf_console *con, u_int x, u_int y, TEXTCOLOR color, int c) {
    u_int off;
    /* 10/10/26: never outside the grid. Two tasks printing on one console at once are not kept
       apart (2004), so a cursor may be read while another writer moves it; a cell one row below
       the last would land past the grid: on a VGA console in its own fields after memBuf (the
       pager's task pointers: seen as a fault in console_page_call_off()), on a framebuffer in the
       next console's grid or past the heap block they share */
    if(x >= COLUMNS || y >= LINES) return;
    off = (x + y * COLUMNS) * 2;
    KCRIT_ENTER
    *(con->vmem + off) = c & 0xFF;
    *(con->vmem + off + 1) = color;
    if(shows_live(con)) {                       /* 10/25/26 (A6): a view back keeps the screen */
        *(conVideo.vmem + off) = c & 0xFF;
        *(conVideo.vmem + off + 1) = color;
        if(video == VIDEO_FB) framebuffer_draw_cell(x, y, c & 0xFF, color);
    }
    KCRIT_LEAVE
}

static void putcell(surf_console *con, TEXTCOLOR color, int c) {
    cell_at(con, con->loc.x, con->loc.y, color, c);
}

/**** escape sequences (10/5/26) ****/

/*
    The subset of ANSI/VT100 that tty.h promises full-screen programs: cursor position and
    moves (CSI H f A B C D), erase in display and line (CSI J K), the attributes bright,
    inverse and the eight colours (CSI m), and hiding the cursor (CSI ? 25 l/h). Each
    console has its own parser. A sequence that is not in the list is read to its end and
    dropped, so none of it is printed; a control character inside one ends it and is
    handled as usual. A console with no attributes set prints in the writer's colour, so
    kprintf() and every older program look exactly as they did.
*/
enum { ESC_NONE, ESC_SEEN, ESC_CSI, ESC_STRING };
#define ESC_STRING_MAX 256      /* ESC ] (a window title), ESC P ...: dropped up to BEL, ESC or this many bytes */

static const u_char ansi_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };  /* ANSI colour order -> VGA attribute order */

/* the attribute a character is drawn in: the writer's, unless SGR changed it */
static TEXTCOLOR ansi_attr(surf_console *con, TEXTCOLOR color) {
    u_int fg, bg, t;
    if(!con->fg && !con->bg && !con->bold && !con->inverse) return color;
    fg = con->fg ? con->fg - 1u : (color & 0x0F);
    bg = con->bg ? con->bg - 1u : ((color >> 4) & 0x07);
    if(con->bold) fg |= 0x08;
    if(con->inverse) { t = fg; fg = bg; bg = t & 0x07; }   /* a text mode background has no bright bit */
    return (TEXTCOLOR)((bg << 4) | fg);
}

/* remember who changed the look of this console, so it can be undone when that task ends */
static void ansi_owner(surf_console *con) {
    bool plain = !con->fg && !con->bg && !con->bold && !con->inverse && !con->cursor_off;
    con->ansi_owner = (plain || in_interrupt()) ? NULL : curTask;
}

void console_reset_ansi(surf_console *con) {
    bool marked = con->fg || con->bg || con->bold || con->inverse || con->cursor_off;
    if(con == conActive && mirror_serial && marked)
        serial_puts("\033[0m\033[?25h");                /* the terminal mirrors this console: undo it there too */
    /* 10/6/26: a stream console's far terminal likewise (a sequence half sent ends at the ESC) */
    if(con->stream && (marked || con->esc) && PTY_OUT_LIT(con, "\033[0m\033[?25h", PTY_OUT_URGENT))
        con->shown = WHITE_TXT;
    con->esc = ESC_NONE;
    con->fg = con->bg = 0;
    con->bold = con->inverse = 0;
    con->cursor_off = 0;
    con->ansi_owner = NULL;
}

/* a task is gone (task.c, through tty_task_gone): undo what it did to the look of a console.
   Interrupts are off; the cursor comes back with the next character printed there. */
void console_task_gone(struct surf_task *task) {
    surf_console *con;
    int i;
    for(i = 0; i < NUM_TTYS; i++) {                     /* 10/6/26: the pseudo-terminals' too */
        con = &conArray[i];
        if(con->ansi_owner == task) console_reset_ansi(con);
        if(con->alt_on && con->alt_owner == task) alt_gone(con);   /* 10/25/26 (A8): the main screen back */
        if(con->page_asker == task) {                   /* 10/6/26: killed at the question: it goes */
            page_unshow(con);
            con->page_asker = NULL;
            wake_up_all(&tty_for_console(con)->page_waiters);
        }
        if(con->page_owner == task) console_page_start(con, NULL);   /* 10/6/26: paging ends with it */
        else if(con->page_owner && page_descends(task, con->page_owner))
            console_page_new(con);                      /* a paged task's last words (a fault report) show */
    }
}

/* panic and wipeout (kernel/panic.c): the report is plain text, whatever was half printed */
void console_panic(void) {
    int i;
    if(conActive && conActive->view) {                  /* 10/25/26 (A6): the live screen, for the report */
        conActive->view = 0;
        memcpy(conVideo.vmem, conActive->vmem, SCREEN_BYTES);
    }
    for(i = 0; i < NUM_TTYS; i++) {
        console_reset_ansi(&conArray[i]);
        console_page_start(&conArray[i], NULL);         /* 10/6/26: nothing paged or dropped, no question */
        conArray[i].page_asker = NULL;
        conArray[i].page_shown = 0;
    }
    panicking = true;                                   /* 10/6/26: a pseudo-terminal's task's report: on the screen */
    framebuffer_panic();
}

/* the cursor moved by a sequence: clamped to the screen, and a pending wrap is cancelled */
static void ansi_moveto(surf_console *con, int x, int y) {
    con->loc.x = x < 0 ? 0 : (x >= (int)COLUMNS ? COLUMNS - 1 : (u_int)x);
    con->loc.y = y < 0 ? 0 : (y >= (int)LINES ? LINES - 1 : (u_int)y);
    con->wrap = 0;
    con->page_fresh = 0;                                /* 10/6/26: the pager asks only at a row's start */
    if(isConsoleActive(con)) syncVideoConsole(false);
}

/* cells [from, to) become blanks in the writer's colours (what ED and EL leave behind) */
static void ansi_blank(surf_console *con, u_int from, u_int to, TEXTCOLOR attr) {
    u_int i;
    if(to > COLUMNS * LINES) to = COLUMNS * LINES;     /* 10/10/26: inside the grid, always */
    if(from >= to) return;
    KCRIT_ENTER
    for(i = from; i < to; i++) {
        con->vmem[i * 2] = ' ';
        con->vmem[i * 2 + 1] = attr;
    }
    if(shows_live(con)) memcpy(conVideo.vmem + from * 2, con->vmem + from * 2, (to - from) * 2);
    KCRIT_LEAVE
    if(shows_live(con) && video == VIDEO_FB) framebuffer_present_lazy();   /* 10/25/26: batched (A6: not in a view back) */
}

static void ansi_sgr(surf_console *con) {
    u_int i, p;
    for(i = 0; i <= con->nparam && i < ESC_PARAMS; i++) {
        p = con->param[i];
        if(p == 0) { con->fg = con->bg = 0; con->bold = con->inverse = 0; }
        else if(p == 1) con->bold = 1;
        else if(p == 7) con->inverse = 1;
        else if(p >= 30 && p <= 37) con->fg = ansi_vga[p - 30] + 1;
        else if(p >= 40 && p <= 47) con->bg = ansi_vga[p - 40] + 1;
        /* 10/25/26 (A8): bright and inverse off, the writer's colours again, bright colours (a text mode
           background has no bright one), and 256 colours or RGB read and dropped, their numbers too */
        else if(p == 22) con->bold = 0;
        else if(p == 27) con->inverse = 0;
        else if(p == 39) con->fg = 0;
        else if(p == 49) con->bg = 0;
        else if(p >= 90 && p <= 97) con->fg = (ansi_vga[p - 90] | 8) + 1;
        else if(p >= 100 && p <= 107) con->bg = ansi_vga[p - 100] + 1;
        else if(p == 38 || p == 48) i += i + 1 <= con->nparam && con->param[i + 1] == 2 ? 4 : 2;
        /* anything else (underline, blink, ...) is not drawn */
    }
    ansi_owner(con);
}

/* 10/25/26 (A8): ESC 7, ESC 8 (with the colours) and ESC[s, ESC[u: the cursor saved and put back */
static void ansi_save(surf_console *con, bool colours) {
    con->saved_loc = con->loc;
    if(!colours) return;
    con->saved_fg = con->fg;
    con->saved_bg = con->bg;
    con->saved_bold = con->bold;
    con->saved_inverse = con->inverse;
}

static void ansi_restore(surf_console *con, bool colours) {
    if(colours) {
        con->fg = con->saved_fg;
        con->bg = con->saved_bg;
        con->bold = con->saved_bold;
        con->inverse = con->saved_inverse;
        ansi_owner(con);
    }
    if(con->stream) {                                   /* the far terminal moves; the column is followed */
        con->loc.x = con->saved_loc.x < con->cols ? con->saved_loc.x : con->cols - 1;
        con->wrap = 0;
        con->at_home = 0;
        return;
    }
    ansi_moveto(con, (int)con->saved_loc.x, (int)con->saved_loc.y);
}

/* 10/25/26 (A8): ESC[?1049h, the alternate screen: the main screen's cells and cursor kept (a stream's far
   terminal keeps its own: the console follows the column), the screen blank, the cursor where it was, as xterm
   has it; ESC[?1049l puts the main screen back. Without memory for the copy the sequence does nothing. A new
   screenful for the pager either way, and the scrollback takes nothing while the alternate screen shows.
   The kernel review (10/25/26, 2): the copy's pages are mapped as they are first touched, from whatever prints,
   and a page with no frame for it panics: so the copy is made only while its frames and SCROLLBACK_SPARE more are
   free (a program at its memory's end), else the sequence does nothing, as without memory. */
static void alt_enter(surf_console *con, TEXTCOLOR attr) {
    u_char *keep;
    u_int i;
    if(con->alt_on) return;
    con->alt_loc = con->loc;
    con->alt_owner = in_interrupt() ? NULL : curTask;
    con->page_rows = 0;
    if(con->stream) {
        con->alt_on = 1;
        return;
    }
    if(pmm_free_frames() < SCREEN_BYTES / 4096 + 1 + SCROLLBACK_SPARE) return;   /* 10/25/26 (the review) */
    keep = (u_char *)kalloc(SCREEN_BYTES);
    if(!keep) return;
    KCRIT_ENTER
    memcpy(keep, con->vmem, SCREEN_BYTES);
    con->alt = keep;
    con->alt_on = 1;
    for(i = 0; i < COLUMNS * LINES; i++) {
        con->vmem[i * 2] = ' ';
        con->vmem[i * 2 + 1] = attr;
    }
    KCRIT_LEAVE
    con->wrap = 0;
    if(shows_live(con)) syncVideoConsole(true);
}

static void alt_leave(surf_console *con) {
    u_char *keep = con->alt;
    if(!con->alt_on) return;
    con->alt_on = 0;
    con->alt_owner = NULL;
    con->page_rows = 0;
    if(con->stream) {
        con->loc.x = con->alt_loc.x < con->cols ? con->alt_loc.x : con->cols - 1;
        con->wrap = 0;
        return;
    }
    KCRIT_ENTER
    memcpy(con->vmem, keep, SCREEN_BYTES);
    con->alt = NULL;
    con->loc = con->alt_loc;
    con->wrap = 0;
    KCRIT_LEAVE
    kfree(keep);
    if(shows_live(con)) syncVideoConsole(true);
    else setpos(con);
}

/* the task that showed the alternate screen ended without ESC[?1049l (console_task_gone(), interrupts off): the
   main screen back, on the terminals too (the serial line's, a stream's far one) */
static void alt_gone(surf_console *con) {
    if(con->stream) PTY_OUT_LIT(con, "\033[?1049l", PTY_OUT_URGENT);
    else if(con == conActive && mirror_serial) serial_puts("\033[?1049l");
    alt_leave(con);
}

/* a whole CSI sequence has arrived: carry it out */
static void ansi_csi(surf_console *con, TEXTCOLOR color, int final) {
    int n = con->param[0] ? con->param[0] : 1;          /* a count of 0 means 1 */
    int x = con->loc.x, y = con->loc.y;
    u_int here = con->loc.y * COLUMNS + con->loc.x, row = con->loc.y * COLUMNS;
    TEXTCOLOR attr = ansi_attr(con, color);

    if(con->stream) {                                    /* 10/6/26: the far terminal carries it out */
        stream_csi(con, final);
        return;
    }
    if(con->esc_mark == '?') {                           /* ESC [ ? 25 l / h; 10/25/26 (A8): ESC [ ? 1049 h / l */
        if((final == 'l' || final == 'h') && con->nparam == 0 && con->param[0] == 25) {
            con->cursor_off = final == 'l';
            ansi_owner(con);
            setpos(con);
        } else if(final == 'h' && con->nparam == 0 && con->param[0] == 1049) {
            alt_enter(con, attr);
        } else if(final == 'l' && con->nparam == 0 && con->param[0] == 1049) {
            alt_leave(con);
        }
        return;
    }
    if(con->esc_mark) return;
    switch(final) {
    case 'H':
    case 'f':
        ansi_moveto(con, (con->nparam >= 1 && con->param[1] ? con->param[1] : 1) - 1, n - 1);
        break;
    case 'A': ansi_moveto(con, x, y - n); break;
    case 'B': ansi_moveto(con, x, y + n); break;
    case 'C': ansi_moveto(con, x + n, y); break;
    case 'D': ansi_moveto(con, x - n, y); break;
    case 'G': ansi_moveto(con, n - 1, y); break;       /* 10/25/26 (A8): the column, the row */
    case 'd': ansi_moveto(con, x, n - 1); break;
    case 's': if(!con->nparam) ansi_save(con, false); break;
    case 'u': if(!con->nparam) ansi_restore(con, false); break;
    case 'X':                                            /* n blanks from the cursor, which stays */
        con->wrap = 0;
        ansi_blank(con, here, here + (u_int)n < row + COLUMNS ? here + (u_int)n : row + COLUMNS, attr);
        break;
    case 'J':                                            /* erase in display: 0 below, 1 above, 2 all */
        con->wrap = 0;
        if(con->param[0] == 0) ansi_blank(con, here, COLUMNS * LINES, attr);
        else if(con->param[0] == 1) ansi_blank(con, 0, here + 1, attr);
        else if(con->param[0] == 2) {
            sb_push_screen(con);                         /* 10/25/26 (A6): clear and Ctrl+L lose nothing */
            ansi_blank(con, 0, COLUMNS * LINES, attr);
        }
        if(con->param[0] == 2 || (con->param[0] == 0 && here == 0))
            con->page_rows = 0;                          /* 10/6/26: an empty screen, a new screenful for the pager */
        break;
    case 'K':                                            /* erase in line: 0 right, 1 left, 2 all */
        con->wrap = 0;
        if(con->param[0] == 0) ansi_blank(con, here, row + COLUMNS, attr);
        else if(con->param[0] == 1) ansi_blank(con, row, here + 1, attr);
        else if(con->param[0] == 2) ansi_blank(con, row, row + COLUMNS, attr);
        break;
    case 'm':
        ansi_sgr(con);
        break;
    default:                                             /* not in tty.h's list */
        break;
    }
}

/* One more byte while a sequence is open. 1: it belonged to the sequence (and is done);
   0: it ended the sequence without belonging to it, and kputch() prints it as usual. */
static int ansi_byte(surf_console *con, TEXTCOLOR color, int c) {
    switch(con->esc) {
    case ESC_STRING:
        if(c == 0x07 || ++con->param[0] >= ESC_STRING_MAX) con->esc = ESC_NONE;
        else if(c == 0x1B) con->esc = ESC_SEEN;
        return 1;
    case ESC_SEEN:
        if(c == '[') {
            con->esc = ESC_CSI;
            con->esc_mark = 0;
            con->nparam = 0;
            con->param[0] = 0;
            return 1;
        }
        if(c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') {
            con->esc = ESC_STRING;
            con->param[0] = 0;
            return 1;
        }
        if(c == 0x1B || (c >= 0x20 && c <= 0x2F)) return 1;      /* ESC again, or ESC ( B and the like */
        con->esc = ESC_NONE;
        if(c == '7') ansi_save(con, true);                       /* 10/25/26 (A8): ESC 7, ESC 8 */
        else if(c == '8') ansi_restore(con, true);
        return c >= 0x30 && c < 0x80;                           /* ESC c, ...: not ours; a UTF-8 byte is printed */
    case ESC_CSI:
        if(c >= '0' && c <= '9') {
            if(con->nparam < ESC_PARAMS && con->param[con->nparam] < 1000)
                con->param[con->nparam] = con->param[con->nparam] * 10 + (c - '0');
            return 1;
        }
        if(c == ';' || c == ':') {
            if(con->nparam < ESC_PARAMS && ++con->nparam < ESC_PARAMS) con->param[con->nparam] = 0;
            return 1;
        }
        if(c >= 0x3C && c <= 0x3F) {                              /* < = > ?: a private sequence */
            if(!con->esc_mark) con->esc_mark = (u_char)c;
            return 1;
        }
        if(c >= 0x20 && c <= 0x2F) {                              /* intermediate bytes: not one of ours */
            con->esc_mark = 1;
            return 1;
        }
        if(c >= 0x40 && c <= 0x7E) {
            con->esc = ESC_NONE;
            ansi_csi(con, color, c);
            return 1;
        }
        if(c == 0x1B) { con->esc = ESC_SEEN; return 1; }
        if(c >= 0 && c < 0x20) { con->esc = ESC_NONE; return 0; }
        return 1;                                                 /* DEL, bytes above 0x7E */
    }
    con->esc = ESC_NONE;
    return 0;
}

/**** stream consoles (10/6/26) ****/

/*
    A pseudo-terminal's console (kernel/pty.c) draws nothing: kputch() hands each byte to
    its master side as it was printed (pty_out()), for a terminal at the far end to draw.
    What the console still does for it:
      * the writer's colour: a byte that draws, or an ESC that starts a sequence, printed in
        another colour than the far terminal draws in now, while no SGR sequence of the
        program's own is in force, goes after an SGR sequence for that colour: ESC[0m for
        the console's own (light grey, the terminal's default), others for kprintf()'s
        yellow or cputs(). A program's own SGR sequences go as they are;
      * it follows the far terminal's cursor column and its VT100 wrap at the width
        PTY_SETSIZE gave (a UTF-8 character is one column, its other bytes none, an escape
        sequence none, TAB goes to the next multiple of 8), so the pager counts the rows
        as they are on that screen; LF starts a row (it goes as it is: the telnet server
        makes it CR LF);
      * Backspace erases, as on the screen: BS SP BS; at a pending wrap the last column
        (CR ESC[999C ESC[K); at a row's start the end of the row above (ESC[A ESC[999C ESC[K);
      * other control characters (but LF, CR, BS, TAB and ESC) are dropped: the screen draws
        none of them.
    A writer that may wait (a task with interrupts on, outside KCRIT_ENTER, holding no
    mutex, its call not cut short) waits while the master's buffer is full; the bytes of
    anyone else are dropped then. The serial mirror, F1-F4 and the active console never
    involve a stream console.
*/
#define STREAM_BASE WHITE_TXT           /* the console's own colour: the far terminal's default */

/* may the writer wait for room in the master's buffer? */
static int stream_how(void) {
    return (curTask && curTask != idle_task && !in_interrupt() && irqs_enabled() && !curTask->crit &&
            !curTask->mutexes) ? PTY_OUT_WAIT : 0;
}

/* the SGR sequence that makes the far terminal draw in VGA attribute a (bright: bold); its length */
static u_int stream_sgr(TEXTCOLOR a, char *out) {
    char *p = out;
    if(a == STREAM_BASE) {
        memcpy(out, "\033[0m", 4);
        return 4;
    }
    *p++ = 0x1B;
    *p++ = '[';
    *p++ = '0';
    if(a & 0x08) {
        *p++ = ';';
        *p++ = '1';
    }
    *p++ = ';';
    *p++ = '3';
    *p++ = (char)('0' + ansi_vga[a & 7]);
    if((a >> 4) & 7) {
        *p++ = ';';
        *p++ = '4';
        *p++ = (char)('0' + ansi_vga[(a >> 4) & 7]);
    }
    *p++ = 'm';
    return (u_int)(p - out);
}

/* a row starts on the far terminal */
static void stream_row(surf_console *con) {
    con->wrap = 0;
    con->loc.x = 0;
    con->page_rows++;
    con->page_fresh = 1;
    con->at_home = 0;
}

/* a whole CSI sequence went to the far terminal (ansi_csi()): what it does to the column, the
   pager's count and the colours */
static void stream_csi(surf_console *con, int final) {
    int n = con->param[0] ? con->param[0] : 1, x = con->loc.x, i;
    if(con->esc_mark == '?') {                           /* ESC [ ? 25 l / h: the cursor hidden */
        if((final == 'l' || final == 'h') && con->nparam == 0 && con->param[0] == 25) {
            con->cursor_off = final == 'l';
            ansi_owner(con);
        } else if(final == 'h' && con->nparam == 0 && con->param[0] == 1049) {
            alt_enter(con, WHITE_TXT);                   /* 10/25/26 (A8): the far terminal's alternate screen */
        } else if(final == 'l' && con->nparam == 0 && con->param[0] == 1049) {
            alt_leave(con);
        }
        return;
    }
    if(con->esc_mark) return;
    switch(final) {
    case 'H':
    case 'f':
        x = (con->nparam >= 1 && con->param[1] ? con->param[1] : 1) - 1;
        con->at_home = n == 1 && x == 0;
        break;
    case 'C': x += n; con->at_home = 0; break;
    case 'D': x -= n; con->at_home = 0; break;
    case 'G': x = n - 1; con->at_home = 0; break;
    case 'A':
    case 'B':
    case 'd': con->at_home = 0; break;                  /* 10/25/26 (A8): ESC[nd, another row */
    case 's': if(!con->nparam) ansi_save(con, false); return;
    case 'u': if(!con->nparam) ansi_restore(con, false); return;
    case 'X': con->wrap = 0; return;
    case 'J':                                            /* an empty screen: a new screenful */
        con->wrap = 0;
        if(con->param[0] == 2 || (con->param[0] == 0 && con->at_home)) con->page_rows = 0;
        return;
    case 'K':
        con->wrap = 0;
        return;
    case 'm':                                            /* a 0 resets the far terminal: its default colours */
        for(i = 0; i <= con->nparam && i < ESC_PARAMS; i++) if(con->param[i] == 0) con->shown = STREAM_BASE;
        ansi_sgr(con);
        return;
    default:
        return;
    }
    /* the cursor moved: on its row still, no pending wrap, and the pager asks only at a row's start */
    con->loc.x = x < 0 ? 0 : (x >= (int)con->cols ? con->cols - 1 : (u_int)x);
    con->wrap = 0;
    con->page_fresh = 0;
}

/* ESC[2J ESC[H to the far terminal (clearConsole()): an empty screen, a new screenful */
static void stream_clear(surf_console *con) {
    u_long flags;
    console_reset_ansi(con);
    PTY_OUT_LIT(con, "\033[2J\033[H", stream_how());
    con->loc.x = 0;
    con->wrap = 0;
    con->at_home = 1;
    con->page_rows = 0;
    con->page_fresh = 1;
    con->page_shown = 0;
    if(con->page_asker && !con->page_cancel) {          /* a task at the question: it comes back at the top */
        flags = irq_save();
        page_show(con);
        irq_restore(flags);
    }
}

/* a sequence of the console's own into out; its length (without the NUL) */
static u_int stream_lit(char *out, const char *s) {
    u_int n = (u_int)strlen(s);
    memcpy(out, s, n);
    return n;
}

/* one byte printed on a stream console (kputch()) */
static void stream_put(surf_console *con, TEXTCOLOR color, u_char b) {
    char out[24];
    u_int n = 0, sgr = 0;
    int how = stream_how();
    if(con->esc && ansi_byte(con, color, b)) {         /* part of a sequence: as it is; the parser follows */
        pty_out(con, (const char *)&b, 1, how);
        return;
    }
    if((b == 0x1B || (b >= 0x20 && b != 0x7F && (b & 0xC0) != 0x80)) && color != con->shown &&
       !con->fg && !con->bg && !con->bold && !con->inverse)
        n = sgr = stream_sgr(color, out);               /* the writer's colour first */
    switch(b) {
    case 0x1B:
        con->esc = ESC_SEEN;
        out[n++] = 0x1B;
        break;
    case '\n':
        stream_row(con);
        out[n++] = '\n';
        break;
    case '\r':
        con->loc.x = 0;
        con->wrap = 0;
        out[n++] = '\r';
        break;
    case 0x08:                                          /* erase the cell before, as the screen does */
        if(con->wrap) {
            n += stream_lit(out + n, "\r\033[999C\033[K");
            con->wrap = 0;
        } else if(con->loc.x > 0) {
            n += stream_lit(out + n, "\b \b");
            con->loc.x--;
        } else if(con->at_home) {
            return;                                     /* the top left: nothing, as on the screen */
        } else {
            n += stream_lit(out + n, "\033[A\033[999C\033[K");
            con->loc.x = con->cols - 1;
        }
        con->page_fresh = 0;
        con->at_home = 0;
        break;
    case '\t':
        if(!con->wrap) con->loc.x = (con->loc.x + 8) & ~7u;
        if(con->loc.x >= con->cols) con->loc.x = con->cols - 1;
        con->page_fresh = 0;
        con->at_home = 0;
        out[n++] = '\t';
        break;
    default:
        if(b < 0x20 || b == 0x7F) return;               /* other control characters: nothing, as on the screen */
        if((b & 0xC0) != 0x80) {                        /* a character (ASCII, or a UTF-8 one's first byte): a column */
            if(con->wrap) stream_row(con);
            con->page_fresh = 0;
            con->at_home = 0;                           /* the cursor left the top left: ESC[J is no clear screen */
            if(con->loc.x + 1 < con->cols) con->loc.x++;
            else con->wrap = 1;
        }
        out[n++] = (char)b;
        break;
    }
    if(pty_out(con, out, n, how) && sgr) con->shown = color;
}

/* a stream console's new size (PTY_SETSIZE, kernel/pty.c): the cursor stays in its row, and a
   question of the pager that the new size has room for is called off: its asker goes on, counting
   the rows at the new size (page_ask()) */
void console_stream_resize(surf_console *con, u_int cols, u_int rows) {
    u_long flags = irq_save();
    con->cols = cols;
    con->rows = rows;
    if(con->loc.x >= cols) {
        con->loc.x = cols - 1;
        con->wrap = 0;
    }
    if(con->page_asker && !con->page_cancel && con->page_rows + 1 < rows) {
        con->page_resized = 1;
        console_page_call_off(con);
    }
    irq_restore(flags);
}

/* a pseudo-terminal's console as new (kernel/pty.c, and at boot): 80x24, plain colours, no
   sequence half sent, not paging. Nobody prints on it meanwhile. */
void console_stream_reset(surf_console *con) {
    u_long flags = irq_save();
    con->vmem = con->memBuf;            /* never drawn; a valid buffer all the same */
    con->txtColor = WHITE_TXT;
    con->loc.x = con->loc.y = 0;
    con->esc = ESC_NONE;
    con->fg = con->bg = 0;
    con->bold = con->inverse = 0;
    con->cursor_off = 0;
    con->ansi_owner = NULL;
    con->wrap = 0;
    con->page_owner = con->page_asker = NULL;
    con->page_rows = 0;
    con->page_fresh = 1;
    con->page_drop = con->page_cancel = con->page_shown = con->page_resized = 0;
    con->page_tail_len = 0;
    con->stream = 1;
    con->at_home = 0;
    con->shown = STREAM_BASE;
    con->cols = 80;
    con->rows = 24;
    irq_restore(flags);
}

/* 10/6/26: UTF-8 is decoded per writer, not per console (sys/cp437.h). A character a task has
   half printed is its own (surf_task.utf), so another task's or an interrupt handler's bytes
   in between never mix with it, and a task that ends takes its half character with it.
   Interrupt handlers, and the boot before there are tasks, share this one. */
static struct utf8_dec shared_utf;

static struct utf8_dec *writer_utf(void) {
    return (curTask && !in_interrupt()) ? &curTask->utf : &shared_utf;
}

/* the next line, scrolling at the bottom. 10/6/26: a row starts, for the pager */
static void newline(surf_console *con) {
    u_int y = con->loc.y + 1;
    con->wrap = 0;
    con->loc.x = 0;
    con->page_rows++;
    con->page_fresh = 1;
    if(y >= LINES) {                    /* 10/10/26: loc.y is never one row below the last, not even */
        sscroll(con);                   /* between the increment and the scroll (cell_at() above) */
        y = LINES - 1;
    }
    con->loc.y = y;
}

/* A character at the cursor. 10/5/26: the VT100 way of wrapping. Writing the last column
   leaves the cursor there; the next character starts the next line (a carriage return or
   a cursor move cancels that). So a full-screen program can write the bottom right corner
   without scrolling. 10/6/26: ch is a code page 437 cell, any of the 256. */
static void put_char(surf_console *con, TEXTCOLOR color, u_char ch) {
    if(con->wrap) newline(con);
    putcell(con, ansi_attr(con, color), ch);
    con->page_fresh = 0;
    if(con->loc.x + 1 < COLUMNS) con->loc.x++;
    else con->wrap = 1;
    if(isConsoleActive(con)) syncVideoConsole(false);
}

/**** the pager (10/6/26) ****/

/*
    TTY_PAGE 1 makes a task the pager's owner on its console (tty.c). What the owner, and
    any task descended from it (10/25/26: through the programs of its foreground, not a job in
    the background), prints on that console in task context is paged: the
    console counts the rows that start (a newline, or the VT100 wrap), and when a
    screenful has been shown, LINES - 1 rows since it began, and a paged task is about to
    draw on, or start, another row, that task first waits: TTY_MORE in inverse video on
    that row, one key from the tty (the keyboard or the serial port), and the question is
    taken away again (the row as it was, the cursor at its start; CR ESC[K on the serial
    terminal). Space or any other key shows another screenful, Enter one more row; q, Q,
    Esc or Ctrl-C drops what the paged tasks print until paging ends, or until a paged task
    reads (what it printed since the last newline, its question, shows then) or ends.

    A task waits only where it may: in task context, with interrupts on, outside
    KCRIT_ENTER, holding no mutex, not in raw mode, and while no other task waits to read
    the terminal (a paged task in the background must not take the shell's keys);
    elsewhere the rows are counted and the question comes at the next row where it may.
    The other order is taken care of too: a task that starts to wait for a key calls the
    question off (tty_getc()), and so does the end of paging or its owner (TTY_PAGE 0, the
    owner's end, another owner); the asker then goes on as after Space, without a key.
    A task whose call is cut short (a kill waits for its end, or Ctrl+C: surfos/task.h)
    waits nowhere here; at the question when that happens, it drops the rest as after q.

    While the question is up, another paged task that may wait waits for the answer (on
    the tty's page_waiters); anyone else who prints there, a task or an interrupt handler
    (the usb thread's +USB: lines), takes the question away first, and it comes back on
    the row after their newline. Such output takes rows but never waits and is never
    dropped, and captured output (console_capture_begin) never reaches the pager.

    10/6/26: on a stream console (a pseudo-terminal) the rows are those of its size
    (PTY_SETSIZE), the question goes into the stream (ESC[7m, TTY_MORE cut to the width
    less one, ESC[27m) and is taken away with CR ESC[K. A new size that leaves room on the
    screen calls a question off, and the asker goes on counting rows at that size.
*/
#define PAGE_ASK_ATTR 0x70              /* the question: black on light grey, inverse video */

/* 10/6/26: the question in a stream: inverse video, TTY_MORE, inverse off unless the program had it on */
static void stream_show(surf_console *con) {
    static const char q[] = TTY_MORE;
    char out[sizeof(q) + 16];
    u_int len = sizeof(q) - 1, n;
    if(len > con->cols - 1) len = con->cols - 1;
    memcpy(out, "\033[7m", 4);
    memcpy(out + 4, q, len);
    n = 4 + len;
    if(!con->inverse) {
        memcpy(out + n, "\033[27m", 5);
        n += 5;
    }
    if(!pty_out(con, out, n, PTY_OUT_URGENT)) return;  /* no room at all (nobody reads): nothing shown to take away */
    con->page_len = (u_char)len;
    con->page_shown = 1;
    con->loc.x = len;
    con->wrap = 0;
    con->at_home = 0;
}

/* the question on the cursor's row, from its start, the cursor after it. Interrupts off. */
static void page_show(surf_console *con) {
    static const char q[] = TTY_MORE;
    u_int len = sizeof(q) - 1, y = con->loc.y, i;
    if(con->stream) {
        stream_show(con);
        return;
    }
    if(len > COLUMNS - 1) len = COLUMNS - 1;
    if(len > PAGE_ASK_MAX) len = PAGE_ASK_MAX;
    if(y >= LINES) y = LINES - 1;       /* 10/10/26: never below the grid (cell_at()) */
    for(i = 0; i < len; i++) {
        con->page_saved[2 * i] = con->vmem[(y * COLUMNS + i) * 2];
        con->page_saved[2 * i + 1] = con->vmem[(y * COLUMNS + i) * 2 + 1];
        cell_at(con, i, y, PAGE_ASK_ATTR, q[i]);
    }
    con->page_y = (u_char)y;
    con->page_len = (u_char)len;
    con->page_shown = 1;
    con->loc.x = len;
    con->wrap = 0;
    setpos(con);
    /* the terminal: inverse video on, the question, inverse off unless the program had it on */
    if(con == conActive && mirror_serial) serial_puts(con->inverse ? "\r\033[7m" TTY_MORE : "\r\033[7m" TTY_MORE "\033[27m");
}

/* the question taken away: its row as it was, the cursor at the start of it. Interrupts off. */
static void page_unshow(surf_console *con) {
    u_int i;
    if(!con->page_shown) return;
    if(con->stream) {                   /* 10/6/26: the far terminal's row cleared, the cursor at its start */
        PTY_OUT_LIT(con, "\r\033[K", PTY_OUT_URGENT);
        con->page_shown = 0;
        con->loc.x = 0;
        con->wrap = 0;
        return;
    }
    for(i = 0; i < con->page_len; i++) cell_at(con, i, con->page_y, con->page_saved[2 * i + 1], con->page_saved[2 * i]);
    con->page_shown = 0;
    con->loc.x = 0;
    con->loc.y = con->page_y;
    con->wrap = 0;
    setpos(con);
    if(con == conActive && mirror_serial) serial_puts("\r\033[K");
}

/* the asker goes on without a key, as after Space. Interrupts off. */
void console_page_call_off(surf_console *con) {
    if(!con->page_asker) return;
    con->page_cancel = 1;
    wake_up_task(con->page_asker);
}

/* a new screenful: paging starts (or ends, owner NULL); a question asked under another
   owner is called off */
void console_page_start(surf_console *con, struct surf_task *owner) {
    u_long flags = irq_save();
    if(con->page_owner != owner) console_page_call_off(con);
    con->page_owner = owner;
    con->page_rows = 0;
    con->page_drop = 0;
    irq_restore(flags);
}

/* a new screenful, and nothing dropped any more */
void console_page_new(surf_console *con) {
    con->page_rows = 0;
    con->page_drop = 0;
    con->page_tail_len = 0;
}

/* The terminal is read (tty.c): the user sees the screen, a new screenful begins. When the
   reader is paged and its output is being dropped after q, the drop ends, and what it
   dropped since the last newline is shown first: the question a program asks before it
   reads (fdisk's "Type rd1 to write ...: "). */
void console_page_read(surf_console *con) {
    u_int i, n = con->page_tail_len;
    if(!con->page_drop || !con->page_owner || !console_paging(con)) {
        console_page_new(con);
        return;
    }
    console_page_new(con);
    for(i = 0; i < n; i++) kputch(con, con->page_tail_color, con->page_tail[i]);
}

/* q: a dropped byte; the ones since the last newline are kept (the last COLUMNS of them) */
static void page_keep(surf_console *con, TEXTCOLOR color, u_char b) {
    u_int max = con_cols(con) < PAGE_TAIL_MAX ? con_cols(con) : PAGE_TAIL_MAX;
    if(b == '\n') {
        con->page_tail_len = 0;
        return;
    }
    if(con->page_tail_len >= max) {
        memmove(con->page_tail, con->page_tail + 1, max - 1);
        con->page_tail_len = max - 1;
    }
    con->page_tail[con->page_tail_len++] = b;
    con->page_tail_color = color;
}

/* is t the owner, or descended from it? Interrupts off. 10/25/26: through the programs of its
   foreground only (TF_FOREGROUND all the way up, as Ctrl+C's job, task.c's in_job()): a job in the
   background, a child of its shell since surfsh has job control, is not paged with the shell's
   command, nor stopped at its question */
static int page_descends(struct surf_task *t, struct surf_task *owner) {
    int depth;
    for(depth = 0; t && depth < 64; t = t->parent, depth++) {
        if(t == owner) return 1;
        if(!(t->flags & TF_FOREGROUND)) return 0;
    }
    return 0;
}

/* Is the current task the owner, or descended from it (its parent's parent, ...) through the
   programs of its foreground? With interrupts off: a parent that ends hands its children to
   init meanwhile. */
int console_paging(surf_console *con) {
    u_long flags;
    int found;
    if(!con->page_owner || !curTask) return 0;
    flags = irq_save();
    found = page_descends(curTask, con->page_owner);
    irq_restore(flags);
    return found;
}

/* may the current task wait here (for a key, or for another task's question)? 10/6/26: not
   when its call is cut short (a kill waits for its end, or Ctrl+C: surfos/task.h) */
static int page_may_wait(struct tty *t) {
    return irqs_enabled() && !curTask->crit && !curTask->mutexes && !t->raw && !curTask->intr;
}

/* Wait for a key at the question. The cursor's row has nothing drawn on it yet, or the row
   is full and the question takes the next one. */
static void page_ask(surf_console *con, struct tty *t) {
    u_long flags;
    int key, resized;
    if(con->wrap) {
        if(con->stream) {                               /* 10/6/26: the far terminal's next row */
            stream_row(con);
            PTY_OUT_LIT(con, "\n", PTY_OUT_URGENT);
        } else {
            newline(con);
            if(con == conActive && mirror_serial) serial_puts("\r\n");
        }
    }
    flags = irq_save();
    con->page_asker = curTask;
    con->page_cancel = 0;
    con->page_resized = 0;
    page_show(con);
    irq_restore(flags);
    key = tty_getc_unless(t, &con->page_cancel);          /* -1: called off, or the asker's call cut short */
    flags = irq_save();
    page_unshow(con);
    con->page_asker = NULL;
    con->page_cancel = 0;
    resized = con->page_resized;
    con->page_resized = 0;
    irq_restore(flags);
    wake_up_all(&t->page_waiters);
    if(key == 'q' || key == 'Q' || key == 0x1B || key == 0x03 || (key < 0 && curTask->intr))
        con->page_drop = 1;                                /* q, Esc, Ctrl-C; 10/6/26: killed or interrupted there */
    else if(key < 0 && resized)
        ;                                                  /* 10/6/26: a bigger pseudo-terminal: the rows go on */
    else con->page_rows = (key == '\n' || key == '\r') ? con_rows(con) - 2 : 0;
}

/* For each byte a console gets while paging is on: 1 when it is dropped (q was pressed).
   Before a paged task's byte draws on, or starts, the row after a screenful, the task
   waits at the question first, where it may; while another task is at the question, it
   waits for the answer first. */
static int page_hold(surf_console *con, TEXTCOLOR color, u_char b) {
    struct tty *t;
    u_long flags;
    if(in_interrupt() || !curTask || curTask == idle_task || !console_paging(con)) return 0;
    t = tty_for_console(con);
    while(con->page_asker && con->page_asker != curTask && page_may_wait(t)) {
        flags = irq_save();
        if(con->page_asker) {
            wait_prepare_intr(&t->page_waiters);        /* 10/6/26: a kill or Ctrl+C ends the wait */
            irq_restore(flags);
            yield();
        } else {
            irq_restore(flags);
        }
        if(!console_paging(con)) return 0;              /* paging ended meanwhile */
    }
    if(con->page_drop) {
        page_keep(con, color, b);
        return 1;
    }
    if(con->page_asker) return 0;                       /* it may not wait: it prints, the question gives way */
    if(con->esc || (con->stream ? (b & 0xC0) == 0x80 : writer_utf()->have) ||
       !(b == '\n' || (b == '\t' && !con->wrap) || (b >= 0x20 && b != 0x7F)))
        return 0;                                       /* draws nothing, or not yet (10/6/26: the rest of a UTF-8 character;
                                                           10/25/26: a TAB starts a row as a character does, except at a
                                                           pending wrap, where it does nothing) */
    /* 10/6/26: on a stream a newline after a full row starts just one row, as on any terminal */
    if((con->wrap && !(con->stream && b == '\n')) ? con->page_rows + 2 < con_rows(con)
                                                  : !con->page_fresh || con->page_rows + 1 < con_rows(con))
        return 0;                                       /* the screenful is not full yet */
    if(!page_may_wait(t) || wq_count(&t->readers))
        return 0;                                       /* no waiting here: at the next row */
    page_ask(con, t);
    if(!con->page_drop) return 0;
    page_keep(con, color, b);
    return 1;
}

/* 10/6/26: a task's output kept instead of shown (console.h) */
void console_capture_begin(struct kcapture *k) {
    if(curTask) curTask->kcap = k;
}

void console_capture_end(void) {
    if(curTask) curTask->kcap = NULL;
}

/* 10/6/26: the pager's question comes back after someone else's newline */
static void page_reshow(surf_console *con, u_char b) {
    u_long flags;
    if(b != '\n' || !con->page_asker || con->page_shown || con->page_cancel) return;
    flags = irq_save();
    if(con->page_asker && !con->page_shown && !con->wrap && !con->loc.x) page_show(con);
    irq_restore(flags);
}

/* 10/6/26: typed input shown as it is typed (the line editor's echo, getchar()) goes to the
   screen even while the task's output is kept for a file: the file gets the command's
   output, not the user's keystrokes, and the user sees what they type */
void console_echo(surf_console *con, TEXTCOLOR color, int c) {
    struct kcapture *kept = (curTask && !in_interrupt()) ? curTask->kcap : NULL;
    if(kept) curTask->kcap = NULL;
    kputch(con, color, c);
    if(kept) curTask->kcap = kept;
}

void kputch(surf_console *con,TEXTCOLOR color, int c) {
    u_char b = (u_char)c;           /* 10/6/26: a char above 0x7f may come signed */
    u_char cells[UTF8_OUT_MAX];
    struct utf8_dec *utf;
    int n, i;
    /* 10/6/26: a redirected kernel command (console.h): its output goes to memory, not the screen */
    if(con && curTask && curTask->kcap && con == curTask->con && !in_interrupt()) {
        struct kcapture *k = curTask->kcap;
        if(k->len < k->size) k->buf[k->len++] = (char)c;
        else k->cut++;
        return;
    }
    /* 10/6/26: a panic's report goes to the screen, whichever console its task printed on */
    if(con && con->stream && panicking) con = conActive;
    /* 10/6/26: the pager (TTY_PAGE): a paged task waits at a full screen, and after q what
       it prints is dropped; the question gives way to anyone else who prints here */
    if(con && con->page_owner && page_hold(con, color, b)) return;
    if(con && con->page_shown) {
        u_long flags = irq_save();
        page_unshow(con);
        irq_restore(flags);
    }
    /* 10/6/26: a pseudo-terminal's console: to its master side (stream consoles, above) */
    if(con && con->stream) {
        stream_put(con, color, b);
        page_reshow(con, b);
        return;
    }
    /* The serial console shows whatever the active console shows, plus anything
       printed before the video console exists (con == NULL during early boot). */
    if((!con || con == conActive) && mirror_serial) serial_console_putc(c);
    if(!con) return;
    con->line_by = in_interrupt() ? NULL : curTask;     /* 10/25/26: whose line the cursor's row is (kprintf) */
    if(con->esc && ansi_byte(con, color, b)) return;
    /* 10/6/26: UTF-8 (driver/cp437.c). A character comes out as the code page 437 cell for
       it ('?' when the set has none), bytes that are not UTF-8 as the cells they are. A byte
       below 0x80 that cuts a character short comes out last, and is handled below as ever. */
    utf = writer_utf();
    if(b >= 0x80 || utf->have) {
        n = utf8_feed(utf, b, cells);
        if(b < 0x80) n--;
        for(i = 0; i < n; i++) put_char(con, color, cells[i]);
        if(b >= 0x80) {
            setpos(con);
            return;
        }
    }
    switch (b) {
    case 0x1B: //10/5/26: an escape sequence starts (tty.h)
        con->esc = ESC_SEEN;
        return;
    case '\n':
        if(con->wrap) newline(con); //a full line still leaves an empty one, as it always did
        newline(con);
        syncVideoConsole(false);
        break;
    case '\r': //10/5/26: carriage return; after a full line it cancels the wrap, so "\r\n" is one line
        con->wrap = 0;
        con->loc.x = 0;
        syncVideoConsole(false);
        break;
    case 0x08:
        /* erase the cell just typed and stay there (the 2004 code stepped back twice);
           10/5/26: that is the last column after a wrap, or the end of the line above */
        if(con->wrap) con->wrap = 0;
        else if(con->loc.x > 0) con->loc.x--;
        else if(con->loc.y > 0) { con->loc.y--; con->loc.x = COLUMNS - 1; }
        else { syncVideoConsole(false); break; }
        putcell(con, ansi_attr(con, color), ' ');
        con->page_fresh = 0;
        syncVideoConsole(false);
        break;
    case '\t': /* 10/25/26: to the next multiple of 8, as on a terminal (and stream_put()): never past the
                  last column, nothing at a pending wrap, and the cells it passes stay as they are */
        if(!con->wrap) con->loc.x = (con->loc.x + 8) & ~7u;
        if(con->loc.x >= COLUMNS) con->loc.x = COLUMNS - 1;
        con->page_fresh = 0;
        syncVideoConsole(false);
        break;
    default:
        if(b >= 32 && b <= 126) put_char(con, color, b);   /* other control characters: nothing */
        break;
    }
    page_reshow(con, b);        /* 10/6/26: the pager's question comes back after someone else's newline */
    setpos(con);
    return;
}

/* 10/25/26: a kernel thread's message comes out whole, no other task's output between its characters:
   surfsh's banner used to cut netman's +NET: lines in two, on the screen and the serial line. From a
   kernel thread (not a process, not a shell, not on a pseudo-terminal, not paged, free to wait) the
   message first waits up to 100 ms for another writer's line to end (the cursor back at a row's start),
   then goes out with preemption off; interrupts stay on, so a slow serial port holds up only this
   thread. A program's line that never ends (a prompt) gets the message after it, as before. Anyone
   else's message goes out as before, a character at a time. */
static bool whole_line_writer(surf_console *con) {
  return con && curTask && curTask != idle_task && !in_interrupt() && irqs_enabled() && !curTask->crit &&
         !curTask->mutexes && !(curTask->flags & (TF_USER | TF_SHELL)) && !con->stream && !console_paging(con);
}

static void kernel_puts(surf_console *con, const char *s) {
  int i;
  if(whole_line_writer(con)) {
    for(i = 0; i < 10 && con->loc.x && con->line_by != curTask; i++) sleep_ms(10);
    KCRIT_ENTER
    for(; *s; s++) kputch(con, KERN_TXT_COLOR, *s);
    KCRIT_LEAVE
    return;
  }
  for(; *s; s++) kputch(con, KERN_TXT_COLOR, *s);
}

void kprintf(const char *format, ...) {
  char buf[512];
  surf_console *con;
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  klog_write(buf);
  /* a task's kernel messages (ps, lspci, a fault report) belong on that task's console;
     boot code and interrupt handlers print on whatever console is showing */
  con = (curTask && curTask->con && !in_interrupt()) ? curTask->con : conActive;
  kernel_puts(con, buf);
}

void kcprintf(surf_console *con, const char *format, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  kernel_puts(con, buf);
}
