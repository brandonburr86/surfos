/* An in-memory file system in place of the system calls user/bin/edit.c and rx.c make (10/17/26), for
   tests/host/test_edit.c and test_rx.c: built against SurfOS's user headers with the C library's names
   (tests/host/libsurf_names.h) and the system calls' (tests/host/sim_names.h). The test sets up names,
   makes a call fail (sim_fs.h), and reads back what the program did. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <surf.h>
#include "sim_fs.h"

typedef char sim_errno_numbers[SIM_EPERM == EPERM && SIM_ENOENT == ENOENT && SIM_EIO == EIO && SIM_ENOMEM == ENOMEM && SIM_EFAULT == EFAULT &&
                               SIM_EBUSY == EBUSY && SIM_EEXIST == EEXIST && SIM_EXDEV == EXDEV && SIM_ENOTDIR == ENOTDIR &&
                               SIM_EISDIR == EISDIR && SIM_EINVAL == EINVAL && SIM_ENOSPC == ENOSPC && SIM_EROFS == EROFS &&
                               SIM_ENAMETOOLONG == ENAMETOOLONG && SIM_ENOSYS == ENOSYS && SIM_ENOTEMPTY == ENOTEMPTY ? 1 : -1];

/**** the file system: names and their bytes; after a half-done rename two names are twins, one file ****/

#define NAMES 40
#define FDS   8

struct node { char name[300]; char *bytes; int len, cap, dir, used, twin, corrupt; unsigned mtime; };
static struct node nodes[NAMES];
static struct { int node, pos, used; } fds[FDS];        /* descriptor 3 + i */
static struct { char name[300]; int on, err, how; } fails[SIM_OPS];
static char log_buf[16384];
static size_t log_len;
static unsigned clock_now = 1000;                       /* each change is a second later */
static int long_paths;                                  /* calls given a path of PATH_MAX bytes or more */
static char arena[16 << 20];
static size_t arena_used;
int surf_errno;
FILE *surf_stderr, *surf_stdout;                       /* what goes to them is dropped */

static void *need(void *p) {                            /* the test's own memory: never short */
    if(!p) for(;;) __builtin_trap();
    return p;
}

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    log_len = strlen(log_buf);
}

static int lookup(const char *name) {
    int i;
    for(i = 0; i < NAMES; i++) if(nodes[i].used && !strcasecmp(nodes[i].name, name)) return i;
    return -1;
}

static int make(const char *name) {
    int i;
    for(i = 0; i < NAMES; i++) {
        if(nodes[i].used) continue;
        memset(&nodes[i], 0, sizeof(nodes[i]));
        strlcpy(nodes[i].name, name, sizeof(nodes[i].name));
        nodes[i].used = 1;
        nodes[i].twin = -1;
        return i;
    }
    return -1;
}

static void set_bytes(int n, const char *bytes, int len) {
    char *p = need(malloc((size_t)len + 1));
    memcpy(p, bytes, (size_t)len);
    free(nodes[n].bytes);
    nodes[n].bytes = p;
    nodes[n].len = nodes[n].cap = len;
    nodes[n].mtime = ++clock_now;
}

/* n's data goes: a twin's is the same data, freed under it */
static void untwin(int n) {
    if(nodes[n].twin < 0) return;
    nodes[nodes[n].twin].corrupt = 1;
    nodes[nodes[n].twin].twin = -1;
    nodes[n].twin = -1;
}

static void drop(int n) {
    untwin(n);
    free(nodes[n].bytes);
    memset(&nodes[n], 0, sizeof(nodes[n]));
}

/* the failure set up for op on name, once: its errno (-1: a short write) and how, or 0 */
static int failing(int op, const char *name, int *how) {
    if(!fails[op].on || strcasecmp(fails[op].name, name)) return 0;
    if(op == SIM_WRITE && fails[op].how > 0) { fails[op].how--; return 0; }
    if(how) *how = fails[op].how;
    fails[op].on = 0;
    return fails[op].err ? fails[op].err : op == SIM_WRITE ? -1 : EIO;
}

static int fail_with(int e) {
    errno = e;
    return -1;
}

static int too_long(const char *path) {                 /* the system calls' string check */
    if(strlen(path) < PATH_MAX) return 0;
    long_paths++;
    return 1;
}

static int slot(int fd) {
    return fd >= 3 && fd < 3 + FDS && fds[fd - 3].used ? fd - 3 : -1;
}

int open(const char *path, int flags, ...) {
    int n, fd, e;
    note("open %s%s%s\n", path, flags & O_EXCL ? " excl" : "", flags & O_TRUNC ? " trunc" : "");
    if(too_long(path)) return fail_with(EFAULT);
    n = lookup(path);
    if((e = failing(SIM_OPEN, path, NULL))) return fail_with(e);
    if(n >= 0 && (flags & O_CREAT) && (flags & O_EXCL)) return fail_with(EEXIST);
    if(n >= 0 && nodes[n].dir && (flags & O_ACCMODE) != O_RDONLY) return fail_with(EISDIR);
    for(fd = 0; fd < FDS && fds[fd].used; fd++) {}
    if(fd == FDS) return fail_with(EMFILE);
    if(n < 0) {
        if(!(flags & O_CREAT)) return fail_with(ENOENT);
        if((n = make(path)) < 0) return fail_with(ENOSPC);
    } else if((flags & O_TRUNC) && nodes[n].len) {  /* fat_truncate frees the chain, a twin's too */
        untwin(n);
        nodes[n].len = 0;
        nodes[n].mtime = ++clock_now;
    }
    fds[fd].used = 1;
    fds[fd].node = n;
    fds[fd].pos = 0;
    return 3 + fd;
}

ssize_t write(int fd, const void *buf, size_t len) {
    int s = slot(fd), e, end;
    struct node *x;
    if(fd == 1 || fd == 2) return (ssize_t)len;         /* the screen */
    if(s < 0) return fail_with(EBADF);
    x = &nodes[fds[s].node];
    note("write %s %u\n", x->name, (unsigned)len);
    if((e = failing(SIM_WRITE, x->name, NULL)) > 0) return fail_with(e);
    if(e < 0) len /= 2;                                 /* a short write: the disk is full */
    end = fds[s].pos + (int)len;
    if(end > x->cap) {
        char *p = need(malloc((size_t)end * 2));
        if(x->len) memcpy(p, x->bytes, (size_t)x->len);
        free(x->bytes);
        x->bytes = p;
        x->cap = end * 2;
    }
    memcpy(x->bytes + fds[s].pos, buf, len);
    fds[s].pos = end;
    if(end > x->len) x->len = end;
    x->mtime = ++clock_now;
    return (ssize_t)len;
}

ssize_t read(int fd, void *buf, size_t len) {
    int s = slot(fd), k;
    struct node *x;
    if(s < 0) return fail_with(EBADF);
    x = &nodes[fds[s].node];
    k = x->len - fds[s].pos;
    if(k > (int)len) k = (int)len;
    if(k <= 0) return 0;
    memcpy(buf, x->bytes + fds[s].pos, (size_t)k);
    fds[s].pos += k;
    return k;
}

int fsync(int fd) {
    int s = slot(fd), e;
    if(s < 0) return fail_with(EBADF);
    note("fsync %s\n", nodes[fds[s].node].name);
    if((e = failing(SIM_FSYNC, nodes[fds[s].node].name, NULL))) return fail_with(e);
    return 0;
}

int close(int fd) {
    int s = slot(fd), e;
    if(s < 0) return fail_with(EBADF);
    note("close %s\n", nodes[fds[s].node].name);
    fds[s].used = 0;
    if((e = failing(SIM_CLOSE, nodes[fds[s].node].name, NULL))) return fail_with(e);
    return 0;
}

static void fill(int n, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->type = nodes[n].dir ? S_IFDIR : S_IFREG;
    st->size = (unsigned)nodes[n].len;
    st->mtime = nodes[n].mtime;
    st->ino = (unsigned)n + 1;                  /* 10/25/26: a name of its own (surfsh's test -ef) */
    strcpy(st->fs, "sim");
}

int fstat(int fd, struct stat *st) {
    int s = slot(fd);
    if(s < 0) return fail_with(EBADF);
    fill(fds[s].node, st);
    return 0;
}

int stat(const char *path, struct stat *st) {
    int n, e;
    note("stat %s\n", path);
    if(too_long(path)) return fail_with(EFAULT);
    if((e = failing(SIM_STAT, path, NULL))) return fail_with(e);
    n = lookup(path);
    if(n < 0) return fail_with(ENOENT);
    fill(n, st);
    return 0;
}

int rename(const char *from, const char *to) {
    int a, b, how = SIM_NONE, e;
    note("rename %s %s\n", from, to);
    if(too_long(from) | too_long(to)) return fail_with(EFAULT);
    a = lookup(from);
    b = lookup(to);
    e = failing(SIM_RENAME, from, &how);
    if(a < 0) return fail_with(e ? e : ENOENT);
    if(e && (how == SIM_NONE || a == b)) return fail_with(e);
    if(e && how == SIM_HALF) {                  /* the new name holds the data, and the old one still does */
        if(b < 0 && (b = make(to)) < 0) return fail_with(ENOSPC);
        untwin(b);
        set_bytes(b, nodes[a].bytes ? nodes[a].bytes : "", nodes[a].len);
        nodes[b].mtime = nodes[a].mtime;            /* the entry is copied, its time too */
        nodes[a].twin = b;
        nodes[b].twin = a;
        return fail_with(e);
    }
    if(b >= 0 && b != a) drop(b);
    strlcpy(nodes[a].name, to, sizeof(nodes[a].name));
    return e ? fail_with(e) : 0;                /* SIM_DONE: all done, and the error said anyway */
}

int unlink(const char *path) {
    int n, e;
    note("unlink %s\n", path);
    if(too_long(path)) return fail_with(EFAULT);
    n = lookup(path);
    if((e = failing(SIM_UNLINK, path, NULL))) return fail_with(e);
    if(n < 0) return fail_with(ENOENT);
    if(nodes[n].dir) return fail_with(EISDIR);
    drop(n);
    return 0;
}

int isatty(int fd) { (void)fd; return 1; }
int ttyctl(int op, unsigned int arg) { (void)op; (void)arg; return 0; }
unsigned int uptime(void) { return 0; }
int fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
int vfprintf(FILE *f, const char *fmt, va_list ap) { (void)f; (void)fmt; (void)ap; return 0; }
int fputs(const char *s, FILE *f) { (void)s; (void)f; return 0; }
int fputc(int c, FILE *f) { (void)f; return c; }
int fflush(FILE *f) { (void)f; return 0; }
int poll(struct pollfd *p, nfds_t n, int timeout_ms) { (void)p; (void)n; (void)timeout_ms; errno = ENOSYS; return -1; }
void _exit(int code) { (void)code; for(;;) __builtin_trap(); }

void *sbrk(intptr_t incr) {
    char *old = arena + arena_used;
    if(incr < 0 || arena_used + (size_t)incr > sizeof(arena)) { errno = ENOMEM; return (void *)-1; }
    arena_used += (size_t)incr;
    return old;
}

/**** what the test calls ****/

void sim_reset(void) {
    int i;
    for(i = 0; i < NAMES; i++) if(nodes[i].used) free(nodes[i].bytes);
    memset(nodes, 0, sizeof(nodes));
    memset(fds, 0, sizeof(fds));
    memset(fails, 0, sizeof(fails));
    long_paths = 0;
    sim_clear_log();
}

void sim_put(const char *name, const char *bytes) {
    int n = lookup(name);
    if(n < 0) n = make(name);
    if(n < 0) for(;;) __builtin_trap();                 /* more names than the test has room for */
    set_bytes(n, bytes, (int)strlen(bytes));
}

void sim_mkdir(const char *name) {
    int n = make(name);
    if(n < 0) for(;;) __builtin_trap();
    nodes[n].dir = 1;
}

int sim_get(const char *name, char *buf, int size) {
    int n = lookup(name), k;
    if(n < 0 || nodes[n].dir) return -1;
    k = nodes[n].len < size - 1 ? nodes[n].len : size - 1;
    if(k > 0) memcpy(buf, nodes[n].bytes, (size_t)k);
    buf[k] = 0;
    return nodes[n].len;
}

int sim_names(void) {
    int i, k = 0;
    for(i = 0; i < NAMES; i++) k += nodes[i].used;
    return k;
}

void sim_same_time(const char *a, const char *b) {
    int x = lookup(a), y = lookup(b);
    if(x < 0 || y < 0) for(;;) __builtin_trap();
    nodes[y].mtime = nodes[x].mtime;
}

int sim_long_paths(void) {
    return long_paths;
}

int sim_corrupt(const char *name) {
    int n = lookup(name);
    return n >= 0 && nodes[n].corrupt;
}

void sim_fail(int op, const char *name, int err, int how) {
    strlcpy(fails[op].name, name, sizeof(fails[op].name));
    fails[op].on = 1;
    fails[op].err = err;
    fails[op].how = how;
}

const char *sim_log(void) {
    return log_buf;
}

void sim_clear_log(void) {
    log_buf[0] = 0;
    log_len = 0;
}

