/*
SurfOS Terminals
--------------------
File: tty.c     Date: 10/3/26 (roadmap C1)
--------------------
10/5/26: raw mode for full-screen programs (tty.h): no echo, no line editing, keys as
the bytes a terminal sends. It ends with the task that set it.
10/6/26: the serial terminal's repaint sends a cell's code page 437 character as UTF-8,
TTY_CAPS says what the console draws, and TTY_PAGE pages a task's output (the pager
itself is in console.c: it counts the rows and asks; here is who is paged).
*/

#include <surfos/types.h>
#include <surfos/tty.h>
#include <surfos/console.h>
#include <surfos/keyboard.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/interrupt.h>
#include <surfos/multiboot.h>
#include <sys/serial.h>
#include <sys/cp437.h>
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
        wq_init(&ttys[i].page_waiters);
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

/* several bytes that belong together (an escape sequence): all of them or none */
static void input_seq(struct tty *t, const char *s) {
    u_long flags = irq_save();
    u_int n = strlen(s), room = (t->in_tail + TTY_INPUT_SIZE - t->in_head - 1) % TTY_INPUT_SIZE;
    if(n > room) {
        t->dropped += n;
    } else {
        for(; *s; s++) {
            t->inq[t->in_head] = (u_char)*s;
            t->in_head = (t->in_head + 1) % TTY_INPUT_SIZE;
        }
    }
    irq_restore(flags);
    wake_up(&t->readers);
}

/* 10/5/26: what a terminal sends for the special keys, which raw mode passes on (tty.h) */
static const char *const raw_keys[RT - INS + 1] = {
    "\033[2~", "\033[3~", "\033[H", "\033[F", "\033[5~", "\033[6~",     /* INS DEL HOME END PGUP PGDN */
    "\033[D", "\033[A", "\033[B", "\033[C"                                /* LEFT UP DOWN RT */
};

/* A key from the keyboard, as a code from keyboard.h. Line mode queues the code; raw mode
   queues the escape sequence for it, and F5-F12 (which have none) are dropped. */
void tty_key(struct tty *t, u_char c) {
    if(t->raw && c >= 0x80) {
        if(c >= INS && c <= RT) input_seq(t, raw_keys[c - INS]);
        return;
    }
    tty_input(t, c);
}

/* 10/6/26: reading the terminal means the user sees the screen: the pager (console.c) starts
   a new screenful, and when the reader's own output was being dropped after q, it shows the
   last line it dropped, the question the reader asks perhaps, and drops no more */
int tty_trygetc(struct tty *t) {
    u_long flags = irq_save();
    int c = -1;
    if(t->in_tail != t->in_head) {
        c = t->inq[t->in_tail];
        t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
        console_page_read(t->con);
    }
    irq_restore(flags);
    return c;
}

int tty_getc(struct tty *t) {
    console_page_read(t->con);
    for(;;) {
        u_long flags = irq_save();
        if(t->in_tail != t->in_head) {
            int c = t->inq[t->in_tail];
            t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
            console_page_read(t->con);
            irq_restore(flags);
            return c;
        }
        if(curTask && curTask != idle_task && !in_interrupt()) {
            /* 10/6/26: a task at the pager's question (console.c) goes on without a key: the
               keys are this reader's now */
            if(t->con->page_asker && t->con->page_asker != curTask) console_page_call_off(t->con);
            wait_prepare(&t->readers);   /* enqueued before interrupts come back: no lost wakeup */
            irq_restore(flags);
            yield();
        } else {
            irq_restore(flags);          /* boot code or an ISR: nothing to switch to */
        }
    }
}

/* 10/6/26: the pager's question (console.c): a key, or -1 once *cancel is set. Whoever sets
   it wakes this task (console_page_call_off()). It is no read of the terminal: the
   screenful is the pager's own business. */
int tty_getc_unless(struct tty *t, volatile u_char *cancel) {
    for(;;) {
        u_long flags = irq_save();
        if(*cancel) {
            irq_restore(flags);
            return -1;
        }
        if(t->in_tail != t->in_head) {
            int c = t->inq[t->in_tail];
            t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
            irq_restore(flags);
            return c;
        }
        if(curTask && curTask != idle_task && !in_interrupt()) {
            wait_prepare(&t->readers);
            irq_restore(flags);
            yield();
        } else {
            irq_restore(flags);
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

/* read(2) on the console (fs/devfs.c). Line mode: a line with its newline, cut to len.
   Raw mode: whatever keys are queued, at least one (it waits for that one), up to len. */
int tty_read(struct tty *t, char *buf, size_t len) {
    char line[TTY_LINE_MAX];
    int n;
    if(!len) return 0;
    if(t->raw) {
        size_t got = 0;
        buf[got++] = (char)tty_getc(t);
        while(got < len && (n = tty_trygetc(t)) >= 0) buf[got++] = (char)n;
        return (int)got;
    }
    n = tty_readline(t, line, sizeof(line));
    if(n < 0) return n;
    line[n++] = '\n';
    if((size_t)n > len) n = (int)len;
    memcpy(buf, line, n);
    return n;
}

/**** console switching ****/

/* the last column of a row with something in it, -1 for an empty row. 10/6/26: a cell is a
   code page 437 character, and only the empty cell, space and no-break space are blank
   (▸ is 0x10, below ' ') */
static int last_used(surf_console *con, int row) {
    int col, last = -1;
    u_char ch;
    for(col = 0; col < (int)COLUMNS; col++) {
        ch = con->vmem[(row * COLUMNS + col) * 2];
        if(ch != 0 && ch != ' ' && ch != 0xFF) last = col;
    }
    return last;
}

/* Repaint the newly active console on the serial terminal: clear, text, cursor. The
   text comes without its colours; the terminal's attributes are reset first and the
   cursor is shown or hidden as on that console (10/5/26, escape sequences).
   10/5/26: the grid may be larger than the terminal (120x33 on a 1920x1080 screen), and
   nothing tells SurfOS the terminal's size. So the rows are painted only down to the last
   one in use (or the cursor's), each up to its last character, and the cursor is put
   back by moving up and right from there rather than to a screen position: a terminal as
   large as the grid shows the screen exactly, a smaller one shows its bottom part, with
   rows longer than the terminal is wide wrapped. 10/6/26: each cell goes as the UTF-8 of
   its code page 437 character (the terminal is taken to speak UTF-8, as for the mirror). */
static void serial_redraw(surf_console *con) {
    char seq[16], utf[CP437_UTF8_MAX];
    int row, col, rows = con->loc.y, n, k;
    serial_puts(con->cursor_off ? "\033[0m\033[?25l" : "\033[0m\033[?25h");
    serial_console_clear();
    for(row = con->loc.y + 1; row < (int)LINES; row++) if(last_used(con, row) >= 0) rows = row;
    for(row = 0; row <= rows; row++) {
        int last = last_used(con, row);
        for(col = 0; col <= last; col++) {
            n = cp437_utf8(con->vmem[(row * COLUMNS + col) * 2], utf);
            for(k = 0; k < n; k++) serial_putc(utf[k]);
        }
        if(row < rows) serial_puts("\r\n");
    }
    serial_putc('\r');
    if(rows > (int)con->loc.y) {
        snprintf(seq, sizeof(seq), "\033[%dA", rows - (int)con->loc.y);
        serial_puts(seq);
    }
    if(con->loc.x) {
        snprintf(seq, sizeof(seq), "\033[%uC", con->loc.x);
        serial_puts(seq);
    }
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

/**** raw mode (10/5/26) ****/

/* Keys typed in one mode mean something else in the other (0x7f and escape sequences are
   not line editing), so leaving raw mode drops what was not read yet. */
static void set_raw(struct tty *t, int on, struct surf_task *owner) {
    u_long flags = irq_save();
    if(t->raw && !on) t->in_tail = t->in_head;
    t->raw = on;
    t->raw_owner = on ? owner : NULL;
    irq_restore(flags);
}

/* 10/6/26: TTY_PAGE (tty.h). Paging belongs to the task that turned it on, its owner: what
   the owner and the tasks descended from it print on this terminal is paged until the
   owner turns it off or ends (tty_task_gone). TTY_PAGE 1 again from the owner starts a new
   screenful. A task that is paged already, a program the owner started, keeps being paged
   whatever it asks: TTY_PAGE 1 and 0 change nothing for it except that output dropped
   after q shows again (a program of its own starts, or one has ended), so a nested shell
   is seen. Any other task cannot take the pager while it is in use: EBUSY. */
static int tty_page(struct tty *t, int on) {
    surf_console *con = t->con;
    int r = 0;
    u_long flags = irq_save();
    if(!con->page_owner || con->page_owner == curTask) {
        if(on) console_page_start(con, curTask);
        else if(con->page_owner) console_page_start(con, NULL);
    } else if(console_paging(con)) {
        con->page_drop = 0;
    } else if(on) {
        r = -16;                                  /* EBUSY */
    }
    irq_restore(flags);
    return r;
}

/* terminal control for full-screen programs (see tty.h) */
int tty_ctl(struct tty *t, int op, u32 arg) {
    if(!t) return -9;                             /* EBADF */
    if(op == TTY_GETSIZE) return (COLUMNS << 16) | LINES;
    /* 10/6/26: the console draws UTF-8 (as code page 437, console.c) and the ANSI colours.
       `ascii` on the kernel command line, for a serial terminal that cannot show UTF-8, leaves
       UTF-8 out: the user shell then draws its prompt in ASCII. */
    if(op == TTY_CAPS) return TTY_CAP_COLOR | (bootinfo_has_arg(&bootinfo, "ascii") ? 0 : TTY_CAP_UTF8);
    if(op == TTY_PAGE) return arg > 1 ? -22 : tty_page(t, (int)arg);
    if(op == TTY_RAW) {
        if(arg > 1) return -22;                   /* EINVAL */
        set_raw(t, (int)arg, curTask);
        if(arg) console_page_read(t->con);        /* 10/6/26: a full-screen program is never paged, nor dropped */
        return 0;
    }
    return -22;                                   /* EINVAL */
}

/* A task is gone (it exited or was killed): raw mode it set ends with it, and so do the
   colours and the hidden cursor it left on a console (console.c), so a crashed editor
   cannot leave its shell's console raw or unreadable. From task.c with interrupts off,
   and from trap_fatal() before the report. */
void tty_task_gone(struct surf_task *task) {
    int i;
    for(i = 0; i < NUM_CONSOLES; i++) {
        if(ttys[i].raw && ttys[i].raw_owner == task) set_raw(&ttys[i], 0, NULL);
    }
    console_task_gone(task);
}
