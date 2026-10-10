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
10/6/26: a read gives up when the reader's call is cut short (a kill waits for its end,
or Ctrl+C: surfos/task.h), and Ctrl+C in line mode interrupts the job in the foreground.
10/6/26: tty_term_input(): the serial line's input rules, which a pseudo-terminal's master
side (kernel/pty.c) follows too.
10/6/26: the pseudo-terminals' ttys follow the consoles' in ttys[] (NUM_TTYS in all), each
with its stream console: raw mode, the pager, Ctrl+C, TTY_GETSIZE (PTY_SETSIZE's size) and
TTY_CAPS (UTF-8 and colour) work on them as on F1-F4; the keyboard, the serial port and
console switching stay with F1-F4.
10/6/26: a line-mode Ctrl+C drops the keys queued before it, on every terminal (POSIX's
VINTR without NOFLSH): what was typed ahead never runs after an interrupt. Room appearing in
a pty's full input queue wakes poll() (its master is ready for writing again, kernel/pty.c).
10/6/26: the rest of a paste is skipped (TTY_SKIP in tty.h): after raw mode's end dropped
keys, or ttyctl(TTY_SKIP, 1), tty_term_input() drops what comes until its input pauses.
10/9/26 (Wi-Fi): tty_readline_secret(), a line that is never shown, for a password: the line
discipline with its echo off for that one line (only the Enter, or ^C, is echoed), no history
either way, and each key wiped from the input queue as it is read.
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
#include <surfos/proc.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <fs/vfs.h>
#include <blibc_common.h>

struct tty ttys[NUM_TTYS];      /* 10/6/26: F1-F4, then the pseudo-terminals' */
struct tty *tty_active;

void init_tty(void) {
    int i;
    for(i = 0; i < NUM_TTYS; i++) {
        memset(&ttys[i], 0, sizeof(struct tty));
        ttys[i].index = i;
        ttys[i].con = &conArray[i];
        wq_init(&ttys[i].readers);
        wq_init(&ttys[i].page_waiters);
    }
    tty_active = &ttys[0];
}

/* 10/6/26: a console's tty, a pseudo-terminal's too (conArray[i] is ttys[i]'s console) */
struct tty *tty_for_console(surf_console *con) {
    if(con >= conArray && con < conArray + NUM_TTYS) return &ttys[con - conArray];
    return &ttys[0];
}

/* 10/6/26: the number programs see (surfos/proc.h): 0-3 for F1-F4, PROC_TTY_PTY + n for pty n */
int tty_number(struct tty *t) {
    if(t >= ttys && t < ttys + NUM_CONSOLES) return (int)(t - ttys);
    if(t >= ttys + NUM_CONSOLES && t < ttys + NUM_TTYS) return PROC_TTY_PTY + (int)(t - ttys - NUM_CONSOLES);
    return -1;
}

/* 10/6/26: a pseudo-terminal's tty as new (kernel/pty.c): no keys queued, line mode, no history.
   Nobody reads it meanwhile, so its wait queues are left as they are (empty). */
void tty_reset(struct tty *t) {
    u_long flags = irq_save();
    t->hungup = 0;
    t->skip = 0;
    t->in_head = t->in_tail = 0;
    t->hist_count = 0;
    t->dropped = 0;
    t->raw = 0;
    t->raw_owner = NULL;
    irq_restore(flags);
}

/**** input ****/

/* 10/6/26: the input queue holds no more keys (a pty's master waits for room, kernel/pty.c) */
static int queue_full(struct tty *t) {
    return (t->in_head + 1) % TTY_INPUT_SIZE == t->in_tail;
}

/* 10/6/26: keys left a pty's input queue that was full (read, or dropped): its master side can be
   written again, which poll() reports (POLLOUT) once woken. Interrupts off. */
static void room_made(struct tty *t, int was_full) {
    if(was_full && t->con->stream) poll_wake();
}

/* 10/6/26: what is queued is dropped (Ctrl+C, raw mode's end, tty_flush()). Interrupts off. */
static void drop_queued(struct tty *t) {
    int full = queue_full(t);
    t->in_tail = t->in_head;
    room_made(t, full);
}

void tty_input(struct tty *t, u_char c) {
    u_long flags = irq_save();
    u_int next;
    /* 10/6/26: Ctrl+C in line mode drops what was typed ahead, so none of it runs after the
       interrupt (POSIX's VINTR, without NOFLSH). While the pager's owner has a job in the
       foreground (a command it runs, the programs it started) the job is interrupted (task.c)
       and the key goes no further; with nothing to interrupt it is a key (tty_readline()). */
    if(c == 0x03 && !t->raw) {
        drop_queued(t);
        if(t->con->page_owner && task_interrupt_job(t->con->page_owner)) {
            irq_restore(flags);
            return;
        }
    }
    next = (t->in_head + 1) % TTY_INPUT_SIZE;
    if(next == t->in_tail) {
        t->dropped++;
    } else {
        t->inq[t->in_head] = c;
        t->in_head = next;
    }
    irq_restore(flags);
    wake_up(&t->readers);
    poll_wake();                        /* 10/6/26: poll() on the console (fs/devfs.c) */
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
    poll_wake();
}

/* 10/6/26: a byte from a terminal at the other end of a line: the serial port (driver/serial.c),
   and a pseudo-terminal's master side (kernel/pty.c). In line mode CR is Enter and DEL is
   Backspace, and the escape sequences of the cursor keys (ESC [ or ESC O, then the final byte)
   become those keys; other sequences are swallowed to their final byte. A lone Esc answers the
   pager's question (it drops the rest), and the rest of a sequence that follows is still
   swallowed. A Ctrl+C ends a sequence in progress and goes through: a sequence cut short (a
   cursor key that did not fit a full queue) never swallows it. In raw mode (tty.h) the bytes
   arrive as they were sent. *esc is the line's state (0 normal, 1 after ESC, 2 inside ESC [ or
   ESC O); one per line. From interrupt handlers too. 10/6/26: while the rest of a paste is
   skipped (TTY_SKIP, tty.h), bytes that come less than TTY_SKIP_MS after the one before are
   dropped, whatever the mode; a Ctrl+C, or a byte after such a pause, ends the skip. */
void tty_term_input(struct tty *t, u_char *esc, u_char c) {
    u_long flags = irq_save(), now = getticks();
    int burst = now - t->last_in < ms_to_ticks(TTY_SKIP_MS);
    t->last_in = now;
    if(t->skip) {
        if(burst && c != 0x03) {        /* the paste goes on: dropped */
            irq_restore(flags);
            return;
        }
        t->skip = 0;                    /* a pause, or Ctrl+C: input again, from a fresh start */
        *esc = 0;
    }
    irq_restore(flags);
    if(t->raw) {
        *esc = 0;
        tty_input(t, c);
        return;
    }
    if(c == 0x03) {                     /* 10/6/26: before the sequence: Ctrl+C always reaches tty_input() */
        *esc = 0;
        tty_input(t, c);
        return;
    }
    if(*esc == 1) {
        *esc = (c == '[' || c == 'O') ? 2 : 0;
        return;
    }
    if(*esc == 2) {
        if(c >= 0x40 && c <= 0x7E) {    /* the final byte: arrows and friends become key codes */
            *esc = 0;
            switch(c) {
            case 'A': tty_input(t, UP); break;
            case 'B': tty_input(t, DOWN); break;
            case 'C': tty_input(t, RT); break;
            case 'D': tty_input(t, LEFT); break;
            case 'H': tty_input(t, HOME); break;
            case 'F': tty_input(t, END); break;
            default: break;
            }
        }
        return;
    }
    if(c == 0x1B) {
        if(t->con->page_asker) tty_input(t, 0x1B);
        *esc = 1;
        return;
    }
    if(c == '\r') c = '\n';
    if(c == 0x7F) c = 0x08;
    tty_input(t, c);
}

/* 10/6/26: does byte c, given to tty_term_input() with the line's state esc, need room in the input
   queue? Not the rest of an escape sequence that is swallowed, nor a line-mode Ctrl+C (it empties
   the queue first, and interrupts the job or is queued then, tty_input()), nor a lone Esc with no
   question up. A pty's master takes those whatever room is left (kernel/pty.c). */
int tty_term_queues(struct tty *t, u_char esc, u_char c) {
    if(t->skip && c != 0x03 && getticks() - t->last_in < ms_to_ticks(TTY_SKIP_MS))
        return 0;                                   /* the rest of a paste: dropped (tty_term_input()) */
    if(t->raw) return 1;
    if(c == 0x03) return 0;                         /* even inside a sequence: it ends it (tty_term_input()) */
    if(esc == 1) return 0;
    if(esc == 2) return c == 'A' || c == 'B' || c == 'C' || c == 'D' || c == 'H' || c == 'F';
    if(c == 0x1B) return t->con->page_asker != NULL;
    return 1;
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
    int c = -1, full = queue_full(t);
    if(t->in_tail != t->in_head) {
        c = t->inq[t->in_tail];
        t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
        room_made(t, full);
        console_page_read(t->con);
    }
    irq_restore(flags);
    return c;
}

/* 10/6/26: -1 when the reader's call is cut short (task_interrupted()): the keys stay queued for
   whoever reads next. -1 also on a pseudo-terminal that has hung up (kernel/pty.c): no key comes. */
int tty_getc(struct tty *t) {
    console_page_read(t->con);
    for(;;) {
        u_long flags = irq_save();
        if((task_interrupted() && !in_interrupt()) || t->hungup) {
            irq_restore(flags);
            return -1;
        }
        if(t->in_tail != t->in_head) {
            int c = t->inq[t->in_tail], full = queue_full(t);
            t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
            room_made(t, full);
            console_page_read(t->con);
            irq_restore(flags);
            return c;
        }
        if(curTask && curTask != idle_task && !in_interrupt()) {
            /* 10/6/26: a task at the pager's question (console.c) goes on without a key: the
               keys are this reader's now */
            if(t->con->page_asker && t->con->page_asker != curTask) console_page_call_off(t->con);
            wait_prepare_intr(&t->readers);   /* enqueued before interrupts come back: no lost wakeup */
            irq_restore(flags);
            yield();
        } else {
            irq_restore(flags);          /* boot code or an ISR: nothing to switch to */
        }
    }
}

/* 10/6/26: the pager's question (console.c): a key, or -1 once *cancel is set. Whoever sets
   it wakes this task (console_page_call_off()). It is no read of the terminal: the
   screenful is the pager's own business. -1 too when the asker's call is cut short. */
int tty_getc_unless(struct tty *t, volatile u_char *cancel) {
    for(;;) {
        u_long flags = irq_save();
        if(*cancel || (task_interrupted() && !in_interrupt()) || t->hungup) {
            irq_restore(flags);
            return -1;
        }
        if(t->in_tail != t->in_head) {
            int c = t->inq[t->in_tail], full = queue_full(t);
            t->in_tail = (t->in_tail + 1) % TTY_INPUT_SIZE;
            room_made(t, full);
            irq_restore(flags);
            return c;
        }
        if(curTask && curTask != idle_task && !in_interrupt()) {
            wait_prepare_intr(&t->readers);
            irq_restore(flags);
            yield();
        } else {
            irq_restore(flags);
        }
    }
}

/* 10/6/26: how many more keys the input queue takes (a pty's master waits for room, kernel/pty.c) */
u_int tty_room(struct tty *t) {
    return (t->in_tail + TTY_INPUT_SIZE - t->in_head - 1) % TTY_INPUT_SIZE;
}

/* 10/6/26: is a key waiting to be read? (poll() on the console, fs/devfs.c) */
int tty_queued(struct tty *t) {
    return t->in_head != t->in_tail;
}

void tty_flush(struct tty *t) {
    u_long flags = irq_save();
    drop_queued(t);
    irq_restore(flags);
}

/**** line discipline ****/

static void echo(struct tty *t, int c) {
    console_echo(t->con, t->con->txtColor, c);      /* 10/6/26: never into a kept output (console.c) */
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

/* 10/9/26: the slot of the key just read is wiped (a secret's keys stay nowhere). Only the
   reader frees slots, and an interrupt handler writes one only while it is free and not the one
   behind in_tail (the queue is full then), so it is this key's still. */
static void scrub_last(struct tty *t) {
    u_long flags = irq_save();
    t->inq[(t->in_tail + TTY_INPUT_SIZE - 1) % TTY_INPUT_SIZE] = 0;
    irq_restore(flags);
}

/* the line discipline; 10/9/26: secret, the echo is off for this line (tty_readline_secret()) */
static int readline(struct tty *t, char *buf, size_t size, int secret) {
    size_t len = 0;
    int pos = t->hist_count;            /* history cursor; == hist_count means the line being typed */
    int oldest = t->hist_count > TTY_HISTORY ? t->hist_count - TTY_HISTORY : 0;
    char saved[TTY_LINE_MAX];

    if(!size) return 0;
    saved[0] = 0;
    buf[0] = 0;

    for(;;) {
        int c = tty_getc(t);

        if(c >= 0 && secret) scrub_last(t);
        if(c < 0) {     /* 10/6/26: the call is cut short: no line. Ctrl+C says so, as below */
            if(curTask->intr & TI_INT) { echo(t, '^'); echo(t, 'C'); echo(t, '\n'); }
            else if(len) echo(t, '\n');
            if(secret) memset(buf, 0, len);
            buf[0] = 0;
            return -1;
        }
        if(c == '\n' || c == '\r') {
            echo(t, '\n');
            buf[len] = 0;
            if(len && !secret) history_add(t, buf);
            return (int)len;
        }
        if(c == 0x08 || c == 0x7F) {
            if(len) {
                len--;
                if(secret) buf[len] = 0;
                else echo(t, '\b');
            }
            continue;
        }
        if(c == 0x15) { /* Ctrl-U: clear the line */
            if(secret) memset(buf, 0, len);
            else erase(t, len);
            len = 0;
            continue;
        }
        if(c == 0x03) { /* Ctrl-C: drop the line */
            echo(t, '^'); echo(t, 'C'); echo(t, '\n');
            if(secret) memset(buf, 0, len);
            buf[0] = 0;
            return 0;
        }
        if(c == UP || c == DOWN) {
            int want;
            if(secret) continue;        /* 10/9/26: no history for a secret, nor into one */
            want = pos + (c == UP ? -1 : 1);
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
            if(!secret) echo(t, c);
        }
    }
}

int tty_readline(struct tty *t, char *buf, size_t size) {
    return readline(t, buf, size, 0);
}

int tty_readline_secret(struct tty *t, char *buf, size_t size) {
    return readline(t, buf, size, 1);
}

/* read(2) on the console (fs/devfs.c). Line mode: a line with its newline, cut to len.
   Raw mode: whatever keys are queued, at least one (it waits for that one), up to len.
   10/6/26: -EINTR when the reader's call is cut short (a kill, Ctrl+C); 0, the end of the
   input, on a pseudo-terminal that has hung up. */
int tty_read(struct tty *t, char *buf, size_t len) {
    char line[TTY_LINE_MAX];
    int n;
    if(!len) return 0;
    if(t->raw) {
        size_t got = 0;
        n = tty_getc(t);
        if(n < 0) return t->hungup ? 0 : -EINTR;
        buf[got++] = (char)n;
        while(got < len && (n = tty_trygetc(t)) >= 0) buf[got++] = (char)n;
        return (int)got;
    }
    n = tty_readline(t, line, sizeof(line));
    if(n < 0) return t->hungup ? 0 : -EINTR;
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
   not line editing), so leaving raw mode drops what was not read yet. 10/6/26: keys after
   the Enter are a paste's rest, and what follows them is skipped (TTY_SKIP); not a lone LF
   or NUL, the end of the Enter itself from a terminal that sends CR LF (or CR NUL, telnet). */
static void set_raw(struct tty *t, int on, struct surf_task *owner) {
    u_long flags = irq_save();
    if(t->raw && !on) {
        u_int n = (t->in_head + TTY_INPUT_SIZE - t->in_tail) % TTY_INPUT_SIZE;
        if(n > 1 || (n == 1 && t->inq[t->in_tail] != '\n' && t->inq[t->in_tail] != 0)) t->skip = 1;
        drop_queued(t);
    }
    t->raw = on;
    t->raw_owner = on ? owner : NULL;
    irq_restore(flags);
}

/* 10/6/26: TTY_PAGE (tty.h). Paging belongs to the task that turned it on, its owner: what
   the owner and the tasks descended from it print on this terminal is paged until the
   owner turns it off or ends (tty_task_gone). TTY_PAGE 1 again from the owner starts a new
   screenful. An ancestor of the owner takes the pager over (a shell whose job in the
   background turned paging on, and which runs a command now). A task that is paged
   already, a program the owner started, keeps being paged whatever it asks: TTY_PAGE 1
   and 0 start a new screenful for it and end a drop (a program of its own starts, or one
   has ended), so a nested shell's prompt is seen and never asked about. Any other task
   cannot take the pager while it is in use: EBUSY, and its TTY_PAGE 0 changes nothing. */
static int tty_page(struct tty *t, int on) {
    surf_console *con = t->con;
    struct surf_task *o;
    int r = 0, depth;
    u_long flags = irq_save();
    for(o = con->page_owner, depth = 0; o && o != curTask && depth < 64; o = o->parent, depth++);
    if(!con->page_owner || o == curTask) {              /* free, its own, or an ancestor's to take over */
        if(on) console_page_start(con, curTask);
        else if(con->page_owner) console_page_start(con, NULL);
    } else if(console_paging(con)) {
        console_page_new(con);
    } else if(on) {
        r = -16;                                  /* EBUSY */
    }
    irq_restore(flags);
    return r;
}

/* terminal control for full-screen programs (see tty.h) */
int tty_ctl(struct tty *t, int op, u32 arg) {
    if(!t) return -9;                             /* EBADF */
    if(op == TTY_GETSIZE) {                       /* 10/6/26: a pseudo-terminal's is PTY_SETSIZE's */
        if(t->con->stream) return (int)((t->con->cols << 16) | t->con->rows);
        return (COLUMNS << 16) | LINES;
    }
    /* 10/6/26: the console draws UTF-8 (as code page 437, console.c) and the ANSI colours.
       `ascii` on the kernel command line, for a serial terminal that cannot show UTF-8, leaves
       UTF-8 out: the user shell then draws its prompt in ASCII. A pseudo-terminal passes both
       on to the terminal at its far end, which is taken to draw them (the telnet client). */
    if(op == TTY_CAPS) {
        if(t->con->stream) return TTY_CAP_COLOR | TTY_CAP_UTF8;
        return TTY_CAP_COLOR | (bootinfo_has_arg(&bootinfo, "ascii") ? 0 : TTY_CAP_UTF8);
    }
    if(op == TTY_PAGE) return arg > 1 ? -22 : tty_page(t, (int)arg);
    if(op == TTY_SKIP) {                          /* 10/6/26: the caller dropped keys it had read (tty.h) */
        u_long flags;
        if(arg > 1) return -22;
        flags = irq_save();
        t->skip = (u_char)arg;
        irq_restore(flags);
        return 0;
    }
    if(op == TTY_RAW) {
        if(arg > 1) return -22;                   /* EINVAL */
        set_raw(t, (int)arg, curTask);
        /* 10/6/26: a full-screen program is never paged nor dropped (raw mode on reads, as far as
           the pager is concerned); after it, a new screenful */
        if(arg) console_page_read(t->con);
        else console_page_new(t->con);
        return 0;
    }
    return -22;                                   /* EINVAL */
}

/* A task is gone (it exited or was killed): raw mode it set ends with it, and so do the
   colours and the hidden cursor it left on a console (console.c), so a crashed editor
   cannot leave its shell's console raw or unreadable. From task.c with interrupts off,
   and from trap_fatal() before the report. 10/6/26: the pseudo-terminals too, and a session
   whose last task this was has ended (kernel/pty.c). */
void tty_task_gone(struct surf_task *task) {
    int i;
    for(i = 0; i < NUM_TTYS; i++) {
        if(ttys[i].raw && ttys[i].raw_owner == task) set_raw(&ttys[i], 0, NULL);
    }
    console_task_gone(task);
    pty_task_gone(task);
}
