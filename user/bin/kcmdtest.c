/*
SurfOS user program: kcmdtest
--------------------
File: kcmdtest.c  Date: 10/6/26
--------------------
kcmdtest <dir>: the bridge to the kernel's shell commands (surfos/kcmd.h: kcmd() and
kcmdinfo()) as the user shell, surfsh, uses it. kcmd() of a command that works (status
0), one that fails (1) and one used wrongly (2); ENOENT for a name that is no command,
EINVAL and EFAULT for bad arguments; run starting a program as a child of this process
and returning its exit status; a command whose output is redirected (a file in <dir> on
descriptor 1 with dup2, then the console back on 1 from /dev/console) and the file read
back, the same for a program started by run, and an output longer than KCMD_CAPTURE_MAX
cut with a note at its end; kcmdinfo() walking the table, which holds lsblk and fdisk and
ends with ENOENT; cd moving this process. 10/6/26: the limits (32 arguments, 255
bytes each: E2BIG beyond), kcmdinfo into a buffer that runs into unmapped memory
(EFAULT, nothing written), and descriptor 1 read-only (the write fails: status 1),
closed (the console) and /dev/null; clear writes escape sequences to descriptor 1, and
rawkeys, which reads the keyboard as it goes, refuses a redirection (status 2); usage
errors (2): mkfs -t alone, ping with a count that is no number or 0, sleep with a time
that is no number, an httpget request too long to send. The files made in <dir> are
removed again.
One line per check, then KCMDTEST PASS (exit 0) or KCMDTEST FAIL.

kcmdtest crash [file]: kcmd crashgp, which kills this process (with descriptor 1 on
file first, when one is given: the fault report still reaches the screen, and the
kernel lets go of the file when the process is collected). It never returns.

kcmdtest run <file> <command> [args...]: one kernel command with descriptor 1 on file
("-": the console), then its status; a question it asks is still seen on the console.
The program's exit status is the command's (125 when kcmd itself failed).

kcmdtest together <dir> (10/6/26): four heaptests at once, each run by a process of its
own with its output in a file in <dir>: one runs at a time and the others are refused,
and a heaptest alone afterwards finds the heap whole.

kcmdtest child-selftest [file] (10/6/26): kcmd selftest from a process that has a child
of its own (hello, ended and not yet collected): the self test passes and leaves the
child to its parent, which collects its exit status 42 afterwards.

kcmdtest pings <dir> <host> (10/6/26): three pings of 3 requests at once, each run by a
process of its own with its output in a file in <dir>: every request is answered (the
kernel's ping takes turns; it had one record for the reply it waits for).

kcmdtest kills <dir> [host] (10/6/26): a kill of a process inside a kernel command waits
for the command to end (surfos/task.h in the kernel). Each command runs in a child, which
this process kills once ps shows it waiting: cp /dev/tty <dir>/HELD.TXT waiting for a
line (its files are closed: <dir>'s volume unmounts afterwards, which the smoke test
shows) and fdisk rd1 at its question, each with the kernel heap as before; sleep 10000,
and ping <host> 100 when a host is given; cp /dev/zero /dev/null, which never waits but
looks between its pieces; a self test, which stops (SELFTEST STOPPED in its file) and
lets go of its claim, so a heaptest runs afterwards. Each ends at once, not
by the kernel's 2-second kill at once, which the kernel log would show; that one comes
for sleep -u 10000, which nothing cuts short, and the kernel log says so.

kcmdtest started <marker> <file|-> <command> [args...] (10/6/26): as run, after making
the file <marker> (so the kills above know the command is under way).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <surf.h>

#define KERNEL_ADDR ((char *)0x100000)          /* the kernel's text, as crash efault uses it */
#define BIG_LINE    20                          /* "kcmdtest line 00001\n" */
#define BIG_SIZE    (KCMD_CAPTURE_MAX + 4096)   /* cat of it leaves 4096 bytes out */

static int checks, failures;
static char dir[PATH_MAX];

static void check(int ok, const char *what) {
    int e = errno;
    checks++;
    if(ok) { printf("  ok    %s\n", what); return; }
    failures++;
    printf("  FAIL  %s (errno %d: %s)\n", what, e, strerror(e));
}

/* dir/name, in memory of its own */
static char *in_dir(const char *name) {
    char *p = malloc(PATH_MAX);
    if(!p) { puts("KCMDTEST FAIL (out of memory)"); exit(1); }
    snprintf(p, PATH_MAX, "%s/%s", dir, name);
    return p;
}

/* descriptor 1 onto a new, empty file at path, or with NULL back onto the console; 0 or -1.
   Nothing is printed in between: stdout's buffer is emptied first. */
static int out_to(const char *path) {
    int fd;
    fflush(stdout);
    fd = path ? open(path, O_WRONLY | O_CREAT | O_TRUNC) : open("/dev/console", O_WRONLY);
    if(fd < 0) return -1;
    if(fd == 1) return 0;                               /* descriptor 1 was closed: the open took it */
    if(dup2(fd, 1) < 0) { close(fd); return -1; }
    close(fd);
    return 0;
}

/* the start of a file, terminated: its length, or -1 */
static long slurp(const char *path, char *buf, long size) {
    long n = 0, r = 0;
    int fd = open(path, O_RDONLY);
    if(fd < 0) return -1;
    while(n < size - 1 && (r = read(fd, buf + n, size - 1 - n)) > 0) n += r;
    close(fd);
    buf[n] = 0;
    return r < 0 ? -1 : n;
}

static void test_status(void) {
    char *works[] = { "echo", "kcmdtest:", "this", "line", "comes", "from", "the", "kernel's", "echo", NULL };
    char *fails[] = { "ls", "/kcmdtest/no/such/directory", NULL };
    char *usage[] = { "stat", NULL };
    char *none[] = { "kcmdtest-no-such-command", NULL };
    char *empty[] = { NULL };
    char *bad[] = { "echo", KERNEL_ADDR, NULL };

    check(kcmd(works) == 0, "kcmd echo: status 0");
    check(kcmd(fails) == 1, "kcmd ls of a missing directory: status 1");
    check(kcmd(usage) == 2, "kcmd stat without a path: status 2, a usage error");
    errno = 0;
    check(kcmd(none) == -1 && errno == ENOENT, "a name that is no command: ENOENT");
    errno = 0;
    check(kcmd(empty) == -1 && errno == EINVAL, "an empty argv: EINVAL");
    errno = 0;
    check(kcmd(bad) == -1 && errno == EFAULT, "an argument in kernel memory: EFAULT");
    errno = 0;
    check(kcmd((char *const *)KERNEL_ADDR) == -1 && errno == EFAULT, "argv in kernel memory: EFAULT");
}

static void test_info(void) {
    struct kcmd_info info;
    int i, r = 0, lsblk = 0, fdisk = 0, sane = 1;
    char what[64];

    for(i = 0; i < 1000; i++) {
        memset(&info, 0x55, sizeof(info));
        r = kcmdinfo(i, &info);
        if(r != 0) break;
        if(!memchr(info.name, 0, sizeof(info.name)) || !memchr(info.args, 0, sizeof(info.args)) ||
           !memchr(info.help, 0, sizeof(info.help)) || !info.name[0] || !info.help[0]) sane = 0;
        if(!strcmp(info.name, "lsblk") && !strcmp(info.args, "[-s]")) lsblk = 1;
        if(!strcmp(info.name, "fdisk")) fdisk = 1;
    }
    check(r == -1 && errno == ENOENT, "kcmdinfo after the last entry: ENOENT");
    snprintf(what, sizeof(what), "kcmdinfo: %d commands, 60 or more", i);
    check(i >= 60, what);
    check(lsblk && fdisk, "kcmdinfo finds lsblk (args [-s]) and fdisk");
    check(sane, "every entry has a name and a help line, each terminated");
    errno = 0;
    check(kcmdinfo(-1, &info) == -1 && errno == ENOENT, "kcmdinfo(-1): ENOENT");
    errno = 0;
    check(kcmdinfo(0, (struct kcmd_info *)KERNEL_ADDR) == -1 && errno == EFAULT, "kcmdinfo into kernel memory: EFAULT");
}

static void test_run(void) {
    char *hello[] = { "run", "hello", "from-kcmdtest", NULL };
    char *nothing[] = { "run", "/initrd/bin/kcmdtest-no-such-program", NULL };
    check(kcmd(hello) == 42, "kcmd run hello: its exit status 42 (run waited for it as this process's child)");
    check(kcmd(nothing) == 1, "kcmd run of a missing program: status 1");
}

static void test_redirect(void) {
    char *a = in_dir("KCMD1.TXT"), *b = in_dir("KCMD2.TXT");
    char *echo[] = { "echo", "kcmd", "redirected", "output", NULL };
    char *run_echo[] = { "run", "echo", "from", "a", "child", NULL };
    char buf[256];
    int r;

    r = out_to(a) == 0 ? kcmd(echo) : -2;
    if(out_to(NULL) < 0) exit(3);                           /* nowhere to say anything */
    check(r == 0 && slurp(a, buf, sizeof(buf)) == (long)strlen("kcmd redirected output\n") &&
          !strcmp(buf, "kcmd redirected output\n"), "kcmd echo with descriptor 1 on a file: the file holds its line");
    r = out_to(b) == 0 ? kcmd(run_echo) : -2;
    if(out_to(NULL) < 0) exit(3);
    check(r == 0 && slurp(b, buf, sizeof(buf)) > 0 && !strncmp(buf, "from a child\n", 13) && strstr(buf, "exit status 0]\n"),
          "kcmd run echo with descriptor 1 on a file: the child writes the file, run's status line follows");
    check(unlink(a) == 0 && unlink(b) == 0, "remove the two files");
}

/* an output longer than KCMD_CAPTURE_MAX: the first KCMD_CAPTURE_MAX bytes, then a note */
static void test_cut(void) {
    char *big = in_dir("KCMDBIG.TXT"), *out = in_dir("KCMDCUT.TXT");
    char *cat[] = { "cat", big, NULL };
    char line[BIG_LINE + 2], head[64], tail[160], want[160];
    struct stat st;
    long i, n = 0;
    int fd, r;

    fd = open(big, O_WRONLY | O_CREAT | O_TRUNC);
    for(i = 0; fd >= 0 && n < BIG_SIZE; i++) {
        snprintf(line, sizeof(line), "kcmdtest line %05ld\n", i);
        if(write(fd, line, BIG_LINE) != BIG_LINE) break;
        n += BIG_LINE;
    }
    if(fd >= 0) close(fd);
    check(n == BIG_SIZE, "a file of KCMD_CAPTURE_MAX + 4096 bytes");
    r = out_to(out) == 0 ? kcmd(cat) : -2;
    if(out_to(NULL) < 0) exit(3);
    snprintf(want, sizeof(want), "\n[cat: the output was cut here: 4096 more bytes did not fit in %d KB]\n", KCMD_CAPTURE_MAX / 1024);
    memset(tail, 0, sizeof(tail));
    fd = open(out, O_RDONLY);
    if(fd >= 0) {
        lseek(fd, KCMD_CAPTURE_MAX, SEEK_SET);
        read(fd, tail, sizeof(tail) - 1);
        close(fd);
    }
    check(r == 0 && stat(out, &st) == 0 && (long)st.size == KCMD_CAPTURE_MAX + (long)strlen(want) && !strcmp(tail, want) &&
          slurp(out, head, sizeof(head)) > 0 && !strncmp(head, "kcmdtest line 00000\nkcmdtest line 00001\n", 40),
          "kcmd cat of it into a file: KCMD_CAPTURE_MAX bytes, then the note");
    check(unlink(big) == 0 && unlink(out) == 0, "remove the two files");
}

/* 10/6/26: the limits of the arguments, kcmdinfo at the end of mapped memory, and what
   descriptor 1 may be */
static void test_edges(void) {
    static char a255[256], a256[257];
    char *argv[40], *end, *ro = in_dir("KCMDRO.TXT");
    struct kcmd_info *info;
    int i, r, fd, untouched;

    for(i = 1; i < 32; i++) argv[i] = "w";
    argv[0] = "true-is-not-a-kernel-command";            /* the count is checked before the name */
    argv[32] = "33rd";
    argv[33] = NULL;
    errno = 0;
    check(kcmd(argv) == -1 && errno == E2BIG, "33 arguments: E2BIG");
    argv[0] = "echo";
    argv[32] = NULL;
    r = out_to("/dev/null") == 0 ? kcmd(argv) : -2;
    if(out_to(NULL) < 0) exit(3);
    check(r == 0, "32 arguments are taken (and descriptor 1 on /dev/null: nothing shown, status 0)");
    memset(a255, 'a', 255);
    memset(a256, 'b', 256);
    argv[1] = a255;
    argv[2] = NULL;
    r = out_to("/dev/null") == 0 ? kcmd(argv) : -2;
    if(out_to(NULL) < 0) exit(3);
    check(r == 0, "an argument of 255 bytes is taken");
    argv[1] = a256;
    errno = 0;
    check(kcmd(argv) == -1 && errno == E2BIG, "one of 256 bytes: E2BIG");

    end = (char *)(((unsigned long)sbrk(0) + 4095) & ~4095UL);   /* the first byte past the heap's pages */
    info = (struct kcmd_info *)(end - 100);
    memset(end - 100, 0x5a, 100);
    errno = 0;
    r = kcmdinfo(0, info);
    for(untouched = 1, i = 0; i < 100; i++) if((unsigned char)end[i - 100] != 0x5a) untouched = 0;
    check(r == -1 && errno == EFAULT && untouched, "kcmdinfo into a buffer that runs into unmapped memory: EFAULT, nothing written");

    fd = open(ro, O_WRONLY | O_CREAT | O_TRUNC);
    if(fd >= 0) close(fd);
    fd = open(ro, O_RDONLY);
    argv[1] = "into";
    argv[2] = "a";
    argv[3] = "read-only";
    argv[4] = "descriptor";
    argv[5] = NULL;
    fflush(stdout);
    r = fd >= 0 && dup2(fd, 1) == 1 ? kcmd(argv) : -2;
    if(fd >= 0) close(fd);
    if(out_to(NULL) < 0) exit(3);
    check(r == 1, "descriptor 1 read-only: the output cannot be written, status 1 (and the console says so)");
    fflush(stdout);
    close(1);
    argv[1] = "kcmdtest: descriptor 1 closed, this line on the console";
    argv[2] = NULL;
    r = kcmd(argv);
    if(out_to(NULL) < 0) exit(3);
    check(r == 0, "descriptor 1 closed: the output goes to the console, status 0");
    argv[0] = "clear";
    argv[1] = NULL;
    {
        char buf[64];
        r = out_to(ro) == 0 ? kcmd(argv) : -2;
        if(out_to(NULL) < 0) exit(3);
        check(r == 0 && slurp(ro, buf, sizeof(buf)) > 0 && strstr(buf, "\033[2J\033[H"),
              "clear writes ESC [2J ESC [H to descriptor 1 (the caller's, not the console that shows)");
    }
    argv[0] = "rawkeys";
    r = out_to("/dev/null") == 0 ? kcmd(argv) : -2;
    if(out_to(NULL) < 0) exit(3);
    check(r == 2, "rawkeys with its output in a file: refused, status 2 (it reads the keyboard as it goes)");
    check(unlink(ro) == 0, "remove the file");
}

/* 10/6/26: what was typed is not how the command is used: status 2 */
static int usage_status(char *const argv[]) {
    int r = out_to("/dev/null") == 0 ? kcmd(argv) : -2;
    if(out_to(NULL) < 0) exit(3);
    return r;
}

static void test_usage(void) {
    static char path[242];
    char *mkfs[] = { "mkfs", "-t", NULL };
    char *ping_abc[] = { "ping", "10.0.2.2", "abc", NULL };
    char *ping_0[] = { "ping", "10.0.2.2", "0", NULL };
    char *sleep_abc[] = { "sleep", "abc", NULL };
    char *httpget[] = { "httpget", "10.0.2.2", "80", path, NULL };
    path[0] = '/';
    memset(path + 1, 'x', 240);
    check(usage_status(mkfs) == 2, "mkfs -t alone: status 2");
    check(usage_status(ping_abc) == 2 && usage_status(ping_0) == 2, "ping with a count that is no number, or 0: status 2");
    check(usage_status(sleep_abc) == 2, "sleep with a time that is no number: status 2");
    check(usage_status(httpget) == 2, "httpget with a request too long to send: refused before it connects, status 2");
}

static void test_cd(void) {
    char got[PATH_MAX];
    char *cd_root[] = { "cd", "/", NULL };
    char *cd_dir[] = { "cd", dir, NULL };
    char *cd_none[] = { "cd", "/kcmdtest/no/such/directory", NULL };
    check(kcmd(cd_root) == 0 && getcwd(got, sizeof(got)) && !strcmp(got, "/"), "kcmd cd /: this process is in / now");
    check(kcmd(cd_dir) == 0 && getcwd(got, sizeof(got)) && !strcmp(got, dir), "kcmd cd <dir>: in <dir> now");
    check(kcmd(cd_none) == 1 && getcwd(got, sizeof(got)) && !strcmp(got, dir), "kcmd cd to a missing directory: 1, and it stays");
}

/* one kernel command, its output in file ("-": on the console) */
static int run(const char *file, char **argv) {
    int r;
    if(strcmp(file, "-") && out_to(file) < 0) { printf("kcmdtest: %s: %s\n", file, strerror(errno)); return 1; }
    r = kcmd(argv);
    if(out_to(NULL) < 0) return 3;
    if(r < 0) printf("kcmdtest: %s: %s\n", argv[0], strerror(errno));
    else printf("kcmdtest: %s: status %d\n", argv[0], r);
    return r < 0 ? 125 : r;
}

/* four heaptests at once: one at a time, the others refused; the heap is whole afterwards */
static int together(void) {
    char *files[4], *argv[4][5], buf[600];
    char *heaptest[] = { "heaptest", NULL };
    int pids[4], i, ran = 0, refused = 0, odd = 0, status, r;
    for(i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "KHEAP%d.TXT", i);
        files[i] = in_dir(name);
        argv[i][0] = "kcmdtest";
        argv[i][1] = "run";
        argv[i][2] = files[i];
        argv[i][3] = "heaptest";
        argv[i][4] = NULL;
    }
    fflush(stdout);
    for(i = 0; i < 4; i++) pids[i] = spawn("/initrd/bin/kcmdtest", argv[i]);
    for(i = 0; i < 4; i++) {
        status = -99;
        buf[0] = 0;
        if(pids[i] < 0 || waitpid(pids[i], &status, 0) != pids[i] || slurp(files[i], buf, sizeof(buf)) < 0) odd++;
        else if(status == 1 && strstr(buf, "one at a time")) refused++;
        else if((status == 0 || status == 1) && strstr(buf, "heaptest: ") && strstr(buf, "HEAPTEST")) ran++;
        else odd++;
        unlink(files[i]);
    }
    printf("kcmdtest: four heaptests at once: %d ran, %d refused, %d otherwise\n", ran, refused, odd);
    check(ran >= 1 && !odd, "four heaptests at once: one at a time, the others refused, no kernel panic");
    r = out_to(files[0]) == 0 ? kcmd(heaptest) : -2;
    if(out_to(NULL) < 0) exit(3);
    check(r == 0 && slurp(files[0], buf, sizeof(buf)) > 0 && strstr(buf, "HEAPTEST PASS"), "a heaptest alone afterwards: the heap is whole");
    unlink(files[0]);
    printf("%d checks, %d failed\n", checks, failures);
    puts(failures ? "KCMDTEST FAIL" : "KCMDTEST PASS");
    return failures ? 1 : 0;
}

/* three pings at once: they take turns, and every request is answered */
static int pings(const char *host) {
    char *files[3], *argv[3][7], buf[600];
    int pids[3], i, good = 0, status;
    for(i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "KPING%d.TXT", i);
        files[i] = in_dir(name);
        argv[i][0] = "kcmdtest";
        argv[i][1] = "run";
        argv[i][2] = files[i];
        argv[i][3] = "ping";
        argv[i][4] = (char *)host;
        argv[i][5] = "3";
        argv[i][6] = NULL;
    }
    fflush(stdout);
    for(i = 0; i < 3; i++) pids[i] = spawn("/initrd/bin/kcmdtest", argv[i]);
    for(i = 0; i < 3; i++) {
        status = -99;
        buf[0] = 0;
        if(pids[i] >= 0 && waitpid(pids[i], &status, 0) == pids[i] && slurp(files[i], buf, sizeof(buf)) > 0 &&
           status == 0 && strstr(buf, "3 packets transmitted, 3 received")) good++;
        else printf("kcmdtest: ping %d: status %d:\n%s\n", i + 1, status, buf);
        unlink(files[i]);
    }
    check(good == 3, "three pings at once: every request answered");
    printf("%d checks, %d failed\n", checks, failures);
    puts(failures ? "KCMDTEST FAIL" : "KCMDTEST PASS");
    return failures ? 1 : 0;
}

/* kcmd selftest from a process with a child of its own that has ended: the child stays its own */
static int child_selftest(const char *file) {
    char *hello[] = { "hello", "kcmdtest's child", NULL };
    char *selftest[] = { "selftest", NULL };
    int pid, r = -2, w, status = -99;
    fflush(stdout);
    pid = spawn("/initrd/bin/hello", hello);
    if(pid < 0) { printf("kcmdtest: hello: %s\n", strerror(errno)); return 1; }
    msleep(300);                                        /* it has ended by now: a child not yet collected */
    if(!file || out_to(file) == 0) r = kcmd(selftest);
    if(file && out_to(NULL) < 0) exit(3);
    w = waitpid(pid, &status, 0);
    printf("kcmdtest: kcmd selftest: status %d; this process's own child: %s, exit status %d\n", r,
           w == pid ? "collected" : "gone", status);
    if(file) unlink(file);
    return r == 0 && w == pid && status == 42 ? 0 : 1;
}

/* a kernel command that faults ends the process that ran it */
static int crash(const char *file) {
    char *crashgp[] = { "crashgp", NULL };
    printf("kcmdtest: kcmd crashgp%s%s\n", file ? " with descriptor 1 on " : "", file ? file : "");
    if(file && out_to(file) < 0) { printf("kcmdtest: %s: %s\n", file, strerror(errno)); return 1; }
    kcmd(crashgp);
    out_to(NULL);
    puts("KCMDTEST FAIL (crashgp came back)");
    return 1;
}

/**** 10/6/26: kills inside a kernel command ****/

static char big[65536];                         /* dmesg, a self test's output */

/* a kernel command with its output in dir/KOUT.TXT, read into big; its status, or -1 */
static int kcmd_into_big(char **argv) {
    char *file = in_dir("KOUT.TXT");
    int r = out_to(file) == 0 ? kcmd(argv) : -2;
    if(out_to(NULL) < 0) exit(3);
    if(slurp(file, big, sizeof(big)) < 0) r = -1;
    unlink(file);
    free(file);
    return r;
}

/* what memstat says the kernel heap holds ("214864 bytes in 142 blocks"), into out; 0 or -1 */
static int heap_used(char *out, size_t size) {
    char *memstat[] = { "memstat", NULL };
    char *p, *e;
    if(kcmd_into_big(memstat) != 0 || !(p = strstr(big, "kernel heap:")) || !(p = strstr(p, "used: ")) ||
       !(e = strstr(p, " blocks")) || (size_t)(e + 7 - (p + 6)) >= size) {
        snprintf(out, size, "?");
        return -1;
    }
    p += 6;
    memcpy(out, p, e + 7 - p);
    out[e + 7 - p] = 0;
    return 0;
}

/* the state ps shows for pid (ready, blocked, sleeping, ...) into st[16]; 0, or -1 when not listed */
static int task_state(int pid, char *st) {
    char *ps[] = { "ps", NULL };
    char *line;
    int p;
    if(kcmd_into_big(ps) != 0) return -1;
    for(line = big; line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
        if(sscanf(line, "%d %15s", &p, st) == 2 && p == pid) return 0;
    return -1;
}

/* did the kernel kill pid at once, its call not ended after 2 s (the kernel log)? */
static int killed_at_once(int pid) {
    char *dmesg[] = { "dmesg", NULL };
    char line[48];
    snprintf(line, sizeof(line), "(pid %d) did not end", pid);
    return kcmd_into_big(dmesg) == 0 && strstr(big, line) != NULL;
}

/* kcmdtest started: the marker, then as run */
static int started(const char *marker, const char *file, char **argv) {
    int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC);
    if(fd < 0) { printf("kcmdtest: %s: %s\n", marker, strerror(errno)); return 1; }
    close(fd);
    return run(file, argv);
}

/* One kernel command in a child (kcmdtest started), killed once the command is under way and,
   when states is given, ps shows the child in one of them (waiting), after pause ms more. The
   child's status (-1: killed), its pid, and the time from the kill to its end. */
static int kill_child(char **cmd, const char *file, const char *states, unsigned pause, int *pid, unsigned *took) {
    char *argv[16], *marker = in_dir("KSTART.TXT"), st[16];
    struct stat sb;
    int n = 0, status = -99;
    unsigned t0;
    unlink(marker);
    argv[n++] = "kcmdtest";
    argv[n++] = "started";
    argv[n++] = marker;
    argv[n++] = (char *)file;
    while(*cmd && n < 15) argv[n++] = *cmd++;
    argv[n] = NULL;
    fflush(stdout);
    *pid = spawn("/initrd/bin/kcmdtest", argv);
    *took = 0;
    if(*pid < 0) { free(marker); return -98; }
    for(t0 = uptime(); stat(marker, &sb) < 0 && uptime() - t0 < 30000; ) msleep(20);
    for(t0 = uptime(); states && uptime() - t0 < 20000; msleep(20)) {
        if(task_state(*pid, st) < 0 || !strcmp(st, "zombie")) break;    /* ended already */
        if(strstr(states, st)) break;
    }
    if(pause) msleep(pause);
    t0 = uptime();
    kill(*pid);
    if(waitpid(*pid, &status, 0) != *pid) status = -97;
    *took = uptime() - t0;
    unlink(marker);
    free(marker);
    return status;
}

static int kills(const char *host) {
    char *held = in_dir("HELD.TXT"), *stfile = in_dir("KILLST.TXT"), before[48], after[48], what[200];
    char *cp_null[] = { "cp", "/dev/null", held, NULL };
    char *cp_tty[] = { "cp", "/dev/tty", held, NULL };
    char *fdisk[] = { "fdisk", "rd1", "new", "gpt", NULL };
    char *sleep10[] = { "sleep", "10000", NULL };
    char *ping[] = { "ping", (char *)host, "100", NULL };
    char *cp_zero[] = { "cp", "/dev/zero", "/dev/null", NULL };
    char *selftest[] = { "selftest", NULL };
    char *heaptest[] = { "heaptest", NULL };
    char *sleep_u[] = { "sleep", "-u", "10000", NULL };
    unsigned took;
    int status, pid, r;

    printf("kcmdtest kills in %s\n", dir);
    kcmd_into_big(cp_null);                         /* HELD.TXT exists: the heap's start is after that */
    heap_used(before, sizeof(before));
    status = kill_child(cp_tty, "-", "blocked", 0, &pid, &took);
    heap_used(after, sizeof(after));
    snprintf(what, sizeof(what), "cp /dev/tty %s, killed while it waits for a line: ended in %u ms", held, took);
    check(status == -1 && took < 2000 && !killed_at_once(pid), what);
    snprintf(what, sizeof(what), "... the kernel heap as before (%s; then %s)", before, after);
    check(!strcmp(before, after) && strcmp(before, "?"), what);
    unlink(held);

    heap_used(before, sizeof(before));
    status = kill_child(fdisk, "-", "blocked", 0, &pid, &took);
    heap_used(after, sizeof(after));
    snprintf(what, sizeof(what), "fdisk rd1 new gpt, killed at its question: ended in %u ms", took);
    check(status == -1 && took < 2000 && !killed_at_once(pid), what);
    snprintf(what, sizeof(what), "... its buffers freed: the kernel heap as before (%s; then %s)", before, after);
    check(!strcmp(before, after) && strcmp(before, "?"), what);

    status = kill_child(sleep10, "-", "sleeping", 0, &pid, &took);
    snprintf(what, sizeof(what), "sleep 10000, killed while it sleeps: ended in %u ms", took);
    check(status == -1 && took < 2000 && !killed_at_once(pid), what);
    if(host) {
        status = kill_child(ping, "-", "blocked sleeping", 1200, &pid, &took);
        snprintf(what, sizeof(what), "ping %s 100, killed while it pings: ended in %u ms", host, took);
        check(status == -1 && took < 2000 && !killed_at_once(pid), what);
    }

    status = kill_child(cp_zero, "-", "ready", 300, &pid, &took);
    snprintf(what, sizeof(what), "cp /dev/zero /dev/null, killed while it copies: ended in %u ms", took);
    check(status == -1 && took < 2000 && !killed_at_once(pid), what);

    status = kill_child(selftest, stfile, NULL, 150, &pid, &took);
    r = slurp(stfile, big, sizeof(big));
    snprintf(what, sizeof(what), "a self test, killed while it runs: it stops (SELFTEST STOPPED) and ends in %u ms", took);
    check(status == -1 && took < 2000 && r > 0 && strstr(big, "SELFTEST STOPPED") && !killed_at_once(pid), what);
    unlink(stfile);
    r = kcmd_into_big(heaptest);
    check(r == 0 && strstr(big, "HEAPTEST PASS"), "a heaptest afterwards runs: the killed self test let go of its claim");

    status = kill_child(sleep_u, "-", "sleeping", 0, &pid, &took);
    snprintf(what, sizeof(what), "sleep -u 10000, which nothing cuts short: killed at once after the kernel's 2 s (%u ms)", took);
    check(status == -1 && took >= 1900 && took < 6000, what);
    check(killed_at_once(pid), "... and the kernel log says so");
    printf("%d checks, %d failed\n", checks, failures);
    puts(failures ? "KCMDTEST FAIL" : "KCMDTEST PASS");
    return failures ? 1 : 0;
}

/* dir: the directory to work in, absolute (so getcwd() can be compared with it); 0 or -1 */
static int set_dir(const char *arg) {
    struct stat st;
    size_t n;
    if(arg[0] == '/') snprintf(dir, sizeof(dir), "%s", arg);
    else {
        char cwd[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", getcwd(cwd, sizeof(cwd)) ? (strcmp(cwd, "/") ? cwd : "") : ".", arg);
    }
    n = strlen(dir);
    while(n > 1 && dir[n - 1] == '/') dir[--n] = 0;
    if(stat(dir, &st) < 0 || !S_ISDIR(st.type)) {
        fprintf(stderr, "kcmdtest: %s: not a directory\n", dir);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if((argc == 2 || argc == 3) && !strcmp(argv[1], "crash")) return crash(argc == 3 ? argv[2] : NULL);
    if(argc >= 4 && !strcmp(argv[1], "run")) return run(argv[2], argv + 3);
    if(argc == 3 && !strcmp(argv[1], "together")) return set_dir(argv[2]) ? 1 : together();
    if((argc == 2 || argc == 3) && !strcmp(argv[1], "child-selftest")) return child_selftest(argc == 3 ? argv[2] : NULL);
    if(argc == 4 && !strcmp(argv[1], "pings")) return set_dir(argv[2]) ? 1 : pings(argv[3]);
    if((argc == 3 || argc == 4) && !strcmp(argv[1], "kills")) return set_dir(argv[2]) ? 1 : kills(argc == 4 ? argv[3] : NULL);
    if(argc >= 5 && !strcmp(argv[1], "started")) return started(argv[2], argv[3], argv + 4);
    if(argc != 2) {
        fprintf(stderr, "usage: kcmdtest <dir>   (a directory to write in: its files are removed again)\n"
                        "       kcmdtest crash [file]\n"
                        "       kcmdtest run <file|-> <command> [args...]\n"
                        "       kcmdtest together <dir>\n"
                        "       kcmdtest child-selftest [file]\n"
                        "       kcmdtest pings <dir> <host>\n"
                        "       kcmdtest kills <dir> [host]\n"
                        "       kcmdtest started <marker> <file|-> <command> [args...]\n");
        return 2;
    }
    if(set_dir(argv[1])) return 1;
    printf("kcmdtest in %s\n", dir);
    test_status();
    test_info();
    test_run();
    test_redirect();
    test_cut();
    test_edges();
    test_usage();
    test_cd();
    printf("%d checks, %d failed\n", checks, failures);
    puts(failures ? "KCMDTEST FAIL" : "KCMDTEST PASS");
    return failures ? 1 : 0;
}
