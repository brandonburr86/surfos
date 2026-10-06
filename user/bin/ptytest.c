/*
SurfOS user program: ptytest
--------------------
File: ptytest.c  Date: 10/6/26
--------------------
Tests of the pseudo-terminals, poll(), the process list, detach() and /dev/kmsg
(include/surfos/pty.h, poll.h, proc.h), for the smoke tests (tools/qemu-run.py) and for
trying them by hand. Each mode ends with "PTYTEST <mode> PASS", or with "PTYTEST <mode>
FAIL: <why>" and exit status 1. What a master read is shown with its control characters
as \e \r \t \b \xNN.

ptytest run PROG [ARGS]   PROG (a path, or a name in /initrd/bin) on a new pty: what the
                          master read, then PROG's exit status
ptytest surfsh            surfsh on an 80x24 pty: "echo hello" typed (CR is Enter) and its
                          output read back; Ctrl+C cuts a kernel command short (sleep) and
                          kills a program (count); dmesg on a pty made tall enough for it,
                          its lines in the kernel's colour (an SGR sequence), with a line
                          this program wrote to /dev/kmsg; "exit": the master reads 0 then
ptytest size COLS ROWS    PTY_SETSIZE (sizes out of range refused), PTY_INDEX, then on the
                          pty `pagetest size` (TTY_GETSIZE) and `pagetest caps` (TTY_CAPS)
ptytest pager             `pagetest 60` on an 80x10 pty: --More-- after every 9 lines, each
                          answered with Space; lines 1-60 in order, then PAGETEST DONE
ptytest flood             `pagetest 1000` on an 80x1024 pty, read only after a second: the
                          program waits for room in the master's buffer, and every line
                          arrives, in order
ptytest hangup            `count 1000 100` on a pty; the master closed after the first
                          numbers: the process is gone at once (procinfo), killed, and the
                          pty is free again
ptytest hangup-call       the master closed 300 ms into `sleep -u 4000` (a kernel command
                          surfsh runs, which looks at nothing): no kill at once after the
                          kill's 2 s, the task ends with the command, and the kernel log
                          (dmesg through kcmd(), on a file of /hda1) says it waited
ptytest tcpecho [PORT]    `tcpecho PORT` (2323) in a session: its kernel thread is on F1,
                          the session ends with `exit`, the thread lives on, and nothing it
                          prints is in the session
ptytest reuse             a second session on the same master: line mode again (the first
                          set raw mode) and none of the keys written in between; the pty
                          opened again is 80x24
ptytest fullqueue         the master's write never waits: 300 keys for a program that is
                          not reading yet take a short count, one more EAGAIN, POLLOUT is
                          back once it reads; in a session, Ctrl+C gets through a full
                          queue, cuts sleep short, and what was typed ahead never runs
ptytest poll              poll(): a master with nothing to read (the timeout), with output
                          (POLLIN), after the end (POLLIN and POLLHUP); spawnpty of no
                          program (POLLHUP at once); a closed descriptor (POLLNVAL); a
                          negative one (skipped); too many (EINVAL); the console; and a kill
                          cuts a poll() with no timeout short at once
ptytest ps                procinfo(): every task
ptytest detach            a child that detaches: init is its parent, PROC_DETACHED, and
                          wait() finds it is no child of this process any more
ptytest kmsg              a line written to /dev/kmsg (the harness finds it with dmesg)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <surf.h>

#define KMSG_LINE "ptytest kmsg: a line for the kernel log"     /* the harness looks for it in dmesg */
#define PROMPT "\xe2\x89\x8b"                                      /* the wave in surfsh's prompt (UTF-8 on a pty) */

static char out[65536];                 /* what the master read in this mode */
static int outlen;
static int answered;                    /* --More-- questions answered */
static int answer_more;                 /* answer them with Space as they come */
static const char *mode = "";

static void show(int from);

static int fail(const char *why) {
    if(outlen) {                                        /* what the master read, to see why */
        printf("ptytest: the master read %d bytes:\n", outlen);
        show(outlen > 2048 ? outlen - 2048 : 0);
    }
    printf("PTYTEST %s FAIL: %s\n", mode, why);
    return 1;
}

static int pass(void) {
    printf("PTYTEST %s PASS\n", mode);
    return 0;
}

/* the master's output so far, from byte from on, with its control characters visible */
static void show(int from) {
    int i;
    for(i = from; i < outlen; i++) {
        unsigned char c = (unsigned char)out[i];
        if(c == '\n') putchar('\n');
        else if(c == 0x1b) fputs("\\e", stdout);
        else if(c == '\r') fputs("\\r", stdout);
        else if(c == '\t') fputs("\\t", stdout);
        else if(c == '\b') fputs("\\b", stdout);
        else if(c < 0x20 || c == 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
    if(outlen > from && out[outlen - 1] != '\n') putchar('\n');
}

/* where what was read goes on after text (found from byte from on); outlen when it is not there */
static int after(const char *text, int from) {
    const char *p = strstr(out + from, text);
    return p ? (int)(p - out) + (int)strlen(text) : outlen;
}

static int count_of(const char *s, int from) {
    int n = 0;
    const char *p = out + from;
    while((p = strstr(p, s)) != NULL) {
        n++;
        p += strlen(s);
    }
    return n;
}

/* Read the master until text appears in what was read from byte from on (text NULL: until the
   end), answering --More-- with Space when answer_more is set. 1: found (or the end, for NULL);
   0: the session ended first; -1: ms passed; -2: an error. */
static int read_until(int m, const char *text, int from, unsigned int ms) {
    unsigned int end = uptime() + ms;
    struct pollfd p;
    int n, left;
    for(;;) {
        if(text && strstr(out + from, text)) return 1;
        while(answer_more && count_of("--More--", 0) > answered) {
            answered++;
            if(write(m, " ", 1) != 1) return -2;
        }
        left = (int)(end - uptime());
        if(left <= 0) return -1;
        p.fd = m;
        p.events = POLLIN;
        p.revents = 0;
        n = poll(&p, 1, left);
        if(n < 0) return -2;
        if(n == 0) continue;
        if(outlen >= (int)sizeof(out) - 1) return -2;
        n = (int)read(m, out + outlen, sizeof(out) - 1 - outlen);
        if(n < 0) return -2;
        if(n == 0) return text ? (strstr(out + from, text) != NULL) : 1;
        outlen += n;
        out[outlen] = 0;
    }
}

static int type(int m, const char *s) {
    return write(m, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
}

/* a new pty with prog on it; the master descriptor, or -1 */
static int start(const char *prog, char *const argv[], unsigned int size, int *pid) {
    int m = openpty();
    if(m < 0) {
        perror("ptytest: openpty");
        return -1;
    }
    if(size && ptyctl(m, PTY_SETSIZE, size) < 0) {
        perror("ptytest: PTY_SETSIZE");
        close(m);
        return -1;
    }
    *pid = spawnpty(prog, argv, m);
    if(*pid < 0) {
        perror("ptytest: spawnpty");
        close(m);
        return -1;
    }
    return m;
}

/* the task with this pid, from procinfo(): 1 found, 0 not */
static int find_task(int pid, struct procinfo *pi) {
    int i;
    for(i = 0; procinfo(i, pi) == 0; i++) if(pi->pid == pid) return 1;
    return 0;
}

static int mode_run(int argc, char **argv) {
    char path[64];
    int m, pid, st = -99, r;
    if(argc < 3) return fail("usage: ptytest run PROG [ARGS]");
    if(strchr(argv[2], '/')) strlcpy(path, argv[2], sizeof(path));
    else snprintf(path, sizeof(path), "/initrd/bin/%s", argv[2]);      /* a program of the initrd by its name */
    m = start(path, argv + 2, 0, &pid);
    if(m < 0) return fail("no program on a pty");
    r = read_until(m, NULL, 0, 30000);
    close(m);
    wait(pid, &st);
    printf("ptytest: %d bytes read:\n", outlen);
    show(0);
    printf("ptytest: exit status %d\n", st);
    return r == 1 ? pass() : fail("the session did not end");
}

static int mode_surfsh(void) {
    char *argv[] = { "surfsh", NULL };
    char line[96];
    int m, pid, st = -99, at, fd;
    m = start("/initrd/bin/surfsh", argv, (80 << 16) | 24, &pid);
    if(m < 0) return fail("surfsh did not start");
    if(read_until(m, "\xe2\x89\x8b", 0, 15000) != 1) return fail("no prompt");
    at = after("\xe2\x89\x8b", 0);
    type(m, "echo hello\r");
    if(read_until(m, "\nhello\n", at, 10000) != 1) return fail("echo hello did not answer hello");
    at = after("\nhello\n", at);
    if(read_until(m, "\xe2\x89\x8b", at, 10000) != 1) return fail("no prompt after echo");
    at = after("\xe2\x89\x8b", at);
    printf("ptytest: typed \"echo hello\", read back \"hello\"\n");

    /* Ctrl+C: a kernel command surfsh runs is cut short, a program it waits for is killed */
    type(m, "sleep 10000\r");
    if(read_until(m, "sleep 10000", at, 10000) != 1) return fail("sleep 10000 was not echoed");
    msleep(300);
    type(m, "\003");
    if(read_until(m, "sleep cut short", at, 10000) != 1) return fail("Ctrl+C did not cut sleep short");
    at = after("sleep cut short", at);
    if(read_until(m, "\xe2\x89\x8b", at, 10000) != 1) return fail("no prompt after sleep");
    at = after("\xe2\x89\x8b", at);
    type(m, "count 100 100\r");
    if(read_until(m, " 3 ", at, 10000) != 1) return fail("count printed nothing");
    type(m, "\003");
    if(read_until(m, "killed]", at, 10000) != 1) return fail("Ctrl+C did not kill count");
    at = after("killed]", at);
    if(read_until(m, "\xe2\x89\x8b", at, 10000) != 1) return fail("no prompt after count");
    at = after("\xe2\x89\x8b", at);
    printf("ptytest: Ctrl+C: sleep cut short, count killed, surfsh lives on\n");

    /* dmesg in one screenful: the pty as tall as it may be */
    snprintf(line, sizeof(line), "ptytest surfsh: a line for the kernel log (pid %d)\n", getpid());
    fd = open("/dev/kmsg", O_WRONLY);
    if(fd < 0 || write(fd, line, strlen(line)) != (ssize_t)strlen(line)) return fail("cannot write /dev/kmsg");
    close(fd);
    if(ptyctl(m, PTY_SETSIZE, (80 << 16) | 1024) < 0) return fail("PTY_SETSIZE 80x1024");
    type(m, "dmesg\r");
    line[strlen(line) - 1] = 0;
    if(read_until(m, line, at, 30000) != 1) return fail("dmesg did not show the line written to /dev/kmsg");
    if(!strstr(out + at, "\033[0;1;33m")) return fail("dmesg's lines came without the kernel's colour (ESC[0;1;33m)");
    printf("ptytest: dmesg: %d bytes, in the kernel's colour (ESC[0;1;33m), with the /dev/kmsg line\n",
           after(line, at) - at);
    at = after(line, at);
    if(read_until(m, "\xe2\x89\x8b", at, 10000) != 1) return fail("no prompt after dmesg");
    at = after("\xe2\x89\x8b", at);
    type(m, "exit\r");
    if(read_until(m, NULL, at, 10000) != 1) return fail("the master did not read 0 after exit");
    close(m);
    if(wait(pid, &st) != pid) return fail("surfsh was not collected");
    printf("ptytest: exit typed: the master read 0; surfsh's exit status %d\n", st);
    return st == 0 ? pass() : fail("surfsh's exit status");
}

static int mode_size(int argc, char **argv) {
    char *size_argv[] = { "pagetest", "size", NULL }, *caps_argv[] = { "pagetest", "caps", NULL };
    char want[48];
    int cols = argc > 2 ? atoi(argv[2]) : 100, rows = argc > 3 ? atoi(argv[3]) : 30;
    int m, pid, st = -99, i;
    static const unsigned int bad[] = { 0, (7 << 16) | 24, (80 << 16) | 1, (1025 << 16) | 24, (80 << 16) | 1025 };
    m = openpty();
    if(m < 0) return fail("openpty");
    for(i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        if(ptyctl(m, PTY_SETSIZE, bad[i]) != -1 || errno != EINVAL) return fail("a size out of range was taken");
    }
    printf("ptytest: sizes out of range refused (EINVAL): 0x0, 7x24, 80x1, 1025x24, 80x1025\n");
    if(ptyctl(m, PTY_SETSIZE, ((unsigned int)cols << 16) | (unsigned int)rows) < 0) return fail("PTY_SETSIZE");
    printf("ptytest: pty %d is %dx%d\n", ptyctl(m, PTY_INDEX, 0), cols, rows);
    if(ptyctl(m, 99, 0) != -1 || errno != EINVAL) return fail("an unknown ptyctl() was taken");
    if(ptyctl(1, PTY_INDEX, 0) != -1 || errno != EINVAL) return fail("ptyctl() on the console was taken");
    pid = spawnpty("/initrd/bin/pagetest", size_argv, m);
    if(pid < 0) return fail("spawnpty pagetest size");
    if(read_until(m, NULL, 0, 10000) != 1) return fail("pagetest size did not end");
    wait(pid, &st);
    pid = spawnpty("/initrd/bin/pagetest", caps_argv, m);          /* a second session on the same pty */
    if(pid < 0) return fail("spawnpty pagetest caps");
    if(read_until(m, NULL, 0, 10000) != 1) return fail("pagetest caps did not end");
    wait(pid, &st);
    close(m);
    show(0);
    snprintf(want, sizeof(want), "TTY_GETSIZE %dx%d\n", cols, rows);
    if(!strstr(out, want)) return fail("TTY_GETSIZE on the pty is not PTY_SETSIZE's size");
    if(!strstr(out, "TTY_CAPS 3: UTF-8 colour")) return fail("TTY_CAPS on the pty is not UTF-8 and colour");
    return pass();
}

/* the question as the stream carries it (console.c), and how it is taken away */
#define QUESTION "\033[7m--More-- (Space: next page, Enter: next line, q: skip the rest)\033[27m"
#define UNASKED  "\r\033[K"

static int mode_pager(void) {
    char *argv[] = { "pagetest", "60", NULL }, num[8];
    int m, pid, st = -99, i, first = 1;
    const char *p, *q;
    m = start("/initrd/bin/pagetest", argv, (80 << 16) | 10, &pid);
    if(m < 0) return fail("pagetest did not start");
    answer_more = 1;
    if(read_until(m, NULL, 0, 30000) != 1) return fail("pagetest 60 did not end");
    close(m);
    wait(pid, &st);
    printf("ptytest: %d questions in the stream, each answered with Space; the lines between them:", answered);
    for(i = 1, p = out; i <= 60; i++) {                 /* every line, in order, 9 between questions */
        snprintf(num, sizeof(num), "%03d\n", i);
        p = strstr(p, num);
        if(!p) {
            printf("\n");
            return fail("a line is missing");
        }
        q = p + 4;
        if(!strncmp(q, QUESTION UNASKED, strlen(QUESTION UNASKED)) || i == 60) {
            printf(" %d-%d", first, i);
            first = i + 1;
        }
    }
    printf("\n");
    if(count_of(QUESTION, 0) != 6 || count_of(UNASKED, 0) != 6) return fail("not 6 questions, in inverse video, taken away");
    for(i = 9, p = out; i < 60; i += 9) {               /* each right after a screenful of 9 lines */
        snprintf(num, sizeof(num), "%03d\n", i);
        p = strstr(p, num);
        if(!p || strncmp(p + 4, QUESTION UNASKED, strlen(QUESTION UNASKED))) return fail("no question after a screenful of 9");
    }
    if(!strstr(out, "PAGETEST DONE")) return fail("no PAGETEST DONE");
    return st == 0 ? pass() : fail("pagetest's exit status");
}

static int mode_flood(void) {
    char *argv[] = { "pagetest", "1000", NULL }, num[8];
    int m, pid, st = -99, i;
    const char *p;
    m = start("/initrd/bin/pagetest", argv, (80 << 16) | 1024, &pid);
    if(m < 0) return fail("pagetest did not start");
    msleep(1000);                                       /* the program fills the buffer and waits */
    if(read_until(m, NULL, 0, 30000) != 1) return fail("pagetest 1000 did not end");
    close(m);
    wait(pid, &st);
    printf("ptytest: %d bytes read after a second's wait\n", outlen);
    for(i = 1, p = out; i <= 1000; i++) {
        snprintf(num, sizeof(num), "%03d\n", i % 1000);
        p = strstr(p, i < 1000 ? num : "line 1000\n");
        if(!p) return fail("a line is missing");
    }
    if(!strstr(out, "PAGETEST DONE")) return fail("no PAGETEST DONE");
    printf("ptytest: lines 1-1000, all of them, in order\n");
    return st == 0 ? pass() : fail("pagetest's exit status");
}

static int mode_hangup(void) {
    char *argv[] = { "count", "1000", "100", NULL };
    struct procinfo pi;
    unsigned int t0;
    int m, pid, st = -99, n, gone = 0;
    m = start("/initrd/bin/count", argv, 0, &pid);
    if(m < 0) return fail("count did not start");
    n = ptyctl(m, PTY_INDEX, 0);
    if(read_until(m, " 3 ", 0, 10000) != 1) return fail("count printed nothing");
    if(!find_task(pid, &pi) || pi.state == PROC_DEAD || pi.tty != PROC_TTY_PTY + n)
        return fail("count is not running on the pty");
    printf("ptytest: count (pid %d) runs on pty %d (tty %d); the master is closed\n", pid, n, pi.tty);
    t0 = uptime();
    close(m);
    while(uptime() - t0 < 3000) {
        if(!find_task(pid, &pi) || pi.state == PROC_DEAD) {
            gone = 1;
            break;
        }
        msleep(10);
    }
    if(!gone) return fail("count still runs 3 s after the hangup");
    printf("ptytest: count is gone %u ms after the hangup\n", uptime() - t0);
    if(wait(pid, &st) != pid || st != -1) return fail("count was not killed");
    printf("ptytest: count's exit status %d (killed)\n", st);
    m = openpty();
    if(m < 0 || ptyctl(m, PTY_INDEX, 0) != n) return fail("the pty is not free again");
    close(m);
    printf("ptytest: pty %d is free again\n", n);
    return pass();
}

/* the kernel log, as dmesg prints it (through kcmd(), its output on a file of /hda1), into out; its length or -1 */
static int kernel_log(void) {
    char *argv[] = { "dmesg", NULL };
    const char *file = "/hda1/PTYKLOG.TXT";
    int fd, n;
    fflush(stdout);
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC);
    if(fd < 0) return -1;
    if(dup2(1, 9) < 0) {                                /* the console, kept aside */
        close(fd);
        return -1;
    }
    dup2(fd, 1);
    kcmd(argv);
    dup2(9, 1);
    close(9);
    close(fd);
    fd = open(file, O_RDONLY);
    if(fd < 0) return -1;
    outlen = 0;
    while(outlen < (int)sizeof(out) - 1 && (n = (int)read(fd, out + outlen, sizeof(out) - 1 - outlen)) > 0) outlen += n;
    out[outlen] = 0;
    close(fd);
    unlink(file);
    return outlen;
}

/* the hangup during a kernel command that looks at nothing for 4 s (sleep -u): no kill at once after
   the kill's 2 s, the command ends first, then the task; the kernel log says it waited */
static int mode_hangup_call(void) {
    char *argv[] = { "surfsh", NULL };
    char want[64];
    struct procinfo pi;
    unsigned int t0, alive = 0, took;
    int m, pid, st = -99, at, gone = 0;
    m = start("/initrd/bin/surfsh", argv, (80 << 16) | 1024, &pid);
    if(m < 0) return fail("surfsh did not start");
    if(read_until(m, PROMPT, 0, 15000) != 1) return fail("no prompt");
    at = after(PROMPT, 0);
    type(m, "sleep -u 4000\r");
    if(read_until(m, "sleep -u 4000", at, 10000) != 1) return fail("sleep -u 4000 was not typed");
    msleep(300);
    t0 = uptime();
    close(m);                                           /* the hangup, 300 ms into 4 s that look at nothing */
    while(uptime() - t0 < 8000) {
        if(!find_task(pid, &pi) || pi.state == PROC_DEAD) {
            gone = 1;
            break;
        }
        alive = uptime() - t0;
        msleep(20);
    }
    took = uptime() - t0;
    if(!gone) return fail("surfsh still runs 8 s after the hangup");
    printf("ptytest: surfsh, in `sleep -u 4000` at the hangup, still there %u ms after it, gone after %u ms\n", alive, took);
    if(alive < 2500) return fail("surfsh was killed in its kernel command (the kill's 2 s)");
    if(wait(pid, &st) != pid || st != -1) return fail("surfsh was not killed at the end of its call");
    if(kernel_log() <= 0) return fail("cannot read the kernel log");
    snprintf(want, sizeof(want), "(pid %d) is still in its kernel command", pid);
    if(!strstr(out, want)) return fail("the kernel log does not say that the hangup's kill waits");
    snprintf(want, sizeof(want), "(pid %d) did not end its", pid);
    if(strstr(out, want)) return fail("the kernel log has a kill at once");
    printf("ptytest: the kernel log says the kill waited for the command; no kill at once\n");
    return pass();
}

/* a kernel thread started in a session (tcpecho) is not part of it: on F1, the session ends with
   exit, the thread lives on after the hangup, and nothing it prints is in the session */
static int mode_tcpecho(int argc, char **argv) {
    char *sargv[] = { "surfsh", NULL };
    char cmd[32];
    struct procinfo pi;
    int before[64], nbefore = 0, port = argc > 2 ? atoi(argv[2]) : 2323;
    int m, pid, st = -99, at, i, k, tpid = -1;
    unsigned int t0;
    for(i = 0; procinfo(i, &pi) == 0; i++)
        if(!strcmp(pi.name, "tcpecho") && nbefore < 64) before[nbefore++] = pi.pid;
    m = start("/initrd/bin/surfsh", sargv, (80 << 16) | 24, &pid);
    if(m < 0) return fail("surfsh did not start");
    if(read_until(m, PROMPT, 0, 15000) != 1) return fail("no prompt");
    at = after(PROMPT, 0);
    snprintf(cmd, sizeof(cmd), "tcpecho %d\r", port);
    type(m, cmd);
    if(read_until(m, "tcpecho server started", at, 10000) != 1) return fail("tcpecho did not start");
    at = after("tcpecho server started", at);
    if(read_until(m, PROMPT, at, 10000) != 1) return fail("no prompt after tcpecho");
    at = after(PROMPT, at);
    for(i = 0; procinfo(i, &pi) == 0; i++) {
        if(strcmp(pi.name, "tcpecho") || pi.state == PROC_DEAD) continue;
        for(k = 0; k < nbefore && before[k] != pi.pid; k++);
        if(k == nbefore) {
            tpid = pi.pid;
            break;
        }
    }
    if(tpid < 0) return fail("no new tcpecho task");
    if(pi.tty != 0) return fail("the tcpecho thread is on the session's terminal, not on F1");
    printf("ptytest: tcpecho (pid %d), started in the session, runs on F1 (tty %d)\n", tpid, pi.tty);
    type(m, "exit\r");
    t0 = uptime();
    if(read_until(m, NULL, at, 5000) != 1) return fail("the master did not read 0 after exit");
    printf("ptytest: exit: the master read 0 after %u ms\n", uptime() - t0);
    if(strstr(out, "tcpecho: listening")) return fail("what the thread printed is in the session");
    close(m);
    wait(pid, &st);
    msleep(200);
    if(!find_task(tpid, &pi) || pi.state == PROC_DEAD) return fail("the thread did not live on");
    printf("ptytest: the session's hangup is over; tcpecho (pid %d) lives on\n", tpid);
    return pass();
}

static int mode_poll(void) {
    char *argv[] = { "hello", "polled", NULL }, *child[] = { "ptytest", "pollwait", NULL };
    struct pollfd p[POLL_MAX + 1];
    unsigned int t0, took;
    int m, pid, st = -99, r;
    m = openpty();
    if(m < 0) return fail("openpty");
    p[0].fd = m;
    p[0].events = POLLIN;
    t0 = uptime();
    r = poll(p, 1, 300);
    took = uptime() - t0;
    if(r != 0 || took < 250 || took > 2000) return fail("a master with nothing to read did not time out after 300 ms");
    printf("ptytest: nothing to read: poll() timed out after %u ms\n", took);
    p[0].fd = 15;                                       /* not open */
    p[0].events = POLLIN;
    p[1].fd = -1;                                       /* skipped */
    p[1].events = POLLIN;
    p[1].revents = 77;
    if(poll(p, 2, 0) != 1 || p[0].revents != POLLNVAL || p[1].revents != 0) return fail("POLLNVAL, or a negative fd");
    printf("ptytest: a closed descriptor: POLLNVAL; a negative one: skipped\n");
    if(poll(p, POLL_MAX + 1, 0) != -1 || errno != EINVAL) return fail("more than POLL_MAX descriptors were taken");
    if(spawnpty("/initrd/bin/no-such-program", NULL, m) != -1 || errno != ENOENT) return fail("spawnpty of nothing");
    p[0].fd = m;
    p[0].events = POLLIN;
    t0 = uptime();
    if(poll(p, 1, 2000) != 1 || p[0].revents != (POLLIN | POLLHUP) || uptime() - t0 > 500 || read(m, out, 1) != 0)
        return fail("a session that could not start is not over at once");
    printf("ptytest: spawnpty of no program: ENOENT, and POLLIN | POLLHUP at once\n");
    close(m);
    m = openpty();
    if(m < 0) return fail("openpty");
    pid = spawnpty("/initrd/bin/hello", argv, m);
    if(pid < 0) return fail("spawnpty hello");
    p[0].fd = m;
    p[0].events = POLLIN;
    if(poll(p, 1, 5000) != 1 || !(p[0].revents & POLLIN)) return fail("no POLLIN when the session printed");
    printf("ptytest: the session printed: revents 0x%x\n", p[0].revents);
    if(read_until(m, NULL, 0, 10000) != 1) return fail("the session did not end");
    wait(pid, &st);
    p[0].fd = m;
    p[0].events = POLLIN;
    if(poll(p, 1, 1000) != 1 || p[0].revents != (POLLIN | POLLHUP)) return fail("no POLLIN | POLLHUP after the end");
    printf("ptytest: after the end and the last read: revents 0x%x (POLLIN | POLLHUP)\n", p[0].revents);
    if(read(m, out, 1) != 0) return fail("a read after the end did not return 0");
    close(m);
    p[0].fd = 1;                                        /* the console: writes are always taken */
    p[0].events = POLLOUT;
    if(poll(p, 1, 0) != 1 || p[0].revents != POLLOUT) return fail("the console is not ready for writing");
    printf("ptytest: the console: POLLOUT\n");
    /* a kill ends a poll() with no timeout at once (EINTR), not after the kill's 2 s */
    pid = spawn("/initrd/bin/ptytest", child);
    if(pid < 0) return fail("spawn ptytest pollwait");
    msleep(300);
    t0 = uptime();
    kill(pid);
    if(wait(pid, &st) != pid) return fail("pollwait was not collected");
    took = uptime() - t0;
    printf("ptytest: a process in poll() with no timeout, killed: gone after %u ms\n", took);
    if(took > 1000) return fail("the kill did not cut poll() short");
    return pass();
}

/* a second session on the same master starts clean: the keys written after the first one ended
   are gone, and the first one's raw mode with it; a pty opened again is 80x24 */
static int mode_reuse(void) {
    char *raw[] = { "ptytest", "rawexit", NULL }, *ask[] = { "pagetest", "ask", "0", NULL };
    char *size[] = { "pagetest", "size", NULL };
    int m, pid, st, n;
    m = openpty();
    if(m < 0) return fail("openpty");
    n = ptyctl(m, PTY_INDEX, 0);
    ptyctl(m, PTY_SETSIZE, (100 << 16) | 30);
    pid = spawnpty("/initrd/bin/ptytest", raw, m);
    if(pid < 0 || read_until(m, NULL, 0, 5000) != 1) return fail("the first session");
    wait(pid, &st);
    type(m, "stale\r");                                 /* after its end: nobody's keys */
    outlen = 0;
    out[0] = 0;
    pid = spawnpty("/initrd/bin/pagetest", ask, m);
    if(pid < 0 || read_until(m, "pagetest question: ", 0, 5000) != 1) return fail("the second session asks nothing");
    type(m, "fresh\r");
    if(read_until(m, NULL, 0, 5000) != 1) return fail("the second session did not end (still raw?)");
    wait(pid, &st);
    if(!strstr(out, "pagetest answer: fresh\n")) return fail("the second session read stale keys");
    printf("ptytest: a second session on the same master: line mode, and only its own keys\n");
    close(m);
    m = openpty();
    if(m < 0 || ptyctl(m, PTY_INDEX, 0) != n) return fail("pty not free again");
    outlen = 0;
    out[0] = 0;
    pid = spawnpty("/initrd/bin/pagetest", size, m);
    if(pid < 0 || read_until(m, NULL, 0, 5000) != 1) return fail("pagetest size");
    wait(pid, &st);
    close(m);
    if(!strstr(out, "TTY_GETSIZE 80x24")) return fail("a pty opened again is not 80x24");
    printf("ptytest: pty %d opened again: 80x24\n", n);
    return pass();
}

/* the master's write never waits (a server polling many masters must not stall on one session) */
static int mode_fullqueue(void) {
    char *reader[] = { "ptytest", "slowreader", "1500", NULL }, *sargv[] = { "surfsh", NULL };
    char keys[300];
    struct pollfd p;
    unsigned int t0, took;
    int m, pid, st = -99, r, at;
    memset(keys, 'x', sizeof(keys));
    m = start("/initrd/bin/ptytest", reader, 0, &pid);
    if(m < 0) return fail("slowreader did not start");
    msleep(200);                                        /* it sleeps 1.5 s before it reads */
    r = (int)write(m, keys, sizeof(keys));
    if(r <= 0 || r >= (int)sizeof(keys)) return fail("300 keys for a program not reading: not a short count");
    printf("ptytest: 300 keys written, %d taken: the input queue is full\n", r);
    p.fd = m;
    p.events = POLLOUT;
    p.revents = 0;
    if(poll(&p, 1, 0) != 0) return fail("POLLOUT with the queue full");
    if(write(m, "y", 1) != -1 || errno != EAGAIN) return fail("a key into the full queue: no EAGAIN");
    printf("ptytest: one more: EAGAIN, and no POLLOUT\n");
    t0 = uptime();
    if(poll(&p, 1, 5000) != 1 || !(p.revents & POLLOUT)) return fail("POLLOUT did not come back when the program read");
    took = uptime() - t0;
    printf("ptytest: POLLOUT came back after %u ms, when the program began to read\n", took);
    if(took < 800) return fail("POLLOUT came back before the program read");
    if(write(m, "\r", 1) != 1) return fail("Enter after room came back");
    if(read_until(m, "slowreader: a line of", 0, 5000) != 1 || read_until(m, NULL, 0, 5000) != 1)
        return fail("the program did not get its line");
    close(m);
    wait(pid, &st);

    /* Ctrl+C through a full queue, in a session; what was typed ahead never runs */
    outlen = 0;
    out[0] = 0;
    m = start("/initrd/bin/surfsh", sargv, (80 << 16) | 1024, &pid);
    if(m < 0 || read_until(m, PROMPT, 0, 15000) != 1) return fail("no prompt");
    at = after(PROMPT, 0);
    type(m, "sleep 5000\r");
    if(read_until(m, "sleep 5000", at, 10000) != 1) return fail("sleep 5000 was not typed");
    msleep(300);
    type(m, "echo TYPED-AHEAD\r");                       /* typed ahead while sleep runs */
    r = (int)write(m, keys, sizeof(keys));
    if(r <= 0 || r >= (int)sizeof(keys) || poll(&p, 1, 0) != 0) return fail("the session's queue did not fill");
    if(write(m, "\003", 1) != 1) return fail("Ctrl+C did not get through the full queue");
    if(read_until(m, "sleep cut short", at, 10000) != 1) return fail("Ctrl+C did not cut sleep short");
    at = after("sleep cut short", at);
    if(read_until(m, PROMPT, at, 10000) != 1) return fail("no prompt after Ctrl+C");
    read_until(m, "TYPED-AHEAD", at, 700);              /* nothing more should come */
    if(strstr(out + at, "TYPED-AHEAD") || strstr(out + at, "xxxx")) return fail("what was typed ahead ran after Ctrl+C");
    printf("ptytest: Ctrl+C through the full queue cut sleep short, and what was typed ahead never ran\n");
    type(m, "exit\r");
    read_until(m, NULL, 0, 5000);
    close(m);
    wait(pid, &st);
    return pass();
}

/* ptytest fullqueue's program: it sleeps, then reads one line and says how long it was */
static int mode_slowreader(int argc, char **argv) {
    char line[512];
    msleep((unsigned int)(argc > 2 ? atoi(argv[2]) : 1500));
    if(!fgets(line, sizeof(line), stdin)) return 1;
    printf("slowreader: a line of %u bytes\n", (unsigned int)strlen(line));
    return 0;
}

/* ptytest reuse's first session: raw mode on, and gone */
static int mode_rawexit(void) {
    return ttyctl(TTY_RAW, 1) < 0;
}

/* ptytest poll's child: a poll() on a fresh pty that nothing ends (but a kill) */
static int mode_pollwait(void) {
    struct pollfd p;
    p.fd = openpty();
    p.events = POLLIN;
    poll(&p, 1, -1);
    return 0;
}

static const char *state_name(int s) {
    static const char *names[] = { "ready", "running", "blocked", "sleeping", "dead" };
    return s >= 0 && s <= 4 ? names[s] : "?";
}

static int mode_ps(void) {
    struct procinfo pi;
    char tty[8];
    int i, me = 0, init = 0, idle = 0;
    printf("  PID  PPID  STATE     TTY    FLAGS  NAME\n");
    for(i = 0; procinfo(i, &pi) == 0; i++) {
        if(pi.tty < 0) strcpy(tty, "-");
        else if(pi.tty >= PROC_TTY_PTY) snprintf(tty, sizeof(tty), "pty%d", pi.tty - PROC_TTY_PTY);
        else snprintf(tty, sizeof(tty), "F%d", pi.tty + 1);
        printf("%5d %5d  %-8s  %-5s  %s%s%s  %s\n", pi.pid, pi.ppid, state_name(pi.state), tty,
               (pi.flags & PROC_USER) ? "u" : "-", (pi.flags & PROC_DETACHED) ? "d" : "-",
               (pi.flags & PROC_SHELL) ? "s" : "-", pi.name);
        if(pi.pid == getpid() && (pi.flags & PROC_USER) && pi.state == PROC_RUNNING && !strcmp(pi.name, "ptytest")) me = 1;
        if(pi.pid == 1 && pi.ppid == -1 && !strcmp(pi.name, "init")) init = 1;
        if(pi.pid == 0 && pi.ppid == -1) idle = 1;
    }
    if(errno != ENOENT) return fail("the list did not end with ENOENT");
    printf("ptytest: %d tasks\n", i);
    if(procinfo(-1, &pi) != -1 || errno != EINVAL) return fail("a negative index was taken");
    if(!me || !init || !idle) return fail("the idle task, init or this process is missing, or wrong");
    return pass();
}

static int mode_detach(void) {
    char *child[] = { "ptytest", "detach-child", NULL };
    struct procinfo pi;
    unsigned int t0 = uptime();
    int pid, st, ok = 0;
    pid = spawn("/initrd/bin/ptytest", child);
    if(pid < 0) return fail("spawn ptytest detach-child");
    while(uptime() - t0 < 3000) {
        if(find_task(pid, &pi) && pi.ppid == 1 && (pi.flags & PROC_DETACHED)) {
            ok = 1;
            break;
        }
        msleep(20);
    }
    if(!ok) return fail("the child did not become init's, detached");
    printf("ptytest: the child (pid %d) detached: parent %d, flags 0x%x\n", pid, pi.ppid, pi.flags);
    if(wait(pid, &st) != -1 || errno != ECHILD) return fail("wait() still found the child");
    printf("ptytest: wait() for it: ECHILD, it is init's now\n");
    if(waitpid(-1, &st, WNOHANG) != -1 || errno != ECHILD) return fail("waitpid(-1) found a child");
    return pass();
}

/* ptytest detach's child: it leaves its parent and lives on a moment; init collects it */
static int mode_detach_child(void) {
    if(detach() < 0) {
        perror("ptytest: detach");
        return 1;
    }
    msleep(1500);
    return 0;
}

static int mode_kmsg(void) {
    int fd = open("/dev/kmsg", O_WRONLY);
    if(fd < 0) return fail("cannot open /dev/kmsg");
    if(write(fd, KMSG_LINE "\n", sizeof(KMSG_LINE)) != (ssize_t)sizeof(KMSG_LINE)) return fail("cannot write /dev/kmsg");
    close(fd);
    printf("ptytest: one line written to /dev/kmsg\n");
    return pass();
}

int main(int argc, char **argv) {
    mode = argc > 1 ? argv[1] : "";
    if(!strcmp(mode, "run")) return mode_run(argc, argv);
    if(!strcmp(mode, "surfsh")) return mode_surfsh();
    if(!strcmp(mode, "size")) return mode_size(argc, argv);
    if(!strcmp(mode, "pager")) return mode_pager();
    if(!strcmp(mode, "hangup")) return mode_hangup();
    if(!strcmp(mode, "hangup-call")) return mode_hangup_call();
    if(!strcmp(mode, "tcpecho")) return mode_tcpecho(argc, argv);
    if(!strcmp(mode, "reuse")) return mode_reuse();
    if(!strcmp(mode, "fullqueue")) return mode_fullqueue();
    if(!strcmp(mode, "slowreader")) return mode_slowreader(argc, argv);
    if(!strcmp(mode, "rawexit")) return mode_rawexit();
    if(!strcmp(mode, "flood")) return mode_flood();
    if(!strcmp(mode, "poll")) return mode_poll();
    if(!strcmp(mode, "pollwait")) return mode_pollwait();
    if(!strcmp(mode, "ps")) return mode_ps();
    if(!strcmp(mode, "detach")) return mode_detach();
    if(!strcmp(mode, "detach-child")) return mode_detach_child();
    if(!strcmp(mode, "kmsg")) return mode_kmsg();
    fprintf(stderr, "usage: ptytest run PROG [ARGS] | surfsh | size [COLS ROWS] | pager | flood | hangup | hangup-call |\n"
                    "       tcpecho [PORT] | reuse | fullqueue | poll | ps | detach | kmsg\n");
    return 2;
}
