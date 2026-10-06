/*
SurfOS Terminals
----------------------
File: tty.h     Date: 10/3/26 (roadmap C1)
----------------------
One tty per virtual console: its own input queue, readers' wait queue and line
history. The active tty receives the keyboard and the serial port; a task reads
from the tty of the console it was started on, so a shell on console 2 only sees
keys typed while console 2 is displayed.
*/

#ifndef _SURFOS_TTY_H
#define _SURFOS_TTY_H

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/wait.h>

#define TTY_INPUT_SIZE 256
#define TTY_HISTORY    8
#define TTY_LINE_MAX   256

struct tty {
    int index;
    surf_console *con;
    u_char inq[TTY_INPUT_SIZE];
    volatile u_int in_head, in_tail;    /* ring: head is written by interrupts */
    wait_queue_t readers;
    char history[TTY_HISTORY][TTY_LINE_MAX];
    int hist_count;                     /* lines ever added */
    u_long dropped;                     /* input bytes lost to a full queue */
    volatile int raw;                   /* TTY_RAW (tty_ctl): read by the keyboard and serial ISRs */
    struct surf_task *raw_owner;        /* the task that set it; raw mode ends with that task */
};

extern struct tty ttys[NUM_CONSOLES];
extern struct tty *tty_active;

void init_tty(void);
struct tty *tty_for_console(surf_console *con);

/* input side (interrupt handlers call tty_input) */
void tty_input(struct tty *t, u_char c);
int tty_getc(struct tty *t);            /* blocks; 0-255 */
int tty_trygetc(struct tty *t);         /* -1 when nothing is queued */
void tty_flush(struct tty *t);

/* line discipline: echo, backspace, Ctrl-U, Ctrl-C, Up/Down history; returns the length */
int tty_readline(struct tty *t, char *buf, size_t size);

/* display + input routing to console `index` (F1..Fn) */
void tty_switch(int index);

/* 10/5/26: what a full-screen program (the text editor) needs. TTY_RAW 1: keys arrive one
   at a time, not echoed or edited, Ctrl-C is just 0x03, and special keys come as escape
   sequences: arrows ESC[A ESC[B ESC[C ESC[D, Home ESC[H, End ESC[F, PgUp ESC[5~,
   PgDn ESC[6~, Insert ESC[2~, Delete ESC[3~, Backspace 0x7f, Enter '\r', Tab '\t',
   Esc 0x1b, Ctrl+letter 0x01-0x1a (F1-F4 still switch consoles). TTY_RAW 0: lines again;
   raw mode also ends when the task that set it exits. TTY_GETSIZE: (columns << 16) | rows.
   The console draws this ANSI subset (and the serial port passes it through): ESC[H,
   ESC[row;colH (or f), ESC[nA/B/C/D, ESC[J ESC[0J ESC[2J, ESC[K ESC[0K ESC[2K, ESC[m ESC[0m,
   ESC[1m (bright), ESC[7m (inverse), ESC[30-37m, ESC[40-47m, ESC[?25l / ESC[?25h (cursor). */
#define TTY_RAW     1
#define TTY_GETSIZE 2
/* 10/6/26 (the SurfOS shell). TTY_PAGE 1: the output of the calling task and of the tasks
   descended from it is paged; the caller owns the pager of this terminal. After a
   screenful (the console's rows less one) the console shows TTY_MORE in inverse video on
   the next row, the last one of a full screen, and waits for a key: Space (or any other
   key) shows the next screenful, Enter one more line, q, Q or Esc drops the rest until
   paging ends. The pause happens only where a task may wait (task context, interrupts on,
   no KCRIT_ENTER, no mutex held) and never in raw mode; elsewhere it comes at the next row
   where it may. Other tasks' output (kernel threads, interrupt handlers) and captured
   output are never paged. Reading from the terminal starts a new screenful and shows
   again what q dropped. TTY_PAGE 0 from the owner ends paging, and so does the owner's
   end. A task that is paged already stays paged: TTY_PAGE 1 or 0 from it only ends a drop
   (a nested shell starts or ends a command). Another task gets EBUSY while the pager is in
   use. The user shell pages every command it runs, the kernel's shell its own commands.
   Kernel code: kernel/console.c (the rows, the question) and kernel/tty.c (who is paged).
   TTY_CAPS: TTY_CAP_* flags. TTY_CAP_UTF8: the console draws UTF-8 text: ASCII, the
   characters of code page 437 (Latin-1 letters, box drawing, blocks, arrows, Greek, some
   mathematics) and SurfOS's ▸ ≋ … ; any other character is drawn as '?'. It is left out
   when the kernel command line has the word `ascii` (a serial terminal that cannot show
   UTF-8). TTY_CAP_COLOR: the ANSI colours above. */
#define TTY_PAGE    3
#define TTY_CAPS    4
#define TTY_CAP_UTF8  1
#define TTY_CAP_COLOR 2
#define TTY_MORE    "--More-- (Space: next page, Enter: next line, q: skip the rest)"
int tty_ctl(struct tty *t, int op, u32 arg);   /* 0, the size or the flags; -errno */

/* the kernel's side of raw mode: keys from the keyboard (special keys become their escape
   sequences), read(2) on /dev/console, and the end of the task that set it (task.c) */
struct surf_task;
void tty_key(struct tty *t, u_char c);
int tty_read(struct tty *t, char *buf, size_t len);  /* a line with its newline; raw: the keys queued (at least one) */
void tty_task_gone(struct surf_task *task);          /* interrupts off */

#endif
