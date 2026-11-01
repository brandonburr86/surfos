/* Host tests for what top and watch share (user/bin/termview.h) and for top's model (user/bin/topmodel.h), built as they
   are with the host's C library (under clang's AddressSanitizer and UBSan by hand on the Mac, never in `make unittest`,
   whose image hangs on a sanitized binary: docs/superpowers/plans/2026-11-01-cli-suite-sys.md): seconds as the options
   take them; keys, in pieces and broken off; characters and text from outside; rows sent only when they changed; the
   readings tidied, shares worked out across a counter's wrap, tasks that came and went; the three orders; every row's
   text, on wide and narrow screens, with names that are not text; random tasks, whose rows must stay fit to show.
   No real tool to compare with: top on Linux and macOS prints other columns, and its numbers are the machine's.
   Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "topmodel.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/**** termview.h ****/

static unsigned int secs(const char *s) {
    unsigned int ms = 12345;
    return tv_seconds(s, &ms) < 0 ? 0 : ms;
}

static void seconds(void) {
    static const char *const bad[] = { "", ".", "abc", "1x", "-1", "+1", "1 ", " 1", "0", "0.0", "0.09", "0.099", "86401",
                                       "86400.001", "99999999999999999999", "1.2.3", "1e3", "0x10" };
    unsigned int i;
    char t[32];
    CHECK(secs("2") == 2000);
    CHECK(secs("0.5") == 500);
    CHECK(secs(".5") == 500);
    CHECK(secs("1.") == 1000);
    CHECK(secs("1.25") == 1250);
    CHECK(secs("0.1") == 100);
    CHECK(secs("1.2345678") == 1234);      /* no more than milliseconds: the rest is read and dropped */
    CHECK(secs("86400") == 86400000);
    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!secs(bad[i]));
    tv_every(2000, t, sizeof(t));
    CHECK(!strcmp(t, "2s"));
    tv_every(500, t, sizeof(t));
    CHECK(!strcmp(t, "0.5s"));
    tv_every(1250, t, sizeof(t));
    CHECK(!strcmp(t, "1.25s"));
    tv_every(1234, t, sizeof(t));
    CHECK(!strcmp(t, "1.234s"));
    tv_every(1005, t, sizeof(t));
    CHECK(!strcmp(t, "1.005s"));
}

/* the last key of those bytes, TV_NONE for none; each given on its own, as a slow line would */
static int key_of(struct tv_keys *k, const char *s) {
    int r = TV_NONE;
    for(; *s; s++) {
        int c = tv_key(k, (unsigned char)*s);
        if(c != TV_NONE) r = c;
    }
    return r;
}

static void keys(void) {
    struct tv_keys k;
    memset(&k, 0, sizeof(k));
    CHECK(key_of(&k, "q") == 'q');
    CHECK(key_of(&k, "\x03") == 3);
    CHECK(key_of(&k, "\x1b[A") == TV_UP);
    CHECK(key_of(&k, "\x1b[B") == TV_DOWN);
    CHECK(key_of(&k, "\x1bOA") == TV_UP);
    CHECK(key_of(&k, "\x1b[H") == TV_HOME);
    CHECK(key_of(&k, "\x1b[F") == TV_END);
    CHECK(key_of(&k, "\x1b[5~") == TV_PGUP);
    CHECK(key_of(&k, "\x1b[6~") == TV_PGDN);
    CHECK(key_of(&k, "\x1b[1~") == TV_HOME);
    CHECK(key_of(&k, "\x1b[4~") == TV_END);
    CHECK(key_of(&k, "\x1b[7~") == TV_HOME);
    CHECK(key_of(&k, "\x1b[8~") == TV_END);
    CHECK(key_of(&k, "\x1b[1;5A") == TV_UP);        /* with a modifier: the key is what counts */
    CHECK(key_of(&k, "\x1b[5;2~") == TV_PGUP);
    CHECK(key_of(&k, "\x1b[C") == TV_NONE);         /* no key of top's */
    CHECK(key_of(&k, "\x1b[15~") == TV_NONE);
    CHECK(key_of(&k, "\x1b[99999999999999~") == TV_NONE);
    CHECK(key_of(&k, "\x1b[1;5C") == TV_NONE);
    CHECK(key_of(&k, "\x1b") == TV_NONE);           /* the Escape key alone: nothing, and the next key is itself */
    CHECK(key_of(&k, "q") == 'q');
    CHECK(key_of(&k, "\x1b[5") == TV_NONE);         /* Ctrl+C in the middle of a sequence gets through */
    CHECK(key_of(&k, "\x03") == 3);
    CHECK(key_of(&k, "c") == 'c');
    CHECK(key_of(&k, "\x1b[\x1b[A") == TV_UP);      /* a sequence cut off by the next */
    CHECK(key_of(&k, "\x1b\x1b[B") == TV_DOWN);
}

static void characters(void) {
    static const struct { const char *s; int n; int want; } utf[] = {
        { "a", 1, 1 }, { "\xc3\xa9", 2, 2 }, { "\xe2\x89\x8b", 3, 3 }, { "\xf0\x9f\x8c\x8a", 4, 4 },
        { "\xc3", 1, -1 }, { "\xe2\x89", 2, -1 }, { "\xf0\x9f\x8c", 3, -1 }, { "\xe2", 1, -1 },
        { "\x80", 1, 0 }, { "\xc0\x80", 2, 0 }, { "\xc1\xbf", 2, 0 }, { "\xff", 1, 0 }, { "\xf5\x80\x80\x80", 4, 0 },
        { "\xe0\x80\x80", 3, 0 }, { "\xe0\x80", 2, 0 }, { "\xed\xa0\x80", 3, 0 }, { "\xf0\x80\x80\x80", 4, 0 },
        { "\xf4\x90\x80\x80", 4, 0 }, { "\xc2\x80", 2, 0 }, { "\xc2\x9f", 2, 0 }, { "\xc2\xa0", 2, 2 }, { "\xe2\x41", 2, 0 },
        { "\xf0\x9f\x41", 3, 0 },
    };
    char out[32];
    unsigned int i;
    for(i = 0; i < sizeof(utf) / sizeof(utf[0]); i++) CHECK(tv_utf8((const unsigned char *)utf[i].s, (size_t)utf[i].n) == utf[i].want);
    CHECK(tv_clean("plain", 5, out, sizeof(out), 1) == 5 && !strcmp(out, "plain"));
    CHECK(tv_clean("a\x1b[2Jb", 6, out, sizeof(out), 1) == 6 && !strcmp(out, "a?[2Jb"));   /* ESC cannot reach the terminal */
    CHECK(tv_clean("a\x7f\n\tb", 5, out, sizeof(out), 1) == 5 && !strcmp(out, "a???b"));
    CHECK(tv_clean("caf\xc3\xa9", 5, out, sizeof(out), 1) == 5 && !strcmp(out, "caf\xc3\xa9"));
    CHECK(tv_clean("caf\xc3\xa9", 5, out, sizeof(out), 0) == 4 && !strcmp(out, "caf?"));
    CHECK(tv_clean("caf\xc3", 4, out, sizeof(out), 1) == 4 && !strcmp(out, "caf?"));         /* cut inside a character */
    CHECK(tv_clean("x\xe2\x89", 3, out, sizeof(out), 1) == 3 && !strcmp(out, "x??"));
    CHECK(tv_clean("\xff\xfe", 2, out, sizeof(out), 1) == 2 && !strcmp(out, "??"));
    CHECK(tv_clean("ab\0cd", 5, out, sizeof(out), 1) == 2 && !strcmp(out, "ab"));            /* a C string ends at its NUL */
    CHECK(tv_clean("abcdefgh", 8, out, 6, 1) == 2 && !strcmp(out, "ab"));                    /* room for a whole character */
    CHECK(tv_clean("abcdefgh", 8, out, 1, 1) == 0 && !out[0]);
    strcpy(out, "h\xc3\xa9llo");
    CHECK(tv_chars(out, strlen(out)) == 5);
    CHECK(tv_cut(out, 2) == 3 && !strcmp(out, "h\xc3\xa9"));
    strcpy(out, "h\xc3\xa9llo");
    CHECK(tv_cut(out, 1) == 1 && !strcmp(out, "h"));
    strcpy(out, "ab");
    CHECK(tv_cut(out, 2) == 2 && tv_cut(out, 80) == 2 && tv_cut(out, 0) == 0 && !out[0]);
}

static int sent(const char *want) {
    int same = tv_out_len == strlen(want) && !memcmp(tv_out, want, tv_out_len);
    tv_out_len = 0;
    return same;
}

static void rows(void) {
    CHECK(tv_resize(0) == 1 && tv_cols == 80 && tv_rows == 25);     /* not known: 80x25 */
    CHECK(sent("\x1b[0m\x1b[2J"));
    CHECK(tv_resize((80 << 16) | 25) == 0 && tv_out_len == 0);
    tv_row(3, "hello", 5);
    CHECK(sent("\x1b[4;1H\x1b[Khello"));
    tv_row(3, "hello", 5);                                          /* the same: nothing goes out */
    CHECK(sent(""));
    tv_row(3, "hullo", 5);
    CHECK(sent("\x1b[4;1H\x1b[Khullo"));
    tv_row(3, "", 0);
    CHECK(sent("\x1b[4;1H\x1b[K"));
    tv_row(3, "", 0);
    CHECK(sent(""));
    tv_row(-1, "x", 1);                                             /* off the table: nothing */
    tv_row(TV_MAX_ROWS, "x", 1);
    CHECK(sent(""));
    CHECK(tv_resize((120 << 16) | 33) == 1 && tv_cols == 120 && tv_rows == 33);
    CHECK(sent("\x1b[0m\x1b[2J"));
    tv_row(3, "", 0);                                               /* a new size: every row again */
    CHECK(sent("\x1b[4;1H\x1b[K"));
    CHECK(tv_resize((4 << 16) | 1) == 1 && tv_cols == 16 && tv_rows == 3);
    tv_out_len = 0;
    CHECK(tv_resize((30000 << 16) | 30000) == 1 && tv_cols == TV_MAX_COLS && tv_rows == TV_MAX_ROWS);
    tv_out_len = 0;
    CHECK(tv_resize(-1) == 1 && tv_cols == 80);
    tv_out_len = 0;
    tv_add("x", 1);                                                 /* the buffer grows past its first size */
    { char *big = malloc(100000); memset(big, 'y', 100000); tv_add(big, 100000); free(big); }
    CHECK(tv_out_len == 100001 && tv_out[0] == 'x' && tv_out[100000] == 'y');
    tv_out_len = 0;
    tv_forget();
}

/**** topmodel.h ****/

static struct procinfo task(int pid, int ppid, int state, unsigned flags, int tty, const char *name, unsigned ticks, unsigned pages) {
    struct procinfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.pid = pid;
    pi.ppid = ppid;
    pi.state = state;
    pi.flags = flags;
    pi.tty = tty;
    strncpy(pi.name, name, sizeof(pi.name) - 1);
    pi.cpu_ticks = ticks;
    pi.mem_pages = pages;
    return pi;
}

static void add(struct top_list *l, struct procinfo pi) {
    CHECK(top_add(l, &pi) == 0);
}

static const struct top_task *of(const struct top_list *l, int pid) {
    int i;
    for(i = 0; i < l->n; i++) if(l->t[i].pi.pid == pid) return &l->t[i];
    return NULL;
}

static void readings(void) {
    struct top_list a, b;
    int i;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    for(i = 0; i < 200; i++) add(&a, task(200 - i, 0, PROC_READY, 0, -1, "t", 0, 0));   /* the list grows */
    CHECK(a.n == 200);
    add(&a, task(5, 0, PROC_READY, 0, -1, "again", 0, 0));          /* read twice: a task before it was collected */
    top_tidy(&a);
    CHECK(a.n == 200 && a.t[0].pi.pid == 1 && a.t[199].pi.pid == 200);
    for(i = 1; i < a.n; i++) CHECK(a.t[i - 1].pi.pid < a.t[i].pi.pid);
    top_free(&a);
    CHECK(!a.t && !a.n && !a.cap);

    CHECK(top_share(0, 100) == 0);
    CHECK(top_share(1, 100) == 10);
    CHECK(top_share(50, 100) == 500);
    CHECK(top_share(100, 100) == 1000);
    CHECK(top_share(150, 100) == 1000);                             /* the readings are no one instant */
    CHECK(top_share(5, 0) == 0);                                    /* two readings in one tick */
    CHECK(top_share(0, 0) == 0);
    CHECK(top_share(1, 3) == 333);
    CHECK(top_share(4294967u, 4294967u * 2) == 500);
    CHECK(top_share(4294968u, 4294968u * 2) == 500);                /* past what * 1000 can hold */
    CHECK(top_share(4000000000u, 4294967295u) == 931);

    /* the second of two readings, 200 ticks later: 0 runs on, 1 ran all of it, 2's counter wrapped, 3 is new, 4 went */
    add(&a, task(0, -1, PROC_RUNNING, 0, -1, "idle", 1000, 0));
    add(&a, task(1, 0, PROC_BLOCKED, 0, -1, "init", 100, 0));
    add(&a, task(2, 1, PROC_READY, PROC_USER, 0, "wrap", 0xfffffff0u, 10));
    add(&a, task(4, 1, PROC_READY, PROC_USER, 0, "gone", 50, 10));
    top_tidy(&a);
    add(&b, task(0, -1, PROC_RUNNING, 0, -1, "idle", 1080, 0));
    add(&b, task(1, 0, PROC_BLOCKED, 0, -1, "init", 300, 0));
    add(&b, task(2, 1, PROC_READY, PROC_USER, 0, "wrap", 0x0000000cu, 10));
    add(&b, task(3, 1, PROC_READY, PROC_USER, 0, "new", 40, 10));
    top_tidy(&b);
    top_shares(&b, &a, 200);
    CHECK(of(&b, 0)->tenths == 400 && of(&b, 1)->tenths == 1000 && of(&b, 2)->tenths == 140 && of(&b, 3)->tenths == 200);
    CHECK(!of(&b, 4));
    top_shares(&b, NULL, 4000);                                     /* the first report: since boot */
    CHECK(of(&b, 0)->tenths == 270 && of(&b, 1)->tenths == 75 && of(&b, 3)->tenths == 10);
    top_shares(&b, &a, 0);
    CHECK(of(&b, 0)->tenths == 0);
    a.n = 0;
    top_shares(&b, &a, 100);                                        /* a reading before with nothing in it */
    CHECK(of(&b, 3)->tenths == 400);
    top_free(&a);
    top_free(&b);
}

static void orders(void) {
    struct top_list l;
    int by;
    memset(&l, 0, sizeof(l));
    add(&l, task(9, 1, PROC_READY, PROC_USER, 0, "a", 0, 3));
    add(&l, task(2, 1, PROC_READY, PROC_USER, 0, "b", 0, 7));
    add(&l, task(5, 1, PROC_READY, PROC_USER, 0, "c", 0, 7));
    add(&l, task(7, 1, PROC_READY, PROC_USER, 0, "d", 0, 0));
    l.t[0].tenths = 10;
    l.t[1].tenths = 300;
    l.t[2].tenths = 10;
    l.t[3].tenths = 900;
    top_order(&l, TOP_BY_CPU);                                      /* the most first, pids in order among equals */
    CHECK(l.t[0].pi.pid == 7 && l.t[1].pi.pid == 2 && l.t[2].pi.pid == 5 && l.t[3].pi.pid == 9);
    top_order(&l, TOP_BY_MEM);
    CHECK(l.t[0].pi.pid == 2 && l.t[1].pi.pid == 5 && l.t[2].pi.pid == 9 && l.t[3].pi.pid == 7);
    top_order(&l, TOP_BY_PID);
    CHECK(l.t[0].pi.pid == 2 && l.t[1].pi.pid == 5 && l.t[2].pi.pid == 7 && l.t[3].pi.pid == 9);
    CHECK(top_order_name("cpu", &by) == 0 && by == TOP_BY_CPU);
    CHECK(top_order_name("mem", &by) == 0 && by == TOP_BY_MEM);
    CHECK(top_order_name("pid", &by) == 0 && by == TOP_BY_PID);
    by = 7;
    CHECK(top_order_name("name", &by) < 0 && top_order_name("", &by) < 0 && top_order_name("CPU", &by) < 0 && by == 7);
    top_free(&l);
}

static void text(void) {
    struct top_list l;
    struct sysinfo si;
    char row[TOP_ROW_MAX], tty[16], t[32];
    struct top_task one;
    memset(&l, 0, sizeof(l));
    memset(&si, 0, sizeof(si));
    si.hz = 100;
    si.ticks = 370000;                                              /* 1:01:40 */
    si.page_size = 4096;
    si.total_pages = 63488;
    si.free_pages = 60000;
    si.program_pages = 1000;
    si.heap_used = 2048 * 1024;
    add(&l, task(0, -1, PROC_RUNNING, 0, -1, "idle", 0, 0));
    add(&l, task(9, 1, PROC_READY, PROC_USER, 0, "a", 0, 0));
    add(&l, task(10, 1, PROC_STOPPED, PROC_USER, 0, "b", 0, 0));
    add(&l, task(11, 1, PROC_SLEEPING, PROC_USER, 0, "c", 0, 0));
    l.t[0].tenths = 968;
    CHECK(top_summary(&l, &si, row, sizeof(row), 512) > 0);
    CHECK(!strcmp(row, "top: up 1:01:40, 4 tasks, 2 running, 1 stopped, processor 3.2% busy"));
    CHECK(top_summary(&l, &si, row, sizeof(row), 15) == 15 && !strcmp(row, "top: up 1:01:40"));
    CHECK(top_memory(&si, row, sizeof(row), 512) > 0);
    CHECK(!strcmp(row, "Memory: 253952 KB, 240000 free, 4000 programs, 2048 kernel heap"));
    si.dying_pages = 25;
    CHECK(top_memory(&si, row, sizeof(row), 512) > 0);
    CHECK(!strcmp(row, "Memory: 253952 KB, 240000 free, 4000 programs, 2048 kernel heap, 100 being freed"));
    si.hz = 0;                                                      /* nothing the kernel says divides by zero here */
    si.page_size = 0;
    CHECK(top_summary(&l, &si, row, sizeof(row), 512) > 0 && top_memory(&si, row, sizeof(row), 512) > 0);
    si.hz = 100;
    si.page_size = 4096;
    top_free(&l);

    memset(&one, 0, sizeof(one));
    one.pi = task(1234, 1, PROC_SLEEPING, PROC_USER, PROC_TTY_PTY + 2, "surfsh", 366100, 1024);
    one.tenths = 125;
    CHECK(top_row(&one, &si, 1, row, sizeof(row), 512) > 0);
    /*                                PID  PPID S TTY    %CPU      TIME MEM (KB) NAME */
    CHECK(!strcmp(row, " 1234     1 S pty2   12.5   1:01:01     4096 surfsh"));
    CHECK(strlen(TOP_HEADING) == 49 && strstr(TOP_HEADING, "NAME") - TOP_HEADING == strstr(row, "surfsh") - row);
    one.pi.flags = 0;                                               /* a process that has not run yet is a program too */
    CHECK(top_row(&one, &si, 1, row, sizeof(row), 512) > 0 && strstr(row, " surfsh") && !strchr(row, '['));
    CHECK(strstr(TOP_HEADING, " TTY") - TOP_HEADING == strstr(row, " pty2") - row);
    one.pi = task(3, -1, PROC_BLOCKED, 0, -1, "fbflush", 7, 0);
    one.tenths = 1000;
    CHECK(top_row(&one, &si, 1, row, sizeof(row), 512) > 0);
    CHECK(!strcmp(row, "    3     - B -     100.0   0:00:00        - [fbflush]"));
    CHECK(top_row(&one, &si, 1, row, sizeof(row), 20) == 20 && !strcmp(row, "    3     - B -     "));
    one.pi = task(4, 1, PROC_RUNNING, PROC_USER, 0, "e\x1b[2J\xc3\xa9", 0, 1);
    CHECK(top_row(&one, &si, 1, row, sizeof(row), 512) > 0 && strstr(row, " e?[2J\xc3\xa9") && !strchr(row, 0x1b));
    CHECK(top_row(&one, &si, 0, row, sizeof(row), 512) > 0 && strstr(row, " e?[2J?") && !strstr(row, "\xc3"));
    memset(one.pi.name, 'x', sizeof(one.pi.name));                  /* no terminator of its own: the row still ends */
    CHECK(top_row(&one, &si, 1, row, sizeof(row), 512) > 0 && strstr(row, " xxxxxxxxxxxxxxxx") && !strstr(row, " xxxxxxxxxxxxxxxxx"));
    memcpy(one.pi.name, "abcdefghijklmn\xe2\x89", 16);              /* a name the kernel cut inside a character */
    CHECK(top_row(&one, &si, 1, row, sizeof(row), 512) > 0 && strstr(row, "abcdefghijklmn??"));

    CHECK(top_state(PROC_READY) == 'r' && top_state(PROC_RUNNING) == 'R' && top_state(PROC_BLOCKED) == 'B' &&
          top_state(PROC_SLEEPING) == 'S' && top_state(PROC_STOPPED) == 'T' && top_state(PROC_DEAD) == 'Z' && top_state(99) == '?');
    top_tty(0, tty, sizeof(tty));
    CHECK(!strcmp(tty, "F1"));
    top_tty(3, tty, sizeof(tty));
    CHECK(!strcmp(tty, "F4"));
    top_tty(PROC_TTY_PTY, tty, sizeof(tty));
    CHECK(!strcmp(tty, "pty0"));
    top_tty(-1, tty, sizeof(tty));
    CHECK(!strcmp(tty, "-"));
    top_hms(0, t, sizeof(t));
    CHECK(!strcmp(t, "0:00:00"));
    top_hms(359999, t, sizeof(t));
    CHECK(!strcmp(t, "99:59:59"));
    top_hms(4294967295u, t, sizeof(t));
    CHECK(!strcmp(t, "1193046:28:15"));

    CHECK(top_status(0, 20, 34, TOP_BY_CPU, row, sizeof(row), 512) > 0);
    CHECK(!strcmp(row, "tasks 1-20 of 34, by CPU   c m p: order   Up Down: move   q: end"));
    CHECK(top_status(14, 20, 34, TOP_BY_MEM, row, sizeof(row), 512) > 0 && !strncmp(row, "tasks 15-34 of 34, by memory ", 29));
    CHECK(top_status(0, 3, 3, TOP_BY_PID, row, sizeof(row), 512) > 0 && !strncmp(row, "tasks 1-3 of 3, by pid ", 23));
    CHECK(top_status(0, 0, 0, TOP_BY_PID, row, sizeof(row), 512) > 0 && !strncmp(row, "tasks 0-0 of 0,", 15));
    CHECK(top_status(0, 3, 3, TOP_BY_PID, row, sizeof(row), 10) == 10);

    CHECK(top_scroll(0, 10, 20) == 0);                              /* it all fits */
    CHECK(top_scroll(5, 10, 20) == 0);
    CHECK(top_scroll(-1, 30, 20) == 0);
    CHECK(top_scroll(9, 30, 20) == 9);
    CHECK(top_scroll(10, 30, 20) == 10);
    CHECK(top_scroll(11, 30, 20) == 10);                            /* no rows to spare at the end */
    CHECK(top_scroll(TOP_MAX_TASKS, 30, 20) == 10);
    CHECK(top_scroll(5, 30, 0) == 5);                               /* a screen with no list rows at all */
    CHECK(top_scroll(100, 30, 0) == 30);
}

/* text fit to show: valid UTF-8, no control character, no more than width characters */
static int fit(const char *s, int width) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = strlen(s), i = 0;
    int chars = 0;
    while(i < n) {
        int len = tv_utf8(p + i, n - i);
        if(len < 1 || (len == 1 && (p[i] < 0x20 || p[i] == 0x7f))) return 0;
        i += (size_t)len;
        chars++;
    }
    return chars <= width;
}

static unsigned int rnd_state = 12345;
static unsigned int rnd(void) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return rnd_state >> 8;
}

static void random_tasks(void) {
    int round, i, widths[] = { 16, 20, 40, 49, 79, 80, 120, 512 };
    for(round = 0; round < 3000; round++) {
        struct top_list l;
        struct sysinfo si;
        struct procinfo pi;
        char row[TOP_ROW_MAX];
        int n = (int)(rnd() % 40), width = widths[rnd() % 8];
        memset(&l, 0, sizeof(l));
        for(i = 0; i < (int)sizeof(si); i++) ((unsigned char *)&si)[i] = (unsigned char)rnd();
        for(i = 0; i < n; i++) {
            unsigned int k;
            for(k = 0; k < sizeof(pi); k++) ((unsigned char *)&pi)[k] = (unsigned char)rnd();
            pi.pid = (int)(rnd() % 30);                             /* some of them twice */
            top_add(&l, &pi);
        }
        top_tidy(&l);
        for(i = 1; i < l.n; i++) CHECK(l.t[i - 1].pi.pid < l.t[i].pi.pid);
        top_shares(&l, NULL, rnd());
        top_order(&l, (int)(rnd() % 3));
        top_summary(&l, &si, row, sizeof(row), width);
        CHECK(fit(row, width));
        top_memory(&si, row, sizeof(row), width);
        CHECK(fit(row, width));
        for(i = 0; i < l.n; i++) {
            top_row(&l.t[i], &si, (int)(rnd() & 1), row, sizeof(row), width);
            CHECK(l.t[i].tenths <= 1000 && fit(row, width));
        }
        top_status(0, l.n, l.n, (int)(rnd() % 3), row, sizeof(row), width);
        CHECK(fit(row, width));
        top_free(&l);
    }
}

int main(void) {
    seconds();
    keys();
    characters();
    rows();
    readings();
    orders();
    text();
    random_tasks();
    tv_forget();
    free(tv_out);
    printf("test_top: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
