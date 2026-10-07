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
   the column count), the serial line's input rules (tty_term_input()). */
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
void sim_stream_pos(int *x, int *wrap);
void sim_term_input(int c);
int sim_term_queues(int c);
int sim_room(void);
void sim_time_ms(int ms);
int sim_poll_wakes(void);
void sim_clear(void);
int sim_key_up(void);
int sim_key_home(void);

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

static void t_interrupt_rows(void) {
    int rows, drop, a, o;
    reset(0);
    paged_lines(1, 20);
    sim_set_in_irq(1);
    sim_kprintf_line("an interrupt handler says hi\n");
    sim_set_in_irq(0);
    page_state(&rows, &drop, &a, &o);
    check(rows == 21 && a < 0, "an interrupt handler's line takes a row of the screenful and never waits");
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
    check(sim_ttyctl(TTY_CAPS, 0) == 3, "TTY_CAPS: UTF-8 and colour");
    reset(1);
    check(sim_ttyctl(TTY_CAPS, 0) == 2, "TTY_CAPS with `ascii` on the command line: colour only");
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
    check(sim_ttyctl(TTY_CAPS, 0) == 3, "a pty's TTY_CAPS: UTF-8 and colour, whatever `ascii` says");
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
    t_new_screenful();
    t_echo_not_kept();
    t_caps();
    t_stream_screenful();
    t_stream_columns();
    t_stream_wrap_ask();
    t_stream_resize();
    t_stream_colour();
    t_stream_backspace();
    t_stream_review();
    t_stream_clear();
    t_stream_tty();
    t_term_room();
    t_paste_skip();
    t_paste_crlf();
    reset(0);
    printf("%s: %d checks, %d failures\n", failures ? "PAGER UNIT TESTS FAILED" : "PAGER UNIT TESTS PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
