/*
SurfOS Console I/O
----------------------
File: console.h Date: Prior than 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_CONSOLE_H
#define _SURFOS_CONSOLE_H

#include <surfos/types.h>
#include <sys/cp437.h>

/* 10/5/26: the text grid. Every console starts at 80x25 in its own memBuf, and VGA text
   mode stays there. On a framebuffer that holds more cells of the same letters (driver/
   fbcore.c: 120x33 at 1920x1080), init_console_grid() moves the consoles to buffers of
   that size from the kernel heap, once the heap can be touched. COLUMNS and LINES were
   the constants 80 and 25 until then. */
extern u_int conColumns, conLines;
#define COLUMNS     conColumns
#define LINES       conLines

#define VMEM_ADDR   0xB8000
#define VIDPORT 0x3D4

#define BUF_SIZE 1024

#define KERN_TXT_COLOR YELLOW_TXT //sets the output color for kernel messages

/* Virtual Console memory allocation*/
#define NUM_CONSOLES 4 //F1..F4; each has a shell and a tty
#define VMEM_SIZE 4000 //each virtual console is 4000 bytes (80x25; a bigger grid's buffers come from the heap)
#define CONSOLE_BASE 0xC000000//0x800 //start storing the first console @
//0x800 /***********************************/

typedef struct surf_point {
    u_int x;
    u_int y;
} surf_point;

#define ESC_PARAMS 8
#define PAGE_ASK_MAX 64             /* 10/6/26: the cells the pager's question covers, at most (TTY_MORE is 63) */
#define PAGE_TAIL_MAX 240           /* 10/6/26: what q dropped since the last newline, kept (the widest grid) */

typedef struct surf_console {
    TEXTCOLOR txtColor;
    surf_point loc;
    u_char *vmem;
    u_char memBuf[4001];
    /* 10/5/26: the ANSI escape sequences of tty.h (kernel/console.c); all zero is plain text */
    u_char esc;                 /* where the parser is in a sequence */
    u_char esc_mark;            /* ESC [ ? ...: the private marker; 1 = intermediate bytes (not ours) */
    u_char nparam;              /* the parameter being read */
    u_short param[ESC_PARAMS];
    u_char fg, bg;              /* SGR 30-37 / 40-47 as a VGA colour + 1; 0 = the writer's own */
    u_char bold, inverse;       /* SGR 1, SGR 7 */
    u_char cursor_off;          /* ESC [ ? 25 l */
    u_char wrap;                /* the last column was written: the next character starts the next line */
    struct surf_task *ansi_owner; /* who changed the colours or hid the cursor; reset when it ends */
    /* 10/6/26: the pager (TTY_PAGE, tty.h); all zero is "not paging" */
    struct surf_task *page_owner; /* the task that asked: what it and its descendants print is paged */
    struct surf_task *page_asker; /* the task waiting at the question */
    u_int page_rows;            /* rows started since this screenful began */
    u_char page_fresh;          /* nothing drawn on the cursor's row since it started */
    u_char page_drop;           /* q: what the paged tasks print is dropped */
    u_char page_cancel;         /* the asker's wait is called off: paging ended or changed hands, or another task reads */
    u_char page_shown;          /* the question is on the screen, at the start of row page_y */
    u_char page_y, page_len;
    u_char page_saved[2 * PAGE_ASK_MAX];  /* the cells it covers */
    u_char page_tail[PAGE_TAIL_MAX];      /* q: the dropped bytes since the last newline, a question perhaps, */
    u_short page_tail_len;                /* shown when the paged task that printed them reads */
    TEXTCOLOR page_tail_color;
} surf_console;

void sscroll(surf_console *con);
inline void setpos(surf_console *con);
void kputch(surf_console* con,TEXTCOLOR color, int c);
void kprintf(const char *format, ...);
void kcprintf(surf_console *con, const char *format, ...);

void clearScreen();
void clearConsole(surf_console *con);
void setConTextColor(surf_console *con, TEXTCOLOR color);
void setPoint(surf_point *pt, u_int x, u_int y);
void switchConsole(u_int conNum);

void syncVideoConsole(bool);

/* 10/5/26 (USB boot): VGA text or the framebuffer, see kernel/console.c */
const char *console_describe(char *buf, size_t size);
void console_replay_log(void);   /* the early boot messages, on a screen that did not exist yet */
void init_console_grid(void);    /* a framebuffer's whole grid, after init_interrupt() (the heap faults its pages in) */

/* 10/5/26: escape sequences. Back to plain text (no sequence half read, the writer's colours,
   the cursor shown) when the task that changed them ends, and for a panic report. */
void console_reset_ansi(surf_console *con);
void console_task_gone(struct surf_task *task);   /* interrupts off */

/* 10/6/26: a task's console output kept in memory instead of shown (the user shell's
   redirection of a kernel command, kernel/syscall.c). From console_capture_begin() until
   console_capture_end() every byte the calling task prints on its own console, outside
   interrupts, goes into buf (escape sequences too); what does not fit is counted in cut. */
struct kcapture {
    char *buf;
    u_int len, size;
    u_long cut;
};
void console_capture_begin(struct kcapture *k);
void console_capture_end(void);

/* 10/6/26: the pager (TTY_PAGE, tty.h): the console counts the rows and asks, tty.c says who
   is paged. */
void console_page_start(surf_console *con, struct surf_task *owner);  /* a new screenful; NULL ends paging */
void console_page_read(surf_console *con);   /* the terminal is read: a new screenful; a dropped question shows */
void console_page_new(surf_console *con);    /* a new screenful, nothing dropped any more (a question comes, say) */
int console_paging(surf_console *con);       /* is what the current task prints there paged? */
void console_page_call_off(surf_console *con);  /* interrupts off: the task at the question goes on without a key */
void console_panic(void);                         /* also takes the screen back (framebuffer_panic) */

extern surf_console *conActive;
extern surf_console conArray[];

#endif
