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

#endif
