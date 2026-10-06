/*
SurfOS Console I/O
--------------------
File: console.c Date: Prior to 4/23/04
--------------------
(C)2004 Brandon Burr

10/5/26: the text grid is set at boot (a framebuffer gets as many cells as it holds),
and the consoles draw the ANSI escape sequences that include/surfos/tty.h lists.
*/

#include <asm/io.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/keyboard.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/multiboot.h>
#include <sys/serial.h>
#include <sys/framebuffer.h>
#include <surfos/klog.h>

#include <mm/kalloc.h>
#include <surfos/irq.h>

#include <blibc_common.h>
#include <stdarg.h>

/* Initial consoles */
surf_console conArray[NUM_CONSOLES]; //array of available consoles
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
void init_console_grid(void) {
    u_int cols, rows, bytes, i;
    u_char *mem;
    u_long flags;
    if(video != VIDEO_FB) return;
    framebuffer_grid(&cols, &rows);
    if(cols == COLUMNS && rows == LINES) return;
    bytes = cols * rows * 2;
    mem = (u_char *)kalloc((NUM_CONSOLES + 1) * bytes);
    if(!mem) return;
    for(i = 0; i < (NUM_CONSOLES + 1) * bytes; i += 2) {   /* cleared cells, as clearConsole() leaves them */
        mem[i] = 0;
        mem[i + 1] = 7;
    }
    if(framebuffer_grow(mem + NUM_CONSOLES * bytes) != 0) {
        kfree(mem);
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

void syncVideoConsole(bool memcopy) {
    surf_console *con = conActive;
    if(!con) return;
    KCRIT_ENTER
    conVideo.loc=con->loc;
    conVideo.txtColor=con->txtColor;
    setpos(con);
    if(memcopy) memcpy(conVideo.vmem,con->vmem,SCREEN_BYTES);
    KCRIT_LEAVE
    if(memcopy && video == VIDEO_FB) framebuffer_present();
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

void switchConsole(u_int conNum) {
    if(conNum < 0 || conNum >= NUM_CONSOLES) return;
    if(isConsoleValid((surf_console*)&conArray[conNum]) == false) return;
    KCRIT_ENTER
    conActive = &conArray[conNum];
    KCRIT_LEAVE
}

void clearConsole(surf_console *con) { //re-initializes a console
  int i;
  if(!con) return;
  for (i=0; i< SCREEN_BYTES; i+=2) {
        *(con->vmem + i) = 0;
       *(con->vmem + i + 1) = 7;
  }
  con->loc.x = 0;
  con->loc.y = 0;
  con->wrap = 0;
  console_reset_ansi(con); //10/5/26: no escape sequence half read, plain colours, the cursor shown
  setpos(con);
  if(isConsoleActive(con)) {
      syncVideoConsole(true);
      serial_console_clear();
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
    if(isConsoleActive(con)==false) return;
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

/* write one character cell at the console's cursor, without moving the cursor */
static void putcell(surf_console *con, TEXTCOLOR color, int c) {
    u_int off = (con->loc.x + con->loc.y * COLUMNS) * 2;
    KCRIT_ENTER
    *(con->vmem + off) = c & 0xFF;
    *(con->vmem + off + 1) = color;
    if(isConsoleActive(con)) {
        *(conVideo.vmem + off) = c & 0xFF;
        *(conVideo.vmem + off + 1) = color;
        if(video == VIDEO_FB) framebuffer_draw_cell(con->loc.x, con->loc.y, c & 0xFF, color);
    }
    KCRIT_LEAVE
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
    if(con == conActive && mirror_serial && (con->fg || con->bg || con->bold || con->inverse || con->cursor_off))
        serial_puts("\033[0m\033[?25h");                /* the terminal mirrors this console: undo it there too */
    con->esc = ESC_NONE;
    con->fg = con->bg = 0;
    con->bold = con->inverse = 0;
    con->cursor_off = 0;
    con->ansi_owner = NULL;
}

/* a task is gone (task.c, through tty_task_gone): undo what it did to the look of a console.
   Interrupts are off; the cursor comes back with the next character printed there. */
void console_task_gone(struct surf_task *task) {
    int i;
    for(i = 0; i < NUM_CONSOLES; i++) if(conArray[i].ansi_owner == task) console_reset_ansi(&conArray[i]);
}

/* panic and wipeout (kernel/panic.c): the report is plain text, whatever was half printed */
void console_panic(void) {
    int i;
    for(i = 0; i < NUM_CONSOLES; i++) console_reset_ansi(&conArray[i]);
    framebuffer_panic();
}

/* the cursor moved by a sequence: clamped to the screen, and a pending wrap is cancelled */
static void ansi_moveto(surf_console *con, int x, int y) {
    con->loc.x = x < 0 ? 0 : (x >= (int)COLUMNS ? COLUMNS - 1 : (u_int)x);
    con->loc.y = y < 0 ? 0 : (y >= (int)LINES ? LINES - 1 : (u_int)y);
    con->wrap = 0;
    if(isConsoleActive(con)) syncVideoConsole(false);
}

/* cells [from, to) become blanks in the writer's colours (what ED and EL leave behind) */
static void ansi_blank(surf_console *con, u_int from, u_int to, TEXTCOLOR attr) {
    u_int i;
    if(from >= to) return;
    KCRIT_ENTER
    for(i = from; i < to; i++) {
        con->vmem[i * 2] = ' ';
        con->vmem[i * 2 + 1] = attr;
    }
    if(isConsoleActive(con)) memcpy(conVideo.vmem + from * 2, con->vmem + from * 2, (to - from) * 2);
    KCRIT_LEAVE
    if(isConsoleActive(con) && video == VIDEO_FB) framebuffer_present();
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
        /* anything else (underline, blink, 256 colours, ...) is not drawn */
    }
    ansi_owner(con);
}

/* a whole CSI sequence has arrived: carry it out */
static void ansi_csi(surf_console *con, TEXTCOLOR color, int final) {
    int n = con->param[0] ? con->param[0] : 1;          /* a count of 0 means 1 */
    int x = con->loc.x, y = con->loc.y;
    u_int here = con->loc.y * COLUMNS + con->loc.x, row = con->loc.y * COLUMNS;
    TEXTCOLOR attr = ansi_attr(con, color);

    if(con->esc_mark == '?') {                           /* only ESC [ ? 25 l / h */
        if((final == 'l' || final == 'h') && con->nparam == 0 && con->param[0] == 25) {
            con->cursor_off = final == 'l';
            ansi_owner(con);
            setpos(con);
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
    case 'J':                                            /* erase in display: 0 below, 1 above, 2 all */
        con->wrap = 0;
        if(con->param[0] == 0) ansi_blank(con, here, COLUMNS * LINES, attr);
        else if(con->param[0] == 1) ansi_blank(con, 0, here + 1, attr);
        else if(con->param[0] == 2) ansi_blank(con, 0, COLUMNS * LINES, attr);
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
        return c >= 0x30;                                       /* ESC 7, ESC c, ...: not ours */
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

/* the next line, scrolling at the bottom */
static void newline(surf_console *con) {
    con->wrap = 0;
    con->loc.x = 0;
    if(++con->loc.y >= LINES) {
        sscroll(con);
        con->loc.y--;
    }
}

void kputch(surf_console *con,TEXTCOLOR color, int c) {
    /* The serial console shows whatever the active console shows, plus anything
       printed before the video console exists (con == NULL during early boot). */
    if((!con || con == conActive) && mirror_serial) serial_console_putc(c);
    if(!con) return;
    if(con->esc && ansi_byte(con, color, c)) return;
    switch (c) {
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
        syncVideoConsole(false);
        break;
    default:
        if(c>=32 && c<=126) {
            /* 10/5/26: the VT100 way of wrapping. Writing the last column leaves the cursor
               there; the next character starts the next line (a carriage return or a cursor
               move cancels that). So a full-screen program can write the bottom right
               corner without scrolling. */
            if(con->wrap) newline(con);
            putcell(con, ansi_attr(con, color), c);
            if(con->loc.x + 1 < COLUMNS) con->loc.x++;
            else con->wrap = 1;
            if(isConsoleActive(con)) syncVideoConsole(false);
        }
        break;
    }
    setpos(con);
    return;
}

void kprintf(const char *format, ...) {
  char buf[512];
  char *p;
  surf_console *con;
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  klog_write(buf);
  /* a task's kernel messages (ps, lspci, a fault report) belong on that task's console;
     boot code and interrupt handlers print on whatever console is showing */
  con = (curTask && curTask->con && !in_interrupt()) ? curTask->con : conActive;
  for(p = buf; *p; p++) kputch(con, KERN_TXT_COLOR, *p);
}

void kcprintf(surf_console *con, const char *format, ...) {
  char buf[512];
  char *p;
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  for(p = buf; *p; p++) kputch(con, KERN_TXT_COLOR, *p);
}
