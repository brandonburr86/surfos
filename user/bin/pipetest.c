/*
SurfOS user program: pipetest
--------------------
File: pipetest.c  Date: 10/24/26
--------------------
Tests of the kernel's pipes (include/surfos/pipe.h) from user space, for the smoke test
(tools/qemu-run.py) and for trying them by hand. Each mode ends with "PIPETEST <mode>
PASS", or with "PIPETEST <mode> FAIL: <why>" and exit status 1. The children it needs
are this program again, with a word of its own (below the list).

pipetest basic       a pipe's two ends: bytes through it as written, the end of the input
                     once the writing end is closed, EBADF for the wrong direction, ESPIPE
                     for lseek, fstat's "pipefs", isatty 0, EFAULT for a bad array
pipetest stream [N]  N bytes (262144) from a child through a pipe, in pieces of every
                     size, in order and whole; then N bytes the other way in one write()
pipetest full        a full pipe: PIPE_SIZE bytes go in without a wait, POLLOUT only once
                     PIPE_BUF bytes fit again; a writer waits until a slow reader reads
pipetest atomic      two children write 64 blocks of PIPE_BUF bytes each into one pipe:
                     the reader gets every block whole, never two mixed
pipetest broken      a child writing to a pipe nobody reads any more ends with 141 (the
                     default); one that asked for the error (broken_pipe_exit(0)) gets EPIPE
pipetest poll        POLLIN, POLLHUP, POLLOUT, POLLERR as surfos/pipe.h says, and a poll()
                     that waits for a child's write
pipetest limits      EMFILE without two free descriptors; ENFILE at PIPE_MAX pipes (five
                     children hold 30, this program takes the last 2); 200 pipes made and
                     closed, and PIPE_MAX are there again
pipetest kill        a child waiting to read an empty pipe, and one waiting for room in a
                     full one, are killed at once (the kill cuts the wait short)
pipetest share       a writing end kept open by dup2() or by a child holds the end of the
                     input back until the last descriptor of it is closed
pipetest all         every mode above, in that order
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <surf.h>

#define SELF "/initrd/bin/pipetest"

static const char *mode = "";

static int fail(const char *why) {
    printf("PIPETEST %s FAIL: %s (errno %d: %s)\n", mode, why, errno, strerror(errno));
    return 1;
}

static int pass(void) {
    printf("PIPETEST %s PASS\n", mode);
    return 0;
}

/* byte i of the test stream: no period a pipe's size divides */
static unsigned char pat(unsigned long i) {
    return (unsigned char)(i * 131 + (i >> 9) * 7 + 1);
}

/* this program as a child with descriptors 0 and 1 (-1: the console); its pid, or -1 */
static int child(char *const argv[], int in, int out) {
    int fds[3];
    fds[0] = in;
    fds[1] = out;
    fds[2] = -1;
    return spawnfd(SELF, argv, fds);
}

static int collect(int pid) {
    int status = -999;
    if(waitpid(pid, &status, 0) != pid) return -999;
    return status;
}

/* events of one descriptor right now */
static int ready(int fd, int events) {
    struct pollfd p;
    p.fd = fd;
    p.events = (short)events;
    p.revents = 0;
    if(poll(&p, 1, 0) < 0) return -1;
    return p.revents;
}

/**** the children ****/

/* writer N: N bytes of the stream on descriptor 1, in pieces of 1 to 5000 bytes */
static int c_writer(unsigned long n) {
    static unsigned char buf[5000];
    unsigned long done = 0, k, i;
    unsigned int step = 1;
    while(done < n) {
        k = step < n - done ? step : n - done;
        for(i = 0; i < k; i++) buf[i] = pat(done + i);
        if(write(1, buf, k) != (ssize_t)k) return 1;
        done += k;
        step = step * 7 % 4999 + 1;
    }
    return 0;
}

/* count N: reads descriptor 0 to its end: 0 when it was exactly N bytes of the stream */
static int c_count(unsigned long n) {
    static unsigned char buf[3000];
    unsigned long got = 0;
    ssize_t r, i;
    while((r = read(0, buf, sizeof(buf))) > 0) {
        for(i = 0; i < r; i++) if(buf[i] != pat(got + (unsigned long)i)) return 2;
        got += (unsigned long)r;
    }
    return r == 0 && got == n ? 0 : 1;
}

/* blocks L K: K writes of PIPE_BUF bytes of the letter L each */
static int c_blocks(char letter, int k) {
    static char buf[PIPE_BUF];
    memset(buf, letter, sizeof(buf));
    while(k-- > 0) if(write(1, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) return 1;
    return 0;
}

/* yes [errors]: lines without end. errors: broken_pipe_exit(0), and 3 when a write says EPIPE */
static int c_yes(int errors) {
    static const char line[] = "y\n";
    if(errors) broken_pipe_exit(0);
    for(;;) {
        if(write(1, line, 2) != 2) return errno == EPIPE ? 3 : 4;
    }
}

/* slowreader MS N: waits MS, then reads to the end: 0 for exactly N bytes */
static int c_slowreader(unsigned int ms, unsigned long n) {
    static char buf[4096];
    unsigned long got = 0;
    ssize_t r;
    msleep(ms);
    while((r = read(0, buf, sizeof(buf))) > 0) got += (unsigned long)r;
    return r == 0 && got == n ? 0 : 1;
}

/* hold N MS: N pipes (as many as it gets), kept MS; the exit status is how many */
static int c_hold(int n, unsigned int ms) {
    int fds[2], got = 0;
    while(got < n && pipe(fds) == 0) got++;
    msleep(ms);
    return got;
}

/* later MS: one byte on descriptor 1 after MS */
static int c_later(unsigned int ms) {
    msleep(ms);
    return write(1, "!", 1) == 1 ? 0 : 1;
}

/* reader: reads descriptor 0 until its end */
static int c_reader(void) {
    char buf[256];
    while(read(0, buf, sizeof(buf)) > 0);
    return 0;
}

/**** the modes ****/

static int m_basic(void) {
    int p[2] = { -1, -1 };
    char buf[64];
    struct stat st;
    mode = "basic";
    if(pipe(p) < 0) return fail("pipe()");
    if(p[0] < 0 || p[1] < 0 || p[0] == p[1]) return fail("pipe() gave no two descriptors");
    if(write(p[1], "hello", 5) != 5) return fail("write of 5 bytes");
    if(read(p[0], buf, sizeof(buf)) != 5 || memcmp(buf, "hello", 5)) return fail("read back");
    if(lseek(p[0], 0, SEEK_SET) != -1 || errno != ESPIPE) return fail("lseek on the reading end is not ESPIPE");
    if(lseek(p[1], 0, SEEK_CUR) != -1 || errno != ESPIPE) return fail("lseek on the writing end is not ESPIPE");
    if(fstat(p[0], &st) < 0 || strcmp(st.fs, "pipefs") || st.size != 0) return fail("fstat");
    if(isatty(p[0]) || isatty(p[1])) return fail("isatty says a pipe is a terminal");
    if(read(p[1], buf, 1) != -1 || errno != EBADF) return fail("the writing end can be read");
    if(write(p[0], "x", 1) != -1 || errno != EBADF) return fail("the reading end can be written");
    if(write(p[1], "ab", 2) != 2) return fail("write of 2 bytes");
    close(p[1]);
    if(read(p[0], buf, 1) != 1 || buf[0] != 'a' || read(p[0], buf, 8) != 1 || buf[0] != 'b')
        return fail("what was written before the end");
    if(read(p[0], buf, sizeof(buf)) != 0) return fail("no end of the input once the writing end is closed");
    if(read(p[0], buf, sizeof(buf)) != 0) return fail("the end of the input does not stay");
    close(p[0]);
    if(pipe((int *)0) != -1 || errno != EFAULT) return fail("pipe(NULL) is not EFAULT");
    if(pipe((int *)0xC0000000) != -1 || errno != EFAULT) return fail("pipe() of a kernel address is not EFAULT");
    printf("pipetest: bytes through, the end of the input, EBADF, ESPIPE, pipefs, EFAULT\n");
    return pass();
}

static int m_stream(unsigned long n) {
    static unsigned char buf[9000];
    char num[16], *wargv[] = { "pipetest", "writer", num, NULL }, *cargv[] = { "pipetest", "count", num, NULL };
    unsigned long got = 0, i;
    unsigned int step = 1, t0;
    int p[2], pid, r, status;
    mode = "stream";
    snprintf(num, sizeof(num), "%lu", n);
    if(pipe(p) < 0) return fail("pipe()");
    t0 = uptime();
    pid = child(wargv, -1, p[1]);
    close(p[1]);
    if(pid < 0) return fail("spawnfd of the writer");
    for(;;) {
        r = (int)read(p[0], buf, step);
        if(r < 0) return fail("read");
        if(r == 0) break;
        for(i = 0; i < (unsigned long)r; i++) {
            if(buf[i] != pat(got + i)) {
                printf("pipetest: byte %lu is %u, not %u\n", got + i, buf[i], pat(got + i));
                return fail("a byte out of order");
            }
        }
        got += (unsigned long)r;
        step = step * 13 % 8999 + 1;
    }
    close(p[0]);
    if(got != n) return fail("not every byte arrived");
    if((status = collect(pid)) != 0) return fail("the writer's exit status");
    printf("pipetest: %lu bytes from a child, in order, in %u ms\n", n, uptime() - t0);

    /* the other way: one write() of all of it, which waits for room again and again */
    if(n > sizeof(buf) * 30) n = sizeof(buf) * 30;
    snprintf(num, sizeof(num), "%lu", n);
    {
        unsigned char *all = malloc(n);
        if(!all) return fail("no memory");
        for(i = 0; i < n; i++) all[i] = pat(i);
        if(pipe(p) < 0) return fail("pipe()");
        pid = child(cargv, p[0], -1);
        close(p[0]);
        if(pid < 0) return fail("spawnfd of the reader");
        if(write(p[1], all, n) != (ssize_t)n) return fail("one write() of everything");
        close(p[1]);
        free(all);
    }
    if((status = collect(pid)) != 0) {
        printf("pipetest: the reader says %d\n", status);
        return fail("the reader did not get exactly the stream");
    }
    printf("pipetest: %lu bytes to a child in one write()\n", n);
    return pass();
}

static int m_full(void) {
    static char buf[PIPE_SIZE];
    char ms[] = "300", num[16], *argv[] = { "pipetest", "slowreader", ms, num, NULL };
    unsigned int t0, took;
    int p[2], pid, status;
    mode = "full";
    if(pipe(p) < 0) return fail("pipe()");
    memset(buf, 'f', sizeof(buf));
    if(!(ready(p[1], POLLOUT) & POLLOUT)) return fail("an empty pipe is not POLLOUT");
    if(write(p[1], buf, PIPE_SIZE) != PIPE_SIZE) return fail("PIPE_SIZE bytes into an empty pipe");
    if(ready(p[1], POLLOUT) & POLLOUT) return fail("a full pipe is POLLOUT");
    if(!(ready(p[0], POLLIN) & POLLIN)) return fail("a full pipe is not POLLIN");
    if(read(p[0], buf, PIPE_BUF - 1) != PIPE_BUF - 1) return fail("read");
    if(ready(p[1], POLLOUT) & POLLOUT) return fail("POLLOUT with less than PIPE_BUF bytes of room");
    if(read(p[0], buf, 1) != 1) return fail("read");
    if(!(ready(p[1], POLLOUT) & POLLOUT)) return fail("no POLLOUT with PIPE_BUF bytes of room");
    if(write(p[1], buf, PIPE_BUF) != PIPE_BUF) return fail("PIPE_BUF bytes into the room there is");
    printf("pipetest: %d bytes go in at once; POLLOUT only with %d bytes of room\n", PIPE_SIZE, PIPE_BUF);

    /* full again: the next write waits for a reader that comes 300 ms later */
    snprintf(num, sizeof(num), "%d", PIPE_SIZE + 1000);
    t0 = uptime();                                      /* before it starts: it reads 300 ms after its start */
    pid = child(argv, p[0], -1);
    close(p[0]);
    if(pid < 0) return fail("spawnfd of the slow reader");
    if(write(p[1], buf, 1000) != 1000) return fail("a write into a full pipe");
    took = uptime() - t0;
    close(p[1]);
    if((status = collect(pid)) != 0) return fail("the slow reader did not get every byte");
    if(took < 250) {
        printf("pipetest: the write took %u ms\n", took);
        return fail("a write into a full pipe did not wait for the reader");
    }
    printf("pipetest: a write into a full pipe waited %u ms for the reader\n", took);
    return pass();
}

static int m_atomic(void) {
    static char buf[PIPE_BUF + 7];
    char k[] = "64", *a[] = { "pipetest", "blocks", "A", k, NULL }, *b[] = { "pipetest", "blocks", "B", k, NULL };
    long got = 0, unit = 0, na = 0, nb = 0;
    char letter = 0;
    int p[2], pa, pb, r, i;
    mode = "atomic";
    if(pipe(p) < 0) return fail("pipe()");
    pa = child(a, -1, p[1]);
    pb = child(b, -1, p[1]);
    close(p[1]);
    if(pa < 0 || pb < 0) return fail("spawnfd of the writers");
    while((r = (int)read(p[0], buf, sizeof(buf))) > 0) {
        for(i = 0; i < r; i++, got++) {
            if(got % PIPE_BUF == 0) {                   /* a block begins: whose? */
                letter = buf[i];
                if(letter == 'A') na++;
                else if(letter == 'B') nb++;
                else return fail("a byte that no writer wrote");
                unit = 0;
            }
            if(buf[i] != letter) {
                printf("pipetest: byte %ld of a block of %c is %c\n", unit, letter, buf[i]);
                return fail("two writers' bytes mixed in a block");
            }
            unit++;
        }
    }
    close(p[0]);
    if(r < 0) return fail("read");
    if(collect(pa) != 0 || collect(pb) != 0) return fail("a writer's exit status");
    if(na != 64 || nb != 64 || got != 128L * PIPE_BUF) return fail("not every block arrived");
    printf("pipetest: 128 writes of %d bytes from two children, each whole\n", PIPE_BUF);
    return pass();
}

static int m_broken(void) {
    char *yes[] = { "pipetest", "yes", NULL }, *yes_err[] = { "pipetest", "yes", "errors", NULL };
    char buf[100];
    int p[2], pid, status;
    mode = "broken";
    if(pipe(p) < 0) return fail("pipe()");
    pid = child(yes, -1, p[1]);
    close(p[1]);
    if(pid < 0) return fail("spawnfd");
    if(read(p[0], buf, sizeof(buf)) <= 0) return fail("read");
    close(p[0]);                                        /* nobody reads any more */
    if((status = collect(pid)) != 141) {
        printf("pipetest: the writer's status is %d\n", status);
        return fail("a writer to a broken pipe did not end with 141");
    }
    printf("pipetest: a writer to a pipe nobody reads ended with status 141\n");

    if(pipe(p) < 0) return fail("pipe()");
    pid = child(yes_err, -1, p[1]);
    close(p[1]);
    if(pid < 0) return fail("spawnfd");
    if(read(p[0], buf, sizeof(buf)) <= 0) return fail("read");
    close(p[0]);
    if((status = collect(pid)) != 3) {
        printf("pipetest: the writer's status is %d\n", status);
        return fail("broken_pipe_exit(0) did not give the writer EPIPE");
    }
    broken_pipe_exit(0);                                /* and here: the error, not the end */
    if(pipe(p) < 0) return fail("pipe()");
    close(p[0]);
    if(write(p[1], "x", 1) != -1 || errno != EPIPE) return fail("a write with no reader is not EPIPE");
    if(!((status = ready(p[1], POLLOUT)) & POLLERR) || !(status & POLLOUT)) return fail("no POLLERR with no reader");
    close(p[1]);
    broken_pipe_exit(1);
    printf("pipetest: with broken_pipe_exit(0) a write says EPIPE\n");
    return pass();
}

static int m_poll(void) {
    char ms[] = "200", *later[] = { "pipetest", "later", ms, NULL }, c;
    struct pollfd q;
    unsigned int t0, took;
    int p[2], pid, r;
    mode = "poll";
    if(pipe(p) < 0) return fail("pipe()");
    if(ready(p[0], POLLIN) != 0) return fail("an empty pipe is ready to read");
    if(ready(p[1], POLLOUT) != POLLOUT) return fail("an empty pipe is not POLLOUT alone");
    if(write(p[1], "1", 1) != 1) return fail("write");
    if(ready(p[0], POLLIN) != POLLIN) return fail("a byte is not POLLIN alone");
    close(p[1]);
    if(ready(p[0], POLLIN) != (POLLIN | POLLHUP)) return fail("a byte after the end is not POLLIN and POLLHUP");
    if(read(p[0], &c, 1) != 1 || read(p[0], &c, 1) != 0) return fail("read");
    if(ready(p[0], POLLIN) != (POLLIN | POLLHUP)) return fail("the end is not POLLIN and POLLHUP");
    if(ready(p[0], 0) != POLLHUP) return fail("POLLHUP is not reported unasked");
    close(p[0]);
    if(pipe(p) < 0) return fail("pipe()");
    close(p[0]);
    if(ready(p[1], POLLOUT) != (POLLOUT | POLLERR)) return fail("no reader is not POLLOUT and POLLERR");
    close(p[1]);
    printf("pipetest: POLLIN, POLLHUP, POLLOUT, POLLERR\n");

    if(pipe(p) < 0) return fail("pipe()");
    t0 = uptime();
    pid = child(later, -1, p[1]);
    close(p[1]);
    if(pid < 0) return fail("spawnfd");
    q.fd = p[0];
    q.events = POLLIN;
    q.revents = 0;
    r = poll(&q, 1, 5000);
    took = uptime() - t0;
    if(r != 1 || !(q.revents & POLLIN)) return fail("poll() did not see the child's byte");
    if(took < 150) return fail("poll() did not wait for the byte");
    if(read(p[0], &c, 1) != 1 || c != '!') return fail("read");
    close(p[0]);
    if(collect(pid) != 0) return fail("the child's exit status");
    printf("pipetest: poll() waited %u ms for a child's byte\n", took);
    return pass();
}

/* until each of the n children sleeps (a holder has its pipes then); 0, or -1 after 10 s */
static int all_sleeping(const int *pids, int n) {
    struct procinfo pi;
    unsigned int end = uptime() + 10000;
    int i, j, asleep;
    while(uptime() < end) {
        for(i = 0, asleep = 0; i < n; i++) {
            for(j = 0; procinfo(j, &pi) == 0; j++) {
                if(pi.pid == pids[i]) {
                    if(pi.state == PROC_SLEEPING) asleep++;
                    break;
                }
            }
        }
        if(asleep == n) return 0;
        msleep(20);
    }
    return -1;
}

static int m_limits(void) {
    char six[] = "6", ms[] = "2000", *hold[] = { "pipetest", "hold", six, ms, NULL };
    int fds[16][2], n = 0, i, pids[5], held = 0, mine = 0, again = 0, status;
    mode = "limits";
    while(n < 16 && pipe(fds[n]) == 0) n++;
    if(n == 16 || errno != EMFILE) return fail("pipes until the descriptors run out: not EMFILE");
    for(i = 0; i < n; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    printf("pipetest: %d pipes, then EMFILE (this process's descriptors)\n", n);

    for(i = 0; i < 5; i++) {
        pids[i] = child(hold, -1, -1);
        if(pids[i] < 0) return fail("spawnfd of a holder");
    }
    if(all_sleeping(pids, 5)) return fail("the holders did not get going");
    while(mine < 16 && pipe(fds[mine]) == 0) mine++;
    if(errno != ENFILE) return fail("pipes past PIPE_MAX: not ENFILE");
    for(i = 0; i < 5; i++) {
        status = collect(pids[i]);
        if(status < 0) return fail("a holder's exit status");
        held += status;
    }
    for(i = 0; i < mine; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    if(held + mine != PIPE_MAX) {
        printf("pipetest: the holders had %d, this program %d\n", held, mine);
        return fail("ENFILE before or after PIPE_MAX pipes");
    }
    printf("pipetest: %d pipes held by five children and %d here, then ENFILE\n", held, mine);

    for(i = 0; i < 200; i++) {
        if(pipe(fds[0]) < 0) return fail("pipe() number 200");
        close(fds[0][0]);
        close(fds[0][1]);
    }
    for(i = 0; i < 5; i++) {                            /* and PIPE_MAX of them are there again */
        pids[i] = child(hold, -1, -1);
        if(pids[i] < 0) return fail("spawnfd of a holder");
    }
    if(all_sleeping(pids, 5)) return fail("the holders did not get going");
    while(again < 16 && pipe(fds[again]) == 0) again++;
    held = 0;
    for(i = 0; i < 5; i++) held += collect(pids[i]);
    for(i = 0; i < again; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    if(held + again != PIPE_MAX) return fail("200 pipes made and closed: fewer than PIPE_MAX left");
    printf("pipetest: 200 pipes made and closed; %d there again\n", PIPE_MAX);
    return pass();
}

/* kill pid after a moment; how long its end took after the kill (ms), or -1 when it was not killed */
static int kill_after(int pid, unsigned int ms) {
    unsigned int t0;
    msleep(ms);
    t0 = uptime();
    if(kill(pid) < 0) return -1;
    if(collect(pid) != -1) return -1;
    return (int)(uptime() - t0);
}

static int m_kill(void) {
    char *reader[] = { "pipetest", "reader", NULL }, *yes[] = { "pipetest", "yes", NULL };
    int p[2], pid, took;
    mode = "kill";
    if(pipe(p) < 0) return fail("pipe()");
    pid = child(reader, p[0], -1);                      /* the writing end stays open here: it waits */
    if(pid < 0) return fail("spawnfd");
    took = kill_after(pid, 300);
    close(p[0]);
    close(p[1]);
    if(took < 0) return fail("the reader was not killed");
    if(took > 1000) return fail("the kill did not cut the reader's wait short");
    printf("pipetest: a reader waiting on an empty pipe killed in %d ms\n", took);
    if(pipe(p) < 0) return fail("pipe()");
    pid = child(yes, -1, p[1]);                         /* nothing reads: it fills the pipe and waits */
    if(pid < 0) return fail("spawnfd");
    took = kill_after(pid, 300);
    close(p[0]);
    close(p[1]);
    if(took < 0) return fail("the writer was not killed");
    if(took > 1000) return fail("the kill did not cut the writer's wait short");
    printf("pipetest: a writer waiting on a full pipe killed in %d ms\n", took);
    return pass();
}

static int m_share(void) {
    char ms[] = "300", *sleeper[] = { "pipetest", "sleeper", ms, NULL }, c;
    unsigned int t0, took;
    int p[2], d, pid;
    mode = "share";
    if(pipe(p) < 0) return fail("pipe()");
    d = dup2(p[1], 12);
    if(d != 12) return fail("dup2");
    close(p[1]);
    if(write(12, "d", 1) != 1) return fail("a write through the copy");
    if(read(p[0], &c, 1) != 1 || c != 'd') return fail("read");
    if(ready(p[0], POLLIN) != 0) return fail("the end of the input while a copy of the writing end is open");
    close(12);
    if(read(p[0], &c, 1) != 0) return fail("no end once the copy is closed");
    close(p[0]);
    printf("pipetest: a dup2() copy holds the end of the input back until it is closed\n");
    if(pipe(p) < 0) return fail("pipe()");
    t0 = uptime();
    pid = child(sleeper, -1, p[1]);
    close(p[1]);
    if(pid < 0) return fail("spawnfd");
    if(read(p[0], &c, 1) != 0) return fail("read");
    took = uptime() - t0;
    close(p[0]);
    if(collect(pid) != 0) return fail("the child's exit status");
    if(took < 250) return fail("the end of the input came before the child ended");
    printf("pipetest: a child's descriptor held the end back %u ms, until it ended\n", took);
    return pass();
}

static int run_all(void) {
    if(m_basic() || m_stream(262144) || m_full() || m_atomic() || m_broken() || m_poll() || m_limits() || m_kill() ||
       m_share()) return 1;
    mode = "all";
    return pass();
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "";
    unsigned long n = argc > 2 ? strtoul(argv[2], NULL, 10) : 0;
    unsigned long n2 = argc > 3 ? strtoul(argv[3], NULL, 10) : 0;
    if(!strcmp(m, "basic")) return m_basic();
    if(!strcmp(m, "stream")) return m_stream(n ? n : 262144);
    if(!strcmp(m, "full")) return m_full();
    if(!strcmp(m, "atomic")) return m_atomic();
    if(!strcmp(m, "broken")) return m_broken();
    if(!strcmp(m, "poll")) return m_poll();
    if(!strcmp(m, "limits")) return m_limits();
    if(!strcmp(m, "kill")) return m_kill();
    if(!strcmp(m, "share")) return m_share();
    if(!strcmp(m, "all")) return run_all();
    /* the children */
    if(!strcmp(m, "writer")) return c_writer(n);
    if(!strcmp(m, "count")) return c_count(n);
    if(!strcmp(m, "blocks") && argc > 3) return c_blocks(argv[2][0], (int)n2);
    if(!strcmp(m, "yes")) return c_yes(argc > 2);
    if(!strcmp(m, "slowreader")) return c_slowreader((unsigned int)n, n2);
    if(!strcmp(m, "hold")) return c_hold((int)n, (unsigned int)n2);
    if(!strcmp(m, "later")) return c_later((unsigned int)n);
    if(!strcmp(m, "reader")) return c_reader();
    if(!strcmp(m, "sleeper")) { msleep((unsigned int)n); return 0; }
    fprintf(stderr, "usage: pipetest basic | stream [N] | full | atomic | broken | poll | limits | kill | share | all\n");
    return 2;
}
