/*
SurfOS Terminals
----------------------
File: tty.h     Date: 10/3/26 (roadmap C1)
----------------------
One tty per virtual console: its own input queue, readers' wait queue and line
history. The active tty receives the keyboard and the serial port; a task reads
from the tty of the console it was started on, so a shell on console 2 only sees
keys typed while console 2 is displayed.
10/6/26: and one per pseudo-terminal (surfos/pty.h, kernel/pty.c), after them in ttys[]:
its keys come from the master side, its console is a stream to the master side.
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
    wait_queue_t page_waiters;          /* 10/6/26: paged tasks waiting while another one is at --More-- */
    volatile u_char hungup;             /* 10/6/26: a pty whose master side is closed: no key comes, reads end */
};

extern struct tty ttys[NUM_TTYS];     /* 10/6/26: F1-F4, then the pseudo-terminals' */
extern struct tty *tty_active;

void init_tty(void);
struct tty *tty_for_console(surf_console *con);
int tty_number(struct tty *t);          /* 10/6/26: 0-3 for F1-F4, PROC_TTY_PTY + n for pty n (surfos/proc.h), -1 */
void tty_reset(struct tty *t);          /* 10/6/26: a pseudo-terminal's, as new: no keys, line mode, no history */

/* input side (interrupt handlers call tty_input) */
void tty_input(struct tty *t, u_char c);  /* 10/6/26: Ctrl+C in line mode interrupts the job in the foreground */
/* 10/6/26: a byte from a terminal at the end of a line (the serial port, a pty's master side): in
   line mode CR is Enter, DEL Backspace, the cursor keys' sequences those keys; raw mode passes it */
void tty_term_input(struct tty *t, u_char *esc, u_char c);
int tty_term_queues(struct tty *t, u_char esc, u_char c);     /* 10/6/26: does that byte take a place in the queue? */
int tty_getc(struct tty *t);            /* blocks; 0-255; 10/6/26: -1 when the reader's call is cut short */
int tty_getc_unless(struct tty *t, volatile u_char *cancel);   /* 10/6/26, the pager: -1 once *cancel is set */
int tty_trygetc(struct tty *t);         /* -1 when nothing is queued */
void tty_flush(struct tty *t);
u_int tty_room(struct tty *t);          /* 10/6/26: how many more keys the input queue takes */
int tty_queued(struct tty *t);          /* 10/6/26: a key is waiting to be read (poll) */

/* line discipline: echo, backspace, Ctrl-U, Ctrl-C, Up/Down history; returns the length.
   10/6/26: -1 and an empty line when the reader's call is cut short (surfos/task.h) */
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
   key) shows the next screenful, Enter one more line; q, Q, Esc or Ctrl-C drops the rest
   (an arrow key from a serial terminal begins with Esc, so it does too). The pause
   happens only where a task may wait (task context, interrupts on, no KCRIT_ENTER, no
   mutex held), never in raw mode, and not while another task waits to read the terminal;
   elsewhere it comes at the next row where it may. A task that starts to read calls the
   question off, and so do the end of paging and its owner's end: the asker goes on as
   after Space. While the question is up, other paged tasks wait for the answer; anyone
   else who prints (kernel threads, interrupt handlers: never paged, nor captured output)
   takes it away until their next newline. Reading from the terminal, raw mode on or off
   and clearing the screen (ESC[2J) start a new screenful. A drop ends with paging, when a
   paged task reads (what it printed since its last newline, its question, is shown then)
   and when one ends (its crash report shows); nothing else of the dropped output comes
   back. TTY_PAGE 0 from the owner ends paging, and so does the owner's end; an ancestor of
   the owner (a shell whose background job turned paging on) takes it over. For a task
   that is paged already (a program the user shell started) TTY_PAGE 1 and 0 only start a
   new screenful and end a drop: it stays paged while its shell pages. Any other task gets
   EBUSY while the pager is in use. The user shell pages every command it runs, the
   kernel's shell its own commands. Kernel code: kernel/console.c (the rows, the
   question) and kernel/tty.c (who is paged). 10/6/26: Ctrl+C in line mode while the
   owner pages a job in the foreground interrupts the job instead of reaching a reader
   (surfos/task.h): a kernel command it runs in its own task is cut short (its status
   130, the owner lives on), and the innermost of the programs it started while paging
   are killed.
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
int tty_read(struct tty *t, char *buf, size_t len);  /* a line with its newline; raw: the keys queued (at least one); -EINTR */
void tty_task_gone(struct surf_task *task);          /* interrupts off */

/* 10/6/26: pseudo-terminals (kernel/pty.c): a task on one ended (from tty_task_gone()); when
   no task has the pty as its terminal any more, its session has ended */
void pty_task_gone(struct surf_task *task);

#endif
