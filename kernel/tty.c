/*
SurfOS Terminals
--------------------
File: tty.c     Date: 10/3/26 (roadmap C1)
--------------------
*/

#include <surfos/types.h>
#include <surfos/tty.h>
#include <surfos/console.h>
#include <surfos/keyboard.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/interrupt.h>
#include <sys/serial.h>
#include <blibc_common.h>

struct tty ttys[NUM_CONSOLES];
struct tty *tty_active;

void init_tty(void) {
    int i;
    for(i = 0; i < NUM_CONSOLES; i++) {
        memset(&ttys[i], 0, sizeof(struct tty));
        ttys[i].index = i;
        ttys[i].con = &conArray[i];
        wq_init(&ttys[i].readers);
    }
    tty_active = &ttys[0];
}

struct tty *tty_for_console(surf_console *con) {
    int i;
    for(i = 0; i < NUM_CONSOLES; i++) if(ttys[i].con == con) return &ttys[i];
    return &ttys[0];
}

/**** input ****/

void tty_input(struct tty *t, u_char c) {
    u_long flags = irq_save();
    u_int next = (t->in_head + 1) % TTY_INPUT_SIZE;
    if(next == t->in_tail) {
        t->dropped++;
    } else {
        t->inq[t->in_head] = c;
        t->in_head = next;
    }
    irq_restore(flags);
    wake_up(&t->readers);
}

int tty_trygetc(struct tty *t) {
    u_long flags = irq_save();
    int c = -1;
    if(t->in_tail != t->in_head) {
        c = t->inq[t->in_tail];
        t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
    }
    irq_restore(flags);
    return c;
}

int tty_getc(struct tty *t) {
    for(;;) {
        u_long flags = irq_save();
        if(t->in_tail != t->in_head) {
            int c = t->inq[t->in_tail];
            t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
            irq_restore(flags);
            return c;
        }
        if(curTask && curTask != idle_task && !in_interrupt()) {
            wait_prepare(&t->readers);   /* enqueued before interrupts come back: no lost wakeup */
            irq_restore(flags);
            yield();
        } else {
            irq_restore(flags);          /* boot code or an ISR: nothing to switch to */
        }
    }
}

void tty_flush(struct tty *t) {
    u_long flags = irq_save();
    t->in_tail = t->in_head;
    irq_restore(flags);
}

/**** line discipline ****/

static void echo(struct tty *t, int c) {
    kputch(t->con, t->con->txtColor, c);
}

static void erase(struct tty *t, size_t n) {
    while(n--) echo(t, '\b');
}

static void history_add(struct tty *t, const char *line) {
    int last = (t->hist_count - 1) % TTY_HISTORY;
    if(t->hist_count && !strcmp(t->history[last], line)) return; /* same as the previous one */
    strlcpy(t->history[t->hist_count % TTY_HISTORY], line, TTY_LINE_MAX);
    t->hist_count++;
}

int tty_readline(struct tty *t, char *buf, size_t size) {
    size_t len = 0;
    int pos = t->hist_count;            /* history cursor; == hist_count means the line being typed */
    int oldest = t->hist_count > TTY_HISTORY ? t->hist_count - TTY_HISTORY : 0;
    char saved[TTY_LINE_MAX];

    if(!size) return 0;
    saved[0] = 0;
    buf[0] = 0;

    for(;;) {
        int c = tty_getc(t);

        if(c == '\n' || c == '\r') {
            echo(t, '\n');
            buf[len] = 0;
            if(len) history_add(t, buf);
            return (int)len;
        }
        if(c == 0x08 || c == 0x7F) {
            if(len) { len--; echo(t, '\b'); }
            continue;
        }
        if(c == 0x15) { /* Ctrl-U: clear the line */
            erase(t, len);
            len = 0;
            continue;
        }
        if(c == 0x03) { /* Ctrl-C: drop the line */
            echo(t, '^'); echo(t, 'C'); echo(t, '\n');
            buf[0] = 0;
            return 0;
        }
        if(c == UP || c == DOWN) {
            int want = pos + (c == UP ? -1 : 1);
            if(want < oldest || want > t->hist_count) continue;
            if(pos == t->hist_count) { buf[len] = 0; strlcpy(saved, buf, sizeof(saved)); } /* keep what was typed */
            pos = want;
            erase(t, len);
            if(pos == t->hist_count) strlcpy(buf, saved, size);
            else strlcpy(buf, t->history[pos % TTY_HISTORY], size);
            len = strlen(buf);
            {
                size_t i;
                for(i = 0; i < len; i++) echo(t, buf[i]);
            }
            continue;
        }
        if(c >= 0x80) continue;         /* other special keys are ignored for now */
        if(c == '\t') c = ' ';
        if(len + 1 < size && len + 1 < TTY_LINE_MAX && c >= 32 && c < 127) {
            buf[len++] = (char)c;
            echo(t, c);
        }
    }
}

/**** console switching ****/

/* Repaint the newly active console on the serial terminal: clear, text, cursor. */
static void serial_redraw(surf_console *con) {
    char seq[16];
    int row, col;
    serial_console_clear();
    for(row = 0; row < LINES; row++) {
        int last = -1;
        for(col = 0; col < COLUMNS; col++) if(con->vmem[(row * COLUMNS + col) * 2] > ' ') last = col;
        for(col = 0; col <= last; col++) {
            u_char ch = con->vmem[(row * COLUMNS + col) * 2];
            serial_putc(ch >= 32 && ch < 127 ? ch : ' ');
        }
        if(row < LINES - 1) serial_puts("\r\n");
    }
    snprintf(seq, sizeof(seq), "\033[%u;%uH", con->loc.y + 1, con->loc.x + 1);
    serial_puts(seq);
}

void tty_switch(int index) {
    u_long flags;
    if(index < 0 || index >= NUM_CONSOLES) return;
    flags = irq_save();
    tty_active = &ttys[index];
    switchConsole(index);
    syncVideoConsole(true);
    irq_restore(flags);
    serial_redraw(tty_active->con);
}
