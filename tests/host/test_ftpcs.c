/* Host-side test of the ftp client whole (11/01/26): user/ftp/main.c, commands.c and session.c as they are, with
   user/ftp/ftpclient.c and user/ftpd/ftp.c, against a simulated kernel (tests/host/ftpc/surf.h): a scripted server
   on simulated connections, a terminal, an in-memory file system whose calls can be made to fail, and a clock
   that jumps, so that every timeout is reached at once.

   Everything a server sends is hostile. Covered: greetings and replies whole, a byte at a time, garbage, oversized,
   endless and silent; the login, anonymous and with a password (asked on the terminal in raw mode, also from a
   script; never sent unless typed, never shown, also when the server says it back); get into a new file and over
   one, with the connection reset, closed early, cut short, stalled, the disk full, the final reply missing or
   negative, or early; text mode's line ends; passive replies malformed, naming another address or a low port; EPSV
   then PASV; reget going on, refused, and fooled by a server that sends the whole file again; put, with the server
   refusing, stopping, or not taking data; listings with escape sequences, long lines and no end; mget with names
   that climb, are absolute, hold controls, or do not match, more names than are kept, files already there; mput;
   Ctrl+C during a transfer and during a reply, and before a command goes, also with REST answered; NOOP's answer after
   ABOR, from servers with no ABOR, late or twice; an upload's data connection ended by the server, which says why only
   then; a password typed however late, and none asked for by a process with no terminal of its own, or one that a
   detached task runs; the commands' mistakes; and last a fuzz of hostile servers. Run by `make unittest`, and by hand with clang's
   sanitizers on the Mac (the account says how). */
#define _GNU_SOURCE                     /* memmem, and DT_REG with glibc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include "ftpc/sim.h"
#include "../../user/ftp/ftpsession.h"
#include "../../user/ftp/ftpcommands.h"

int ftp_main(int argc, char **argv);    /* user/ftp/main.c, built with -Dmain=ftp_main */

static int failures, checks;
static void dump(void);

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
        if(getenv("FTPC_VERBOSE")) dump();
    }
}

/**** the server: rules for what it says to each command ****/

#define ONCE      1                     /* the rule counts one time */
#define D_RST     2                     /* the data connection is reset after its bytes */
#define D_OPEN    4                     /* it stays open and silent after them */
#define C_CLOSE   8                     /* the control connection closes after the reply */
#define C_RST    16                     /* or is reset */
#define A_EARLY  32                     /* after comes with reply, before the data */
#define D_FULL   64                     /* the data connection takes no bytes */
#define D_NONE  128                     /* nothing comes on the data connection, and it stays */
#define C_MUTE  256                     /* no after, ever */
#define C_FULL  512                     /* the control connection takes no more bytes after this reply */
#define A_LATE 1024                     /* after comes 3 seconds later, and what the server says meanwhile behind it */
#define A_TAKEN 2048                    /* after comes once the data has been read, its connection still open */

struct rule {
    const char *cmd;                    /* what a line starts with; NULL ends the rules */
    const char *reply;                  /* said at once */
    const char *data;                   /* bytes on the data connection */
    int data_len;                       /* 0: strlen(data) */
    const char *after;                  /* said when the data has been taken, or its connection closed */
    int flags;
};

struct conn {
    int open, fd;
    unsigned char *in;                  /* what the server sent */
    size_t in_len, in_off, in_cap;
    int fin, rst, full;
    int wfail;                          /* the next write fails (a reset poll() has not seen yet) */
    unsigned char *got;                 /* what the client wrote */
    size_t got_len, got_cap;
};

#define CTL_FD  100
#define DATA_FD 200
#define TTY_FD  90
#define DEV_NULL_FD 91
#define SERVER_IP 0x0202000au           /* 10.0.2.2, first octet first */

static struct conn ctl, dat;
static int data_serial;
static const struct rule *rules;
static char rule_used[64];
static const struct rule *current;      /* the rule the data connection serves */
static char sent_after;
static char lines[1 << 20];             /* every command the server got, as lines */
static size_t lines_len;
static size_t line_start;               /* where the line being received starts in ctl.got */
static const char *greeting;
static size_t greeting_len;
static int greet_rst;
static int refuse_data;
static int connects, bad_connects;      /* connections; to anything but the server, or a low data port */
static int last_port;
static int max_read;                    /* a read gives at most this: 1 splits everything */
static int max_write;                   /* a write to a connection queues at most this, as the kernel's may */
static int control_seen;                /* a control connection was made in this run */

/* the terminal, the clock, Ctrl+C */
static unsigned int now_ms;
static int have_tty, stdin_tty, raw_on, raw_reads_ok = 1, catching;
static const char *tty_keys;
static char tty_out[1024];
static size_t tty_out_len;
static int intr_pending;
static unsigned long long intr_after_bytes;
static unsigned long long data_taken;
static unsigned int intr_at_ms;
static int intr_with_ctl;               /* once data has come, a Ctrl+C as poll() says the control connection is readable: after the look before it */
static int intr_on_fsync;               /* a Ctrl+C during an fsync, in no wait that a poll() sees */
static int intr_on_connect;             /* one that cuts the next connection short (EINTR) */
static int intr_on_line;                /* one as the input's line of this number is read, 1 the first */
static int lines_read;
static unsigned int hold_ms;            /* each write to standard output or error takes this: a pager at --More-- */
static int out_broken;                  /* standard output fails */
static int fail_strdup_at, strdups;     /* the strdup of this number fails, 1 the first */
static const char *lied_name;           /* stat says this name has lied_size bytes */
static unsigned int lied_size;
static int tty_hang;                    /* a terminal that nobody types on: a detached task's */
static char late_text[1024];            /* replies held back until late_at, and whatever the server says after them */
static unsigned int late_at;
static int endless_data;                /* the data connection has no end */
static unsigned int drip_next;          /* a byte on the data connection every 59 s from this time on, 0 for none */
static int ran_away;                    /* the client did not end by itself: the test ended it */
static int proc_tty;                    /* procinfo's tty for this process: -1, none */
static int proc_detached;               /* and it is detached: the desktop's Run program... started it */
static int proc_collect, proc_gone;     /* scans of procinfo() in which a task before this one is collected as it is read */
static int parent_detached;             /* this process's parent, on its terminal, is detached: a script Run program... started */
static int grand_detached;              /* that parent's parent, on the same terminal, is: a ring-0 shell's spawn of a script */
static int pty_session;                 /* this process and its parent are on a pty, whose detached parent is on a console: telnetd */
static const char *intr_on_read;        /* a Ctrl+C once a read of the control connection gives this */
static const char *intr_on_send;        /* a Ctrl+C once the server has a command that starts with this */
static size_t data_take;                /* an upload's data connection takes this many bytes, and then the server ends it */
static int data_end;                    /* how: DE_WRITE the next write fails, DE_RST a reset, DE_HUP a close */
static unsigned int data_end_gap;       /* and says the transfer's last reply this much later (ms; it is late then) */
#define DE_WRITE 1
#define DE_RST   2
#define DE_HUP   3
static int tty_opens;                   /* opens of /dev/tty */
static unsigned int tty_key_at;         /* the password's keys come at this time (a read waits for them) */
static int intr_with_stray;             /* a Ctrl+C as a look at the control connection, with no wait, finds a line there */
static unsigned int drip_end;           /* the dripping data connection ends at this time, 0 never */
static const char *intr_on_say;         /* a Ctrl+C once standard error holds this */
static int intr_again, intr_again_in;   /* a second Ctrl+C this many looks after the first is seen (1: the next look) */
static char closes[64];                 /* the connections' ends in order: c control, d data */
static long polls;

/* stdin, standard output and standard error */
static const char *script;
static char out_buf[1 << 22], err_buf[1 << 18];
static size_t out_len, err_len;

static void grow(unsigned char **p, size_t *cap, size_t need) {
    if(need <= *cap) return;
    *cap = need * 2 + 256;
    *p = realloc(*p, *cap);
    if(!*p) abort();
}

static void conn_reset(struct conn *c) {
    free(c->in);
    free(c->got);
    memset(c, 0, sizeof(*c));
}

static void conn_put(struct conn *c, const void *p, size_t n) {
    if(!n) return;
    grow(&c->in, &c->in_cap, c->in_len + n);
    memcpy(c->in + c->in_len, p, n);
    c->in_len += n;
}

static void say_ctl(const char *s) {
    if(!s) return;
    if(late_at) {                       /* a server answers in turn: nothing passes what is held back */
        snprintf(late_text + strlen(late_text), sizeof(late_text) - strlen(late_text), "%s", s);
        return;
    }
    conn_put(&ctl, s, strlen(s));
}

static const struct rule defaults[] = {
    { "USER", "331 password\r\n", 0, 0, 0, 0 },
    { "PASS", "230 in\r\n", 0, 0, 0, 0 },
    { "TYPE", "200 ok\r\n", 0, 0, 0, 0 },
    { "EPSV", "229 ok (|||5000|)\r\n", 0, 0, 0, 0 },
    { "SIZE", "550 no size\r\n", 0, 0, 0, 0 },
    { "ABOR", "426 stopped\r\n226 abort done\r\n", 0, 0, 0, 0 },
    { "NOOP", "200 ok\r\n", 0, 0, 0, 0 },
    { "QUIT", "221 bye\r\n", 0, 0, 0, 0 },
    { "NLST", "150 here\r\n", "", 0, "226 done\r\n", 0 },
    { NULL, 0, 0, 0, 0, 0 }
};

static const struct rule *find_rule(const char *line) {
    int i;
    for(i = 0; rules && rules[i].cmd && i < 64; i++) {
        if(rule_used[i]) continue;
        if(!strncmp(line, rules[i].cmd, strlen(rules[i].cmd))) {
            if(rules[i].flags & ONCE) rule_used[i] = 1;
            return &rules[i];
        }
    }
    for(i = 0; defaults[i].cmd; i++) if(!strncmp(line, defaults[i].cmd, strlen(defaults[i].cmd))) return &defaults[i];
    return NULL;
}

static void data_start(const struct rule *r) {
    size_t n;
    current = r;
    sent_after = 0;
    if(!r->data && !(r->flags & (D_FULL | D_NONE))) return;
    if(r->flags & D_FULL) dat.full = 1;
    if(r->flags & D_NONE) return;
    n = r->data_len ? (size_t)r->data_len : r->data ? strlen(r->data) : 0;
    if(n) conn_put(&dat, r->data, n);
    if(r->flags & D_RST) dat.rst = 1;
    else if(!(r->flags & D_OPEN) && r->data) dat.fin = 1;
}

/* the data has been taken, or its connection closed: the transfer's last reply */
static void after_data(void) {
    if(!current || sent_after) return;
    sent_after = 1;
    if(!(current->flags & C_MUTE)) say_ctl(current->after);
}

static void server_line(const char *line) {
    const struct rule *r = find_rule(line);
    size_t n = strlen(line);
    if(lines_len + n + 2 < sizeof(lines)) {
        memcpy(lines + lines_len, line, n);
        lines_len += n;
        lines[lines_len++] = '\n';
        lines[lines_len] = 0;
    }
    if(intr_on_send && !strncmp(line, intr_on_send, strlen(intr_on_send))) {
        intr_on_send = NULL;
        intr_pending++;
    }
    if(!r) {
        say_ctl("500 not understood\r\n");
        return;
    }
    say_ctl(r->reply);
    if(r->flags & A_LATE) {
        snprintf(late_text, sizeof(late_text), "%s", r->after);
        late_at = now_ms + 3000;
        return;
    }
    if(r->flags & A_EARLY) {
        say_ctl(r->after);
        sent_after = 1;
    }
    if(r->data || (r->flags & (D_FULL | D_NONE)) || r->after) {
        char early = sent_after && (r->flags & A_EARLY);
        data_start(r);
        sent_after = early;
    }
    if(r->flags & C_CLOSE) ctl.fin = 1;
    if(r->flags & C_RST) ctl.rst = 1;
    if(r->flags & C_FULL) ctl.full = 1;
}

/**** the file system ****/

#define FILES 64
#define FDS   32

struct file { char name[300]; char *bytes; size_t len, cap; int used, dir; };
static struct file files[FILES];
static struct { int file, app; size_t pos; int used; } fds[FDS];
static char fs_log[1 << 16];
static size_t fs_log_len;
static long write_room = -1;            /* bytes writes may still take; -1 no limit */
static char fail_rename[300], fail_fsync[300];
static int rename_err;

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fs_log_len += (size_t)vsnprintf(fs_log + fs_log_len, sizeof(fs_log) - fs_log_len, fmt, ap);
    va_end(ap);
    if(fs_log_len >= sizeof(fs_log)) fs_log_len = sizeof(fs_log) - 1;
}

static int lookup(const char *name) {
    int i;
    for(i = 0; i < FILES; i++) if(files[i].used && !strcasecmp(files[i].name, name)) return i;
    return -1;
}

static int make(const char *name) {
    int i;
    for(i = 0; i < FILES; i++) if(!files[i].used) {
        files[i].used = 1;
        files[i].dir = 0;
        files[i].len = 0;
        snprintf(files[i].name, sizeof(files[i].name), "%s", name);
        return i;
    }
    return -1;
}

static void put_file(const char *name, const char *text) {
    int i = lookup(name);
    size_t n = strlen(text);
    if(i < 0) i = make(name);
    grow((unsigned char **)&files[i].bytes, &files[i].cap, n + 1);
    memcpy(files[i].bytes, text, n + 1);
    files[i].len = n;
}

static void put_dir(const char *name) {
    int i = make(name);
    files[i].dir = 1;
}

static int file_is(const char *name, const char *text) {
    int i = lookup(name);
    return i >= 0 && files[i].len == strlen(text) && !memcmp(files[i].bytes, text, files[i].len);
}

static int no_file(const char *name) {
    return lookup(name) < 0;
}

static int file_count(void) {
    int i, n = 0;
    for(i = 0; i < FILES; i++) n += files[i].used;
    return n;
}

static int any_tmp(void) {
    int i;
    for(i = 0; i < FILES; i++) {
        size_t n = strlen(files[i].name);
        if(files[i].used && n > 4 && !strcmp(files[i].name + n - 4, ".tmp")) return 1;
    }
    return 0;
}

int sim_open(const char *path, int flags, ...) {
    int i, f;
    if(!strcmp(path, "/dev/tty")) {
        tty_opens++;
        if(!have_tty) {
            errno = ENXIO;
            return -1;
        }
        return TTY_FD;
    }
    if(!strcmp(path, "/dev/null")) {            /* a device: writes go nowhere */
        note("open %s\n", path);
        return DEV_NULL_FD;
    }
    f = lookup(path);
    note("open %s%s\n", path, (flags & O_EXCL) ? " excl" : (flags & O_APPEND) ? " append" : (flags & O_TRUNC) ? " TRUNC" : "");
    if((flags & O_EXCL) && f >= 0) {
        errno = EEXIST;
        return -1;
    }
    if(f < 0) {
        if(!(flags & O_CREAT)) {
            errno = ENOENT;
            return -1;
        }
        if(write_room == 0) {
            errno = ENOSPC;
            return -1;
        }
        f = make(path);
        if(f < 0) {
            errno = ENOSPC;
            return -1;
        }
    }
    for(i = 0; i < FDS; i++) if(!fds[i].used) {
        fds[i].used = 1;
        fds[i].file = f;
        fds[i].app = (flags & O_APPEND) != 0;
        fds[i].pos = 0;
        return 3 + i;
    }
    errno = EMFILE;
    return -1;
}

static int fd_slot(int fd) {
    return fd >= 3 && fd < 3 + FDS && fds[fd - 3].used ? fd - 3 : -1;
}

int sim_fsync(int fd) {
    int i = fd_slot(fd);
    if(i < 0) {
        errno = EBADF;
        return -1;
    }
    note("fsync %s\n", files[fds[i].file].name);
    if(intr_on_fsync) {
        intr_on_fsync = 0;
        intr_pending++;
    }
    if(fail_fsync[0] && !strcasecmp(fail_fsync, files[fds[i].file].name)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int sim_ftruncate(int fd, off_t size) {
    int i = fd_slot(fd);
    if(i < 0) {
        errno = EBADF;
        return -1;
    }
    note("ftruncate %s %ld\n", files[fds[i].file].name, (long)size);
    files[fds[i].file].len = (size_t)size;
    return 0;
}

int sim_stat(const char *path, struct sim_stat *st) {
    int f = lookup(path);
    memset(st, 0, sizeof(*st));
    if(!strncmp(path, "/dev/", 5)) {
        st->type = 1;
        strcpy(st->fs, "devfs");
        return 0;
    }
    if(f < 0) {
        errno = ENOENT;
        return -1;
    }
    st->type = files[f].dir ? 2 : 1;
    st->size = lied_name && !strcasecmp(path, lied_name) ? lied_size : (unsigned int)files[f].len;
    st->mtime = 1000;                   /* one time for all: two names of one size look like the twins of saveutil.h */
    strcpy(st->fs, "fat");
    return 0;
}

int sim_rename(const char *from, const char *to) {
    int f = lookup(from), t = lookup(to);
    note("rename %s %s\n", from, to);
    if(fail_rename[0] && !strcasecmp(fail_rename, from)) {
        errno = rename_err;
        return -1;
    }
    if(f < 0) {
        errno = ENOENT;
        return -1;
    }
    if(t >= 0) files[t].used = 0;
    snprintf(files[f].name, sizeof(files[f].name), "%s", to);
    return 0;
}

int sim_unlink(const char *path) {
    int f = lookup(path);
    note("unlink %s\n", path);
    if(f < 0) {
        errno = ENOENT;
        return -1;
    }
    files[f].used = 0;
    return 0;
}

static char cwd[64] = "/hda1";

int sim_chdir(const char *path) {
    int f = lookup(path);
    if(f < 0 || !files[f].dir) {
        errno = ENOENT;
        return -1;
    }
    snprintf(cwd, sizeof(cwd), "%s", path);
    return 0;
}

char *sim_getcwd(char *buf, size_t size) {
    snprintf(buf, size, "%s", cwd);
    return buf;
}

static struct { int next, open; char dir[300]; } dirs;
static struct dirent dir_entry;

void *sim_opendir(const char *path) {
    if(strcmp(path, ".") && lookup(path) < 0) {
        errno = ENOENT;
        return NULL;
    }
    dirs.next = 0;
    dirs.open = 1;
    snprintf(dirs.dir, sizeof(dirs.dir), "%s", strcmp(path, ".") ? path : "");
    return &dirs;
}

struct dirent *sim_readdir(void *d) {
    size_t n = strlen(dirs.dir);
    (void)d;
    while(dirs.next < FILES) {
        struct file *f = &files[dirs.next++];
        const char *name = f->name;
        if(!f->used) continue;
        if(n) {
            if(strncmp(name, dirs.dir, n) || name[n] != '/') continue;
            name += n + 1;
        }
        if(strchr(name, '/')) continue;
        snprintf(dir_entry.d_name, sizeof(dir_entry.d_name), "%s", name);
        dir_entry.d_type = f->dir ? DT_DIR : DT_REG;
        return &dir_entry;
    }
    return NULL;
}

int sim_closedir(void *d) {
    (void)d;
    dirs.open = 0;
    return 0;
}

/**** the kernel ****/

static struct conn *conn_of(int fd) {
    if(fd == CTL_FD && ctl.open) return &ctl;
    if(fd == DATA_FD + data_serial && dat.open) return &dat;
    return NULL;
}

int sim_tcp_connect(unsigned int ip, unsigned short port) {
    connects++;
    last_port = port;
    if(ip != SERVER_IP) bad_connects++;
    if(!ctl.open && intr_on_connect) {
        intr_on_connect = 0;
        intr_pending++;
        errno = EINTR;
        return -1;
    }
    if(!ctl.open) {
        conn_reset(&ctl);
        ctl.open = 1;
        ctl.fd = CTL_FD;
        control_seen = 1;
        line_start = 0;
        if(greeting_len) conn_put(&ctl, greeting, greeting_len);
        else if(greeting) ctl.fin = 1;
        if(greet_rst) ctl.rst = 1;
        return CTL_FD;
    }
    if(port < 1024) bad_connects++;
    if(intr_on_connect) {
        intr_on_connect = 0;
        intr_pending++;
        errno = EINTR;
        return -1;
    }
    if(refuse_data) {
        errno = ECONNREFUSED;
        return -1;
    }
    conn_reset(&dat);
    dat.open = 1;
    dat.fd = DATA_FD + ++data_serial;
    current = NULL;
    return dat.fd;
}

ssize_t sim_read(int fd, void *buf, size_t n) {
    struct conn *c = conn_of(fd);
    size_t left;
    if(fd == TTY_FD) {
        if(!raw_on) raw_reads_ok = 0;           /* a password read with the terminal echoing */
        if(tty_hang) {
            ran_away = 1;                       /* this read would wait for ever */
            return 0;
        }
        if(tty_key_at && now_ms < tty_key_at) now_ms = tty_key_at;     /* the read waits for the key */
        if(!tty_keys || !*tty_keys) return 0;
        *(char *)buf = *tty_keys++;
        return 1;
    }
    if(!c) {
        int i = fd_slot(fd);
        struct file *f;
        if(i < 0) {
            errno = EBADF;
            return -1;
        }
        f = &files[fds[i].file];
        left = fds[i].pos < f->len ? f->len - fds[i].pos : 0;
        if(n > left) n = left;
        memcpy(buf, f->bytes + fds[i].pos, n);
        fds[i].pos += n;
        return (ssize_t)n;
    }
    left = c->in_len - c->in_off;
    if(c == &dat && endless_data && !left) {
        size_t k = 0;
        char line[16];
        static unsigned int serial;
        while(k + 12 < n && k < 900) {
            int m = snprintf(line, sizeof(line), "n%07u\r\n", serial++);
            memcpy((char *)buf + k, line, (size_t)m);
            k += (size_t)m;
        }
        data_taken += k;
        return (ssize_t)k;
    }
    if(left) {
        if(n > left) n = left;
        if(max_read && n > (size_t)max_read) n = (size_t)max_read;
        memcpy(buf, c->in + c->in_off, n);
        c->in_off += n;
        if(c == &ctl && intr_on_read && memmem(buf, n, intr_on_read, strlen(intr_on_read))) {
            intr_on_read = NULL;
            intr_pending++;
        }
        if(c == &dat) {
            if(c->in_off == c->in_len && current && (current->flags & A_TAKEN)) after_data();
            data_taken += n;
            if(intr_after_bytes && data_taken >= intr_after_bytes) {
                intr_pending++;
                intr_after_bytes = 0;
            }
        }
        return (ssize_t)n;
    }
    if(c->rst) {
        errno = ECONNRESET;
        return -1;
    }
    if(c->fin) {
        if(c == &dat) after_data();
        return 0;
    }
    errno = EAGAIN;                     /* a read that would wait, which poll() said it would not */
    failures++;
    printf("FAIL: a read that would wait\n");
    return -1;
}

ssize_t sim_write(int fd, const void *buf, size_t n) {
    struct conn *c = conn_of(fd);
    if(fd == 1 || fd == 2) {
        now_ms += hold_ms;
        if(fd == 1 && out_broken) {
            errno = EIO;
            return -1;
        }
        if(fd == 1 && out_len + n < sizeof(out_buf)) {
            memcpy(out_buf + out_len, buf, n);
            out_len += n;
            out_buf[out_len] = 0;
        }
        return (ssize_t)n;
    }
    if(fd == DEV_NULL_FD) return (ssize_t)n;
    if(fd == TTY_FD) {
        if(!raw_on && n > 1) raw_reads_ok = 0;  /* the prompt printed with the terminal still echoing: a key then shows */
        if(tty_out_len + n < sizeof(tty_out)) {
            memcpy(tty_out + tty_out_len, buf, n);
            tty_out_len += n;
            tty_out[tty_out_len] = 0;
        }
        return (ssize_t)n;
    }
    if(!c) {
        int i = fd_slot(fd);
        struct file *f;
        if(i < 0) {
            errno = EBADF;
            return -1;
        }
        f = &files[fds[i].file];
        if(write_room == 0) {
            errno = ENOSPC;
            return -1;
        }
        if(write_room > 0 && n > (size_t)write_room) n = (size_t)write_room;
        if(write_room > 0) write_room -= (long)n;
        if(fds[i].app) fds[i].pos = f->len;
        grow((unsigned char **)&f->bytes, &f->cap, fds[i].pos + n + 1);
        memcpy(f->bytes + fds[i].pos, buf, n);
        fds[i].pos += n;
        if(fds[i].pos > f->len) f->len = fds[i].pos;
        f->bytes[f->len] = 0;
        return (ssize_t)n;
    }
    if(c->rst || c->wfail) {
        errno = ECONNRESET;
        return -1;
    }
    if(c->full) {                       /* poll() never said POLLOUT */
        failures++;
        printf("FAIL: a write to a connection that takes nothing\n");
        errno = EAGAIN;
        return -1;
    }
    if(max_write && n > (size_t)max_write) n = (size_t)max_write;
    grow(&c->got, &c->got_cap, c->got_len + n + 1);
    memcpy(c->got + c->got_len, buf, n);
    c->got_len += n;
    if(c == &dat && data_take && c->got_len >= data_take) {    /* the server ends it here, and then says why */
        data_take = 0;
        if(data_end == DE_WRITE) c->wfail = 1;
        else if(data_end == DE_RST) c->rst = 1;
        else c->full = c->fin = 1;
        if(current && !sent_after && !(current->flags & C_MUTE) && current->after) {
            sent_after = 1;
            if(data_end_gap) {
                snprintf(late_text, sizeof(late_text), "%s", current->after);
                late_at = now_ms + data_end_gap;
            } else say_ctl(current->after);
        }
    }
    if(c == &ctl) {
        size_t i;
        for(i = line_start; i + 1 < c->got_len; i++) {
            if(c->got[i] == '\r' && c->got[i + 1] == '\n') {
                char line[2048];
                size_t k = i - line_start;
                if(k >= sizeof(line)) k = sizeof(line) - 1;
                memcpy(line, c->got + line_start, k);
                line[k] = 0;
                line_start = i + 2;
                server_line(line);
                i++;
            }
        }
    }
    return (ssize_t)n;
}

int sim_close(int fd) {
    struct conn *c = conn_of(fd);
    if(fd == TTY_FD || fd == DEV_NULL_FD) return 0;
    if(c == &dat) {
        dat.open = 0;
        if(strlen(closes) + 1 < sizeof(closes)) strcat(closes, "d");
        after_data();
        return 0;
    }
    if(c == &ctl) {
        ctl.open = 0;
        if(strlen(closes) + 1 < sizeof(closes)) strcat(closes, "c");
        return 0;
    }
    if(fd_slot(fd) < 0) {
        errno = EBADF;
        return -1;
    }
    fds[fd - 3].used = 0;
    return 0;
}

static int revents_of(const struct pollfd *p) {
    struct conn *c = conn_of(p->fd);
    int r = 0;
    if(p->fd == TTY_FD) return tty_hang || (tty_key_at && now_ms < tty_key_at) ? 0 : p->events & POLLIN;
    if(!c) return POLLNVAL;
    if((p->events & POLLIN) && c == &dat && endless_data) r |= POLLIN;
    if((p->events & POLLIN) && (c->in_off < c->in_len || c->fin || c->rst)) r |= POLLIN;
    if((p->events & POLLOUT) && !c->full && !c->rst) r |= POLLOUT;
    if(c->fin || c->rst) r |= POLLHUP;
    if(c->rst) r |= POLLERR;
    return r;
}

int sim_poll(struct pollfd *p, unsigned int n, int timeout) {
    unsigned int i;
    int pass;
    if(++polls > 5000000) {
        failures++;
        printf("FAIL: the client polls without end\n");
        exit(1);
    }
    if((endless_data && polls > 200000) || now_ms > 6u * 3600 * 1000) {     /* it would go on for ever: this ends it */
        ran_away = 1;
        endless_data = 0;
        drip_next = 0;
        dat.rst = 1;
        ctl.rst = 1;
        tty_hang = 0;
    }
    if(late_at && now_ms >= late_at) {
        late_at = 0;
        conn_put(&ctl, late_text, strlen(late_text));
        late_text[0] = 0;
    }
    if(drip_next && now_ms >= drip_next && dat.open) {
        conn_put(&dat, drip_end ? "n\r\n" : "x", drip_end ? 3 : 1);
        drip_next += 59000;
        if(drip_end && now_ms >= drip_end) {
            drip_next = 0;
            dat.fin = 1;
        }
    }
    for(pass = 0; pass < 2; pass++) {
        int count = 0;
        for(i = 0; i < n; i++) {
            p[i].revents = (short)revents_of(&p[i]);
            if(p[i].revents) count++;
        }
        if(count) {
            for(i = 0; i < n; i++) if(intr_with_ctl && p[i].fd == CTL_FD && p[i].revents && data_taken) {
                intr_with_ctl = 0;
                intr_pending++;
            }
            for(i = 0; i < n; i++) if(intr_with_stray && !timeout && p[i].fd == CTL_FD && (p[i].revents & POLLIN)) {
                intr_with_stray = 0;            /* after the look that came before it: the next wait must see it */
                intr_pending++;
            }
            return count;
        }
        if(!pass) {
            if(timeout < 0) {
                failures++;
                printf("FAIL: a wait without a limit, with nothing to come\n");
                exit(1);
            }
            if(intr_at_ms && now_ms + (unsigned int)timeout >= intr_at_ms) {
                now_ms = intr_at_ms;
                intr_at_ms = 0;
                intr_pending++;
                errno = EINTR;
                return -1;
            }
            now_ms += (unsigned int)timeout;
            if(late_at && now_ms >= late_at) {  /* what is held back comes now, in this wait */
                late_at = 0;
                conn_put(&ctl, late_text, strlen(late_text));
                late_text[0] = 0;
            }
        }
    }
    return 0;
}

int sim_ttyctl(int op, unsigned int arg) {
    if(op == 1) {
        raw_on = (int)arg;
        return 0;
    }
    if(op == 6) {
        int n;
        if(intr_again_in && !--intr_again_in) intr_pending++;
        n = intr_pending;
        intr_pending = 0;
        catching = (int)arg;
        if(n && intr_again) {
            intr_again_in = intr_again;
            intr_again = 0;
        }
        return n;
    }
    return -1;
}

int sim_isatty(int fd) {
    return fd == 0 ? stdin_tty : 0;
}

unsigned int sim_uptime(void) {
    now_ms++;
    return now_ms;
}

int sim_readline(char *buf, size_t size) {
    size_t n = 0;
    if(!script || !*script) return -1;
    if(++lines_read == intr_on_line) intr_pending++;
    while(*script && *script != '\n') {
        if(n + 1 < size) buf[n++] = *script;
        script++;
    }
    if(*script) script++;
    buf[n] = 0;
    return (int)n;
}

int sim_net_resolve(const char *host, unsigned int *ip) {
    (void)host;
    (void)ip;
    errno = ENOENT;
    return -1;
}

int sim_ipparse(const char *s, unsigned int *ip) {
    int a, b, c, d;
    char end;
    if(sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &end) != 4) return -1;
    *ip = (unsigned int)a | (unsigned int)b << 8 | (unsigned int)c << 16 | (unsigned int)d << 24;
    return 0;
}

char *sim_ipstr(unsigned int ip, char buf[16]) {
    snprintf(buf, 16, "%u.%u.%u.%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24);
    return buf;
}

static void capture(FILE *f, const char *text, size_t n) {
    now_ms += hold_ms;
    if(f == stdout && out_broken) return;
    if(f == stderr) {
        if(err_len + n < sizeof(err_buf)) {
            memcpy(err_buf + err_len, text, n);
            err_len += n;
            err_buf[err_len] = 0;
        }
        if(intr_on_say && strstr(err_buf, intr_on_say)) {
            intr_on_say = NULL;
            intr_pending++;
        }
    } else if(out_len + n < sizeof(out_buf)) {
        memcpy(out_buf + out_len, text, n);
        out_len += n;
        out_buf[out_len] = 0;
    }
}

int sim_vfprintf(FILE *f, const char *fmt, va_list ap) {
    char text[8192];
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    if(n >= (int)sizeof(text)) n = (int)sizeof(text) - 1;
    capture(f, text, (size_t)n);
    return n;
}

int sim_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = sim_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sim_printf(const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = sim_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int sim_fputs(const char *s, FILE *f) {
    capture(f, s, strlen(s));
    return 0;
}

int sim_fputc(int c, FILE *f) {
    char b = (char)c;
    capture(f, &b, 1);
    return c;
}

int sim_fflush(FILE *f) {
    (void)f;
    return f == stdout && out_broken ? EOF : 0;
}

int sim_ferror(FILE *f) {
    return f == stdout && out_broken;
}

/* the task list, by position as SurfOS's is: init; task 60, telnetd or a detached surfsh when a test says so; this
   process's parent, a shell (70); an older task (2, until it is collected); this process (77); another (80) */
int sim_procinfo(int index, struct sim_procinfo *pi) {
    struct { int pid, ppid, tty; unsigned int flags; } t[6];
    int n = 0, up = grand_detached || pty_session;
    t[n].pid = 1, t[n].ppid = -1, t[n].tty = 0, t[n].flags = 0, n++;
    t[n].pid = 60, t[n].ppid = 1, t[n].tty = 0, t[n].flags = up ? PROC_DETACHED : 0, n++;
    t[n].pid = 70, t[n].ppid = up ? 60 : 1, t[n].tty = pty_session ? 17 : 0, t[n].flags = parent_detached ? PROC_DETACHED : 0, n++;
    if(!proc_gone) t[n].pid = 2, t[n].ppid = 1, t[n].tty = 0, t[n].flags = 0, n++;
    t[n].pid = 77, t[n].ppid = 70, t[n].tty = pty_session ? 17 : proc_tty, t[n].flags = proc_detached ? PROC_DETACHED : 0, n++;
    t[n].pid = 80, t[n].ppid = 1, t[n].tty = 0, t[n].flags = 0, n++;
    if(index < 0) {
        errno = EINVAL;
        return -1;
    }
    if(index >= n) {
        if(proc_gone && proc_collect > 0) proc_gone = 0;     /* the next scan meets another such task */
        errno = ENOENT;
        return -1;
    }
    memset(pi, 0, sizeof(*pi));
    pi->pid = t[index].pid;
    pi->ppid = t[index].ppid;
    pi->tty = t[index].tty;
    pi->flags = t[index].flags;
    if(pi->pid == 2 && proc_collect > 0) {      /* collected just after it was read: the rest move up one */
        proc_collect--;
        proc_gone = 1;
    }
    return 0;
}

int sim_getpid(void) {
    return 77;
}

char *sim_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p;
    if(++strdups == fail_strdup_at) return NULL;
    p = malloc(n);
    if(p) memcpy(p, s, n);
    return p;
}

/**** a run ****/

static void dump(void) {
    printf("--- standard output:\n%s--- standard error:\n%s--- the server got:\n%s--- the files:\n%s---\n", out_buf, err_buf, lines, fs_log);
}

static void fresh(const struct rule *r) {
    int i;
    for(i = 0; i < FILES; i++) if(files[i].used) files[i].used = 0;
    memset(fds, 0, sizeof(fds));
    conn_reset(&ctl);
    conn_reset(&dat);
    rules = r;
    memset(rule_used, 0, sizeof(rule_used));
    current = NULL;
    sent_after = 0;
    lines[0] = 0;
    lines_len = line_start = 0;
    greeting = "220 hello\r\n";
    greeting_len = strlen(greeting);
    greet_rst = refuse_data = connects = bad_connects = last_port = max_read = max_write = control_seen = 0;
    data_serial = 0;
    have_tty = stdin_tty = raw_on = catching = 0;
    raw_reads_ok = 1;
    tty_keys = NULL;
    tty_out[0] = 0;
    tty_out_len = 0;
    intr_pending = 0;
    intr_after_bytes = data_taken = 0;
    intr_at_ms = 0;
    intr_with_ctl = 0;
    intr_on_fsync = intr_on_connect = intr_on_line = lines_read = 0;
    hold_ms = 0;
    out_broken = 0;
    fail_strdup_at = strdups = 0;
    lied_name = NULL;
    tty_hang = 0;
    late_at = 0;
    late_text[0] = 0;
    endless_data = 0;
    drip_next = 0;
    ran_away = 0;
    proc_tty = 0;
    proc_detached = 0;
    proc_collect = proc_gone = 0;
    parent_detached = grand_detached = pty_session = 0;
    intr_on_read = intr_on_send = NULL;
    data_take = 0;
    data_end = 0;
    data_end_gap = 0;
    intr_on_say = NULL;
    intr_again = intr_again_in = 0;
    closes[0] = 0;
    tty_opens = 0;
    tty_key_at = 0;
    intr_with_stray = 0;
    drip_end = 0;
    polls = 0;
    out_buf[0] = err_buf[0] = 0;
    out_len = err_len = 0;
    fs_log[0] = 0;
    fs_log_len = 0;
    write_room = -1;
    fail_rename[0] = fail_fsync[0] = 0;
    now_ms = 1000;
    snprintf(cwd, sizeof(cwd), "/hda1");
}

/* ftp <args> with these lines on standard input: its status */
static int run(const char *args, const char *lines_in) {
    char copy[256], *argv[8], *p;
    int argc = 0;
    argv[argc++] = "ftp";
    snprintf(copy, sizeof(copy), "%s", args);
    for(p = strtok(copy, " "); p && argc < 7; p = strtok(NULL, " ")) argv[argc++] = p;
    argv[argc] = NULL;
    script = lines_in;
    ftp_auto_login = 1;
    argc = ftp_main(argc, argv);
    {
        int i, open_files = 0;
        for(i = 0; i < FDS; i++) open_files += fds[i].used;
        check(!open_files, "a run leaves no file open");
        check(!dat.open, "a run leaves no data connection open");
        check(!ctl.open, "a run leaves no control connection open");
        check(!raw_on, "a run leaves the terminal out of raw mode");
    }
    return argc;
}

static int said(const char *needle) {
    return strstr(err_buf, needle) != NULL;
}

/* shown to the user, on either stream; the tests of standard output alone compare out_buf */
static int shown(const char *needle) {
    return strstr(out_buf, needle) != NULL || strstr(err_buf, needle) != NULL;
}

static int server_got(const char *line) {
    return strstr(lines, line) != NULL;
}

static int logged(const char *needle) {
    return strstr(fs_log, needle) != NULL;
}

/**** greetings and logging in ****/

static void test_greeting(void) {
    static char big[200000];
    size_t n = 0;
    int i, r;

    fresh(NULL);
    r = run("10.0.2.2 2121", "quit\n");
    check(r == 0 && server_got("USER anonymous\nPASS anonymous@surfos\nQUIT\n") && shown("220 hello") && shown("221 bye"),
          "a greeting, the anonymous login, quit: status 0");
    check(connects == 1 && last_port == 2121, "... to the port given");

    fresh(NULL);
    r = run("-n 10.0.2.2", "quit\n");
    check(r == 0 && !server_got("USER") && last_port == 21, "-n does not log in, and the port is 21 by itself");

    fresh(NULL);
    r = run("", "status\nls\nquit\n");
    check(r == 2 && !connects && said("not connected") && shown("Not connected"), "no host: commands that need one say so: status 2");

    fresh(NULL);
    greeting = "220-first\r\n230 a line with numbers\r\n  220 not the end\r\n220 the end\r\n";
    greeting_len = strlen(greeting);
    r = run("-n 10.0.2.2", "quit\n");
    check(r == 0 && shown("first") && shown("220 the end"), "a greeting of several lines ends only at its code and a blank");

    fresh(NULL);
    greeting = "120 soon\r\n220 now\r\n";
    greeting_len = strlen(greeting);
    max_read = 1;
    r = run("10.0.2.2", "quit\n");
    check(r == 0 && server_got("QUIT"), "a 120 before the 220, a byte at a time");

    fresh(NULL);
    greeting = "421 too many\r\n";
    greeting_len = strlen(greeting);
    r = run("10.0.2.2", "quit\n");
    check(r == 2 && said("421 too many") && !server_got("USER"), "a 421: nothing is sent, status 2");

    fresh(NULL);
    greeting = "HTTP/1.1 200 OK\r\n\r\n";
    greeting_len = strlen(greeting);
    r = run("10.0.2.2", "quit\n");
    check(r == 2 && said("not FTP") && !server_got("USER"), "something that is no FTP");

    fresh(NULL);
    greeting = "220 \x1b[2J\x1b]0;owned\x07 hello\xc2\x9b\xff\r\n";
    greeting_len = strlen(greeting);
    r = run("-n 10.0.2.2", "quit\n");
    check(r == 0 && !strchr(out_buf, 0x1b) && !strchr(out_buf, 0x07) && !strstr(out_buf, "\xc2\x9b"),
          "a greeting's escape sequences, BEL and C1 controls never reach the terminal");

    memset(big, 'x', sizeof(big));
    fresh(NULL);
    greeting = big;
    greeting_len = sizeof(big);
    r = run("10.0.2.2", "quit\n");
    check(r == 2 && said("no reply came") && !server_got("USER"), "a greeting of 200000 bytes with no line end: dropped as it comes, and over at the time limit");

    n = 0;
    n += (size_t)snprintf(big + n, sizeof(big) - n, "220-\r\n");
    for(i = 0; i < 3000; i++) n += (size_t)snprintf(big + n, sizeof(big) - n, "x\r\n");
    fresh(NULL);
    greeting = big;
    greeting_len = n;
    r = run("10.0.2.2", "quit\n");
    check(r == 2 && said("not FTP"), "a reply with no end: dropped at its limit in lines");

    fresh(NULL);
    greeting = NULL;
    greeting_len = 0;
    r = run("10.0.2.2", "quit\n");
    check(r == 2 && said("no reply came") && now_ms >= 120000, "a server that says nothing: 120 seconds, then status 2");

    fresh(NULL);
    greeting = "";
    greeting_len = 0;
    r = run("10.0.2.2", "quit\n");
    check(r == 2 && said("the server closed it"), "a server that closes at once");

    fresh(NULL);
    greeting = "220 h";
    greeting_len = 5;
    greet_rst = 1;
    r = run("10.0.2.2", "quit\n");
    check(r == 2 && said("the connection is closed"), "a reset in the middle of the greeting");

    fresh(NULL);
    r = run("nosuch.example", "quit\n");
    check(r == 2 && said("no such host") && !connects, "a name that does not resolve");

    fresh(NULL);
    r = run("10.0.2.2 0", "quit\n");
    check(r == 2 && said("0 is no port") && !connects, "port 0");
    fresh(NULL);
    r = run("-x", "");
    check(r == 2 && said("usage: ftp"), "an option there is not");
    fresh(NULL);
    r = run("10.0.2.2 21 22", "");
    check(r == 2 && said("usage: ftp"), "too many words");
}

#define PW "hunter2-made-up"

static int password_hidden(void) {
    return !strstr(out_buf, PW) && !strstr(err_buf, PW) && !strstr(tty_out, PW) && !strstr(fs_log, PW);
}

static void test_password(void) {
    static char keys[400];
    int r;

    fresh(NULL);
    have_tty = 1;
    tty_keys = PW "\r";
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 0 && server_got("USER alice\nPASS " PW "\n"), "a password typed goes to the server");
    check(raw_reads_ok && !raw_on, "... read in raw mode (no echo), which ends again");
    check(password_hidden() && strstr(tty_out, "Password for alice: ") != NULL, "... asked on the terminal, and shown nowhere");
    check(shown("230 in"), "... and the login's answer is shown");

    {
        static const struct rule says_it[] = { { "PASS", "530 " PW " is wrong, " PW "\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(says_it);
        have_tty = 1;
        tty_keys = PW "\r";
        stdin_tty = 1;
        r = run("-n 10.0.2.2", "user alice\nstatus\nquit\n");
        check(r == 1 && password_hidden() && shown("530 ****"), "a server that says the password back: stars");
        check(shown("Not logged in"), "... and a refused login stays not logged in");
    }
    {
        static const struct rule many[] = { { "PASS", "230-" PW "\r\n" PW "\r\n230 " PW "\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(many);
        have_tty = 1;
        tty_keys = PW "\r";
        r = run("-n 10.0.2.2", "user alice\nquit\n");
        check(r == 0 && password_hidden(), "... in every line of a reply");
    }

    fresh(NULL);
    r = run("-n 10.0.2.2", "user alice " PW "\nquit\n");
    check(r == 2 && !server_got("USER") && password_hidden() && said("never typed"), "a password on the line is refused, and nothing goes");

    fresh(NULL);
    have_tty = 0;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && said("no terminal"), "no terminal to ask on: no PASS");

    fresh(NULL);
    have_tty = 1;
    tty_keys = "abc\x03";
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && !raw_on, "Ctrl+C at the password: nothing goes, raw mode ends");
    fresh(NULL);
    have_tty = 1;
    tty_keys = "abc";
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && !raw_on, "the terminal gone at the password: the same");

    memset(keys, 'k', 300);
    keys[300] = '\r';
    keys[301] = 0;
    fresh(NULL);
    have_tty = 1;
    tty_keys = keys;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && said("too long"), "too long a password is refused, not cut");

    fresh(NULL);
    have_tty = 1;
    tty_keys = "abX\x7f\x1b[Ac\r";
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 0 && server_got("PASS abc\n"), "Backspace edits, an arrow key's sequence is dropped");

    fresh(NULL);
    have_tty = 1;
    tty_keys = "a\xff" "b\r";
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 0 && ctl.got && memmem(ctl.got, ctl.got_len, "PASS a\xff\xff" "b\r\n", 10) != NULL, "a byte 255 in a password is doubled");

    fresh(NULL);
    have_tty = 1;
    tty_keys = PW "\r";
    r = run("-n 10.0.2.2", "user alice\nsecond-line\nquit\n");
    check(r == 2 && server_got("PASS " PW "\n") && !server_got("second-line") && said("no command second-line"),
          "from a script too the password comes from the terminal, never from the next line");

    {
        static const struct rule in_at_once[] = { { "USER", "230 no password needed\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(in_at_once);
        have_tty = 1;
        tty_keys = PW "\r";
        r = run("-n 10.0.2.2", "user alice\nquit\n");
        check(r == 0 && !server_got("PASS") && !strstr(tty_out, "Password"), "a 230 to USER: no password is asked");
    }
    {
        static const struct rule account[] = { { "USER", "332 account\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(account);
        r = run("-n 10.0.2.2", "user alice\nquit\n");
        check(r == 1 && said("account") && !server_got("PASS"), "a 332: refused, status 1");
    }
    {
        static const struct rule refused[] = { { "USER", "530 no\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(refused);
        r = run("10.0.2.2", "quit\n");
        check(r == 1 && !server_got("PASS"), "anonymous refused: status 1, and the session goes on");
        check(server_got("QUIT"), "... to its quit");
    }
    {
        static const struct rule asks[] = { { "USER", "331 mail\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(asks);
        have_tty = 1;
        r = run("10.0.2.2", "quit\n");
        check(r == 0 && server_got("PASS anonymous@surfos") && !strstr(tty_out, "Password"), "anonymous never asks for a password");
    }
}

/**** get ****/

static const struct rule get_ok[] = {
    { "RETR a.txt", "150 here\r\n", "hello\n", 0, "226 done\r\n", 0 },
    { 0, 0, 0, 0, 0, 0 }
};

static void test_get(void) {
    static char big[100000];
    int r;

    fresh(get_ok);
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 0 && file_is("a.txt", "hello\n") && logged("open a.txt excl\n") && logged("fsync a.txt\n") && file_count() == 1,
          "get into a new file: O_EXCL, synced, the bytes there");
    check(shown("a.txt: 6 bytes in"), "... and said");
    check(server_got("TYPE I\nSIZE a.txt\nEPSV\nRETR a.txt\n"), "... binary, EPSV first");

    fresh(get_ok);
    put_file("a.txt", "the old file");
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 0 && file_is("a.txt", "hello\n") && logged("open a.txt.tmp excl\n") && logged("rename a.txt.tmp a.txt\n") &&
          !logged("TRUNC") && file_count() == 1, "get over a file: through saveutil, never truncated");

    fresh(get_ok);
    r = run("10.0.2.2", "get a.txt b.txt\nquit\n");
    check(r == 0 && file_is("b.txt", "hello\n") && no_file("a.txt"), "get remote local");

    {
        static const struct rule sub[] = { { "RETR sub/deep/x.bin", "150 here\r\n", "z", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(sub);
        r = run("10.0.2.2", "get sub/deep/x.bin\nquit\n");
        check(r == 0 && file_is("x.bin", "z"), "a remote path's file lands under its last name");
        fresh(sub);
        r = run("10.0.2.2", "get sub/deep/\nquit\n");
        check(r == 2 && said("no last name") && !server_got("RETR"), "a path ending in / has no local name");
    }

    fresh(get_ok);
    r = run("10.0.2.2", "get a.txt -\nquit\n");
    check(r == 0 && !strcmp(out_buf, "hello\n") && file_count() == 0, "get remote - writes standard output");

    fresh(get_ok);
    put_dir("a.txt");
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && said("is a folder") && server_got("ABOR"), "a folder in the way: the transfer is aborted");

    {
        static const struct rule no[] = { { "RETR", "550 no such file\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(no);
        put_file("keep.txt", "mine");
        r = run("10.0.2.2", "get a.txt\nget keep.txt\nquit\n");
        check(r == 1 && said("a.txt: 550 no such file") && file_count() == 1 && file_is("keep.txt", "mine") && !logged("open"),
              "a 550: status 1, nothing made, nothing touched");
    }

    {   /* the data connection is reset */
        static const struct rule rst[] = { { "RETR", "150 here\r\n", "part of it", 0, 0, D_RST }, { 0, 0, 0, 0, 0, 0 } };
        fresh(rst);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 2 && no_file("a.txt") && logged("unlink a.txt\n") && !any_tmp(), "a reset partway: the new file is deleted again");
        fresh(rst);
        put_file("a.txt", "the old file");
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 2 && file_is("a.txt", "the old file") && !any_tmp(), "... and a file that was there stays as it was");
    }
    {   /* the data ends and no final reply comes: the control connection closes */
        static const struct rule gone[] = { { "RETR", "150 here\r\n", "all of it", 0, 0, C_CLOSE }, { 0, 0, 0, 0, 0, 0 } };
        fresh(gone);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 2 && no_file("a.txt") && !any_tmp(), "no final reply: not a file");
        fresh(gone);
        put_file("a.txt", "old");
        r = run("10.0.2.2", "get a.txt\n");
        check(r == 2 && file_is("a.txt", "old"), "... over one: it stays");
    }
    {   /* a negative final reply after all the data */
        static const struct rule bad[] = { { "RETR", "150 here\r\n", "looks whole", 0, "426 broken\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(bad);
        put_file("a.txt", "old");
        r = run("10.0.2.2", "get a.txt\nget b.txt\nquit\n");
        check(r == 1 && file_is("a.txt", "old") && no_file("b.txt") && !any_tmp() && said("426 broken"),
              "a 426 after the data: the old file stays, a new one goes");
    }
    {   /* no final reply at all, the control connection silent */
        static const struct rule mute[] = { { "RETR", "150 here\r\n", "all", 0, 0, C_MUTE }, { 0, 0, 0, 0, 0, 0 } };
        fresh(mute);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 2 && said("no reply came") && no_file("a.txt"), "a final reply that never comes: bounded, no file");
    }
    {   /* a data connection that stops */
        static const struct rule stuck[] = { { "RETR", "150 here\r\n", "some", 0, 0, D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(stuck);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 2 && said("no data came for 60 s") && no_file("a.txt") && server_got("ABOR"), "data that stops: 60 seconds, ABOR, no file");
    }
    {   /* the final reply comes before the last data */
        static const struct rule early[] = { { "RETR", "150 here\r\n", "hello\n", 0, "226 done\r\n", A_EARLY }, { 0, 0, 0, 0, 0, 0 } };
        fresh(early);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 0 && file_is("a.txt", "hello\n"), "a 226 before the data has been read: all of it is kept");
        fresh(early);
        max_read = 1;
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 0 && file_is("a.txt", "hello\n"), "... a byte at a time");
    }
    {   /* a negative reply while data still comes */
        static const struct rule early[] = { { "RETR", "150 here\r\n", "part", 0, "451 disk error\r\n", A_EARLY | D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(early);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 1 && no_file("a.txt") && said("451 disk error"), "a 451 during the transfer ends it at once");
    }
    {   /* a 2xx with no 1xx first */
        static const struct rule direct[] = { { "RETR", "226 here it is\r\n", "direct", 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(direct);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 0 && file_is("a.txt", "direct"), "a 226 with no 150: what the connection holds is read");
    }
    {   /* SIZE against what came */
        static const struct rule cut[] = { { "SIZE", "213 1000\r\n", 0, 0, 0, 0 }, { "RETR", "150 here\r\n", "short", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(cut);
        put_file("a.txt", "old");
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 2 && file_is("a.txt", "old") && said("only 5 of the 1000 bytes"), "fewer bytes than SIZE said, with a 226: not the file");
    }
    {
        static const struct rule grew[] = { { "SIZE", "213 3\r\n", 0, 0, 0, 0 }, { "RETR", "150 here\r\n", "longer", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(grew);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 0 && file_is("a.txt", "longer"), "a file that grew meanwhile is taken");
    }
    {
        static const struct rule huge[] = { { "SIZE", "213 5000000000\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(huge);
        r = run("10.0.2.2", "get a.txt\nquit\n");
        check(r == 2 && said("less than 4 GB") && !server_got("RETR"), "a file of 4 GB or more is refused before it starts");
    }
    {
        static const struct rule junk[] = { { "SIZE", "213 12x\r\n", 0, 0, 0, 0 }, { "RETR a", "150 here\r\n", "hi", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(junk);
        r = run("10.0.2.2", "get a\nquit\n");
        check(r == 0 && file_is("a", "hi"), "a SIZE answer that is no number counts for nothing");
    }

    /* the disk */
    fresh(get_ok);
    write_room = 0;
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && no_file("a.txt") && server_got("ABOR"), "no room for a new file: said, aborted");
    fresh(get_ok);
    write_room = 3;
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && no_file("a.txt") && !any_tmp() && server_got("ABOR"), "the disk full partway: the new file goes");
    fresh(get_ok);
    put_file("a.txt", "old");
    write_room = 3;
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && file_is("a.txt", "old") && !any_tmp(), "... over a file: it stays, the .tmp goes");

    fresh(get_ok);
    put_file("a.txt", "old");
    snprintf(fail_fsync, sizeof(fail_fsync), "a.txt.tmp");
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && file_is("a.txt", "old") && !any_tmp(), "fsync fails: the file stays");
    fresh(get_ok);
    snprintf(fail_fsync, sizeof(fail_fsync), "a.txt");
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && no_file("a.txt"), "fsync of a new file fails: it goes");

    fresh(get_ok);
    put_file("a.txt", "old");
    snprintf(fail_rename, sizeof(fail_rename), "a.txt.tmp");
    rename_err = ENOSPC;
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && file_is("a.txt", "old") && !any_tmp(), "a rename refused: the old file stays, the new one goes");
    fresh(get_ok);
    put_file("a.txt", "old");
    snprintf(fail_rename, sizeof(fail_rename), "a.txt.tmp");
    rename_err = EIO;
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && any_tmp() && said("check the disk"), "a rename that may have half happened: the .tmp is kept, as saveutil says");

    fresh(get_ok);
    put_file("a.txt", "x");
    put_file("a.txt.tmp", "x");
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && said("may share its data") && file_is("a.txt", "x") && file_is("a.txt.tmp", "x"),
          "a name beside it with its size and time (a half-done rename, maybe): both left alone");

    /* a big file, split */
    memset(big, 'b', sizeof(big));
    {
        struct rule b[2];
        memset(b, 0, sizeof(b));
        b[0].cmd = "RETR";
        b[0].reply = "150 here\r\n";
        b[0].data = big;
        b[0].data_len = sizeof(big);
        b[0].after = "226 done\r\n";
        fresh(b);
        max_read = 777;
        r = run("10.0.2.2", "get big\nquit\n");
        check(r == 0 && lookup("big") >= 0 && files[lookup("big")].len == sizeof(big), "100000 bytes in reads of 777");
    }

    /* text mode */
    {
        static const struct rule text[] = { { "RETR", "150 here\r\n", "a\r\nb\rc\r\n\r", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(text);
        r = run("10.0.2.2", "ascii\nget t\nquit\n");
        check(r == 0 && file_is("t", "a\nb\rc\n\r") && server_got("TYPE A") && !server_got("SIZE"), "ascii: CR LF becomes LF, a CR alone stays, also the last one");
        fresh(text);
        max_read = 1;
        r = run("10.0.2.2", "ascii\nget t\nquit\n");
        check(r == 0 && file_is("t", "a\nb\rc\n\r"), "... a byte at a time");
    }
    {
        static const struct rule again[] = { { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(again);
        r = run("10.0.2.2", "get a\nget b\nascii\nget c\nbinary\nget d\nquit\n");
        check(r == 0 && strstr(lines, "TYPE I\nSIZE a") && strstr(lines, "RETR b\nTYPE A\nEPSV\nRETR c\nTYPE I\nSIZE d"),
              "TYPE is sent when it changes");
    }
}

/**** data connections ****/

static void test_passive(void) {
    int r;
    {
        static const struct rule pasv[] = {
            { "EPSV", "500 what\r\n", 0, 0, 0, 0 },
            { "PASV", "227 Entering Passive Mode (10,0,2,2,19,136).\r\n", 0, 0, 0, 0 },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(pasv);
        r = run("10.0.2.2", "get a\nget b\nquit\n");
        check(r == 0 && file_is("a", "x") && file_is("b", "x") && last_port == 5000, "EPSV unknown: PASV, the port from its numbers");
        check(strstr(lines, "EPSV\nPASV\nRETR a\nSIZE b\nPASV\nRETR b") != NULL, "... and PASV alone from then on");
    }
    {
        static const struct rule elsewhere[] = {
            { "EPSV", "502 no\r\n", 0, 0, 0, 0 },
            { "PASV", "227 ok (192,168,9,9,19,136)\r\n", 0, 0, 0, 0 },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(elsewhere);
        r = run("10.0.2.2", "get a\nquit\n");
        check(r == 0 && !bad_connects && connects == 2 && file_is("a", "x"), "PASV naming another address: the data goes only to the server's own");
    }
    {
        static const struct rule bare[] = {
            { "EPSV", "502 no\r\n", 0, 0, 0, 0 },
            { "PASV", "227 =10,0,2,2,19,136\r\n", 0, 0, 0, 0 },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(bare);
        r = run("10.0.2.2", "get a\nquit\n");
        check(r == 0 && last_port == 5000, "PASV's numbers without brackets (RFC 1123)");
    }
    {
        static const struct rule low[] = { { "EPSV", "229 ok (|||21|)\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(low);
        r = run("10.0.2.2", "ls\nquit\n");
        check(r == 2 && !bad_connects && connects == 1 && said("port 21"), "EPSV naming a low port is refused");
    }
    {
        static const struct rule low[] = {
            { "EPSV", "502 no\r\n", 0, 0, 0, 0 },
            { "PASV", "227 ok (10,0,2,2,0,25)\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(low);
        r = run("10.0.2.2", "ls\nquit\n");
        check(r == 2 && !bad_connects && connects == 1 && said("port 25"), "so is PASV");
    }
    {
        static const struct rule junk[] = {
            { "EPSV", "229 ok (|||99999|)\r\n", 0, 0, 0, 0 },
            { "PASV", "227 ok (10,0,2,2,300,1)\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(junk);
        r = run("10.0.2.2", "ls\nquit\n");
        check(r == 2 && connects == 1 && server_got("EPSV\nPASV\n") && server_got("QUIT"), "both replies unreadable: status 2, the session goes on");
    }
    {
        static const struct rule odd[] = {
            { "EPSV", "502 no\r\n", 0, 0, 0, 0 }, { "PASV", "200 well, no\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(odd);
        r = run("10.0.2.2", "ls\nquit\n");
        check(r == 2 && said("no passive reply: 200 well, no") && connects == 1, "a 2xx to PASV that is no passive reply is said");
    }
    {
        static const struct rule retry[] = {
            { "EPSV", "229 ok (junk)\r\n", 0, 0, 0, 0 },
            { "PASV", "227 ok (10,0,2,2,19,136)\r\n", 0, 0, 0, 0 },
            { "NLST", "150 here\r\n", "a\r\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(retry);
        r = run("10.0.2.2", "ls\nls\nquit\n");
        check(r == 0 && strstr(lines, "EPSV\nPASV\nNLST\nEPSV\nPASV\nNLST") != NULL, "an EPSV reply that cannot be read: PASV, and EPSV is asked again next time");
    }
    fresh(get_ok);
    refuse_data = 1;
    r = run("10.0.2.2", "get a.txt\nquit\n");
    check(r == 2 && said("the data connection") && no_file("a.txt") && !server_got("RETR"), "a data connection refused: nothing is asked for");
}

/**** reget ****/

static void test_reget(void) {
    int r;
    static const struct rule go[] = {
        { "SIZE", "213 11\r\n", 0, 0, 0, 0 },
        { "REST 5", "350 ok\r\n", 0, 0, 0, 0 },
        { "RETR", "150 here\r\n", " more\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };

    fresh(go);
    put_file("f", "hello");
    r = run("10.0.2.2", "reget f\nquit\n");
    check(r == 0 && file_is("f", "hello more\n") && logged("open f append\n") && server_got("EPSV\nREST 5\nRETR f\n"),
          "reget goes on from the file's end: REST just before RETR");

    {
        static const struct rule none[] = { { "SIZE", "213 4\r\n", 0, 0, 0, 0 }, { "RETR", "150 here\r\n", "good", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(none);
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 0 && file_is("f", "good") && !server_got("REST"), "no file yet: it starts from nothing");
    }
    {
        static const struct rule whole[] = { { "SIZE", "213 5\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(whole);
        put_file("f", "hello");
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 0 && file_is("f", "hello") && shown("whole already") && !server_got("RETR"), "a whole file: nothing to do");
        fresh(whole);
        put_file("f", "hello there");
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 2 && file_is("f", "hello there") && said("longer") && !server_got("RETR"), "a local file longer than the server's: left alone");
    }
    {
        static const struct rule refused[] = { { "SIZE", "213 11\r\n", 0, 0, 0, 0 }, { "REST", "502 no\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(refused);
        put_file("f", "hello");
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 1 && file_is("f", "hello") && !server_got("RETR") && said("will not go on"), "REST refused: no RETR, which would add the whole file");
    }
    {   /* the trap: a server that says 350 and sends the whole file */
        static const struct rule fool[] = {
            { "SIZE", "213 11\r\n", 0, 0, 0, 0 }, { "REST", "350 ok\r\n", 0, 0, 0, 0 },
            { "RETR", "150 here\r\n", "hello more\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(fool);
        put_file("f", "hello");
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 2 && file_is("f", "hello") && logged("ftruncate f 5") && said("more than it said"), "a server that ignores REST: what it added is cut off again");
    }
    {
        static const struct rule cut[] = {
            { "SIZE", "213 11\r\n", 0, 0, 0, 0 }, { "REST", "350 ok\r\n", 0, 0, 0, 0 },
            { "RETR", "150 here\r\n", " mo", 0, 0, D_RST }, { 0, 0, 0, 0, 0, 0 } };
        fresh(cut);
        put_file("f", "hello");
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 2 && file_is("f", "hello mo") && shown("reget goes on"), "cut short again: what came stays, for the next reget");
    }
    {
        static const struct rule cut[] = {
            { "SIZE", "213 11\r\n", 0, 0, 0, 0 },
            { "RETR", "150 here\r\n", "hello", 0, 0, D_RST }, { 0, 0, 0, 0, 0, 0 } };
        fresh(cut);
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 2 && file_is("f", "hello") && !logged("unlink"), "started from nothing and cut short: reget keeps it too, where get would delete it");
    }
    {
        static const struct rule shorter[] = {
            { "SIZE", "213 11\r\n", 0, 0, 0, 0 }, { "REST", "350 ok\r\n", 0, 0, 0, 0 },
            { "RETR", "150 here\r\n", " m", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(shorter);
        put_file("f", "hello");
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 2 && file_is("f", "hello m") && said("only 2 of the 6 bytes"), "a 226 too soon: kept, and said");
    }
    fresh(go);
    r = run("10.0.2.2", "ascii\nreget f\nquit\n");
    check(r == 2 && said("binary") && !server_got("RETR"), "reget refuses ascii mode");
    fresh(go);
    r = run("10.0.2.2", "reget f -\nquit\n");
    check(r == 2 && said("- is none"), "reget wants a file");
    fresh(go);
    put_dir("f");
    r = run("10.0.2.2", "reget f\nquit\n");
    check(r == 2 && said("is a folder"), "a folder in the way");
}

/**** put ****/

static void test_put(void) {
    int r;
    static const struct rule take[] = { { "STOR", "150 send\r\n", 0, 0, "226 stored\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };

    fresh(take);
    put_file("up.txt", "line one\nline two\n");
    r = run("10.0.2.2", "put up.txt\nquit\n");
    check(r == 0 && dat.got_len == 18 && !memcmp(dat.got, "line one\nline two\n", 18) && server_got("STOR up.txt") && shown("up.txt: 18 bytes in"),
          "put: the file's bytes as they are");

    fresh(take);
    put_file("up.txt", "line one\nline two\r\nend");
    r = run("10.0.2.2", "ascii\nput up.txt remote.txt\nquit\n");
    check(r == 0 && dat.got_len == 23 && !memcmp(dat.got, "line one\r\nline two\r\nend", 23) && server_got("STOR remote.txt"),
          "ascii: LF becomes CR LF, a CR LF stays");

    fresh(take);
    put_file("up.txt", "line one\nline two\n");
    max_write = 1;
    have_tty = 1;
    tty_keys = PW "\r";
    r = run("-n 10.0.2.2", "user alice\nascii\nput up.txt\nquit\n");
    check(r == 0 && server_got("PASS " PW "\nTYPE A\nEPSV\nSTOR up.txt\n") && dat.got_len == 20 && !memcmp(dat.got, "line one\r\nline two\r\n", 20),
          "writes that queue a byte at a time: commands, a password and the data arrive whole");

    fresh(take);
    r = run("10.0.2.2", "put nothing\nquit\n");
    check(r == 2 && said("nothing") && !server_got("STOR"), "a file that is not there");

    fresh(take);
    put_dir("d");
    r = run("10.0.2.2", "put d\nquit\n");
    check(r == 2 && said("is a folder") && !server_got("STOR"), "a folder");

    fresh(take);
    put_file("/hda1/sub/x", "y");
    r = run("10.0.2.2", "put /hda1/sub/x\nquit\n");
    check(r == 0 && server_got("STOR x\n"), "the remote name is the last name of the local path");

    {
        static const struct rule no[] = { { "STOR", "553 refused\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(no);
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 1 && said("553 refused") && !dat.got_len, "a refusal: status 1, nothing sent");
    }
    {
        static const struct rule stop[] = { { "STOR", "150 send\r\n", 0, 0, "552 disk full\r\n", A_EARLY | D_NONE }, { 0, 0, 0, 0, 0, 0 } };
        fresh(stop);
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 1 && said("552 disk full"), "a 552 during the upload ends it");
    }
    {
        static const struct rule stall[] = { { "STOR", "150 send\r\n", 0, 0, 0, D_FULL }, { 0, 0, 0, 0, 0, 0 } };
        fresh(stall);
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 2 && said("took no byte for 60 s") && server_got("ABOR"), "a server that stops taking data: 60 seconds, ABOR");
    }
    {
        static const struct rule mute[] = { { "STOR", "150 send\r\n", 0, 0, 0, C_MUTE }, { 0, 0, 0, 0, 0, 0 } };
        fresh(mute);
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 2 && said("no reply came"), "no final reply: not stored");
    }
    {
        static const struct rule done[] = { { "STOR", "226 thanks\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(done);
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 2 && said("before the data"), "a 226 before any data is no upload");
    }
}

/**** listings ****/

static void test_list(void) {
    static char big[400000];
    int r;
    {
        static const struct rule l[] = { { "LIST", "150 here\r\n", "-rw-r--r-- 1 a a 5 Oct 1 12:00 \x1b[31mred\x1b[0m\r\nplain\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 0 && !strchr(out_buf, 0x1b) && shown("?[31mred?[0m\n") && shown("plain\n"), "a listing's escape sequences are shown as text");
        fresh(l);
        max_read = 1;
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 0 && shown("plain\n"), "... a byte at a time");
    }
    {
        static const struct rule l[] = { { "NLST sub", "150 here\r\n", "last line no end", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        r = run("10.0.2.2", "ls sub\nquit\n");
        check(r == 0 && shown("last line no end\n"), "a last line with no end is shown");
    }
    {
        static const struct rule l[] = { { "NLST", "550 no\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        r = run("10.0.2.2", "ls nowhere\nquit\n");
        check(r == 1 && said("nowhere: 550 no"), "a refused listing: status 1");
    }
    {
        struct rule l[2];
        size_t i;
        for(i = 0; i < sizeof(big); i++) big[i] = (i % 3000 == 2999) ? '\n' : 'n';
        memset(l, 0, sizeof(l));
        l[0].cmd = "NLST";
        l[0].reply = "150 here\r\n";
        l[0].data = big;
        l[0].data_len = sizeof(big);
        l[0].after = "226 done\r\n";
        fresh(l);
        r = run("10.0.2.2", "ls\nquit\n");
        check(r == 0 && said("too long: left out") && out_len == 1001, "lines of 3000 bytes are left out; a last one of 1000 is shown whole");
    }
    {
        static const struct rule l[] = { { "LIST", "150 here\r\n", "a\r\n", 0, 0, D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 2 && said("no data came") && shown("a\n"), "a listing that stops: bounded");
    }
}

/**** mget and mput ****/

static void test_mget(void) {
    int r;
    {
        static const struct rule m[] = {
            { "NLST", "150 here\r\n", "a.txt\r\nb.txt\r\nc.bin\r\n.hidden.txt\r\n", 0, "226 done\r\n", 0 },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        r = run("10.0.2.2", "mget *.txt\nquit\n");
        check(r == 0 && file_is("a.txt", "x") && file_is("b.txt", "x") && no_file("c.bin") && no_file(".hidden.txt") && file_count() == 2,
              "mget takes the names its pattern matches, here, and no name it does not; * passes no leading dot");
    }
    {
        static const struct rule m[] = {
            { "NLST", "150 here\r\n",
              "../../etc/passwd.txt\r\n/etc/shadow.txt\r\nsub/../../up.txt\r\n..\r\n.\r\n..\\..\\win.txt\r\nnice.txt\r\nspace .txt\r\n"
              "c:evil.txt\r\nx\x1b[2J.txt\r\n-rf.txt\r\ndots.txt.\r\nblank.txt \r\na|b.txt\r\n", 0, "226 done\r\n", 0 },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        int i;
        fresh(m);
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 1, "names that are not safe: status 1");
        check(file_is("nice.txt", "x") && file_is("space .txt", "x"), "the good ones come");
        check(no_file("..\\..\\win.txt") && no_file("c:evil.txt") && no_file("-rf.txt") && no_file("dots.txt.") && no_file("blank.txt ") && no_file("a|b.txt"),
              "the others do not");
        for(i = 0; i < FILES; i++) {
            if(files[i].used && (strchr(files[i].name, '/') || strchr(files[i].name, 0x1b))) check(0, "a local name with a / or an escape in it");
        }
        check(!strchr(err_buf, 0x1b) && said("left out: x?[2J.txt"), "an unsafe name is shown cleaned");
        check(server_got("RETR passwd.txt\n") && server_got("RETR shadow.txt\n") && server_got("RETR up.txt\n"),
              "a listed path counts only by its last name: nothing climbs, on either side");
        check(!server_got("RETR ../") && !server_got("RETR /") && !server_got("RETR ."), "... and nothing else is asked for");
    }
    {
        static const struct rule m[] = {
            { "NLST sub", "150 here\r\n", "sub/a.txt\r\n", 0, "226 done\r\n", 0 },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        r = run("10.0.2.2", "mget sub/*.txt\nquit\n");
        check(r == 0 && server_got("RETR sub/a.txt") && file_is("a.txt", "x"), "a folder in the pattern is asked of the server, and stays out of the local name");
    }
    {
        static const struct rule m[] = {
            { "NLST", "150 here\r\n", "a\r\nb\r\n", 0, "226 done\r\n", 0 },
            { "RETR", "150 here\r\n", "new", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        put_file("a", "mine");
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 1 && file_is("a", "mine") && file_is("b", "new") && said("a is there already"), "in a script mget replaces nothing");
        fresh(m);
        put_file("a", "mine");
        put_file("b", "mine");
        stdin_tty = 1;
        r = run("10.0.2.2", "mget *\nn\ny\nquit\n");
        check(r == 1 && file_is("a", "mine") && file_is("b", "new") && shown("replace a?") && shown("replace b?"), "at a terminal it asks");
        fresh(m);
        put_file("a", "mine");
        put_file("b", "mine");
        stdin_tty = 1;
        r = run("10.0.2.2", "mget *\na\nquit\n");
        check(r == 0 && file_is("a", "new") && file_is("b", "new") && !shown("replace b?"), "a is yes to the rest");
        fresh(m);
        put_file("a", "mine");
        put_file("b", "mine");
        stdin_tty = 1;
        r = run("10.0.2.2", "mget *\nq\nquit\n");
        check(r == 0 && file_is("a", "mine") && file_is("b", "mine") && !server_got("RETR"), "q stops");
    }
    {
        static char list[200000];
        struct rule m[3];
        size_t n = 0;
        int i;
        for(i = 0; i < 5000; i++) n += (size_t)snprintf(list + n, sizeof(list) - n, "n%d\r\n", i);
        memset(m, 0, sizeof(m));
        m[0].cmd = "NLST";
        m[0].reply = "150 here\r\n";
        m[0].data = list;
        m[0].data_len = (int)n;
        m[0].after = "226 done\r\n";
        m[1].cmd = "RETR";
        m[1].reply = "550 no\r\n";
        fresh(m);
        r = run("10.0.2.2", "mget nothing*\nquit\n");
        check(r == 2 && said("too long"), "more names than are kept: said, status 2");
    }
    {
        static const struct rule m[] = { { "NLST", "150 here\r\n", "a\r\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        r = run("10.0.2.2", "mget z*\nquit\n");
        check(r == 1 && said("no name matches"), "nothing matches: status 1");
    }
    {
        static const struct rule m[] = {
            { "NLST", "150 here\r\n", "with\0nul.txt\r\ngood.txt\r\n", 22, "226 done\r\n", 0 },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 1 && file_is("good.txt", "x") && file_count() == 1, "a NUL in a name: refused, not cut short");
    }
    {
        static const struct rule m[] = {
            { "NLST", "150 here\r\n", "a\r\nb\r\n", 0, "226 done\r\n", 0 },
            { "RETR a", "150 here\r\n", "x", 0, 0, C_CLOSE }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 2 && no_file("a") && no_file("b"), "the connection lost in mget: it stops");
    }
}

static void test_mput(void) {
    int r;
    static const struct rule take[] = { { "STOR", "150 send\r\n", 0, 0, "226 stored\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
    fresh(take);
    put_file("A.TXT", "1");
    put_file("b.txt", "2");
    put_file("c.bin", "3");
    put_dir("d.txt");
    r = run("10.0.2.2", "mput *.txt\nquit\n");
    check(r == 0 && server_got("STOR A.TXT") && server_got("STOR b.txt") && !server_got("STOR c.bin") && !server_got("STOR d.txt"),
          "mput: files that match, ignoring case as FAT does, and no folder");
    fresh(take);
    r = run("10.0.2.2", "mput *.txt\nquit\n");
    check(r == 1 && said("no file matches"), "nothing matches: status 1");
    fresh(take);
    r = run("10.0.2.2", "mput /nowhere/*\nquit\n");
    check(r == 2 && said("/nowhere"), "no such folder");
    fresh(take);
    put_dir("/hda1/s");
    put_file("/hda1/s/x", "1");
    r = run("10.0.2.2", "mput /hda1/s/*\nquit\n");
    check(r == 0 && server_got("STOR x\n"), "a local folder in the pattern stays out of the remote name");
}

/**** Ctrl+C ****/

static void test_interrupt(void) {
    static char big[50000];
    struct rule b[2];
    int r;
    memset(b, 0, sizeof(b));
    memset(big, 'b', sizeof(big));
    b[0].cmd = "RETR";
    b[0].reply = "150 here\r\n";
    b[0].data = big;
    b[0].data_len = sizeof(big);
    b[0].after = "226 done\r\n";

    fresh(b);
    max_read = 1000;
    stdin_tty = 1;
    intr_after_bytes = 3000;
    r = run("10.0.2.2", "get big\nstatus\nquit\n");
    check(no_file("big") && !any_tmp() && server_got("ABOR") && server_got("QUIT"), "Ctrl+C in a get: ABOR, no file, and the session goes on");
    check(r == 2 && shown("Connected to 10.0.2.2"), "... still connected");
    check(!catching, "Ctrl+C is ftp's only while a command runs");

    fresh(b);
    max_read = 1000;
    put_file("big", "old");
    stdin_tty = 1;
    intr_after_bytes = 3000;
    r = run("10.0.2.2", "get big\nquit\n");
    check(file_is("big", "old") && !any_tmp(), "... over a file: it stays");

    fresh(b);
    max_read = 1000;
    intr_after_bytes = 3000;
    r = run("10.0.2.2", "get big\nls\nquit\n");
    check(r == 130 && !server_got("NLST") && no_file("big"), "in a script Ctrl+C ends ftp: status 130");

    {
        static const struct rule hang[] = { { "CWD", 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(hang);
        stdin_tty = 1;
        intr_at_ms = 5000;
        r = run("10.0.2.2", "cd x\nstatus\nquit\n");
        check(r == 2 && shown("Not connected") && said("stopped by Ctrl+C"), "Ctrl+C waiting for a reply: there is no way back in step, the connection goes");
    }
    {   /* Ctrl+C between RETR and its 150: the same as in the transfer, and the server's 150 still comes first */
        static const struct rule slow[] = {
            { "RETR", 0, 0, 0, 0, 0 }, { "ABOR", "150 late\r\n426 stopped\r\n226 abort done\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(slow);
        stdin_tty = 1;
        intr_at_ms = 5000;
        put_file("a", "old");
        r = run("10.0.2.2", "get a\nstatus\nquit\n");
        check(r == 2 && server_got("ABOR") && shown("stopped\n") && shown("Connected to") && file_is("a", "old") && !any_tmp(), "Ctrl+C before the 150: ABOR, and the session goes on");
        check(!said("is not FTP"), "... and its replies stay in step");
    }
    {
        static const struct rule slow[] = { { "STOR", 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(slow);
        stdin_tty = 1;
        intr_at_ms = 5000;
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nstatus\nquit\n");
        check(r == 2 && server_got("ABOR") && shown("Connected to"), "the same for an upload");
    }
    {   /* ABOR's replies, all preliminary, without end */
        static const struct rule endless[] = {
            { "RETR", 0, 0, 0, 0, 0 },
            { "ABOR", "150 a\r\n151 a\r\n152 a\r\n153 a\r\n154 a\r\n155 a\r\n156 a\r\n157 a\r\n158 a\r\n159 a\r\n"
                      "160 a\r\n161 a\r\n162 a\r\n163 a\r\n164 a\r\n165 a\r\n166 a\r\n167 a\r\n", 0, 0, 0, 0 },
            { 0, 0, 0, 0, 0, 0 } };
        fresh(endless);
        stdin_tty = 1;
        intr_at_ms = 5000;
        r = run("10.0.2.2", "get a\nstatus\nquit\n");
        check(r == 2 && said("no end to its replies") && shown("Not connected"), "answers to ABOR without end: the connection goes");
    }
    {
        static const struct rule noabor[] = {
            { "ABOR", 0, 0, 0, 0, 0 }, { "NOOP", 0, 0, 0, 0, 0 }, { "RETR", "150 here\r\n", "some", 0, 0, D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(noabor);
        stdin_tty = 1;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\nstatus\nquit\n");
        check(no_file("a") && said("no answer to NOOP after ABOR") && shown("Not connected"), "a server that does not answer ABOR is left");
    }
}

/**** the commands ****/

static void test_commands(void) {
    int r;
    {
        static const struct rule simple[] = {
            { "CWD ok", "250 there\r\n", 0, 0, 0, 0 }, { "CWD no", "550 no\r\n", 0, 0, 0, 0 },
            { "PWD", "257 \"/a\"\"b\" is the folder\r\n", 0, 0, 0, 0 },
            { "DELE x", "250 gone\r\n", 0, 0, 0, 0 }, { "MKD x", "257 \"x\" made\r\n", 0, 0, 0, 0 }, { "RMD x", "250 gone\r\n", 0, 0, 0, 0 },
            { "RNFR a", "350 ok\r\n", 0, 0, 0, 0 }, { "RNTO b", "250 done\r\n", 0, 0, 0, 0 }, { "RNFR n", "550 no\r\n", 0, 0, 0, 0 },
            { "SIZE s", "213 1234\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(simple);
        r = run("10.0.2.2", "cd ok\npwd\ndelete x\nmkdir x\nrmdir x\nrename a b\nsize s\nquit\n");
        check(r == 0 && server_got("CWD ok\nPWD\nDELE x\nMKD x\nRMD x\nRNFR a\nRNTO b\nTYPE I\nSIZE s\n"), "the simple commands");
        check(shown("250 there") && shown("/a\"b\n") && shown("s: 1234 bytes"), "... and what they say");
        fresh(simple);
        r = run("10.0.2.2", "cd no\nquit\n");
        check(r == 1 && said("no: 550 no"), "a refusal: on standard error, status 1");
        fresh(simple);
        r = run("10.0.2.2", "rename n b\nquit\n");
        check(r == 1 && !server_got("RNTO"), "RNFR refused: no RNTO");
    }
    fresh(NULL);
    stdin_tty = 1;
    r = run("10.0.2.2", "get\nput\nmkdir\nrename a\nfoo\nhelp nothing\nquit\n");
    check(r == 2 && said("usage: get") && said("usage: put") && said("no command foo") && said("no command nothing"), "mistakes in commands");

    fresh(NULL);
    r = run("10.0.2.2", "# a comment\n\n   \nhelp\nhelp get\nquit\n");
    check(r == 0 && shown("reget remote [local]") && shown("get remote [local]"), "comments and blank lines are nothing; help");

    {
        static const struct rule q[] = { { "RETR my file", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(q);
        r = run("10.0.2.2", "get \"my file\" \"my copy\"\nquit\n");
        check(r == 0 && file_is("my copy", "x"), "double quotes keep a name with blanks whole");
    }
    fresh(NULL);
    r = run("10.0.2.2", "get \"not closed\nquit\n");
    check(r == 2 && said("a quote is not closed") && !server_got("RETR"), "a quote not closed");
    fresh(NULL);
    r = run("10.0.2.2", "ls a b c d e f g h i j k l m n o p q r s t u v w x y z 0 1 2 3 4 5 6 7\nquit\n");
    check(r == 2 && said("too many words"), "too many words");

    {
        static const struct rule again[] = { { "USER", "230 in\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(again);
        r = run("10.0.2.2", "open 10.0.2.2 2121\nquit\n");
        check(r == 0 && server_got("QUIT\n"), "open while connected ends the session first");
    }

    fresh(NULL);
    r = run("10.0.2.2", "");
    check(r == 0 && server_got("QUIT"), "the end of the input is a quit");

    fresh(NULL);
    put_dir("/mnt");
    r = run("", "lcd /mnt\nstatus\nlcd /nowhere\nquit\n");
    check(r == 2 && shown("The local folder is /mnt") && said("/nowhere"), "lcd");

    /* a reply nobody asked for, before a command */
    {
        static const struct rule idle[] = { { "PASS", "230 in\r\n421 idle\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(idle);
        r = run("10.0.2.2", "pwd\nquit\n");
        check(r == 2 && said("going away") && !server_got("PWD"), "a 421 that came on its own: the connection is over, nothing is sent");
    }
    {
        static const struct rule extra[] = { { "PASS", "230 in\r\n200 stray\r\n", 0, 0, 0, 0 }, { "PWD", "257 \"/x\"\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(extra);
        r = run("10.0.2.2", "pwd\nquit\n");
        check(r == 0 && shown("/x\n"), "a stray reply is not taken for the next command's");
    }

    /* names that would make two commands */
    {
        static const struct rule m[] = { { "NLST", "150 here\r\n", "a\rDELE b\r\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 1 && !server_got("RETR") && !server_got("DELE"), "a name with a CR in it never reaches the control connection");
    }
    fresh(NULL);
    r = run("10.0.2.2", "cd \"a\tb\"\nquit\n");
    check(r == 2 && said("control character") && !server_got("CWD"), "a control character in an argument is refused");
}

/**** what coverage showed was missing ****/

static void test_more_hostile(void) {
    int r;
    {
        static const struct rule endless[] = {
            { "CWD", "150 a\r\n151 a\r\n152 a\r\n153 a\r\n154 a\r\n155 a\r\n156 a\r\n157 a\r\n158 a\r\n159 a\r\n"
                     "160 a\r\n161 a\r\n162 a\r\n163 a\r\n164 a\r\n165 a\r\n166 a\r\n167 a\r\n168 a\r\n169 a\r\n", 0, 0, 0, 0 },
            { 0, 0, 0, 0, 0, 0 } };
        fresh(endless);
        r = run("10.0.2.2", "cd x\nquit\n");
        check(r == 2 && said("no end to its replies"), "preliminary replies without end: dropped at 16");
    }
    {
        static const struct rule endless[] = {
            { "RETR", "150 here\r\n", "x", 0,
              "150 a\r\n151 a\r\n152 a\r\n153 a\r\n154 a\r\n155 a\r\n156 a\r\n157 a\r\n158 a\r\n159 a\r\n"
              "160 a\r\n161 a\r\n162 a\r\n163 a\r\n164 a\r\n165 a\r\n166 a\r\n167 a\r\n168 a\r\n169 a\r\n", 0 },
            { 0, 0, 0, 0, 0, 0 } };
        fresh(endless);
        r = run("10.0.2.2", "get a\nquit\n");
        check(r == 2 && said("no end to its replies") && no_file("a"), "... and after the data: no file");
    }
    {
        static const struct rule stuck[] = { { "PWD", "257 \"/\"\r\n", 0, 0, 0, C_FULL }, { 0, 0, 0, 0, 0, 0 } };
        fresh(stuck);
        stdin_tty = 1;
        r = run("10.0.2.2", "pwd\ncd x\nstatus\nquit\n");
        check(r == 2 && said("takes no command") && shown("Not connected") && now_ms >= 30000, "a server that stops reading commands: 30 seconds, then the connection goes");
    }
    {
        static const struct rule junk[] = { { "RETR", "150 here\r\n", "some", 0, "this is no reply\r\n", A_EARLY | D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(junk);
        r = run("10.0.2.2", "get a\nquit\n");
        check(r == 2 && said("not FTP") && no_file("a"), "something that is no reply during a transfer: the connection goes, no file");
    }
    {
        static const struct rule more[] = { { "RETR", "150 here\r\n", "hello", 0, "125 going on\r\n226 done\r\n", A_EARLY }, { 0, 0, 0, 0, 0, 0 } };
        fresh(more);
        max_read = 1;
        r = run("10.0.2.2", "get a\nquit\n");
        check(r == 0 && file_is("a", "hello"), "a second 1xx during the transfer, a byte at a time");
    }
    {
        static const struct rule gone[] = { { "RETR", "150 here\r\n", "some", 0, 0, C_CLOSE | D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(gone);
        put_file("a", "old");
        r = run("10.0.2.2", "get a\nquit\n");
        check(r == 2 && file_is("a", "old") && !any_tmp() && said("the server closed it"), "the control connection closes during the transfer");
    }
    fresh(get_ok);
    r = run("10.0.2.2", "get a.txt /dev/null\nquit\n");
    check(r == 0 && file_count() == 0 && logged("open /dev/null\n"), "a device is written, not replaced");

    {
        static const struct rule late[] = { { "STOR", "150 send\r\n", 0, 0, "552 disk full\r\n", A_EARLY | D_NONE }, { 0, 0, 0, 0, 0, 0 } };
        fresh(late);
        max_read = 1;
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 1 && said("552 disk full"), "a 552 that comes apart from the 150, during the upload");
    }
    {
        static const struct rule cut[] = { { "STOR", "150 send\r\n", 0, 0, 0, D_RST | D_FULL | C_MUTE }, { 0, 0, 0, 0, 0, 0 } };
        fresh(cut);
        put_file("up", "x");
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 2 && said("before the end") && server_got("ABOR"), "the data connection reset during an upload");
    }

    /* Ctrl+C in mget and mput: the rest of them does not run */
    {
        static char big[20000];
        struct rule m[3];
        memset(big, 'b', sizeof(big));
        memset(m, 0, sizeof(m));
        m[0].cmd = "NLST";
        m[0].reply = "150 here\r\n";
        m[0].data = "a\r\nb\r\n";
        m[0].after = "226 done\r\n";
        m[1].cmd = "RETR";
        m[1].reply = "150 here\r\n";
        m[1].data = big;
        m[1].data_len = sizeof(big);
        m[1].after = "226 done\r\n";
        fresh(m);
        max_read = 1000;
        stdin_tty = 1;
        intr_after_bytes = 3000;
        r = run("10.0.2.2", "mget *\nstatus\nquit\n");
        check(no_file("a") && no_file("b") && !server_got("RETR b") && shown("Connected to"), "Ctrl+C in mget: the rest is left, the session goes on");
        fresh(m);
        max_read = 1000;
        intr_after_bytes = 3000;
        r = run("10.0.2.2", "mget *\nls\nquit\n");
        check(r == 130 && !server_got("RETR b"), "... and a script ends, status 130");
    }
    {
        static const struct rule hang[] = { { "STOR", "150 send\r\n", 0, 0, 0, D_FULL }, { 0, 0, 0, 0, 0, 0 } };
        fresh(hang);
        put_file("a", "1");
        put_file("b", "2");
        put_file("c.x", "3");
        intr_at_ms = 9000;
        r = run("10.0.2.2", "mput a b c.x\nls\nquit\n");
        check(r == 130 && !server_got("STOR b") && !server_got("STOR c.x") && !server_got("NLST"), "Ctrl+C in mput: the same, and the patterns after it do not run");
    }
    {   /* Ctrl+C during mget's listing: a list cut short is no list to go by */
        static const struct rule m[] = {
            { "NLST", "150 here\r\n", "a\r\nb\r\n", 0, 0, D_OPEN },
            { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        intr_at_ms = 9000;
        r = run("10.0.2.2", "mget * *.x\nls\nquit\n");
        check(r == 130 && !server_got("RETR") && file_count() == 0 && strstr(lines, "NLST") && !strstr(strstr(lines, "NLST") + 4, "NLST"),
              "Ctrl+C in mget's listing: nothing is fetched from what came, and the next pattern is not listed");
    }
    {   /* a Ctrl+C that lands between the transfer's poll and its read of the control connection: told once, so that read
           must pass it on, or it is used up and the transfer goes on */
        static const struct rule talk[] = { { "RETR", "150 here\r\n", "some", 0, "125 still here\r\n", A_EARLY | D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(talk);
        max_read = 1;                   /* the 125 is left to read when the transfer begins */
        stdin_tty = 1;
        intr_with_ctl = 1;
        r = run("10.0.2.2", "get a\nstatus\nquit\n");
        check(shown("stopped\n") && server_got("ABOR") && !said("no data came") && no_file("a") && shown("Connected to"),
              "a Ctrl+C just before the control connection is read stops the transfer, and the session goes on");
    }
    {   /* a line too long is not run on what is left of it */
        static char longline[1400];
        memset(longline, 'x', sizeof(longline));
        memcpy(longline, "delete ", 7);
        longline[sizeof(longline) - 2] = '\n';
        longline[sizeof(longline) - 1] = 0;
        fresh(NULL);
        r = run("10.0.2.2", longline);
        check(r == 2 && said("too long: nothing was done") && !server_got("DELE"), "a line cut by its length is refused, not run");
    }

    fresh(get_ok);
    r = run("10.0.2.2", "put sub/\nquit\n");
    check(r == 2 && said("has no last name") && !server_got("STOR"), "put of a path with no last name");
    fresh(get_ok);
    r = run("10.0.2.2", "mget sub/\nquit\n");
    check(r == 2 && said("no name to match"), "mget of a folder alone");
    fresh(get_ok);
    r = run("10.0.2.2", "mput sub/\nquit\n");
    check(r == 2 && said("no name to match"), "mput of a folder alone");

    {
        static const struct rule noterm[] = { { "SIZE", "213 12x\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(noterm);
        r = run("10.0.2.2", "size a\nquit\n");
        check(r == 2 && said("no size"), "size: an answer that is no number");
    }
}

/**** the review of 69203a8: each of these failed before its fix ****/

static void test_review(void) {
    int r;

    /* 2: standard output holds only what was asked for */
    fresh(get_ok);
    r = run("10.0.2.2", "get a.txt -\nquit\n");
    check(r == 0 && !strcmp(out_buf, "hello\n"), "get remote -: standard output holds the file's bytes and nothing else");
    check(said("220 hello") && said("221 bye"), "... and what the server says goes to standard error");
    {
        static const struct rule l[] = { { "LIST", "150 here\r\n", "a listing\r\n", 0, "226 done\r\n", 0 }, { "PWD", "257 \"/x\"\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        r = run("10.0.2.2", "pwd\nbinary\nstatus\nhelp get\ndir\nquit\n");
        check(r == 0 && !strcmp(out_buf, "a listing\n"), "standard output holds a listing and no status line");
    }
    fresh(get_ok);
    stdin_tty = 1;
    r = run("10.0.2.2", "quit\n");
    check(r == 0 && !out_buf[0] && said("ftp> "), "the prompt goes to standard error too");

    /* 4: a script stops at its first failure */
    {
        static const struct rule no[] = { { "CWD", "550 no such folder\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(no);
        r = run("10.0.2.2", "cd backup\ndelete keep.txt\nquit\n");
        check(r == 1 && !server_got("DELE") && server_got("QUIT") && said("the rest of the script"), "cd refused in a script: no delete, status 1, and it is said");
        fresh(no);
        stdin_tty = 1;
        r = run("10.0.2.2", "cd backup\ndelete keep.txt\nquit\n");
        check(server_got("DELE keep.txt"), "at a terminal the next command is the user's to give");
    }
    fresh(NULL);
    r = run("10.0.2.2", "lcd /nowhere\nmget *\nquit\n");
    check(r == 2 && !server_got("NLST"), "lcd failing in a script: no mget into the wrong folder");
    {
        static const struct rule no[] = { { "USER", "530 no\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(no);
        r = run("10.0.2.2", "delete x\nquit\n");
        check(r == 1 && !server_got("DELE"), "a failed login at the start stops the script too");
    }

    /* 5: a Ctrl+C outside any wait */
    {
        static const struct rule big[] = { { "RETR", "150 here\r\n", "bytes", 0, "226 done\r\n", 0 }, { "DELE", "250 gone\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(big);
        intr_on_fsync = 1;
        r = run("10.0.2.2", "get big\ndelete big\nquit\n");
        check(r == 130 && !server_got("DELE") && file_is("big", "bytes"), "a Ctrl+C during the last fsync: the file is whole, the script ends, status 130");
        fresh(big);
        intr_on_line = 2;
        r = run("10.0.2.2", "get big\ndelete big\nquit\n");
        check(r == 130 && !server_got("DELE"), "a Ctrl+C between two commands of a script: the next does not run");
    }
    fresh(NULL);
    intr_on_connect = 1;
    r = run("", "open 10.0.2.2\nopen 10.0.2.2\nuser bob\ndelete x\nquit\n");
    check(r == 130 && connects == 1 && !server_got("USER"), "a Ctrl+C in open ends a script");

    /* 6: time at --More-- is no time without data */
    {
        static const struct rule l[] = { { "LIST", "150 here\r\n", "one\r\ntwo\r\nthree\r\nfour\r\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        max_read = 5;
        hold_ms = 70000;
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 0 && !said("no data came") && !said("no reply came") && shown("four\n"), "70 seconds at --More-- for each line: no time-out, data or reply");
    }
    fresh(NULL);
    greeting = "220-one\r\ntwo\r\nthree\r\nfour\r\n220 five\r\n";
    greeting_len = strlen(greeting);
    hold_ms = 70000;
    max_read = 8;                       /* in pieces: the reply's time is looked at between them */
    r = run("-n 10.0.2.2", "quit\n");
    check(r == 0 && !said("no reply came"), "... nor in a long greeting");

    /* 7: a listing that does not end */
    {
        static const struct rule m[] = { { "NLST", "150 here\r\n", "", 0, "226 done\r\n", D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        endless_data = 1;
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 2 && !ran_away && said("too long") && server_got("ABOR") && !server_got("RETR"), "an NLST without end: stopped at the most names kept, nothing fetched");
        check(data_taken < 100000, "... having read no more than it keeps");
    }
    {   /* a listing shown is as a file's transfer: no limit of its own, and Ctrl+C is the way out */
        static const struct rule l[] = { { "LIST", "150 here\r\n", "", 0, "226 done\r\n", D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        drip_next = 2000;
        intr_at_ms = 3600000;
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 130 && !ran_away && !said("took too long") && server_got("ABOR") && now_ms >= 3600000, "a byte of a listing every 59 s, shown: an hour on, Ctrl+C stops it");
    }

    /* 8: q stops all of mget */
    {
        static const struct rule m[] = { { "NLST", "150 here\r\n", "a1\r\nb1\r\n", 0, "226 done\r\n", 0 }, { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(m);
        put_file("a1", "mine");
        stdin_tty = 1;
        r = run("10.0.2.2", "mget a* b*\nq\nquit\n");
        check(!server_got("RETR") && file_is("a1", "mine"), "q at mget's question stops every pattern");
    }

    /* 9: the answer to ABOR that comes late is not the next command's */
    {
        static const struct rule slow[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN },
            { "ABOR", "426 stopped\r\n", 0, 0, "226 abort ok\r\n", A_LATE },
            { "DELE", "550 no such file\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(slow);
        stdin_tty = 1;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\ndelete keep.txt\nquit\n");
        check(said("keep.txt: 550 no such file") && !said("226 abort ok"), "a late answer to ABOR is not taken for the next command's");
    }

    /* 10: reget past 2 GB */
    {
        static const struct rule go[] = { { "SIZE", "213 3500000000\r\n", 0, 0, 0, 0 }, { "REST", "350 ok\r\n", 0, 0, 0, 0 }, { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(go);
        put_file("f", "hello");
        lied_name = "f";
        lied_size = 3000000000u;
        r = run("10.0.2.2", "reget f\nquit\n");
        check(r == 2 && !server_got("RETR") && !logged("ftruncate") && said("2 GB"), "reget past 2 GB, where nothing it added could be cut off again: refused");
    }

    /* 11: a file that shrinks under put */
    {
        static const struct rule take[] = { { "STOR", "150 send\r\n", 0, 0, "226 stored\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(take);
        put_file("up", "short now");
        lied_name = "up";
        lied_size = 3000000;
        r = run("10.0.2.2", "put up\nquit\n");
        check(r == 2 && said("shorter") && server_got("ABOR"), "a file that shrinks under put is no success: ABOR");
    }

    /* 12: strdup fails in mput's listing (a realloc has moved the array: ASan on the Mac sees it) */
    {
        static const struct rule take[] = { { "STOR", "150 send\r\n", 0, 0, "226 stored\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(take);
        put_file("a1", "1");
        put_file("a2", "2");
        put_file("a3", "3");
        fail_strdup_at = 3;
        r = run("10.0.2.2", "mput a*\nquit\n");
        check(r == 2 && !server_got("STOR"), "no memory in mput's listing: said, nothing sent");
    }

    /* the password: a terminal nobody types on, a detached task's */
    fresh(NULL);
    have_tty = 1;
    tty_hang = 1;
    proc_detached = 1;
    r = run("-n 10.0.2.2", "user bob\nquit\n");
    check(r == 2 && !ran_away && !server_got("PASS") && !tty_opens && said("no terminal") && now_ms < 10000, "a detached task's password: not asked for, the login fails at once");

    /* nits */
    {
        static const struct rule cut[] = { { "SIZE", "213 1000\r\n", 0, 0, 0, 0 }, { "RETR", "150 here\r\n", "short", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(cut);
        r = run("10.0.2.2", "get a -\nquit\n");
        check(r == 2 && said("only 5 of the 1000 bytes"), "get - cut short says so");
    }
    {
        static char line[800];
        static struct rule l[] = { { "LIST", "150 here\r\n", line, 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        memset(line, 'd', sizeof(line) - 1);
        memcpy(line + sizeof(line) - 6, "END\r\n", 5);
        fresh(l);
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 0 && shown("dEND\n"), "a listing's line of 800 bytes is shown whole");
    }
    fresh(NULL);
    put_dir("/x\x1b[2J");
    r = run("", "lcd /x\x1b[2J\nquit\n");
    check(!strchr(err_buf, 0x1b) && !strchr(out_buf, 0x1b), "lcd shows a folder's name cleaned");
    {
        static const struct rule l[] = { { "LIST", "150 here\r\n", "one\r\ntwo\r\n", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(l);
        out_broken = 1;
        stdin_tty = 1;
        r = run("10.0.2.2", "dir\ndir\nquit\n");
        check(r == 2 && said("standard output") && strstr(lines, "LIST\n") == strstr(lines, "LIST") && !strstr(strstr(lines, "LIST") + 4, "LIST"),
              "standard output failing ends the run, also at a terminal: one LIST, no second");
    }
    {
        static char many[20000];
        struct rule l[2];
        size_t i;
        for(i = 0; i < sizeof(many); i++) many[i] = i % 20 == 19 ? '\n' : 'n';
        memset(l, 0, sizeof(l));
        l[0].cmd = "LIST";
        l[0].reply = "150 here\r\n";
        l[0].data = many;
        l[0].data_len = sizeof(many);
        l[0].after = "226 done\r\n";
        fresh(l);
        out_broken = 1;
        max_read = 100;
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 2 && server_got("ABOR") && data_taken <= 200, "... and the listing is stopped there, not read to its end");
    }
    fresh(get_ok);
    out_broken = 1;
    r = run("10.0.2.2", "get a.txt -\nget a.txt -\nquit\n");
    check(r == 2 && said("standard output"), "... for get - too");
}

/**** the review of 856c4d8: each of these failed on it ****/

/* fresh(), over connections whose writes queue at most part bytes at a time, as SurfOS's tcp_write may */
static void fresh_part(const struct rule *r, int part) {
    fresh(r);
    max_write = part;
}

static void test_review2(void) {
    static char big[100000];
    int r;

    /* 2: a refusal during an upload is the server's, said as such */
    {
        static const struct rule full[] = { { "STOR", "150 send\r\n", 0, 0, "552 Exceeded storage allocation\r\n", A_EARLY | D_FULL }, { 0, 0, 0, 0, 0, 0 } };
        fresh(full);
        memset(big, 'x', sizeof(big) - 1);
        put_file("big", big);
        max_write = 1000;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 1 && said("big: 552 Exceeded storage allocation") && !said("shorter"), "a 552 during an upload is said as the server's no, status 1");
    }

    /* 3: what comes after ABOR, up to NOOP's answer, is the abort's */
    {
        static const struct rule no_abor[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN },
            { "ABOR", "426 Connection closed; transfer aborted\r\n502 ABOR not implemented\r\n", 0, 0, 0, 0 },
            { "DELE", "550 x: no such file\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(no_abor);
        stdin_tty = 1;
        max_write = 3;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\ndelete x\nquit\n");
        check(said("x: 550 x: no such file") && !said("200 ok"), "a server with no ABOR: its 502 is the abort's, and the delete gets its own answer");
    }
    {   /* the review's case: NOOP's 200 comes a moment later, after the look before the next command */
        static const struct rule no_abor[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN },
            { "ABOR", "426 Connection closed; transfer aborted\r\n502 ABOR not implemented\r\n", 0, 0, 0, 0 },
            { "NOOP", "", 0, 0, "200 NOOP ok\r\n", A_LATE },
            { "DELE", "550 x: no such file\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh(no_abor);
        stdin_tty = 1;
        max_write = 3;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\ndelete x\nquit\n");
        check(said("x: 550 x: no such file") && !said("200 NOOP ok"), "a server with no ABOR, NOOP answered late: the delete gets its own answer");
    }
    {
        static const struct rule no_abor[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN },
            { "ABOR", "426 stopped\r\n500 ABOR: unknown command\r\n", 0, 0, 0, 0 },
            { "NOOP", "", 0, 0, "200 NOOP ok\r\n", A_LATE },
            { "DELE", "550 x: no such file\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(no_abor, 5);
        stdin_tty = 1;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\ndelete x\nquit\n");
        check(said("x: 550 x: no such file") && !said("200 NOOP ok"), "... and with ABOR answered 500");
    }
    {
        static const struct rule going[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN },
            { "ABOR", "426 stopped\r\n421 shutting down\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(going, 5);
        stdin_tty = 1;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\nstatus\nquit\n");
        check(said("the server is going away") && shown("Not connected"), "a 421 among the abort's replies: the connection goes");
    }
    {   /* two quick presses: the second before ABOR went. The connection goes first, then the data connection: its end
           alone would tell a server taking an upload that the file is whole */
        static const struct rule full[] = { { "STOR", "150 send\r\n", 0, 0, "226 done\r\n", D_FULL }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(full, 6);
        memset(big, 'x', sizeof(big) - 1);
        put_file("big", big);
        stdin_tty = 1;
        intr_at_ms = 7000;
        intr_again = 1;
        r = run("10.0.2.2", "put big\nstatus\nquit\n");
        check(!server_got("ABOR") && !strncmp(closes, "cd", 2) && shown("Not connected"), "a second Ctrl+C before ABOR went: the connection ends before the data connection");
    }
    {   /* the second press after ABOR went, before NOOP: no way to know which reply is whose, so the connection goes */
        static const struct rule stuck[] = { { "RETR", "150 here\r\n", "some", 0, 0, D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(stuck, 6);
        stdin_tty = 1;
        intr_at_ms = 7000;
        intr_again = 2;
        r = run("10.0.2.2", "get a\nstatus\nquit\n");
        check(server_got("ABOR") && !server_got("NOOP") && said("stopped by Ctrl+C") && shown("Not connected") && no_file("a"),
              "a second Ctrl+C before NOOP went: the connection goes");
    }
    {
        static const struct rule late[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN },
            { "ABOR", "426 stopped\r\n", 0, 0, "226 abort ok\r\n", A_LATE },
            { "DELE", "550 x: no such file\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(late, 5);
        stdin_tty = 1;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\ndelete x\nquit\n");
        check(said("x: 550 x: no such file") && !said("226 abort ok"), "ABOR answered late: the same");
    }
    {
        static const struct rule twice[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN },
            { "ABOR", "426 stopped\r\n225 aborted\r\n226 abort ok\r\n", 0, 0, 0, 0 },
            { "DELE", "550 x: no such file\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(twice, 5);
        stdin_tty = 1;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\ndelete x\nquit\n");
        check(said("x: 550 x: no such file"), "ABOR answered twice: the same");
    }
    {
        static const struct rule mute[] = {
            { "RETR", "150 here\r\n", "part", 0, 0, D_OPEN }, { "NOOP", 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(mute, 5);
        stdin_tty = 1;
        intr_at_ms = 7000;
        r = run("10.0.2.2", "get a\nstatus\nquit\n");
        check(said("no answer to NOOP") && shown("Not connected"), "no answer to NOOP after ABOR: the connection goes, and that is what is said");
    }

    /* 4: a listing that did not come whole is no list to go by */
    {
        static const struct rule cut[] = {
            { "NLST", "150 here\r\n", "a\r\nb\r\n", 0, 0, D_RST }, { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(cut, 5);
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 2 && !server_got("RETR") && said("did not come whole"), "a listing reset part-way: nothing fetched");
    }
    {
        static const struct rule cut[] = {
            { "NLST", "150 here\r\n", "a\r\nb\r\n", 0, "451 local error\r\n", A_TAKEN | D_OPEN }, { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(cut, 5);
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 1 && !server_got("RETR") && said("451 local error") && said("did not come whole"), "a 451 part-way through a listing: nothing fetched");
    }
    {
        static const struct rule cut[] = {
            { "NLST", "150 here\r\n", "a\r\nb\r\n", 0, 0, D_OPEN }, { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(cut, 5);
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 2 && !server_got("RETR") && said("no data came"), "a listing that stalls: nothing fetched");
    }
    {
        static const struct rule drip[] = { { "NLST", "150 here\r\n", "", 0, "226 done\r\n", D_OPEN }, { "RETR", "150 here\r\n", "x", 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(drip, 5);
        drip_next = 2000;
        drip_end = 600000;                      /* a name a minute: five of them by the limit */
        r = run("10.0.2.2", "mget *\nquit\n");
        check(r == 2 && !ran_away && !server_got("RETR") && said("the listing took too long") && said("did not come whole") && now_ms < 400000,
              "mget's listing that drips: over in 5 minutes, nothing fetched of the names that came");
    }
    {   /* (a nit) dir's listing goes where it was asked to: no time limit there */
        static const struct rule drip[] = { { "LIST", "150 here\r\n", "", 0, "226 done\r\n", D_OPEN }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(drip, 5);
        drip_next = 2000;
        drip_end = 400000;
        r = run("10.0.2.2", "dir\nquit\n");
        check(r == 0 && !said("took too long") && now_ms >= 400000, "a dir that takes 7 minutes is shown whole");
    }

    /* 5: no time limit on a password, and no terminal is known before anything is asked */
    fresh_part(NULL, 5);
    have_tty = 1;
    tty_keys = PW "\r";
    tty_key_at = 200000;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 0 && server_got("PASS " PW "\n") && !said("no command") && password_hidden(), "a password typed after 3 minutes is the password");
    fresh_part(NULL, 5);
    have_tty = 1;
    proc_tty = -1;
    tty_hang = 1;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && !tty_opens && said("no terminal") && now_ms < 10000, "a process with no terminal: the login fails at once, nothing read");
    fresh_part(NULL, 5);
    have_tty = 1;
    tty_hang = 1;
    proc_detached = 1;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && !tty_opens && said("no terminal") && now_ms < 10000, "a detached process: the same");
    fresh_part(NULL, 5);
    have_tty = 1;
    tty_keys = PW "\r";
    proc_collect = 3;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 0 && server_got("PASS " PW "\n") && !said("no terminal") && password_hidden(),
          "tasks collected while procinfo() is read, three scans running, shift this process past them: it is looked for again");

    /* 6: a Ctrl+C that a look before a command finds is kept */
    {
        static const struct rule stray[] = { { "PASS", "230 in\r\n250 by the way: quota 90% used\r\n", 0, 0, 0, 0 }, { "DELE", "250 gone\r\n", 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(stray, 5);
        intr_with_stray = 1;
        max_read = 4;
        r = run("10.0.2.2", "delete x\nquit\n");
        check(r == 130 && !server_got("DELE"), "a Ctrl+C found as a stray reply is looked at: the delete does not go");
        fresh_part(stray, 5);
        stdin_tty = 1;
        intr_with_stray = 1;
        max_read = 4;
        r = run("10.0.2.2", "delete x\nstatus\nquit\n");
        check(!server_got("DELE") && said("stopped") && said("Connected to 10.0.2.2") && said("quota 90% used") && server_got("QUIT"),
              "... at a terminal: the session goes on, in step, and the stray reply is shown later");
    }
    {   /* a Ctrl+C before a command's first byte, with nothing to drain: the command does not go, the session stays */
        static const struct rule stor[] = { { "STOR", "150 send\r\n", 0, 0, "226 done\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(stor, 5);
        put_file("a", "1");
        put_file("b", "2");
        stdin_tty = 1;
        intr_on_say = "a: 1 bytes";
        r = run("10.0.2.2", "mput a b\nstatus\nquit\n");
        check(server_got("STOR a") && !server_got("STOR b") && said("stopped") && said("Connected to 10.0.2.2"),
              "a Ctrl+C between mput's files: the next command is not sent, and the connection stays");
    }
}

/**** the review of 790203e: each of these failed on it, but where it says a guard ****/

static void test_review3(void) {
    static char part[1001], up[3001], big[200000];
    static const struct rule resume[] = {
        { "SIZE big", "213 5000\r\n", 0, 0, 0, 0 }, { "REST", "350 restarting\r\n", 0, 0, 0, 0 },
        { "RETR", "150 here\r\n", "y", 0, "226 done\r\n", 0 }, { "STOR", "150 send\r\n", 0, 0, "226 stored\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
    int r;
    memset(part, 'B', sizeof(part) - 1);
    memset(up, 'N', sizeof(up) - 1);
    memset(big, 'x', sizeof(big) - 1);

    /* 1: REST answered, and the transfer's command not sent: the server keeps REST for its next transfer */
    fresh_part(resume, 5);
    put_file("big", part);
    put_file("up.txt", up);
    stdin_tty = 1;
    intr_on_read = "350 ";
    r = run("10.0.2.2", "reget big\nput up.txt\nstatus\nquit\n");
    check(server_got("REST 1000") && !server_got("RETR") && !server_got("STOR") && said("stopped") && said("after REST") && shown("Not connected"),
          "a Ctrl+C as REST's 350 comes: the connection goes, so no put meets that REST");
    fresh_part(resume, 5);
    put_file("big", part);
    stdin_tty = 1;
    intr_on_read = "350 ";
    r = run("10.0.2.2", "reget big\nascii\nget up.txt got.txt\nquit\n");
    check(!server_got("RETR") && no_file("got.txt") && said("after REST"), "... nor an ascii get");
    fresh_part(resume, 5);                      /* the other order, a guard: the Ctrl+C before REST went */
    put_file("big", part);
    put_file("up.txt", up);
    stdin_tty = 1;
    intr_on_read = "229 ";
    r = run("10.0.2.2", "reget big\nput up.txt\nquit\n");
    check(!server_got("REST") && server_got("STOR up.txt") && said("stopped") && !said("connection is closed") && said("up.txt: 3000 bytes"),
          "a Ctrl+C before REST went: nothing waits at the server, and the session goes on");
    fresh_part(resume, 5);                      /* and one as REST's answer is awaited, a guard: out of step, so the connection goes */
    put_file("big", part);
    stdin_tty = 1;
    intr_on_send = "REST";
    r = run("10.0.2.2", "reget big\nstatus\nquit\n");
    check(server_got("REST 1000") && !server_got("RETR") && shown("Not connected"), "a Ctrl+C while REST's answer is awaited: the connection goes");

    /* 2: the server ends an upload's data connection, and says why only then (vsftpd's order on a full disk) */
    {
        static const struct rule full552[] = { { "STOR", "150 send\r\n", 0, 0, "552 Exceeded storage allocation\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        static const struct rule full451[] = { { "STOR", "150 send\r\n", 0, 0, "451 Failure writing to local file\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        static const struct rule mute[] = { { "STOR", "150 send\r\n", 0, 0, "552 never said\r\n", C_MUTE }, { 0, 0, 0, 0, 0, 0 } };
        static const struct rule ok[] = { { "STOR", "150 send\r\n", 0, 0, "226 stored\r\n", 0 }, { 0, 0, 0, 0, 0, 0 } };
        fresh_part(full552, 1000);
        put_file("big", big);
        data_take = 30000;
        data_end = DE_WRITE;
        data_end_gap = 10;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 1 && said("big: 552 Exceeded storage allocation") && !server_got("ABOR") && !said("the data connection"),
              "a write refused by a reset, then 552 10 ms later: the server's refusal, status 1, no ABOR");
        fresh_part(full451, 1000);
        put_file("big", big);
        data_take = 30000;
        data_end = DE_RST;
        data_end_gap = 10;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 1 && said("big: 451 Failure writing to local file") && !server_got("ABOR"), "a reset poll() sees, then 451 10 ms later: the same");
        fresh_part(full552, 1000);
        put_file("big", big);
        data_take = 30000;
        data_end = DE_HUP;
        data_end_gap = 10;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 1 && said("big: 552 Exceeded storage allocation") && !server_got("ABOR"), "a close, then 552 10 ms later: the same");
        fresh_part(full552, 1000);              /* a guard: the 552 at once, read before the data connection's end */
        put_file("big", big);
        data_take = 30000;
        data_end = DE_RST;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 1 && said("big: 552 Exceeded storage allocation") && !server_got("ABOR"), "a reset and a 552 at once: the same");
        fresh_part(ok, 1000);
        put_file("big", big);
        data_take = 30000;
        data_end = DE_RST;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 2 && said("less than the whole file") && !server_got("ABOR"), "a reset, then 226: no success, the file there is not whole");
        fresh_part(mute, 1000);                 /* guards: no reply, and one past the bound */
        put_file("big", big);
        data_take = 30000;
        data_end = DE_RST;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 2 && said("the server closed the data connection before the end") && server_got("ABOR"), "a reset and no reply: said, and ABOR");
        fresh_part(full552, 1000);
        put_file("big", big);
        data_take = 30000;
        data_end = DE_RST;
        data_end_gap = 8000;
        r = run("10.0.2.2", "put big\nquit\n");
        check(r == 2 && server_got("ABOR") && said("before the end"), "a reset and a 552 after 8 s: ABOR, which passes the 552 over");
    }

    /* 3: a script that a detached task runs: its programs are not detached themselves */
    fresh_part(NULL, 5);
    have_tty = 1;
    tty_hang = 1;
    parent_detached = 1;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && !tty_opens && said("no terminal"), "a parent on the same terminal is detached: no terminal, nothing read");
    fresh_part(NULL, 5);
    have_tty = 1;
    tty_hang = 1;
    grand_detached = 1;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 2 && !server_got("PASS") && !tty_opens && said("no terminal"), "... or its parent's parent");
    fresh_part(NULL, 5);
    have_tty = 1;
    tty_keys = PW "\r";
    pty_session = 1;
    r = run("-n 10.0.2.2", "user alice\nquit\n");
    check(r == 0 && server_got("PASS " PW "\n") && password_hidden(), "a telnetd session: the detached server is on another terminal, so the session's is its own (a guard)");
}

/**** hostile servers at random ****/

static unsigned int fuzz_seed;

static unsigned int fuzz(unsigned int n) {
    fuzz_seed = fuzz_seed * 1103515245u + 12345u;
    return (fuzz_seed >> 16) % n;
}

static const char *const nasty[] = {
    "200 ok\r\n", "226 done\r\n", "150 here\r\n", "550 no\r\n", "421 bye\r\n", "229 x (|||5000|)\r\n", "229 x (|||21|)\r\n", "229 (|||\r\n",
    "227 (1,2,3,4,19,136)\r\n", "227 (1,2,3,4,0,21)\r\n", "227 (999,0,0,0,0,0)\r\n", "213 99999999999999999999999\r\n", "213 5\r\n", "213 -1\r\n",
    "257 \"unclosed\r\n", "257 \"a\"\"b\"\r\n", "350 ok\r\n", "331 pass\r\n", "332 acct\r\n", "230 in\r\n", "100\r\n", "1\r\n", "\r\n", "\n", "\r",
    "123-x\r\n", "123 x\r\n", "999 x\r\n", "\xff\xfb\x01", "\xff\xff", "\xff", "\x1b[2J", "\x1b]0;x\x07", "\xc2\x9b", "\xf0\x9f", "\0\0\0\0",
    "200-\r\n200 \r\n", "220 \x1b[31m", "HTTP/1.1 400\r\n",
};

static void fuzz_reply(struct conn *c) {
    int i, n = (int)fuzz(4);
    static char junk[3000];
    for(i = 0; i < n; i++) {
        unsigned int k = fuzz((unsigned int)(sizeof(nasty) / sizeof(nasty[0])) + 2);
        if(k < sizeof(nasty) / sizeof(nasty[0])) {
            const char *s = nasty[k];
            conn_put(c, s, strncmp(s, "\0\0", 2) ? strlen(s) : 4);
        } else {
            size_t len = fuzz(k & 1 ? 40 : sizeof(junk)), j;
            for(j = 0; j < len; j++) junk[j] = (char)fuzz(256);
            conn_put(c, junk, len);
        }
    }
}

static struct rule fuzz_rules[32];
static char fuzz_text[32][700];
static char fuzz_data[32][4000];

static int fuzz_clean_output(void) {
    size_t i;
    for(i = 0; i < out_len; i++) {
        unsigned char c = (unsigned char)out_buf[i];
        if((c < 0x20 && c != '\n') || c == 0x7f) {
            /* get - writes the file's own bytes: this script does not */
            return 0;
        }
    }
    for(i = 0; i < err_len; i++) {
        unsigned char c = (unsigned char)err_buf[i];
        if((c < 0x20 && c != '\n') || c == 0x7f) return 0;
    }
    return 1;
}

static void test_fuzz(int rounds) {
    static const struct { const char *verb, *good; int data; } verbs[] = {
        { "USER", "331 pass\r\n", 0 },       { "PASS", "230 in\r\n", 0 },      { "TYPE", "200 ok\r\n", 0 },
        { "EPSV", "229 x (|||5000|)\r\n", 0 }, { "PASV", "227 (9,9,9,9,19,136)\r\n", 0 }, { "SIZE", "213 40\r\n", 0 },
        { "REST", "350 ok\r\n", 0 },         { "RETR", "150 here\r\n", 1 },    { "STOR", "150 send\r\n", 1 },
        { "NLST", "150 here\r\n", 1 },       { "LIST", "150 here\r\n", 1 },    { "CWD", "250 ok\r\n", 0 },
        { "PWD", "257 \"/x\"\r\n", 0 },      { "ABOR", "226 ok\r\n", 0 },      { "QUIT", "221 bye\r\n", 0 },
        { "MKD", "257 \"d\"\r\n", 0 },       { "DELE", "250 ok\r\n", 0 },      { "RNFR", "350 ok\r\n", 0 },
        { "RNTO", "250 ok\r\n", 0 } };
    int round, nv = (int)(sizeof(verbs) / sizeof(verbs[0]));
    int deep_retr = 0, deep_stor = 0, replaced = 0, kept = 0, climbed = 0;
    for(round = 0; round < rounds; round++) {
        struct conn scratch;
        const char *retr_data = NULL;
        int retr_len = 0, i;
        unsigned int hostility;                 /* one reply in this many is a bad one: the rounds differ */
        fuzz_seed = (unsigned int)round * 2654435761u + 1;
        hostility = 2 + fuzz(30);
        for(i = 0; i < nv; i++) {
            size_t n, k;
            struct rule *r = &fuzz_rules[i];
            memset(r, 0, sizeof(*r));
            r->cmd = verbs[i].verb;
            if(fuzz(hostility)) {               /* mostly what it should say, so that a session gets deep */
                snprintf(fuzz_text[i], sizeof(fuzz_text[i]), "%s", verbs[i].good);
            } else {
                memset(&scratch, 0, sizeof(scratch));
                fuzz_reply(&scratch);
                n = scratch.in_len < sizeof(fuzz_text[i]) - 1 ? scratch.in_len : sizeof(fuzz_text[i]) - 1;
                for(k = 0; k < n; k++) fuzz_text[i][k] = scratch.in[k] ? (char)scratch.in[k] : 1;
                fuzz_text[i][n] = 0;
                free(scratch.in);
            }
            r->reply = fuzz_text[i];
            if(!fuzz(hostility * 8)) r->flags |= C_CLOSE;       /* these end a session: rare, or none gets far */
            if(!fuzz(hostility * 8)) r->flags |= C_RST;
            if(!fuzz(hostility * 8)) r->flags |= C_FULL;
            if(verbs[i].data) {
                if(!fuzz(5)) r->flags |= D_RST;
                if(!fuzz(6)) r->flags |= D_OPEN;
                if(!fuzz(8)) r->flags |= D_FULL;
                if(!fuzz(8)) r->flags |= C_MUTE;
                if(!fuzz(8)) r->flags |= A_EARLY;
            }
            if(verbs[i].data) {
                n = fuzz(sizeof(fuzz_data[i]));
                for(k = 0; k < n; k++) fuzz_data[i][k] = (char)(fuzz(4) ? "a.txt\r\n../x.txt\n"[fuzz(16)] : fuzz(256));
                r->data = fuzz_data[i];
                r->data_len = (int)n;
                r->after = fuzz(4) ? "226 done\r\n" : "426 no\r\n";
                if(!strcmp(verbs[i].verb, "RETR")) {
                    retr_data = r->data;
                    retr_len = (int)n;
                }
            }
        }
        fresh(fuzz_rules);
        rules = fuzz_rules;
        max_read = 1 + (int)fuzz(64);
        have_tty = 1;
        tty_keys = PW "\r";
        stdin_tty = round % 4 != 0;             /* at a prompt a failure does not end the run: every command is tried */
        put_file("keep.txt", "the user's own");
        put_file("up.txt", "to send\n");
        run("10.0.2.2", "user alice\npwd\ncd sub\nls\ndir\nget a.txt\nget keep.txt\nsize a\nreget r.txt\nascii\nget t.txt\nbinary\n"
                        "put up.txt\nmget *\nmget *.txt\nmput *.txt\nrename a b\ndelete c\nmkdir d\nstatus\nquit\n");
        check(!bad_connects, "fuzz: a connection only to the server, and from port 1024");
        check(fuzz_clean_output(), "fuzz: nothing but clean text shown");
        check(password_hidden(), "fuzz: the password shown nowhere");
        check(!any_tmp(), "fuzz: no .tmp left behind");
        {
            int k = lookup("keep.txt");
            int same = k >= 0 && file_is("keep.txt", "the user's own");
            int whole = k >= 0 && retr_data && files[k].len == (size_t)retr_len && !memcmp(files[k].bytes, retr_data, (size_t)retr_len);
            check(same || whole, "fuzz: the user's file is as it was, or the whole of what the server sent: never a part of it");
            replaced += !same;
            kept += same;
        }
        for(i = 0; i < FILES; i++) if(files[i].used) {
            const char *dots = strstr(files[i].name, "..");
            if(strchr(files[i].name, '/') || (dots && dots == files[i].name && !dots[2])) climbed++;     /* "a..b" is a name */
        }
        deep_retr += data_taken > 0;
        deep_stor += server_got("STOR") != 0;
        if(failures) {
            printf("  (round %d)\n", round);
            return;
        }
    }
    check(!climbed, "fuzz: no file made outside the folder");
    /* a fuzz that dies at its first command proves nothing */
    if(rounds >= 1000) {
        check(deep_retr > rounds / 4, "fuzz: data flowed in a good part of the rounds");
        check(deep_stor > rounds / 5, "fuzz: uploads were tried in a good part of the rounds");
        check(replaced > rounds / 50 && kept > rounds / 4, "fuzz: the user's file was both replaced and kept");
    }
    printf("test_ftpcs: fuzz: %d rounds, data flowed in %d, uploads tried in %d, keep.txt replaced %d, kept %d\n",
           rounds, deep_retr, deep_stor, replaced, kept);
}

int main(int argc, char **argv) {
    test_greeting();
    test_password();
    test_get();
    test_passive();
    test_reget();
    test_put();
    test_list();
    test_mget();
    test_mput();
    test_interrupt();
    test_commands();
    test_more_hostile();
    test_review();
    test_review2();
    test_review3();
    test_fuzz(argc > 1 ? atoi(argv[1]) : 3000);
    printf("test_ftpcs: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
