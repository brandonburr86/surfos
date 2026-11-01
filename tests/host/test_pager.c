/* Host-side test of the console and the pager (10/6/26): the real kernel/console.c,
   kernel/tty.c and driver/cp437.c, built for the host with tests/host/pager_bridge.c in
   place of the rest of the kernel. Every SurfOS task of a scenario is a thread here, and
   only one runs at a time: run() hands the CPU to a task with something to do, and it
   comes back when the task is done or blocks (yield()); resume() lets a blocked task go on.
   So a task can wait at --More-- while others print, read or end, in any order.
   Covered: the screenful and the question (rows, wraps, the keys, the screen and the
   serial port), where a task may not wait, readers, UTF-8 per writer, TTY_CAPS. Run by
   `make unittest`. 10/6/26: the same pager on a pseudo-terminal's stream console (the
   rows and columns of its size, the question in the stream, a new size), and what else
   the stream does (the writer's colour as SGR, Backspace, UTF-8 and escape sequences in
   the column count), the serial line's input rules (tty_term_input()). 10/9/26 (Wi-Fi): a
   secret line (tty_readline_secret(), a password): nothing typed reaches the screen, the
   serial line or a pty's stream, the history, or stays in the input queue. 10/25/26: a job in
   the background is not paged with its shell's command; TAB on the screen and the serial line,
   and the pager at a row that starts with one. 10/25/26 (track A, A6): the scrollback of a
   framebuffer console: rows kept as they scroll off and by ESC[2J and a clear, Shift+PgUp, PgDn,
   Home and End, the view's tag and hidden cursor, output that does not move the view, any other key,
   a console switch, the pager's question, a graphics session, no memory, a full ring; a kernel
   thread's message whole, after another writer's line. 10/25/26 (A8): the escape sequences of
   tests/host/vt_cases.h (shared with the desktop's terminal), and the alternate screen on the
   screen and on a pseudo-terminal, the scrollback left alone, a task that ends without leaving it. 10/26/26: a writer waiting for room in a pty's
   full buffer has changed nothing on its console, so the shell's question never lands in its line.
   10/16/26 (audit C10): the console lock: a task stopped inside its line (a preemption) keeps another task's
   line out of it, an interrupt handler's line is drawn after it, and a paged task's line
   gives the lock back while it waits at the question. */
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt_cases.h"

/* tasks (tests/host/pager_bridge.c): init's children are the shell and the usb thread; the
   shell's are a command (CHILD, which has GRAND) and a background job (BG) */
enum { IDLE, INIT, SHELL, CHILD, GRAND, BG, USB, NT };
#define TTY_RAW  1
#define TTY_PAGE 3
#define TTY_CAPS 4

/* pager_bridge.c */
void sim_init(int ascii);
void sim_cur(int task);
int sim_task(void);
void sim_print(const char *s, int len);
void sim_kprintf_line(const char *s);
int sim_ttyctl(int op, unsigned arg);
void sim_input(int c);
void sim_key(int c);
int sim_getc(void);
int sim_readline(char *buf, int size);
int sim_lines(void);
int sim_cols(void);
void sim_row(int r, char *out, int *attr0);
unsigned char sim_cell(int x, int y);
void sim_cursor(int *x, int *y);
int sim_serial(char *out, int max);
void sim_page(int *rows, int *drop, int *asker, int *owner);
void sim_task_gone(int task);
void sim_set_if(int on);
void sim_set_in_irq(int n);
void sim_set_mutexes(int task, int n);
void sim_set_crit(int task, int n);
int sim_on_list(int task);
int sim_readers(void);
int sim_raw(void);
void sim_capture(int on);
const char *sim_captured(void);
void sim_term(int n);
int sim_stream(char *out, int max);
void sim_stream_size(int cols, int rows);
void sim_stream_drop(int on);
void sim_stream_full(int on);
void sim_preempt_irq(int task, int n);
void sim_stream_pos(int *x, int *wrap);
void sim_term_input(int c);
int sim_term_queues(int c);
int sim_room(void);
void sim_time_ms(int ms);
int sim_poll_wakes(void);
void sim_clear(void);
int sim_key_up(void);
int sim_key_home(void);
int sim_readline_secret(char *buf, int size);
int sim_inq_has(const char *s);
int sim_scroll_key(int which);              /* 10/25/26 (A6): 0 Shift+PgUp, 1 PgDn, 2 Home, 3 End */
int sim_attr(int x, int y);                 /* 10/25/26 (A8) */
void sim_view_live(void);
void sim_screen_row(int r, char *out, int *attr_last);
int sim_view(void);
int sim_kept(void);
void sim_fb_cursor(int *x, int *y);
void sim_graphics(int on);
void sim_heap_refuse(int on);
void sim_free_frames(unsigned long n);      /* 10/25/26 (the review) */
void sim_serial_preempt(int after);
void sim_switch(int n);
extern void (*sim_serial_hook)(int c);

static int failures, checks;

static void dump(void);

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
        if(getenv("PAGER_DEBUG")) dump();
    }
}

/**** the tasks as threads: one runs at a time ****/

enum { FREE, RUNNING, BLOCKED, DONE };
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int running = -1;                    /* the task that has the CPU; -1 the test */
static struct {
    pthread_t th;
    int state, abort;
    void (*work)(int, int);
    int a, b;
    jmp_buf top;
} task[NT];

void emu_fail(const char *why) {
    printf("FAIL: %s (task %d)\n", why, sim_task());
    exit(1);
}

static void *task_main(void *arg) {
    int id = (int)(long)arg;
    pthread_mutex_lock(&mu);
    for(;;) {
        while(running != id) pthread_cond_wait(&cv, &mu);
        task[id].state = RUNNING;
        pthread_mutex_unlock(&mu);
        sim_cur(id);
        if(!setjmp(task[id].top)) task[id].work(task[id].a, task[id].b);
        pthread_mutex_lock(&mu);
        task[id].state = DONE;
        task[id].abort = 0;
        running = -1;
        pthread_cond_broadcast(&cv);
    }
    return NULL;
}

/* yield() in pager_bridge.c: task id blocks, the test has the CPU until it resumes it */
void emu_block(int id) {
    if(id < 0 || !pthread_equal(pthread_self(), task[id].th)) emu_fail("a block outside a task's thread");
    pthread_mutex_lock(&mu);
    task[id].state = BLOCKED;
    running = -1;
    pthread_cond_broadcast(&cv);
    while(running != id) pthread_cond_wait(&cv, &mu);
    task[id].state = RUNNING;
    if(task[id].abort) {
        pthread_mutex_unlock(&mu);
        longjmp(task[id].top, 1);
    }
    pthread_mutex_unlock(&mu);
    sim_cur(id);
}

static int cpu_to(int id) {
    int st;
    pthread_mutex_lock(&mu);
    running = id;
    pthread_cond_broadcast(&cv);
    while(running != -1) pthread_cond_wait(&cv, &mu);
    st = task[id].state;
    pthread_mutex_unlock(&mu);
    return st;
}

/* task id does work(a, b); DONE, or BLOCKED where it waits */
static int run(int id, void (*work)(int, int), int a, int b) {
    if(task[id].state == BLOCKED) emu_fail("run: the task is blocked");
    task[id].work = work;
    task[id].a = a;
    task[id].b = b;
    return cpu_to(id);
}

/* a blocked task that was woken goes on */
static int resume(int id) {
    if(task[id].state != BLOCKED) emu_fail("resume: the task is not blocked");
    if(sim_on_list(id)) emu_fail("resume: the task was not woken");
    return cpu_to(id);
}

static void reset(int ascii) {
    int i;
    for(i = 0; i < NT; i++) {
        if(task[i].state == BLOCKED) {
            task[i].abort = 1;
            cpu_to(i);
        }
        task[i].state = FREE;
    }
    sim_init(ascii);
}

/**** what the tasks do ****/

static void w_page(int on, int unused) { (void)unused; sim_ttyctl(TTY_PAGE, (unsigned)on); }
static void w_raw(int on, int unused) { (void)unused; sim_ttyctl(TTY_RAW, (unsigned)on); }

static void w_lines(int from, int to) {
    char buf[32];
    int i;
    for(i = from; i <= to; i++) {
        int n = snprintf(buf, sizeof(buf), "line %03d\n", i);
        sim_print(buf, n);
    }
}

static const char *say_text;
static void w_say(int unused, int unused2) { (void)unused; (void)unused2; sim_print(say_text, (int)strlen(say_text)); }
static void say(int id, const char *s) { say_text = s; run(id, w_say, 0, 0); }

static int got_key;
static void w_getc(int unused, int unused2) { (void)unused; (void)unused2; got_key = sim_getc(); }

static const char *kp_text;
static void w_kp_text(int unused, int unused2) { (void)unused; (void)unused2; sim_kprintf_line(kp_text); }
static int kprintf_as(int id, const char *s) { kp_text = s; return run(id, w_kp_text, 0, 0); }

/* 10/16/26: at the hook_n-th byte the serial mirror gets, the task stops there (a preemption in
   the middle of its line), or with hook_irq an interrupt handler prints a line */
static int hook_n, hook_irq, hook_busy;
static void hook(int c) {
    (void)c;
    if(hook_busy || !hook_n || --hook_n) return;
    hook_busy = 1;
    if(hook_irq) {
        sim_set_in_irq(1);
        sim_kprintf_line("irq line\n");
        sim_set_in_irq(0);
    } else {
        emu_block(sim_task());
    }
    hook_busy = 0;
}

/**** looking at the screen ****/

static void row(int r, char *out) { sim_row(r, out, NULL); }

static int screen_has(const char *s) {
    char r[256];
    int i;
    for(i = 0; i < sim_lines(); i++) {
        row(i, r);
        if(strstr(r, s)) return 1;
    }
    return 0;
}

/* rows 0..23 are "line from" .. "line from+23" and the question is on row 24 in inverse video */
static int asking_after(int last) {
    char r[256], want[32];
    int i, attr;
    sim_row(24, r, &attr);
    if(strncmp(r, "--More--", 8) || attr != 0x70) return 0;
    for(i = 0; i < 24; i++) {
        row(i, r);
        snprintf(want, sizeof(want), "line %03d", last - 23 + i);
        if(strcmp(r, want)) return 0;
    }
    return 1;
}

static void page_state(int *rows, int *drop, int *asker, int *owner) { sim_page(rows, drop, asker, owner); }

/* PAGER_DEBUG=1: the screen and the pager's state with every failure */
static void dump(void) {
    char r[256];
    int i, x, y, rows, drop, a, o;
    for(i = 0; i < sim_lines(); i++) {
        row(i, r);
        printf("    %2d|%s\n", i, r);
    }
    sim_cursor(&x, &y);
    sim_page(&rows, &drop, &a, &o);
    printf("    cursor %d,%d  rows %d drop %d asker %d owner %d readers %d\n", x, y, rows, drop, a, o, sim_readers());
}

static int asker(void) { int r, d, a, o; page_state(&r, &d, &a, &o); return a; }
static int dropping(void) { int r, d, a, o; page_state(&r, &d, &a, &o); return d; }
static int owner(void) { int r, d, a, o; page_state(&r, &d, &a, &o); return o; }

/* the shell turns paging on and its command (CHILD) prints lines from..to */
static int paged_lines(int from, int to) {
    run(SHELL, w_page, 1, 0);
    return run(CHILD, w_lines, from, to);
}

/**** scenarios ****/

static void t_screenful(void) {
    char ser[4096];
    int st;
    reset(0);
    st = paged_lines(1, 24);
    check(st == DONE && asker() < 0, "24 lines at 80x25 fit: no question");
    run(SHELL, w_page, 0, 0);
    say(SHELL, "SURF > ");
    check(asker() < 0 && owner() < 0, "the prompt after TTY_PAGE 0 does not ask either");

    reset(0);
    sim_serial(ser, sizeof(ser));
    st = paged_lines(1, 60);
    check(st == BLOCKED && asker() == CHILD && asking_after(24), "60 lines: --More-- after 24, on the last row in inverse video");
    sim_serial(ser, sizeof(ser));
    check(strstr(ser, "line 024\r\n\r\033[7m--More-- (Space: next page, Enter: next line, q: skip the rest)\033[27m") != NULL,
          "the serial terminal gets the question in inverse video (ESC[7m .. ESC[27m) after a CR");
    sim_input(' ');
    st = resume(CHILD);
    sim_serial(ser, sizeof(ser));
    check(st == BLOCKED && asking_after(48), "Space: the next 24 lines, then the question again");
    check(!strncmp(ser, "\r\033[K", 4) && strstr(ser, "line 025") == ser + 4, "the terminal: CR ESC[K takes the question away");
    sim_input('\r');
    st = resume(CHILD);
    check(st == BLOCKED && asking_after(49), "Enter: one more line");
    sim_input('\n');
    st = resume(CHILD);
    check(st == BLOCKED && asking_after(50), "\\n is Enter too");
    sim_input('x');
    st = resume(CHILD);
    check(st == DONE && screen_has("line 060") && asker() < 0, "any other key: the next screenful (and the rest fits)");
}

static void t_quit(void) {
    static const int keys[] = { 'q', 'Q', 0x1B, 0x03 };
    char what[96];
    unsigned i;
    int st;
    for(i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        reset(0);
        st = paged_lines(1, 30);
        if(st == BLOCKED) {
            sim_input(keys[i]);
            st = resume(CHILD);
        }
        snprintf(what, sizeof(what), "key 0x%02x at --More--: the rest is dropped, the row is as before", keys[i]);
        check(st == DONE && dropping() && !screen_has("line 025") && !screen_has("--More--") && screen_has("line 024"), what);
    }
    run(SHELL, w_page, 0, 0);
    say(SHELL, "SURF > ");
    check(!dropping() && screen_has("SURF >"), "TTY_PAGE 0 from the owner ends the drop with paging");
}

static void t_wrap(void) {
    char l80[81], ser[4096];
    int i, st, x, y;
    reset(0);
    memset(l80, 'x', 80);
    l80[80] = 0;
    run(SHELL, w_page, 1, 0);
    for(i = 0; i < 12; i++) {
        say(CHILD, l80);
        say(CHILD, "\n");
    }
    check(asker() < 0, "12 lines of exactly 80 columns, each with the empty row after it: 24 rows, no question");
    say_text = "y";
    st = run(CHILD, w_say, 0, 0);
    check(st == BLOCKED, "the 25th row asks first");
    sim_input(' ');
    resume(CHILD);

    reset(0);
    run(SHELL, w_page, 1, 0);
    run(CHILD, w_lines, 1, 23);
    say(CHILD, l80);                        /* the 24th row is full, the wrap pending */
    sim_serial(ser, sizeof(ser));
    say_text = "z";
    st = run(CHILD, w_say, 0, 0);
    sim_serial(ser, sizeof(ser));
    check(st == BLOCKED && !strncmp(ser, "\r\n\r\033[7m--More--", 15),
          "a full row with the wrap pending: the question goes on the next row (CR LF first on the terminal)");
    sim_input(' ');
    resume(CHILD);
    sim_cursor(&x, &y);
    check(sim_cell(0, 24) == 'z' && x == 1 && y == 24, "and the character goes where the question was");
}

/* nowhere to wait: no question, the rows counted; the next row where it may asks */
static void t_no_waiting(void) {
    static const char *const why[] = { "interrupts off", "KCRIT_ENTER", "a mutex held", "raw mode", "an interrupt handler" };
    char what[128];
    int k, st;
    for(k = 0; k < 5; k++) {
        reset(0);
        run(SHELL, w_page, 1, 0);
        if(k == 3) run(CHILD, w_raw, 1, 0);
        if(k == 0) sim_set_if(0);
        if(k == 1) sim_set_crit(CHILD, 1);
        if(k == 2) sim_set_mutexes(CHILD, 1);
        if(k == 4) sim_set_in_irq(1);
        st = run(CHILD, w_lines, 1, 30);
        snprintf(what, sizeof(what), "%s: no question, the output goes on", why[k]);
        check(st == DONE && asker() < 0 && screen_has("line 030"), what);
        sim_set_if(1);
        sim_set_crit(CHILD, 0);
        sim_set_mutexes(CHILD, 0);
        sim_set_in_irq(0);
        if(k == 3) run(CHILD, w_raw, 0, 0);
        if(k == 3) continue;                /* raw mode off starts a new screenful */
        st = run(CHILD, w_lines, 31, 31);
        snprintf(what, sizeof(what), "%s, then not: the next row asks at once", why[k]);
        check(st == BLOCKED, what);
    }
}

static void t_reader_first(void) {
    int st;
    reset(0);
    run(SHELL, w_page, 1, 0);
    st = run(BG, w_getc, 0, 0);             /* someone waits for a key on the terminal */
    check(st == BLOCKED && sim_readers() == 1, "a task waits to read the terminal");
    st = run(CHILD, w_lines, 1, 40);
    check(st == DONE && asker() < 0, "while another task waits to read, nobody asks: the output goes on");
    sim_input('k');
    resume(BG);
    check(got_key == 'k', "and the reader gets its key");
}

static void t_capture(void) {
    int st;
    reset(0);
    run(SHELL, w_page, 1, 0);
    sim_capture(1);                         /* (the shell's output kept for a file) */
    st = run(SHELL, w_lines, 1, 40);
    sim_capture(0);
    check(st == DONE && asker() < 0 && strstr(sim_captured(), "line 040") && !screen_has("line 001"),
          "captured output is never paged, and never shown");
}

static void w_irq_line(int unused, int unused2) {
    (void)unused;
    (void)unused2;
    sim_set_in_irq(1);
    sim_kprintf_line("an interrupt handler says hi\n");
    sim_set_in_irq(0);
}

static void t_interrupt_rows(void) {
    int rows, drop, a, o;
    reset(0);
    paged_lines(1, 20);
    run(USB, w_irq_line, 0, 0);             /* 10/26/26: come in on a task the pager does not cover */
    page_state(&rows, &drop, &a, &o);
    check(rows == 21 && a < 0,
          "an interrupt handler's line takes a row of the screenful and never waits, whichever task it came in on");
}

/* 10/26/26 (the telnet stalled-client check's "never filled"): on the screen too, the rows that a job in the
   background or a kernel thread starts count toward no screenful; the command asks after 24 rows of its own */
static void t_background_rows(void) {
    char r[256];
    int rows, drop, a, o, st;
    reset(0);
    paged_lines(1, 20);
    run(BG, w_lines, 101, 110);
    run(USB, w_lines, 201, 210);
    page_state(&rows, &drop, &a, &o);
    check(rows == 20 && a < 0, "a job in the background and a kernel thread print 20 rows: none of them counts");
    st = run(CHILD, w_lines, 21, 30);
    sim_row(24, r, NULL);
    check(st == BLOCKED && asker() == CHILD && !strncmp(r, "--More--", 8),
          "the command asks after 4 more rows of its own");
    row(23, r);
    check(!strcmp(r, "line 024"), "its 24th row just above the question");
    sim_input('q');
    resume(CHILD);
}

static void t_owner(void) {
    int st;
    reset(0);
    run(SHELL, w_page, 1, 0);
    check(owner() == SHELL, "TTY_PAGE 1: the caller owns the pager");
    run(USB, w_page, 1, 0);
    check(owner() == SHELL, "a task that is not the owner's descendant cannot take it (EBUSY)");
    sim_task_gone(SHELL);
    check(owner() < 0, "the owner's end ends paging");
    reset(0);
    run(SHELL, w_page, 1, 0);
    st = run(CHILD, w_lines, 1, 30);
    check(st == BLOCKED && asker() == CHILD, "a descendant of the owner is paged");
}

static void t_utf8_writers(void) {
    char r[256];
    reset(0);
    say(SHELL, "\xE2\x96");                 /* the first two bytes of ▸ */
    say(USB, "ab");                         /* another task in between */
    say(SHELL, "\xB8");
    sim_set_in_irq(1);                      /* and an interrupt handler with its own half character */
    sim_print("\xC3", 1);
    sim_set_in_irq(0);
    say(SHELL, "!");
    sim_set_in_irq(1);
    sim_print("\xA9", 1);
    sim_set_in_irq(0);
    row(0, r);
    check(!strcmp(r, "ab\x10!\x82"), "UTF-8 per writer: ▸ split by another task and é split by an interrupt both come out whole");
}

/* the other order: a task that starts to wait for a key calls the question off */
static void t_question_then_reader(void) {
    int st;
    reset(0);
    st = paged_lines(1, 30);
    check(st == BLOCKED && asker() == CHILD, "a command waits at --More--");
    st = run(BG, w_getc, 0, 0);             /* then the shell, say, waits for a key at its prompt */
    check(st == BLOCKED && sim_readers() == 1 && !sim_on_list(CHILD), "a reader comes: the question is called off");
    st = resume(CHILD);
    check(st == DONE && asker() < 0 && !screen_has("--More--") && screen_has("line 030"),
          "the asker goes on as after Space, and with the reader there it does not ask again");
    sim_input('k');
    resume(BG);
    check(got_key == 'k', "the next key is the reader's");
}

/* paging ends, or its owner: a descendant's question is called off */
static void t_question_owner_gone(void) {
    int st;
    reset(0);
    paged_lines(1, 30);
    run(SHELL, w_page, 0, 0);               /* the shell's command ended: TTY_PAGE 0 */
    check(!sim_on_list(CHILD), "TTY_PAGE 0 from the owner calls a descendant's question off");
    say(SHELL, "SURF > ");
    check(!screen_has("--More--") && screen_has("SURF >"), "the prompt after it is not glued to the question");
    st = resume(CHILD);
    check(st == DONE && screen_has("line 030") && !screen_has("--More--"), "and the background task goes on, unpaged");

    reset(0);
    paged_lines(1, 30);
    sim_task_gone(SHELL);
    check(!sim_on_list(CHILD) && owner() < 0, "the owner's end calls the question off");
    st = resume(CHILD);
    check(st == DONE && screen_has("line 030"), "and the asker goes on");
}

/* while one task is at the question, another paged one waits for the answer */
static void t_waiting_for_the_answer(void) {
    int st;
    reset(0);
    paged_lines(1, 30);
    st = run(GRAND, w_lines, 101, 130);
    check(st == BLOCKED && asking_after(24), "a paged grandchild waits while its parent is at --More--; the question stays");
    sim_input(' ');
    st = resume(CHILD);
    check(st == DONE && !sim_on_list(GRAND), "after Space the asker goes on, and the waiting task is woken");
    st = resume(GRAND);
    check(st == BLOCKED && asker() == GRAND && screen_has("line 118") && !screen_has("line 119"),
          "then it prints, paged with the rest: it asks in its turn");
    sim_input(' ');
    st = resume(GRAND);
    check(st == DONE && screen_has("line 130"), "and finishes");
}

/* anyone else who prints takes the question away; it comes back after their newline */
static void t_question_gives_way(void) {
    char r[256], ser[4096];
    int st;
    reset(0);
    paged_lines(1, 30);
    sim_serial(ser, sizeof(ser));
    say(USB, "+USB: port 3: unplugged\n");
    sim_serial(ser, sizeof(ser));
    row(23, r);
    check(!strcmp(r, "+USB: port 3: unplugged") && !strncmp(ser, "\r\033[K+USB: port 3: unplugged\r\n\r\033[7m--More--", 41),
          "another task's line: the question gives way (CR ESC[K), then comes back on the next row");
    sim_row(24, r, &st);
    check(!strncmp(r, "--More--", 8) && st == 0x70, "the question is on the last row again");
    sim_input(' ');
    st = resume(CHILD);
    check(st == DONE && !screen_has("--More--") && screen_has("line 030"), "Space: the asker goes on");

    reset(0);
    paged_lines(1, 30);
    sim_set_in_irq(1);
    sim_kprintf_line("an interrupt handler's line\n");
    sim_set_in_irq(0);
    row(23, r);
    check(!strcmp(r, "an interrupt handler's line") && asker() == CHILD && sim_cell(0, 24) == '-',
          "an interrupt handler's line: the same");

    reset(0);
    paged_lines(1, 30);
    st = run(GRAND, w_lines, 101, 102);     /* waiting for the answer */
    sim_task_gone(CHILD);                   /* the asker is killed */
    check(asker() < 0 && !screen_has("--More--") && !sim_on_list(GRAND), "an asker killed at the question takes it away, and wakes who waited");
    run(SHELL, w_page, 0, 0);
    say(SHELL, "[pid 3 killed]\n");
    row(23, r);
    check(!strcmp(r, "[pid 3 killed]"), "the next line goes where the question was");
    st = resume(GRAND);
    check(st == DONE && screen_has("line 102"), "and the waiting task prints");
}

/**** a full input queue (10/31/26) ****/

/* a paste longer than the queue: the bytes past it are dropped and wake nobody, the reader still gets what is queued,
   and what is typed once it has read goes in as ever */
static void t_full_queue(void) {
    int i, wakes, st;
    reset(0);
    st = run(CHILD, w_getc, 0, 0);
    check(st == BLOCKED && sim_on_list(CHILD), "a reader waits for a key");
    sim_input('a');
    check(!sim_on_list(CHILD), "the first byte of a paste wakes it");
    for(i = 0; i < 254; i++) sim_input('b');         /* the queue holds 255 */
    wakes = sim_poll_wakes();
    for(i = 0; i < 1000; i++) sim_input('c');
    check(sim_poll_wakes() == wakes, "the bytes a full queue drops wake nobody");
    st = resume(CHILD);
    check(st == DONE && got_key == 'a', "the reader gets the queue's first byte");
    for(i = 0; i < 254; i++) {
        st = run(CHILD, w_getc, 0, 0);
        if(st != DONE || got_key != 'b') break;
    }
    check(i == 254, "and the rest of what fitted, none of what was dropped");
    st = run(CHILD, w_getc, 0, 0);
    check(st == BLOCKED, "then it waits again");
    sim_input('d');
    st = resume(CHILD);
    check(st == DONE && got_key == 'd', "and a key typed after the paste reaches it");
}

/**** the console lock (10/16/26, audit C10) ****/

/* a task preempted inside its line holds the lock: another task's line waits, and both come out
   whole, in order, on the screen and on the serial line */
static void t_lock_preempted(void) {
    char r0[256], r1[256], ser[4096];
    int a, b, i;
    reset(0);
    sim_serial(ser, sizeof(ser));
    sim_serial_hook = hook;
    hook_irq = 0;
    hook_n = 6;                                 /* CHILD stops after "first" */
    a = kprintf_as(CHILD, "first line, from the command\n");
    b = kprintf_as(USB, "second line, from the usb thread\n");
    for(i = 0; i < 210 && b == BLOCKED && !sim_on_list(USB); i++) b = resume(USB);   /* (10/25/26: a kernel thread's message
                                                   waits 100 ms for the other's line to end and 1 s for it to hold
                                                   the console, then for the lock) */
    check(a == BLOCKED && b == BLOCKED && sim_on_list(USB), "a task stopped inside its line keeps the other task's line waiting");
    a = resume(CHILD);
    check(a == DONE && !sim_on_list(USB), "its line ends, and the waiting task is woken");
    b = resume(USB);
    sim_serial_hook = NULL;
    row(0, r0);
    row(1, r1);
    sim_serial(ser, sizeof(ser));
    check(b == DONE && !strcmp(r0, "first line, from the command") && !strcmp(r1, "second line, from the usb thread"),
          "both lines whole on the screen, in that order");
    check(strstr(ser, "first line, from the command\r\nsecond line, from the usb thread\r\n") != NULL,
          "and on the serial line");
}

/* an interrupt handler's line while a task draws: on the serial line at once, on the screen when
   the task lets go (after its line, not inside it) */
static void t_lock_interrupt(void) {
    char r0[256], r1[256], ser[4096];
    int a;
    reset(0);
    sim_serial(ser, sizeof(ser));
    sim_serial_hook = hook;
    hook_irq = 1;
    hook_n = 6;
    a = kprintf_as(CHILD, "first line, from the command\n");
    sim_serial_hook = NULL;
    hook_irq = 0;
    row(0, r0);
    row(1, r1);
    sim_serial(ser, sizeof(ser));
    check(a == DONE && !strcmp(r0, "first line, from the command") && !strcmp(r1, "irq line"),
          "an interrupt handler's line while a task draws its own: on the screen after it, not inside it");
    check(strstr(ser, "firstirq line\r\n line, from the command\r\n") != NULL, "on the serial line at once");
}

/* a paged task's kprintf() line that meets the question gives the lock back while it waits:
   another task prints meanwhile, and a paged task that waits for the answer does not hold it either */
static void t_lock_question(void) {
    int st, u, g, i;
    reset(0);
    run(SHELL, w_page, 1, 0);
    run(CHILD, w_lines, 1, 24);
    st = kprintf_as(CHILD, "line 025 in a kprintf call\n");
    check(st == BLOCKED && asker() == CHILD, "a paged kprintf() line meets the question");
    u = kprintf_as(USB, "+USB: a line meanwhile\n");
    for(i = 0; i < 10 && u == BLOCKED; i++) u = resume(USB);   /* (10/25/26: 100 ms for the question's row, as for a prompt) */
    check(u == DONE && screen_has("+USB: a line meanwhile"), "the lock was given back: another task prints while it waits");
    g = kprintf_as(GRAND, "line 101 from the grandchild\n");
    check(g == BLOCKED && asker() == CHILD, "a paged grandchild's kprintf() line waits for the answer");
    sim_input(' ');
    st = resume(CHILD);
    check(st == DONE && screen_has("line 025 in a kprintf call") && !sim_on_list(GRAND),
          "Space: the line goes on with the lock taken back, and the grandchild is woken");
    g = resume(GRAND);
    check(g == DONE && screen_has("line 101 from the grandchild"), "then the grandchild prints: nobody waited holding the lock");
}

/* after q: what a program prints before it reads, its question, still shows; so does its
   crash report */
static void t_drop_question(void) {
    char ser[4096];
    int st;
    reset(0);
    paged_lines(1, 30);
    sim_input('q');
    resume(CHILD);
    say(CHILD, "line 031\n    Type rd1 to write it: ");
    check(dropping() && !screen_has("line 031") && !screen_has("Type rd1"), "after q the command's next lines are dropped");
    sim_serial(ser, sizeof(ser));
    st = run(CHILD, w_getc, 0, 0);
    sim_serial(ser, sizeof(ser));
    check(st == BLOCKED && screen_has("    Type rd1 to write it:") && !screen_has("line 031") && !dropping() &&
          !strcmp(ser, "    Type rd1 to write it: "),
          "when it reads, the drop ends and its question shows: the last line q dropped (on the terminal too)");
    sim_input('y');
    resume(CHILD);
    say(CHILD, "written\n");
    check(got_key == 'y' && screen_has("written"), "and what it prints after the answer shows");
}

static void w_crash(int unused, int unused2) {
    (void)unused;
    (void)unused2;
    sim_set_if(0);                          /* trap_fatal(): an interrupt gate, */
    sim_task_gone(sim_task());              /* tty_task_gone() first, */
    sim_print("Process ('crash':3) killed by \"Page Fault\"\n", 44);   /* then the report */
    sim_set_if(1);
}

static void t_drop_crash(void) {
    reset(0);
    paged_lines(1, 30);
    sim_input('q');
    resume(CHILD);
    run(CHILD, w_crash, 0, 0);
    check(screen_has("killed by \"Page Fault\"") && !dropping(), "after q, a paged program's crash report still shows");
}

/* TTY_PAGE from a task that is paged already (a nested shell): a new screenful */
static void t_nested(void) {
    int st;
    reset(0);
    paged_lines(1, 24);                     /* the nested shell's command filled the screen exactly */
    run(CHILD, w_page, 0, 0);               /* the nested shell: its command ended */
    say_text = "SURF > ";
    st = run(CHILD, w_say, 0, 0);
    check(st == DONE && asker() < 0 && screen_has("SURF >") && owner() == SHELL,
          "a nested TTY_PAGE 0 starts a new screenful: its prompt after an exact fit does not ask");
    reset(0);
    paged_lines(1, 30);
    sim_input('q');
    resume(CHILD);
    run(GRAND, w_page, 1, 0);               /* a program the command starts turns paging on */
    say(GRAND, "grand\n");
    check(owner() == SHELL && screen_has("grand"), "and ends a drop, while the pager stays its owner's");
}

/* 10/25/26: a job in the background (no TF_FOREGROUND) is not paged with the shell's command */
static void t_background_unpaged(void) {
    int st;
    reset(0);
    run(SHELL, w_page, 1, 0);
    st = run(BG, w_lines, 1, 30);
    check(st == DONE && asker() < 0 && !screen_has("--More--") && screen_has("line 030"),
          "a job in the background prints on, unpaged, while the shell pages its command");
    st = run(CHILD, w_lines, 31, 70);
    check(st == BLOCKED && asker() == CHILD, "the command itself is paged all the same");
    sim_input(' ');
    resume(CHILD);
}

/* a shell whose background job turned paging on takes the pager for its next command */
static void t_takeover(void) {
    int st;
    reset(0);
    run(BG, w_page, 1, 0);                  /* the job, while the shell waits at its prompt */
    check(owner() == BG, "a background job takes the free pager");
    st = run(SHELL, w_page, 1, 0);
    check(owner() == SHELL, "the shell, its parent, takes it over for its command (no EBUSY)");
    st = run(CHILD, w_lines, 1, 30);
    check(st == BLOCKED && asker() == CHILD, "and the command is paged");
    sim_input(' ');
    resume(CHILD);
    run(USB, w_page, 1, 0);
    check(owner() == SHELL, "a task that is no ancestor still cannot take it");
}

/* an empty screen, or the end of raw mode: a new screenful */
static void t_new_screenful(void) {
    int st;
    reset(0);
    paged_lines(1, 20);
    say(CHILD, "\033[2J\033[H");
    st = run(CHILD, w_lines, 101, 124);
    check(st == DONE && asker() < 0, "ESC[2J ESC[H: 24 more lines fit the empty screen");
    st = run(CHILD, w_lines, 125, 125);
    check(st == BLOCKED, "the 25th asks");
    reset(0);
    paged_lines(1, 20);
    say(CHILD, "\033[H\033[J");
    st = run(CHILD, w_lines, 101, 124);
    check(st == DONE && asker() < 0, "ESC[H ESC[J: the same");
    reset(0);
    paged_lines(1, 20);
    run(CHILD, w_raw, 1, 0);
    run(CHILD, w_raw, 0, 0);
    st = run(CHILD, w_lines, 101, 124);
    check(st == DONE && asker() < 0, "after raw mode: a new screenful");
}

/* the echo of what is typed goes to the screen even while the output is kept for a file */
static char line_got[64];
static void w_readline_kept(int unused, int unused2) {
    (void)unused;
    (void)unused2;
    sim_capture(1);
    sim_print("Enter a string: ", 16);
    sim_readline(line_got, sizeof(line_got));
    sim_print("thanks\n", 7);
    sim_capture(0);
}

static void t_echo_not_kept(void) {
    int st;
    reset(0);
    sim_input('h');
    sim_input('i');
    sim_input('\n');
    st = run(CHILD, w_readline_kept, 0, 0);
    check(st == DONE && !strcmp(line_got, "hi") && !strcmp(sim_captured(), "Enter a string: thanks\n") && screen_has("hi") &&
          !screen_has("Enter"), "a kept output gets the command's output; the typed line's echo is shown, not kept");
}

static void t_caps(void) {
    reset(0);
    check(sim_ttyctl(TTY_CAPS, 0) == 7, "TTY_CAPS: UTF-8, colour (10/25/26: and the alternate screen)");
    reset(1);
    check(sim_ttyctl(TTY_CAPS, 0) == 6, "TTY_CAPS with `ascii` on the command line: colour (and the alternate screen)");
}

/**** 10/6/26: a pseudo-terminal: the stream console ****/

#define QUESTION "\033[7m--More-- (Space: next page, Enter: next line, q: skip the rest)\033[27m"
#define UNASKED  "\r\033[K"

static char strm[65536], want[65536];
static int strm_len;

/* what the master got since the last look */
static const char *stream_now(void) {
    strm_len = sim_stream(strm, sizeof(strm));
    return strm;
}

/* it was exactly s, by its length too: a NUL in the stream shows */
static int stream_is(const char *s) {
    stream_now();
    return strm_len == (int)strlen(s) && !memcmp(strm, s, (size_t)strm_len);
}

/* "line from\n" .. "line to\n" after prefix, then suffix */
static const char *lines_text(const char *prefix, int from, int to, const char *suffix) {
    int i, n = snprintf(want, sizeof(want), "%s", prefix);
    for(i = from; i <= to; i++) n += snprintf(want + n, sizeof(want) - n, "line %03d\n", i);
    snprintf(want + n, sizeof(want) - n, "%s", suffix);
    return want;
}

static void w_clear(int unused, int unused2) { (void)unused; (void)unused2; sim_clear(); }

/* the tasks on pty 0, cols x rows, the shell paging */
static void on_pty(int cols, int rows, int paging) {
    reset(0);
    sim_term(0);
    sim_stream_size(cols, rows);
    if(paging) run(SHELL, w_page, 1, 0);
    stream_now();
}

static void t_stream_screenful(void) {
    int st;
    on_pty(80, 10, 1);
    st = run(CHILD, w_lines, 1, 30);
    check(st == BLOCKED && asker() == CHILD, "a pty at 80x10: the 10th line asks first");
    check(stream_is(lines_text("", 1, 9, QUESTION)),
          "the stream: lines 1-9 as printed (LF alone), then the question in inverse video");
    sim_input(' ');
    st = resume(CHILD);
    check(st == BLOCKED && stream_is(lines_text(UNASKED, 10, 18, QUESTION)),
          "Space: CR ESC[K takes the question away, 9 more lines, the question again");
    sim_input('\r');
    st = resume(CHILD);
    check(st == BLOCKED && stream_is(lines_text(UNASKED, 19, 19, QUESTION)), "Enter: one more line");
    sim_input('q');
    st = resume(CHILD);
    check(st == DONE && dropping() && stream_is(UNASKED), "q: the rest is dropped");
    run(SHELL, w_page, 0, 0);
    say(SHELL, "prompt> ");
    check(stream_is("prompt> "), "and paging ends with TTY_PAGE 0");
}

static void t_stream_columns(void) {
    char l80[81], u80[161];
    int i, st, x, wrap, rows, drop, a, o;
    memset(l80, 'x', 80);
    l80[80] = 0;
    for(i = 0; i < 80; i++) memcpy(u80 + 2 * i, "\xc3\xa9", 2);    /* é */
    u80[160] = 0;

    on_pty(80, 10, 1);
    for(i = 0; i < 9; i++) {
        say(CHILD, l80);
        say(CHILD, "\n");
    }
    check(asker() < 0, "9 lines of exactly 80 columns: 9 rows on a terminal (no empty row after each, as on the screen)");
    say_text = "y";
    st = run(CHILD, w_say, 0, 0);
    check(st == BLOCKED, "the 10th row asks first");
    sim_input(' ');
    resume(CHILD);

    on_pty(80, 10, 1);
    say(CHILD, "\033[1;33m");
    say(CHILD, u80);
    say(CHILD, "\033[0m");
    sim_stream_pos(&x, &wrap);
    check(x == 79 && wrap, "80 two-byte UTF-8 characters, in an SGR sequence, fill one row of 80 columns: the wrap pending");
    say(CHILD, "z");
    sim_stream_pos(&x, &wrap);
    page_state(&rows, &drop, &a, &o);
    check(x == 1 && !wrap && rows == 1, "the next character starts the next row");
    say(CHILD, "\tab\t");
    sim_stream_pos(&x, &wrap);
    check(x == 16, "TAB goes to the next multiple of 8");
    say(CHILD, "\033[5D");
    sim_stream_pos(&x, &wrap);
    check(x == 11, "ESC[5D: 5 columns back");
    snprintf(want, sizeof(want), "\033[1;33m%s\033[0mz\tab\t\033[5D", u80);
    check(stream_is(want), "the bytes go as they were printed: no UTF-8 decoding, no cells");
}

/* 10/25/26: TAB on the screen (F1) and its serial mirror, by stream_put()'s rules: to the next multiple
   of 8, never past the last column, nothing at a pending wrap, the cells it passes left as they are.
   For the pager a TAB starts a row as a character does, on the screen and on a pty. */
static void t_tab(void) {
    char r[256], ser[4096], l77[78];
    int x, y, st;
    memset(l77, 'x', 77);
    l77[77] = 0;
    reset(0);
    sim_serial(ser, sizeof(ser));
    say(SHELL, "a\tb|");
    sim_cursor(&x, &y);
    row(0, r);
    check(!strcmp(r, "a       b|") && sim_cell(1, 0) == 0 && sim_cell(7, 0) == 0 && x == 10 && y == 0,
          "a TAB b: b in column 8, the cells passed left empty");
    sim_serial(ser, sizeof(ser));
    check(!strcmp(ser, "a\tb|"), "the serial terminal gets the TAB as it is");
    say(SHELL, "\rabcdefghij\r\tX");
    row(0, r);
    check(!strcmp(r, "abcdefghXj"), "a TAB moves over cells without erasing them");
    say(SHELL, "\n");
    say(SHELL, l77);
    say(SHELL, "\t");
    sim_cursor(&x, &y);
    check(x == 79 && y == 1, "a TAB in column 77 stops at the last column");
    say(SHELL, "y\t");
    sim_cursor(&x, &y);
    check(x == 79 && y == 1 && sim_cell(79, 1) == 'y', "with the last column written (the wrap pending) a TAB does nothing");
    say(SHELL, "z");
    sim_cursor(&x, &y);
    check(sim_cell(0, 2) == 'z' && x == 1 && y == 2, "and the next character starts the next row");

    reset(0);
    run(SHELL, w_page, 1, 0);
    run(CHILD, w_lines, 1, 24);
    say_text = "\tindented\n";
    st = run(CHILD, w_say, 0, 0);
    check(st == BLOCKED && asking_after(24), "after a screenful, a row that starts with a TAB asks first");
    if(st == BLOCKED) {
        sim_input(' ');
        st = resume(CHILD);
    }
    row(23, r);
    check(st == DONE && !strcmp(r, "        indented"), "then the TAB moves along the row where the question was");
    run(SHELL, w_page, 0, 0);

    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 9);
    stream_now();
    say_text = "\tindented\n";
    st = run(CHILD, w_say, 0, 0);
    check(st == BLOCKED && stream_is(QUESTION), "a pty: after a screenful, a row that starts with a TAB asks first");
    if(st == BLOCKED) {
        sim_input(' ');
        st = resume(CHILD);
    }
    check(st == DONE && stream_is(UNASKED "\tindented\n"), "and the TAB goes out after the question is taken away");
    run(SHELL, w_page, 0, 0);
}

/* 10/25/26 (track A, A6): the screen's row r starts with "line n" */
static int screen_line(int r, int n) {
    char s[256], want[32];
    sim_screen_row(r, s, NULL);
    snprintf(want, sizeof(want), "line %03d", n);
    return !strncmp(s, want, 8);
}

/* the screen shows the console as it is (no view back): every row the same */
static int screen_live(void) {
    char a[256], b[256];
    int i;
    for(i = 0; i < sim_lines(); i++) {
        row(i, a);
        sim_screen_row(i, b, NULL);
        if(strcmp(a, b)) return 0;
    }
    return 1;
}

enum { S_PGUP, S_PGDN, S_HOME, S_END };

static void t_scrollback(void) {
    char r[256];
    int x, y, attr, st, ok, i;
    reset(0);
    run(SHELL, w_lines, 1, 40);
    check(sim_kept() == 16 && screen_live(), "40 lines on 25 rows: the 16 that scrolled off are kept, the screen as ever");
    check(sim_scroll_key(S_PGUP) == 1 && sim_view() == 16, "Shift+PgUp: back a screenful, or as far as there are rows");
    sim_screen_row(0, r, &attr);
    check(screen_line(0, 1) && screen_line(15, 16) && screen_line(16, 17) && screen_line(23, 24),
          "the view: the kept rows, oldest first, then the console's own");
    check(strlen(r) == 79 && !strcmp(r + 75, " -16") && attr == 0x70, "the view's tag in the top right corner, inverse");
    sim_fb_cursor(&x, &y);
    row(0, r);
    check(y == 25 && !strcmp(r, "line 017"), "no cursor in the view; the console's own rows are as they were");
    run(SHELL, w_lines, 41, 42);
    check(screen_line(0, 1) && screen_line(16, 17) && sim_view() == 18 && sim_kept() == 18,
          "output meanwhile: the console scrolls, the screen keeps what it shows, the view moves along");
    check(sim_scroll_key(S_PGDN) == 1 && sim_view() == 0 && screen_live() && screen_line(0, 19),
          "Shift+PgDn back past the live screen: the live screen");
    sim_fb_cursor(&x, &y);
    check(x == 0 && y == 24, "and the cursor where the console has it");
    check(sim_scroll_key(S_HOME) == 1 && sim_view() == 18 && screen_line(0, 1), "Shift+Home: the oldest row at the top");
    check(sim_scroll_key(S_END) == 1 && sim_view() == 0 && screen_live(), "Shift+End: the live screen");
    sim_scroll_key(S_PGUP);
    sim_view_live();
    check(sim_view() == 0 && screen_live(), "any other key: the live screen");
    sim_scroll_key(S_PGUP);
    sim_switch(1);
    sim_switch(0);
    check(sim_view() == 0 && screen_live(), "a console switch: back live");

    reset(0);
    say(SHELL, "one\ntwo\nthree\n\033[2J\033[H");
    check(sim_kept() == 3, "ESC[2J keeps the screen's rows, up to the last that shows anything");
    sim_scroll_key(S_PGUP);
    sim_screen_row(2, r, NULL);
    check(sim_view() == 3 && !strcmp(r, "three"), "and Shift+PgUp shows them");
    sim_scroll_key(S_END);
    say(SHELL, "four\n");
    sim_clear();
    check(sim_kept() == 4, "a clear (clearConsole) keeps them too");

    reset(0);
    run(SHELL, w_lines, 1, 30);
    st = paged_lines(31, 60);
    check(st == BLOCKED && asking_after(54), "a paged command at the question");
    ok = sim_scroll_key(S_PGUP) == 1 && sim_view() == 24 && screen_line(0, 7);
    check(ok && asker() == CHILD && sim_on_list(CHILD), "Shift+PgUp at the question: the view moves, the question waits");
    check(sim_scroll_key(S_END) == 1 && screen_live() && asking_after(54), "Shift+End: the question again");
    if(st == BLOCKED) {
        sim_input('q');
        st = resume(CHILD);
    }
    check(st == DONE, "and a key answers it");
    run(SHELL, w_page, 0, 0);

    sim_graphics(1);
    check(sim_scroll_key(S_PGUP) == 0 && sim_view() == 0, "under a graphics session the keys are the session's");
    sim_graphics(0);

    reset(0);
    sim_heap_refuse(1);
    run(SHELL, w_lines, 1, 40);
    check(sim_kept() == 0 && sim_scroll_key(S_PGUP) == 1 && sim_view() == 0 && screen_live(),
          "no memory for a ring: nothing kept, the keys do nothing");
    reset(0);                                   /* 10/25/26 (the review): a ring only with its frames and 512 to spare */
    sim_free_frames(127 + 512);                 /* the ring of 80 columns: 524,160 bytes, 128 pages */
    run(SHELL, w_lines, 1, 40);
    check(sim_kept() == 0 && screen_live(), "too few free frames for the whole ring and 512 to spare: no ring");
    reset(0);
    sim_free_frames(128 + 512);
    run(SHELL, w_lines, 1, 40);
    check(sim_kept() == 16, "with just enough: the ring");

    reset(0);
    run(SHELL, w_lines, 1, 3400);
    sim_scroll_key(S_HOME);
    sim_screen_row(0, r, NULL);
    check(sim_kept() == 3276 && sim_view() == 3276 && screen_line(0, 101) && !strcmp(r + 73, " -3276"),
          "a full ring (512 KB: 3,276 rows of 80 columns) keeps the newest rows");
    for(i = 0; i < 200 && sim_view(); i++) sim_scroll_key(S_PGDN);
    check(sim_view() == 0 && screen_live(), "and Shift+PgDn comes back down to the live screen");
}

/* 10/25/26 (track A, A8): the cases of vt_cases.h on the console */
static void t_vt_cases(void) {
    char r[256], what[160];
    int i, k, x, y, ok;
    for(i = 0; i < VT_CASES; i++) {
        const struct vt_case *c = &vt_cases[i];
        reset(0);
        say(SHELL, c->input);
        for(ok = 1, k = 0; k < VT_ROWS_MAX && c->rows[k].text; k++) {
            row(c->rows[k].row, r);
            if(strcmp(r, c->rows[k].text)) ok = 0;
        }
        for(k = 0; k < VT_ATTRS_MAX && c->attrs[k].attr >= 0; k++)
            if(sim_attr(c->attrs[k].col, c->attrs[k].row) != c->attrs[k].attr) ok = 0;
        sim_cursor(&x, &y);
        if(c->col >= 0 && (x != c->col || y != c->row)) ok = 0;
        snprintf(what, sizeof(what), "escape sequences: %s", c->name);
        check(ok, what);
    }
}

static void t_alt_screen(void) {
    char r[256], ser[4096], out[512];
    int x, y, kept, i;
    reset(0);
    check(sim_ttyctl(TTY_CAPS, 0) & 4, "TTY_CAPS says the console has the alternate screen (TTY_CAP_ALTSCREEN)");
    run(SHELL, w_lines, 1, 30);
    say(SHELL, "prompt> ");
    kept = sim_kept();
    say(SHELL, "\033[?1049h");
    sim_cursor(&x, &y);
    row(0, r);
    check(!strcmp(r, "") && x == 8 && y == 24, "ESC[?1049h: a blank screen, the cursor where it was");
    say(SHELL, "\033[HALT");
    for(i = 0; i < 30; i++) say(SHELL, "\n");
    say(SHELL, "\033[2J");
    check(sim_kept() == kept, "the alternate screen's rows never go into the scrollback, ESC[2J's neither");
    say(SHELL, "\033[?1049l");
    sim_cursor(&x, &y);
    row(23, r);
    check(!strcmp(r, "line 030") && x == 8 && y == 24 && sim_cell(0, 24) == 'p' && !screen_has("ALT"),
          "ESC[?1049l: the main screen as it was, and its cursor");
    say(SHELL, "\033[?1049l");
    check(sim_cell(0, 24) == 'p', "ESC[?1049l on the main screen does nothing");

    reset(0);                                   /* 10/25/26 (the kernel review, 2): the copy's frames */
    say(SHELL, "main");
    sim_free_frames(512);
    say(SHELL, "\033[?1049hfull");
    row(0, r);
    check(!strcmp(r, "mainfull"), "too few free frames for the main screen's copy and 512 more: ESC[?1049h does nothing");
    sim_free_frames(513);
    say(SHELL, "\033[?1049h");
    row(0, r);
    check(!strcmp(r, ""), "with just enough: the alternate screen");
    say(SHELL, "\033[?1049l");

    reset(0);
    say(SHELL, "main");
    say_text = "\033[?1049hfull screen";
    run(CHILD, w_say, 0, 0);
    sim_serial(ser, sizeof(ser));
    sim_task_gone(CHILD);
    sim_serial(ser, sizeof(ser));
    row(0, r);
    check(!strcmp(r, "main") && strstr(ser, "\033[?1049l"), "a task that ends on the alternate screen: the main one back, "
          "on the serial terminal too");

    on_pty(80, 24, 0);
    say(SHELL, "abc");
    say(SHELL, "\033[?1049h\033[Hxyz");
    say(SHELL, "\033[?1049l");
    sim_stream_pos(&x, &y);
    sim_stream(out, sizeof(out));
    check(x == 3 && strstr(out, "\033[?1049h") && strstr(out, "\033[?1049l"),
          "a pseudo-terminal: the sequences go to the far terminal, the column comes back with the main screen");
    say_text = "\033[?1049h";
    run(CHILD, w_say, 0, 0);
    sim_stream(out, sizeof(out));
    sim_task_gone(CHILD);
    sim_stream(out, sizeof(out));
    check(strstr(out, "\033[?1049l") != NULL, "and its far terminal leaves the alternate screen when the task that asked ends");
}

/* 10/25/26: a kernel thread's message comes out whole (kprintf): it waits for another writer's line to end */
static void w_kprintf(int unused, int unused2) { (void)unused; (void)unused2; sim_kprintf_line(say_text); }

static void t_whole_kernel_lines(void) {
    char ser[4096], r[256];
    int st, i;
    reset(0);
    sim_serial(ser, sizeof(ser));
    say(SHELL, "Brandon Burr");
    say_text = "+NET: eth0 up\n";
    st = run(USB, w_kprintf, 0, 0);
    check(st == BLOCKED, "a kernel thread's message waits while a program's line is half written");
    say(SHELL, "'s SurfOS\n");
    st = resume(USB);
    sim_serial(ser, sizeof(ser));
    row(0, r);
    check(st == DONE && !strcmp(ser, "Brandon Burr's SurfOS\r\n+NET: eth0 up\r\n") && !strcmp(r, "Brandon Burr's SurfOS"),
          "then comes out whole after the line, on the screen and the serial line");
    say_text = "+NET: lease ";
    run(USB, w_kprintf, 0, 0);
    say_text = "10.0.2.15\n";
    st = run(USB, w_kprintf, 0, 0);
    row(2, r);
    check(st == DONE && !strcmp(r, "+NET: lease 10.0.2.15"), "its own half line does not make it wait");
    say(SHELL, "SURF > ");
    say_text = "+USB: plugged\n";
    st = run(USB, w_kprintf, 0, 0);
    for(i = 0; i < 10 && st == BLOCKED; i++) st = resume(USB);
    row(3, r);
    check(st == DONE && i == 10 && !strcmp(r, "SURF > +USB: plugged"), "a line that never ends (a prompt): after 100 ms, as before");
    paged_lines(1, 3);
    say(CHILD, "half ");
    sim_set_in_irq(1);
    sim_kprintf_line("irq\n");
    sim_set_in_irq(0);
    check(screen_has("half irq"), "an interrupt handler's message goes at once, as before");
    run(SHELL, w_page, 0, 0);
}

/* 10/25/26 (the review): a kernel thread's line goes out with preemption on, the console held for it (line_owner) */
static void t_line_owner(void) {
    char ser[4096], r[256];
    int st;
    reset(0);
    sim_serial(ser, sizeof(ser));
    say_text = "+NET: preempted half way\n";
    sim_serial_preempt(6);
    st = run(USB, w_kprintf, 0, 0);
    check(st == BLOCKED, "a kernel thread's line can be preempted half way: preemption stays on (a slow serial port)");
    say_text = "user line\n";
    st = run(SHELL, w_say, 0, 0);
    check(st == BLOCKED, "a program that writes on that console meanwhile waits for the line's end");
    st = resume(USB);
    check(st == DONE, "the kernel thread's line goes on and ends");
    st = resume(SHELL);
    sim_serial(ser, sizeof(ser));
    row(0, r);
    check(st == DONE && !strcmp(ser, "+NET: preempted half way\r\nuser line\r\n") && !strcmp(r, "+NET: preempted half way"),
          "then the program's line, after it, whole on the screen and the serial line");
    say_text = "+NET: two ";
    sim_serial_preempt(3);
    st = run(USB, w_kprintf, 0, 0);
    sim_task_gone(USB);
    say_text = "after\n";
    st = run(SHELL, w_say, 0, 0);
    check(st == DONE, "a task that ends holding the console lets it go");
}

static void t_stream_wrap_ask(void) {
    char l80[81];
    int st;
    memset(l80, 'x', 80);
    l80[80] = 0;
    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 8);
    say(CHILD, l80);                                /* the 9th row is full, the wrap pending */
    stream_now();
    say_text = "z";
    st = run(CHILD, w_say, 0, 0);
    check(st == BLOCKED && stream_is("\n" QUESTION),
          "the 9th row full with the wrap pending: a newline, then the question on the 10th");
    sim_input(' ');
    resume(CHILD);
    check(stream_is(UNASKED "z"), "and the character goes where the question was");

    on_pty(40, 10, 1);
    run(CHILD, w_lines, 1, 30);
    check(stream_is(lines_text("", 1, 9, "\033[7m--More-- (Space: next page, Enter: next\033[27m")),
          "at 40 columns the question is cut to 39");
}

/* 10/26/26 (the telnet stalled-client check's lost line): with the master's buffer full, a job in the background
   waits to print the newline that ends its line. Waiting, it has changed nothing on the console yet: a command
   printing behind a screenful of its own rows finds the job's row unfinished and asks nothing there. Its question
   used to land in the middle of the job's line ("job 1\033[7m--More--"), with the newline after it, and the CR ESC[K
   that took it away cleared the job's next row instead. Once there is room the job's newline goes first, then the
   command asks at the start of a row of its own. */
static void t_stream_full_row(void) {
    int st;
    on_pty(80, 10, 1);                              /* the shell pages its command line */
    run(CHILD, w_lines, 1, 9);                      /* the command: 9 rows; its next row asks first */
    say(BG, "job 1");                               /* a job in the background starts a line, unpaged */
    stream_now();
    sim_stream_full(1);                             /* the client stopped reading */
    say_text = "\n";
    st = run(BG, w_say, 0, 0);
    check(st == BLOCKED && stream_is(""), "the master's buffer full: the job waits to print its newline");
    st = run(CHILD, w_lines, 10, 10);
    check(st == BLOCKED && asker() < 0 && stream_is(""),
          "the command waits for room too, and asks nothing: the job's row has not ended");
    sim_stream_full(0);                             /* the master read: room */
    st = resume(BG);
    check(st == DONE && stream_is("\n"), "room: the job's newline goes first");
    st = resume(CHILD);
    check(st == BLOCKED && asker() == CHILD && stream_is(QUESTION),
          "then the command's question, at the start of a row of its own");
    sim_input(' ');
    st = resume(CHILD);
    check(st == DONE && stream_is(UNASKED "line 010\n"), "and its line where the question was");
}

/* 10/26/26 (the telnet stalled-client check's "never filled"): a job's rows in the background fill no screenful of
   its shell's. The shell, paging its command line, used to stop at --More-- when it printed "[1] started" behind a
   screenful of the job's rows, and waited for a key nobody would type while the job printed on around the
   question. The command's own output is paged after a screenful of its own rows. */
static void t_stream_background_rows(void) {
    int st;
    on_pty(80, 10, 1);
    run(BG, w_lines, 1, 30);                        /* three screenfuls of the job's rows */
    stream_now();
    say_text = "[1] started\n";
    st = run(SHELL, w_say, 0, 0);
    check(st == DONE && asker() < 0 && stream_is("[1] started\n"),
          "the shell prints its line behind the job's rows and asks nothing: none of them were its own");
    st = run(CHILD, w_lines, 1, 30);
    check(st == BLOCKED && asker() == CHILD && stream_is(lines_text("", 1, 8, QUESTION)),
          "its command is paged after a screenful of the shell's and its own rows");
    sim_input('q');
    st = resume(CHILD);
    check(st == DONE, "q: the rest is dropped");
}

/* 10/26/26 (the review of the gap fix): a tick between the pager's look at a job's byte and the step that prints
   it. A command asks meanwhile, at the row's start; the job's byte then takes the question away in its own step
   before it goes in, instead of landing behind the question for the next CR ESC[K to erase */
static void t_stream_question_tick(void) {
    int st;
    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 9);                      /* the command: its next row asks first */
    stream_now();
    sim_preempt_irq(BG, 2);                         /* the job, past page_hold()'s look, before stream_put()'s step */
    say_text = "job 1\n";
    st = run(BG, w_say, 0, 0);
    check(st == BLOCKED && stream_is(""), "a tick: the job stops before its first byte goes in");
    st = run(CHILD, w_lines, 10, 10);
    check(st == BLOCKED && asker() == CHILD && stream_is(QUESTION), "meanwhile the command asks, at the row's start");
    st = resume(BG);
    check(st == DONE && stream_is(UNASKED "job 1\n" QUESTION),
          "the job's byte takes the question away in its own step; the question comes back after its newline");
    sim_input(' ');
    st = resume(CHILD);
    check(st == DONE && stream_is(UNASKED "line 010\n"), "and the command goes on where it was");
}

/* 10/26/26 (the review of the gap fix): between a command's look at the row and its question, a job opens an escape
   sequence there (ESC leaves the row's start as it was), or a program of the command's asks first. No question goes
   into the job's sequence, and none is asked twice */
static void t_stream_ask_again(void) {
    int st;
    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 9);
    stream_now();
    sim_preempt_irq(CHILD, 2);                      /* the command, past page_hold()'s look, before page_ask()'s step */
    st = run(CHILD, w_lines, 10, 10);
    check(st == BLOCKED && asker() < 0 && stream_is(""), "a tick before the command's question");
    say(BG, "\033");                                /* the job opens a sequence at the row's start */
    stream_now();
    st = resume(CHILD);
    check(st == BLOCKED && asker() == CHILD && stream_is("l" QUESTION),
          "no question inside the job's sequence: the command's byte ends it (one parser), then it asks at the row start");
    sim_input(' ');
    st = resume(CHILD);
    check(st == DONE && stream_is(UNASKED "ine 010\n"), "a key: the command goes on");

    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 9);
    stream_now();
    sim_preempt_irq(CHILD, 2);
    st = run(CHILD, w_lines, 10, 10);
    check(st == BLOCKED && asker() < 0, "again a tick before the command's question");
    say_text = "grand\n";
    st = run(GRAND, w_say, 0, 0);                   /* a program of the command's asks first */
    check(st == BLOCKED && asker() == GRAND && stream_is(QUESTION), "its program asks first");
    st = resume(CHILD);
    check(st == BLOCKED && asker() == GRAND && stream_is(""),
          "the command asks no second question and prints nothing through the program's: it waits for the answer");
    sim_input(' ');
    st = resume(GRAND);
    check(st == DONE && stream_is(UNASKED "grand\n"), "a key: the program goes on");
    st = resume(CHILD);
    check(st == DONE && stream_is("line 010\n"), "and then the command, its line whole");
}

static void t_stream_resize(void) {
    int st;
    on_pty(80, 10, 1);
    st = run(CHILD, w_lines, 1, 40);
    stream_now();
    sim_stream_size(80, 5);
    check(st == BLOCKED && asker() == CHILD && sim_on_list(CHILD), "a smaller size leaves the question up");
    sim_stream_size(80, 20);
    st = resume(CHILD);
    check(st == BLOCKED && stream_is(lines_text(UNASKED, 10, 19, QUESTION)),
          "PTY_SETSIZE 80x20 at the question: it goes without a key, and the rows go on to 19 before it comes back");
    sim_input(' ');
    st = resume(CHILD);
    check(st == BLOCKED && stream_is(lines_text(UNASKED, 20, 38, QUESTION)), "then 19 lines a screenful");
    sim_input('q');
    resume(CHILD);
}

static void t_stream_colour(void) {
    on_pty(80, 24, 0);
    sim_kprintf_line("kernel\n");
    check(stream_is("\033[0;1;33mkernel\n"), "a kernel message (yellow): its SGR sequence first");
    say(SHELL, "plain\n");
    check(stream_is("\033[0mplain\n"), "the console's own colour again: ESC[0m, the terminal's default");
    say(SHELL, "\033[36mcyan ");
    sim_kprintf_line("k2 ");
    check(stream_is("\033[36mcyan k2 "), "while a program's SGR is in force, the writer's colour is not sent");
    say(SHELL, "\033[0m");
    sim_kprintf_line("k3");
    say(SHELL, "\033[7m!");
    check(stream_is("\033[0m\033[0;1;33mk3\033[0m\033[7m!"),
          "after the program's ESC[0m the colours are sent again, also before a program's own sequence");
    say(SHELL, "\033[0m");
    stream_now();
    say(SHELL, "\033[7m\033[?25l");
    stream_now();
    sim_task_gone(SHELL);
    check(stream_is("\033[0m\033[?25h"),
          "a program that ends with inverse video and the cursor hidden: ESC[0m ESC[?25h to the far terminal");
}

static void t_stream_backspace(void) {
    char l80[81];
    memset(l80, 'x', 80);
    l80[80] = 0;
    on_pty(80, 24, 0);
    say(SHELL, "ab\b");
    check(stream_is("ab\b \b"), "Backspace erases the cell before: BS SP BS");
    say(SHELL, "\r");
    say(SHELL, l80);
    stream_now();
    say(SHELL, "\b");
    check(stream_is("\r\033[999C\033[K"), "at a pending wrap: the last column (CR ESC[999C ESC[K)");
    say(SHELL, "\n\b");
    check(stream_is("\n\033[A\033[999C\033[K"), "at a row's start: the end of the row above");
    say(SHELL, "a\001\002\177b\033[1mc");
    check(stream_is("ab\033[1mc"), "other control characters and DEL are dropped, as the screen draws none");
    say(SHELL, "\033[0m\033[H");
    stream_now();
    say(SHELL, "\b");
    check(stream_is(""), "at the top left: nothing, as on the screen");
}

/* the review's probes: ESC[J after printing at the top left is no new screenful; a question
   that could not be sent is not taken away */
static void t_stream_review(void) {
    int st, x, wrap, rows, drop, a, o;
    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 5);
    say(CHILD, "\033[Habc\033[J");
    page_state(&rows, &drop, &a, &o);
    sim_stream_pos(&x, &wrap);
    check(rows == 5 && x == 3, "ESC[H, text, ESC[J: not an empty screen, the 5 rows still count");
    stream_now();
    say(CHILD, "\n");
    st = run(CHILD, w_lines, 6, 30);
    check(st == BLOCKED && stream_is(lines_text("\n", 6, 8, QUESTION)), "so the question comes after 9 rows, not later");
    sim_input('q');
    resume(CHILD);

    on_pty(80, 10, 1);
    sim_stream_drop(1);
    st = run(CHILD, w_lines, 1, 12);
    sim_stream_drop(0);
    check(st == BLOCKED && asker() == CHILD, "the question with no room left in the master's buffer: it is asked all the same");
    sim_input(' ');
    st = resume(CHILD);
    check(st == DONE && stream_is(lines_text("", 10, 12, "")), "and with nothing shown, nothing is taken away (no CR ESC[K)");
}

static void t_stream_clear(void) {
    int st;
    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 8);
    say(CHILD, "\033[2J");
    st = run(CHILD, w_lines, 9, 17);
    check(st == DONE && asker() < 0, "ESC[2J: a new screenful, 9 more lines without a question");
    say(CHILD, "\033[H\033[J");
    st = run(CHILD, w_lines, 18, 26);
    check(st == DONE && asker() < 0, "ESC[H ESC[J: a new screenful too");
    stream_now();
    run(CHILD, w_clear, 0, 0);
    check(stream_is("\033[2J\033[H"), "clearing the console: ESC[2J ESC[H to the far terminal");
    st = run(CHILD, w_lines, 27, 35);
    check(st == DONE && asker() < 0, "and a new screenful");
    run(SHELL, w_page, 0, 0);
}

static void t_stream_tty(void) {
    int st;
    reset(1);                                       /* `ascii` on the kernel command line */
    sim_term(0);
    check(sim_ttyctl(TTY_CAPS, 0) == 7, "a pty's TTY_CAPS: UTF-8, colour and the alternate screen, whatever `ascii` says");
    check(sim_ttyctl(2, 0) == ((80 << 16) | 24), "TTY_GETSIZE: 80x24 at first");
    sim_stream_size(100, 30);
    check(sim_ttyctl(2, 0) == ((100 << 16) | 30), "TTY_GETSIZE: the size PTY_SETSIZE gave");

    /* the serial line's rules for what the master writes */
    on_pty(80, 24, 0);
    sim_term_input('\r');
    run(SHELL, w_getc, 0, 0);
    check(got_key == '\n', "line mode: CR is Enter");
    sim_term_input(0x7F);
    run(SHELL, w_getc, 0, 0);
    check(got_key == 0x08, "DEL is Backspace");
    sim_term_input(0x1B);
    sim_term_input('[');
    sim_term_input('A');
    run(SHELL, w_getc, 0, 0);
    check(got_key == sim_key_up(), "ESC [ A is Up");
    sim_term_input(0x1B);
    sim_term_input('O');
    sim_term_input('H');
    sim_term_input(0x1B);
    sim_term_input('[');
    sim_term_input('3');
    sim_term_input('~');
    sim_term_input('k');
    run(SHELL, w_getc, 0, 0);
    check(got_key == sim_key_home(), "ESC O H is Home");
    run(SHELL, w_getc, 0, 0);
    check(got_key == 'k', "a sequence it does not know is swallowed to its final byte");
    check(sim_term_queues('k') && !sim_term_queues(0x03) && !sim_term_queues(0x1B), "line mode: a key takes a place in "
          "the queue; Ctrl+C and a lone Esc (with no question up) do not, so a full queue never holds them up");
    sim_term_input(0x1B);
    check(!sim_term_queues('['), "nor what follows an Esc");
    sim_term_input('[');
    check(!sim_term_queues('1') && sim_term_queues('A') && !sim_term_queues('~'), "only the final byte of a cursor key does");
    check(!sim_term_queues(0x03), "inside a sequence, Ctrl+C needs no room either");
    sim_term_input(0x03);                           /* a cursor key cut short by a full queue, then Ctrl+C */
    sim_term_input('k');
    run(SHELL, w_getc, 0, 0);
    st = got_key;
    run(SHELL, w_getc, 0, 0);
    check(st == 0x03 && got_key == 'k', "Ctrl+C ends a sequence in progress and gets through; what follows is plain again");
    run(SHELL, w_raw, 1, 0);
    sim_term_input('\r');
    sim_term_input(0x1B);
    sim_term_input('[');
    sim_term_input('A');
    run(SHELL, w_getc, 0, 0);
    st = got_key;
    run(SHELL, w_getc, 0, 0);
    check(st == '\r' && got_key == 0x1B, "raw mode: the bytes as they were sent");
    run(SHELL, w_raw, 0, 0);

    /* a lone Esc answers the question; killed at the question, it goes */
    on_pty(80, 10, 1);
    st = run(CHILD, w_lines, 1, 30);
    sim_term_input(0x1B);
    st = resume(CHILD);
    check(st == DONE && dropping(), "Esc from the master answers --More--: the rest is dropped");
    on_pty(80, 10, 1);
    run(CHILD, w_lines, 1, 30);
    stream_now();
    sim_task_gone(CHILD);
    check(asker() < 0 && stream_is(UNASKED), "the asker killed at the question: CR ESC[K takes it away");
}

/* 10/6/26: a line-mode Ctrl+C drops what was typed ahead (F1 and a pty alike); room appearing in a
   pty's full input queue wakes poll(), its master being ready for writing again */
static void t_term_room(void) {
    int i, w, room;
    reset(0);                                       /* F1, line mode */
    sim_input('r');
    sim_input('m');
    sim_input('\n');
    sim_input(0x03);
    run(SHELL, w_getc, 0, 0);
    check(got_key == 0x03, "F1: Ctrl+C in line mode drops the keys typed ahead (\"rm\\n\" never runs)");
    run(SHELL, w_raw, 1, 0);
    sim_input('a');
    sim_input(0x03);
    run(SHELL, w_getc, 0, 0);
    i = got_key;
    run(SHELL, w_getc, 0, 0);
    check(i == 'a' && got_key == 0x03, "raw mode: Ctrl+C is a key like any other, nothing dropped");
    run(SHELL, w_raw, 0, 0);

    on_pty(80, 24, 0);
    for(i = 0; sim_room() && i < 1000; i++) sim_input('x');
    check(i == 255 && !sim_room(), "a pty's input queue holds 255 keys");
    w = sim_poll_wakes();
    run(SHELL, w_getc, 0, 0);
    check(got_key == 'x' && sim_room() == 1 && sim_poll_wakes() == w + 1, "a key read from the full queue wakes poll()");
    w = sim_poll_wakes();
    run(SHELL, w_getc, 0, 0);
    check(sim_poll_wakes() == w, "and only from a full one");
    while(sim_room()) sim_input('x');
    w = sim_poll_wakes();
    check(sim_term_queues(0x03) == 0, "Ctrl+C needs no room: it fits a full queue");
    sim_term_input(0x03);
    room = sim_room();
    run(SHELL, w_getc, 0, 0);
    check(got_key == 0x03 && room == 254 && sim_poll_wakes() > w,
          "a pty: Ctrl+C drops the full queue's keys (poll() woken: room), and is the next key");
}

/* 10/6/26: the rest of a paste is skipped until the input pauses (TTY_SKIP in tty.h) */
#define TTY_SKIP 5
static void feed(const char *s, int gap_ms) {
    for(; *s; s++) {
        sim_time_ms(gap_ms);
        sim_term_input((unsigned char)*s);
    }
}

static void t_paste_skip(void) {
    int a, b, c;
    on_pty(80, 24, 0);
    run(SHELL, w_raw, 1, 0);
    feed("ls\rjunk", 1);                          /* a line and more, pasted: the reader takes the line */
    run(SHELL, w_getc, 0, 0);
    a = got_key;
    run(SHELL, w_getc, 0, 0);
    b = got_key;
    run(SHELL, w_getc, 0, 0);
    c = got_key;
    run(SHELL, w_raw, 0, 0);                        /* raw mode ends with "junk" queued */
    check(a == 'l' && b == 's' && c == '\r' && sim_room() == 255, "raw mode's end drops the keys after the Enter");
    feed("echo frag\r", 50);
    check(sim_room() == 255, "and what comes on in the same burst is dropped too, line mode or not");
    check(!sim_term_queues('x'), "a byte that is dropped needs no room");
    sim_time_ms(250);
    sim_term_input('y');
    check(sim_room() == 254, "after a pause of 200 ms a key is input again");
    run(SHELL, w_getc, 0, 0);

    sim_ttyctl(TTY_SKIP, 1);                        /* a program dropped keys of its own */
    feed("ab", 50);
    check(sim_room() == 255, "TTY_SKIP 1: the burst is dropped");
    sim_time_ms(50);
    sim_term_input(0x03);
    feed("c", 10);
    run(SHELL, w_getc, 0, 0);
    a = got_key;
    run(SHELL, w_getc, 0, 0);
    check(a == 0x03 && got_key == 'c', "Ctrl+C gets through the skip and ends it");

    sim_ttyctl(TTY_SKIP, 1);
    sim_key('k');                                   /* the PS/2 keyboard */
    run(SHELL, w_getc, 0, 0);
    check(got_key == 'k', "the keyboard's keys are never skipped");
    check(sim_ttyctl(TTY_SKIP, 0) == 0 && sim_ttyctl(TTY_SKIP, 2) < 0, "TTY_SKIP 0 ends it; 2 is refused");
    feed("q", 10);
    run(SHELL, w_getc, 0, 0);
    check(got_key == 'q', "after TTY_SKIP 0, input at once");

    run(SHELL, w_raw, 1, 0);                        /* a line typed by hand: nothing queued after its Enter */
    feed("ls\r", 100);
    run(SHELL, w_getc, 0, 0);
    run(SHELL, w_getc, 0, 0);
    run(SHELL, w_getc, 0, 0);
    run(SHELL, w_raw, 0, 0);
    feed("pwd", 30);
    check(sim_room() == 252, "raw mode's end with nothing queued skips nothing: type-ahead is kept");
}

/* 10/6/26: a line read in raw mode from n bytes that come 10 ms apart; raw mode ends after its
   CR, and "pwd" comes 10 ms later. How many of its keys are input (-1: keys left after the CR
   were kept)? The terminal is left with nothing queued and no skip. */
static int after_line(const char *s, int n) {
    int i, kept;
    run(SHELL, w_raw, 1, 0);
    for(i = 0; i < n; i++) {
        sim_time_ms(10);
        sim_term_input((unsigned char)s[i]);
    }
    do run(SHELL, w_getc, 0, 0); while(got_key != '\r');
    run(SHELL, w_raw, 0, 0);
    kept = sim_room() == 255 ? 0 : -1;
    feed("pwd", 10);
    if(kept == 0) kept = 255 - sim_room();
    while(sim_room() != 255) run(SHELL, w_getc, 0, 0);
    sim_ttyctl(TTY_SKIP, 0);
    return kept;
}

/* 10/6/26: what a terminal sends after the CR of its Enter (CR LF, or a telnet client's CR NUL)
   may still be queued when raw mode ends: dropped, but it is no paste's rest, so the next
   command, sent at once (a script, a fast typist), is kept. More than that starts the skip. */
static void t_paste_crlf(void) {
    int t;
    for(t = -1; t <= 0; t++) {
        const char *where = t < 0 ? "F1 (the serial line)" : "a pty";
        char what[160];
        reset(0);
        sim_term(t);
        snprintf(what, sizeof(what), "%s: raw mode's end drops a lone LF (CR LF's) and starts no skip", where);
        check(after_line("ls\r\n", 4) == 3, what);
        snprintf(what, sizeof(what), "%s: nor does a lone NUL (CR NUL's)", where);
        check(after_line("ls\r\0", 4) == 3, what);
        snprintf(what, sizeof(what), "%s: two LFs left start the skip", where);
        check(after_line("ls\r\n\n", 5) == 0, what);
        snprintf(what, sizeof(what), "%s: so does a lone key that is no LF or NUL", where);
        check(after_line("ls\rx", 4) == 0, what);
        snprintf(what, sizeof(what), "%s: and an LF with a key after it", where);
        check(after_line("ls\r\nx", 5) == 0, what);
    }
}

/**** 10/9/26 (Wi-Fi): a secret line ****/

static char secret_got[64];
static int secret_len;
static void w_secret(int unused, int unused2) {
    (void)unused;
    (void)unused2;
    sim_print("Password for net: ", 18);
    secret_len = sim_readline_secret(secret_got, sizeof(secret_got));
    sim_print("done\n", 5);
}

static void w_line(int unused, int unused2) {
    (void)unused;
    (void)unused2;
    sim_readline(line_got, sizeof(line_got));
}

static void type(const char *k, int term) {
    for(; *k; k++) {
        if(term) sim_term_input((unsigned char)*k);
        else sim_input((unsigned char)*k);
    }
}

static void t_secret(void) {
    char ser[4096];
    int st;
    reset(0);
    type("ls -l\n", 0);
    run(SHELL, w_line, 0, 0);                      /* a line for the history */
    sim_serial(ser, sizeof(ser));
    type("hunter2x\b", 0);
    sim_input('\n');
    st = run(CHILD, w_secret, 0, 0);
    check(st == DONE && secret_len == 7 && !strcmp(secret_got, "hunter2"), "a secret line is read, Backspace and all");
    check(screen_has("Password for net:") && screen_has("done") && !screen_has("hunter"),
          "the screen shows the question and none of the keys typed");
    sim_serial(ser, sizeof(ser));
    check(strstr(ser, "Password for net: ") && !strstr(ser, "hunter") && !strchr(ser, '\b'),
          "the serial line gets the question and the Enter, no key and no Backspace");
    check(!sim_inq_has("hunter"), "its keys are wiped from the input queue as they are read");
    sim_input(sim_key_up());
    sim_input('\n');
    run(SHELL, w_line, 0, 0);
    check(!strcmp(line_got, "ls -l"), "Up then recalls the line before it: the secret is in no history");
    sim_input(sim_key_up());
    type("x\n", 0);
    run(CHILD, w_secret, 0, 0);
    check(!strcmp(secret_got, "x"), "Up in a secret line recalls nothing into it");
    type("abc\x15" "def\n", 0);
    run(CHILD, w_secret, 0, 0);
    check(!strcmp(secret_got, "def") && !screen_has("abc"), "Ctrl-U clears it, unseen");
    type("zzz\x03", 0);
    st = run(CHILD, w_secret, 0, 0);
    check(st == DONE && secret_len == 0 && secret_got[0] == 0 && screen_has("^C") && !screen_has("zzz"),
          "Ctrl-C: ^C and an empty answer");
    sim_input('a');
    check(sim_inq_has("a"), "keys typed after it are still queued for the next reader");

    /* a pseudo-terminal (telnet): the stream carries the question and the newline only */
    on_pty(80, 24, 0);
    type("pa55word\r", 1);
    st = run(CHILD, w_secret, 0, 0);
    stream_now();
    check(st == DONE && !strcmp(secret_got, "pa55word"), "a pty: the secret line is read");
    check(strstr(strm, "Password for net: ") && strstr(strm, "done") && !strstr(strm, "pa55"),
          "a pty's stream: the question and the Enter, no key typed");
    check(!sim_inq_has("pa55"), "a pty's input queue keeps none of it");
}

int main(void) {
    int i;
    for(i = 0; i < NT; i++) pthread_create(&task[i].th, NULL, task_main, (void *)(long)i);
    t_screenful();
    t_quit();
    t_wrap();
    t_no_waiting();
    t_reader_first();
    t_capture();
    t_interrupt_rows();
    t_background_rows();
    t_owner();
    t_utf8_writers();
    t_question_then_reader();
    t_question_owner_gone();
    t_waiting_for_the_answer();
    t_question_gives_way();
    t_drop_question();
    t_drop_crash();
    t_nested();
    t_takeover();
    t_background_unpaged();
    t_new_screenful();
    t_echo_not_kept();
    t_caps();
    t_stream_screenful();
    t_stream_columns();
    t_tab();
    t_scrollback();
    t_full_queue();
    t_whole_kernel_lines();
    t_line_owner();
    t_vt_cases();
    t_alt_screen();
    t_stream_wrap_ask();
    t_stream_full_row();
    t_stream_background_rows();
    t_stream_question_tick();
    t_stream_ask_again();
    t_stream_resize();
    t_stream_colour();
    t_stream_backspace();
    t_stream_review();
    t_stream_clear();
    t_stream_tty();
    t_term_room();
    t_paste_skip();
    t_paste_crlf();
    t_secret();
    t_lock_preempted();
    t_lock_interrupt();
    t_lock_question();
    reset(0);
    printf("%s: %d checks, %d failures\n", failures ? "PAGER UNIT TESTS FAILED" : "PAGER UNIT TESTS PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
