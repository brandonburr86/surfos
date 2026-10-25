/*
SurfOS file transfer server: ftpd
--------------------
File: server.c  Date: 10/24/26
--------------------
The server. ftpd on starts it with the listening descriptor as its descriptor 0 and
/dev/null as 1 and 2; it detach()es from the shell that started it (init is its parent
from then on, and Ctrl+C there does not reach it) and says what it does in the kernel's
log. ftpd serve runs the same loop on its own console instead and prints each command
there. Either way one poll() loop serves the files of one folder (conf->root) to up to
MAX_SESSIONS sessions. Nothing in it waits for one client: even the connection out that
active mode (PORT, EPRT) makes is polled for (tcp_connect_start()), so a client whose
firewall drops it holds up no other session.

A session reads its control connection into raw[], cuts it into lines (ftp_ctl_feed():
Telnet's sequences taken out) and runs one command at a time; replies go into out[],
written whenever poll() says POLLOUT. While OUT_WAIT bytes of replies wait for a client
that does not read them, the next commands wait in raw[] and in the kernel (TCP holds the
rest at the client). During a transfer the control connection is still read: ABOR and STAT
are answered at once, and the other commands are held, HELD_MAX of them (held[]), to run in
their order once the transfer has ended (10/25/26, review: one held command stopped the
reading, and an ABOR behind it waited for the end of the transfer). With HELD_MAX held, or
OUT_WAIT bytes of replies waiting, the rest waits unread (a client sending STAT after STAT
and reading nothing makes replies pile up no further). A client that closes its side
of the control connection (its FIN) has the commands it sent run and their replies sent,
then the session closes; while that client takes no replies, poll() would say POLLHUP at
once every time, so the connection rests REST_MS before it is polled again (as httpd's).

The data connection. PASV and EPSV listen on a port of the passive range (with a backlog
of 1): the connection that comes is taken at once if it comes from the client's own
address (another is closed, and the listener waits on), then the listener is closed. PORT
and EPRT name an address, the client's own and a port from 1024 (no FTP bounce), which is
connected to when the transfer begins. A transfer (RETR, STOR, STOU, APPE, LIST, NLST, MLSD)
needs one of them first (425 without). It takes the connection (or waits DATA_MS for it
to come, or makes it), answers 150, moves the bytes a piece of DATA_SIZE at a time as far
as the connection takes them, closes the data connection and answers 226; one that moves
nothing for STALL_MS is given up (426), as is one whose data connection fails. Text mode
(TYPE A) converts line ends both ways (ftp.c). Listings are made in memory, then sent, so
no folder is held open while a client reads one. 10/25/26 (review): what a transfer reads or
writes is opened, and its 150 given, once its data connection is there (taken by accept(),
begun for active mode, or there before the command), as vsftpd and ProFTPD do it: a
transfer waiting for its passive connection holds the listener alone, so accept() always
has a descriptor for it (one got the 150, then none: 425), and a failure is the reply in
place of the 150. An upload whose connection never comes leaves its file as it was.

Files: a session's paths (ftp_path()) never leave the folder served, and /dev and /.netman
are never reached (ftp_reserved()). RETR reads a file, STOR writes it in place (after REST:
cut at that offset and written from it, so a resumed upload ends as the file sent), APPE
appends, STOU makes a name of its own; an upload is fsync()ed before its 226. A file is open
only while it moves, and the server's own directory is /, so ftpd holds a volume only during
a transfer of one of its files: umount works between them.

Times: a session that sends no command for IDLE_MS is told so (421) and closed, as is one
whose replies are not taken for STALL_MS. The kill of ftpd off ends the server at the end of
its poll(); its descriptors close with it, so every client sees its connections end.
*/

#include "ftpd.h"

#define DATA_SIZE   8192                /* a piece of a transfer */
#define RAW_SIZE    1024                /* the control connection read, not yet run */
#define OUT_WAIT    16384               /* replies waiting: no more commands are run until they are taken */
#define HELD_MAX    4                   /* commands held during a transfer, to run after it */
#define LISTING_MAX (8u << 20)          /* a listing (or a STAT reply) made in memory */
#define IDLE_MS     300000              /* a session that sends no command for 5 minutes is closed */
#define DATA_MS     30000               /* a data connection must come within this */
#define STALL_MS    60000               /* a transfer, or replies, the client takes nothing of for a minute */
#define REST_MS     20                  /* a half-closed connection that takes nothing now is polled again after this */
#define PAUSE_MS    1000                /* the listener rests this long after accept() failed */
#define FILE_MAX    0x7fffffffUL        /* SurfOS's file offsets stop at 2 GB */

enum { S_FREE, S_CMD, S_XFER, S_CLOSING };
enum { D_NONE, D_LISTEN, D_READY, D_PORT };     /* the data connection for the next transfer */
enum { X_SEND, X_RECV };
enum { T_LIST, T_RETR, T_STOR, T_APPE, T_STOU };       /* what a transfer opens (xkind) */

struct session {
    int state, id;
    unsigned int epoch;                 /* changes whenever one of its descriptors closes (poll()'s list is old then) */
    int fd;                             /* the control connection */
    unsigned int peer, local;           /* the client's address, and the one it reached (network order) */
    unsigned short peer_port;
    char peer_str[16];
    unsigned int since;                 /* the last command (S_CMD) */
    const char *why;                    /* S_CLOSING: why the session ends, for the log */
    int eof;                            /* the client has closed its side of the control connection */
    int crest;                          /* the control connection rests until crest_end (a half-close) */
    unsigned int crest_end;
    int user, logged;                   /* USER came; and PASS */
    int ascii, epsv_all;                /* TYPE A; EPSV ALL came */
    long rest;                          /* REST's offset for the next transfer, 0 for none */
    char cwd[FTP_PATH_MAX];             /* the session's folder (its own path, "/" the folder served) */
    char rnfr[FTP_PATH_MAX];            /* RNFR's path in the file system, "" when none came just before */
    char rnfr_vp[FTP_PATH_MAX];
    int code;                           /* the last reply's code, for the log */
    int began;                          /* the command being run began a transfer (it is logged at its end) */
    int broken;                         /* a reply could not be kept (no memory): the session ends */
    /* the control connection in */
    struct ftp_ctl ctl;
    char raw[RAW_SIZE];
    int raw_len, raw_off;
    char held[HELD_MAX][FTP_LINE_MAX + 1];  /* commands that came during a transfer, run after it in order */
    unsigned char held_long[HELD_MAX];  /* ... or a line too long, answered 500 then */
    int held_first, held_n;
    /* and out */
    char *out;
    int out_len, out_off, out_cap;
    unsigned int out_since;             /* replies began to wait, or the client last took some */
    /* the data connection for the next transfer */
    int dstate, dfd;                    /* the listener (D_LISTEN) or the connection (D_READY) */
    unsigned int dip;                   /* D_PORT's address and port */
    unsigned short dport;
    unsigned int dsince;                /* when it was asked for */
    /* the transfer (S_XFER) */
    int dir;                            /* X_SEND, X_RECV */
    int conn;                           /* the data connection, -1 while it is awaited */
    int connecting;                     /* conn is active mode's, its handshake not done yet */
    int drest;                          /* the data connection rests until drest_end (a half-close) */
    unsigned int drest_end;
    int file;                           /* the file read or written, or -1 */
    char *mem;                          /* or the listing sent */
    unsigned int mem_len, mem_off;
    unsigned long pos;                  /* where an upload writes in its file */
    int cr;                             /* text mode's carry from piece to piece */
    unsigned long bytes;                /* moved over the data connection */
    unsigned int xsince;                /* the transfer's start or its last progress */
    char what[8];                       /* the command, and the path it is about, for the log and 226 */
    char xpath[FTP_PATH_MAX];
    char done_note[FTP_PATH_MAX + 32];  /* 226's text after the bytes (STOU's name) */
    int xkind;                          /* what it opens once its connection is there (T_LIST: nothing) */
    char xreal[FTP_PATH_MAX];           /* ... the file's path in the file system (STOU's: the name tried first) */
    long xrest;                         /* ... and REST's offset */
    char mark[FTP_PATH_MAX + 96];       /* its 150's text */
    unsigned int files_out, files_in;   /* for the log */
    int dlen, doff;
    char dbuf[2 * DATA_SIZE];           /* bytes for the data connection it has not taken yet: last, as a new
                                           session clears everything before it (new_session()) */
};

static struct session sessions[MAX_SESSIONS];
static const struct ftpd_conf *conf;
static int kmsg = -1, next_id, pasv_next;
static int resting;                     /* the listener is left out of poll() until rest_end */
static unsigned int rest_end;
static char scratch[DATA_SIZE];         /* a piece read, before it is converted or written */
static char conv[DATA_SIZE + 1];        /* text mode's piece from the network */

static const char *const feat_lines[] = { "EPSV", "PASV", "SIZE", "REST STREAM", "TVFS", "UTF8", NULL };

/* a line for the kernel's log (ftpd on's server) */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    int n;
    if(kmsg < 0) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    write(kmsg, buf, (size_t)n);
}

/* a line on the console of ftpd serve, after the client's address. The user shell pages every command it
   runs, and a paged server would wait at --More-- after a screenful of lines, holding every client: TTY_PAGE
   0 first starts a new screenful for a program the shell pages (and changes nothing for one it does not) */
static void note(const struct session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(const struct session *s, const char *fmt, ...) {
    char buf[FTP_LINE_MAX + 64];
    va_list ap;
    if(!conf->verbose) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ftp_clean(buf, sizeof(buf), buf);
    ttyctl(TTY_PAGE, 0);
    printf("%s %s\n", s->peer_str, buf);
}

/* is a before b, by the millisecond clock (which wraps)? */
static int before(unsigned int a, unsigned int b) {
    return (int)(a - b) < 0;
}

/**** replies ****/

static void out_add(struct session *s, const char *p, int n) {
    if(s->out_off == s->out_len) s->out_off = s->out_len = 0;
    else if(s->out_off) {
        memmove(s->out, s->out + s->out_off, (size_t)(s->out_len - s->out_off));
        s->out_len -= s->out_off;
        s->out_off = 0;
    }
    if(s->out_len + n > s->out_cap) {
        int cap = s->out_cap ? s->out_cap : 1024;
        char *grown;
        while(cap < s->out_len + n) cap *= 2;
        if((unsigned int)cap > LISTING_MAX + 65536u || !(grown = realloc(s->out, (size_t)cap))) {
            s->broken = 1;
            return;
        }
        s->out = grown;
        s->out_cap = cap;
    }
    if(!s->out_len) s->out_since = uptime();
    memcpy(s->out + s->out_len, p, (size_t)n);
    s->out_len += n;
}

/* a reply line: "code text" (sep ' ', the last line of a reply), "code-text" (sep '-', the first of
   several) or " text" (code 0, a line between them); controls in it become '?' */
static void vline(struct session *s, int code, char sep, const char *fmt, va_list ap) {
    char line[FTP_LINE_MAX + 2 * FTP_PATH_MAX];
    int n = code ? snprintf(line, sizeof(line), "%03d%c", code, sep) : snprintf(line, sizeof(line), " ");
    vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    ftp_clean(line, (int)sizeof(line) - 2, line);
    n = (int)strlen(line);
    line[n++] = '\r';
    line[n++] = '\n';
    out_add(s, line, n);
    if(code && sep == ' ') s->code = code;
}

static void reply(struct session *s, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void reply(struct session *s, int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline(s, code, ' ', fmt, ap);
    va_end(ap);
}

static void reply_first(struct session *s, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void reply_first(struct session *s, int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline(s, code, '-', fmt, ap);
    va_end(ap);
}

static void reply_line(struct session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply_line(struct session *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline(s, 0, ' ', fmt, ap);
    va_end(ap);
}

/* the reply to a file call that failed with errno e, about the session's path vp */
static void failed(struct session *s, const char *vp, int e) {
    const char *text;
    int code = ftp_errno_reply(e, &text);
    reply(s, code, "%s: %s", vp, text ? text : strerror(e));
}

/**** sessions ****/

static void close_fd(struct session *s, int *fd) {
    if(*fd >= 0) {
        close(*fd);
        s->epoch++;
    }
    *fd = -1;
}

/* the data connection prepared for a transfer that did not come */
static void drop_data(struct session *s) {
    close_fd(s, &s->dfd);
    s->dstate = D_NONE;
}

/* what a transfer holds goes */
static void release_transfer(struct session *s) {
    close_fd(s, &s->conn);
    close_fd(s, &s->file);
    free(s->mem);
    s->mem = NULL;
    s->mem_len = s->mem_off = 0;
    s->dlen = s->doff = 0;
    s->drest = s->connecting = 0;
}

static void end_session(struct session *s, const char *why) {
    release_transfer(s);
    drop_data(s);
    close_fd(s, &s->fd);
    free(s->out);
    s->out = NULL;
    s->out_len = s->out_off = s->out_cap = 0;
    say("ftpd: session %d ended (%s): %u files sent, %u received", s->id, why, s->files_out, s->files_in);
    note(s, "session %d ended (%s): %u files sent, %u received", s->id, why, s->files_out, s->files_in);
    s->state = S_FREE;
}

/* the session closes once its last replies are out */
static void closing(struct session *s, const char *why) {
    release_transfer(s);
    drop_data(s);
    s->state = S_CLOSING;
    s->why = why;
}

/* the transfer has ended: what it held goes, and its last reply (226, 4xx, 5xx) is given */
static void end_transfer(struct session *s, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void end_transfer(struct session *s, int code, const char *fmt, ...) {
    va_list ap;
    release_transfer(s);
    drop_data(s);
    s->state = S_CMD;
    s->since = uptime();
    va_start(ap, fmt);
    vline(s, code, ' ', fmt, ap);
    va_end(ap);
    note(s, "%s %s -> %d, %lu bytes", s->what, s->xpath, code, s->bytes);
}

/* replies the client takes now (POLLOUT) */
static void to_client(struct session *s) {
    ssize_t n = write(s->fd, s->out + s->out_off, (size_t)(s->out_len - s->out_off));
    if(n < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        s->out_off = s->out_len = 0;    /* the connection failed: nothing more can be said */
        closing(s, "the connection failed");
        return;
    }
    if(n > 0) s->out_since = uptime();
    s->out_off += (int)n;
    if(s->out_off == s->out_len) s->out_off = s->out_len = 0;
}

/**** paths ****/

/* the paths of arg: the session's (vp) and the file system's (real), FTP_PATH_MAX bytes each: 0, or -1
   with the reply given */
static int resolve(struct session *s, const char *arg, char *vp, char *real) {
    int r = ftp_path(s->cwd, arg, vp, FTP_PATH_MAX);
    if(r == -2) {
        reply(s, 501, "A path with a control character in it");
        return -1;
    }
    if(r < 0 || ftp_real(conf->root, vp, real, FTP_PATH_MAX) < 0) {
        reply(s, 553, "The path is too long");
        return -1;
    }
    if(ftp_reserved(real)) {
        reply(s, 550, "%s: not a file or folder ftpd serves", vp);
        return -1;
    }
    return 0;
}

/* may what is at real be changed? Not when the folder is served read-only, nor on a volume mounted
   read-only, the initrd (tarfs) or the root of mount points (rootfs) */
static int writable_at(const char *real) {
    struct statfs fs;
    if(conf->readonly || statfs(real, &fs) < 0) return 0;
    return !(fs.flags & ST_RDONLY) && strcmp(fs.fs, "tarfs") && strcmp(fs.fs, "rootfs");
}

/**** listings ****/

struct buf {
    char *p;
    unsigned int len, cap;
};

static int buf_add(struct buf *b, const char *s, int n) {
    if(b->len + (unsigned int)n > b->cap) {
        unsigned int cap = b->cap ? b->cap : 4096;
        char *grown;
        while(cap < b->len + (unsigned int)n) cap *= 2;
        if(cap > LISTING_MAX || !(grown = realloc(b->p, cap))) return -1;
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += (unsigned int)n;
    return 0;
}

/* a listing's line for e: LIST's (a STAT reply's too), NLST's or MLSD's */
static int listing_line(char *line, int size, int kind, const struct ftp_entry *e, const char *prefix, unsigned int now,
                        int writable) {
    if(kind == FTP_NLST) return ftp_nlst_line(line, size, prefix, e->name);
    if(kind == FTP_MLSD) return ftp_mlsx_line(line, size, e, writable, 0);
    return ftp_list_line(line, size, e, now, writable);
}

/* the listing of real (the path given, as the client wrote it) into b: 0, or an errno. all: names
   starting with a dot too (MLSD always lists them) */
static int make_listing(int kind, int all, const char *given, const char *real, struct buf *b, int *count) {
    char line[FTP_PATH_MAX + 128], child[FTP_PATH_MAX + 2];
    unsigned int now = (unsigned int)time(NULL);
    struct ftp_entry e;
    struct dirent *de;
    struct stat st;
    int writable, top, n;
    DIR *d;
    *count = 0;
    if(stat(real, &st) < 0) return errno;
    if(!S_ISDIR(st.st_mode)) {          /* one file: its own line, under the name it was asked by */
        if(kind == FTP_MLSD) return ENOTDIR;
        e.name = *given ? given : ftp_basename(real);
        e.dir = 0;
        e.size = st.st_size;
        e.mtime = st.st_mtime;
        n = listing_line(line, sizeof(line), kind, &e, "", now, writable_at(real));
        if(n < 0 || buf_add(b, line, n) < 0) return ENOMEM;
        *count = 1;
        return 0;
    }
    if(!(d = opendir(real))) return errno;
    top = !strcmp(real, "/");           /* the mount points, each a volume of its own */
    writable = top ? 0 : writable_at(real);
    while((de = readdir(d)) != NULL) {
        if(de->d_name[0] == '.' && !all && kind != FTP_MLSD) continue;
        snprintf(child, sizeof(child), "%s/%s", top ? "" : real, de->d_name);
        if(ftp_reserved(child)) continue;
        e.name = de->d_name;
        e.dir = de->d_type == DT_DIR;
        e.size = de->size;
        e.mtime = de->mtime;
        n = listing_line(line, sizeof(line), kind, &e, kind == FTP_NLST ? given : "", now, top ? writable_at(child) : writable);
        if(n < 0) continue;             /* a name too long for a line of its own: no path can name it either */
        if(buf_add(b, line, n) < 0) {
            closedir(d);
            return ENOMEM;
        }
        (*count)++;
    }
    closedir(d);
    return 0;
}

/**** the data connection ****/

/* a passive listener for the next transfer: its port, or -1 with the reply given */
static int open_passive(struct session *s) {
    int span = conf->pasv_last - conf->pasv_first + 1, i, fd = -1, port = 0, e = 0;
    drop_data(s);
    for(i = 0; i < span; i++) {
        port = conf->pasv_first + pasv_next++ % span;
        if((fd = tcp_listen_backlog((unsigned short)port, 1)) >= 0) break;
        if((e = errno) != EADDRINUSE) break;
    }
    if(fd < 0) {
        if(e == EADDRINUSE) reply(s, 425, "Every passive port (%d-%d) is in use", conf->pasv_first, conf->pasv_last);
        else if(e == EMFILE || e == ENFILE) reply(s, 425, "Too many connections at once; try again");
        else reply(s, 425, "No passive port: %s", strerror(e));
        return -1;
    }
    s->dfd = fd;
    s->dstate = D_LISTEN;
    s->dport = (unsigned short)port;
    s->dsince = uptime();
    return port;
}

/* the transfer can begin: its data connection is fd */
static void start_moving(struct session *s, int fd) {
    s->conn = fd;
    s->dstate = D_NONE;
    s->xsince = uptime();
    s->dlen = s->doff = 0;
}

static int open_transfer(struct session *s);

/* a connection came to the passive listener (POLLIN): taken when it is the client's. A transfer that waited for it
   opens its file now (the listener's descriptor is free for it), then gives its 150 */
static void data_accept(struct session *s) {
    struct netaddr from;
    char ip[16];
    int fd = tcp_accept(s->dfd, &from);
    if(fd < 0) {
        int e = errno;
        if(e == EINTR || e == EAGAIN) return;
        if(s->state == S_XFER) end_transfer(s, 425, "The data connection could not be taken: %s; try again", strerror(e));
        else drop_data(s);
        return;
    }
    if(from.ip != s->peer) {            /* someone else's: the client's own may still come */
        note(s, "a data connection from %s refused", ipstr(from.ip, ip));
        close(fd);
        return;
    }
    drop_data(s);
    if(s->state != S_XFER) {            /* before its command: kept for it */
        s->dfd = fd;
        s->dstate = D_READY;
        return;
    }
    if(open_transfer(s) < 0) {          /* its reply came in place of the 150: the transfer is over */
        close(fd);
        release_transfer(s);
        s->state = S_CMD;
        s->since = uptime();
        note(s, "%s %s -> %d, 0 bytes", s->what, s->xpath, s->code);
        return;
    }
    reply(s, 150, "%s", s->mark);
    start_moving(s, fd);
}

/* the next piece for the data connection into dbuf: its length, 0 at the end, -1 on an error (the
   transfer has ended then) */
static int refill(struct session *s) {
    int n;
    if(s->mem) {
        n = (int)(s->mem_len - s->mem_off < DATA_SIZE ? s->mem_len - s->mem_off : DATA_SIZE);
        memcpy(s->dbuf, s->mem + s->mem_off, (size_t)n);
        s->mem_off += (unsigned int)n;
    } else {
        n = (int)read(s->file, s->ascii ? scratch : s->dbuf, DATA_SIZE);
        if(n < 0) {
            end_transfer(s, 451, "%s: %s; the transfer is aborted", s->xpath, strerror(errno));
            return -1;
        }
        if(s->ascii) n = ftp_to_crlf(scratch, n, s->dbuf, &s->cr);
    }
    s->doff = 0;
    s->dlen = n;
    return n;
}

/* the data connection takes bytes (POLLOUT) */
static void send_step(struct session *s) {
    ssize_t n;
    int r;
    if(s->doff == s->dlen) {
        if((r = refill(s)) < 0) return;
        if(r == 0) {                    /* all of it is out (the FIN follows it): done */
            if(!s->mem) s->files_out++;
            end_transfer(s, 226, "Transfer complete: %lu bytes%s", s->bytes, s->done_note);
            return;
        }
    }
    n = write(s->conn, s->dbuf + s->doff, (size_t)(s->dlen - s->doff));
    if(n < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        end_transfer(s, 426, "The data connection failed: %s; the transfer is aborted", strerror(errno));
        return;
    }
    if(n > 0) s->xsince = uptime();
    s->doff += (int)n;
    s->bytes += (unsigned long)n;
}

static int write_all(int fd, const char *p, int n) {
    while(n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if(w <= 0) return -1;
        p += w;
        n -= (int)w;
    }
    return 0;
}

/* an upload's bytes into its file: 0, or -1 with the transfer ended */
static int store(struct session *s, const char *p, int n) {
    if(s->pos + (unsigned long)n > FILE_MAX) {
        end_transfer(s, 552, "%s: files stop at 2 GB here; the transfer is aborted", s->xpath);
        return -1;
    }
    if(n && write_all(s->file, p, n) < 0) {
        int e = errno, code;
        const char *text;
        code = ftp_errno_reply(e, &text);
        end_transfer(s, code == 550 ? 451 : code, "%s: %s; the transfer is aborted (what came before is kept)",
                     s->xpath, text ? text : strerror(e));
        return -1;
    }
    s->pos += (unsigned long)n;
    return 0;
}

/* the data connection has bytes, or its end (POLLIN) */
static void recv_step(struct session *s) {
    ssize_t n = read(s->conn, scratch, DATA_SIZE);
    int m, e = 0, fd;
    if(n < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        end_transfer(s, 426, "The data connection failed: %s; what came is kept", strerror(errno));
        return;
    }
    if(n == 0) {                        /* the client has sent it all */
        if(s->ascii && (m = ftp_from_crlf("", 0, conv, &s->cr)) > 0 && store(s, conv, m) < 0) return;
        fd = s->file;
        s->file = -1;
        s->epoch++;
        if(fsync(fd) < 0) e = errno;
        if(close(fd) < 0 && !e) e = errno;
        if(e) {
            end_transfer(s, 451, "%s: %s: the file may not be whole", s->xpath, strerror(e));
            return;
        }
        s->files_in++;
        end_transfer(s, 226, "Transfer complete: %lu bytes%s", s->bytes, s->done_note);
        return;
    }
    s->xsince = uptime();
    s->bytes += (unsigned long)n;
    if(s->ascii) store(s, conv, ftp_from_crlf(scratch, (int)n, conv, &s->cr));
    else store(s, scratch, (int)n);
}

/**** commands ****/

/* What a transfer reads or writes, opened when its data connection is there (10/25/26, review: see the top), with
   its 150's text made where it depends on that (an upload's offset, STOU's name). 0, or -1 with the reply given in
   place of the 150 and nothing kept */
static int open_transfer(struct session *s) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX];
    struct stat st;
    int fd = -1, i, e;
    switch(s->xkind) {
    case T_LIST:                        /* the listing is made already */
        return 0;
    case T_RETR:
        if((fd = open(s->xreal, O_RDONLY)) >= 0 && s->xrest && lseek(fd, s->xrest, SEEK_SET) != s->xrest) {
            e = errno;
            close(fd);
            fd = -1;
            errno = e;
        }
        break;
    case T_STOU:                        /* the name given or "upload", then name.1, name.2, ...: a new file */
        for(i = 0; i < 1000; i++) {
            if(!i) {
                snprintf(vp, sizeof(vp), "%s", s->xpath);
                snprintf(real, sizeof(real), "%s", s->xreal);
            } else if(snprintf(vp, sizeof(vp), "%s.%d", s->xpath, i) >= (int)sizeof(vp) ||
                      snprintf(real, sizeof(real), "%s.%d", s->xreal, i) >= (int)sizeof(real)) {
                errno = ENAMETOOLONG;
                break;
            }
            if((fd = open(real, O_WRONLY | O_CREAT | O_EXCL)) >= 0 || errno != EEXIST) break;
        }
        if(fd >= 0) {
            snprintf(s->xpath, sizeof(s->xpath), "%s", vp);
            snprintf(s->done_note, sizeof(s->done_note), ", as %s", ftp_basename(vp));
            snprintf(s->mark, sizeof(s->mark), "FILE: %s", ftp_basename(vp));
        }
        break;
    default:                            /* STOR and APPE */
        if(s->xkind == T_APPE) fd = open(s->xreal, O_WRONLY | O_CREAT | O_APPEND);
        else if(s->xrest) fd = open(s->xreal, O_WRONLY | O_CREAT);
        else fd = open(s->xreal, O_WRONLY | O_CREAT | O_TRUNC);
        if(fd < 0) break;
        if(fstat(fd, &st) < 0) st.st_size = 0;
        s->pos = 0;
        if(s->xkind == T_APPE) s->pos = st.st_size;
        else if(s->xrest) {             /* a resumed upload: what was sent before stays, the rest goes */
            if((unsigned long)s->xrest > st.st_size) {
                close(fd);
                reply(s, 554, "REST %ld is past the end of %s (%u bytes)", s->xrest, s->xpath, st.st_size);
                return -1;
            }
            if(ftruncate(fd, s->xrest) < 0 || lseek(fd, s->xrest, SEEK_SET) != s->xrest) {
                e = errno;
                close(fd);
                fd = -1;
                errno = e;
                break;
            }
            s->pos = (unsigned long)s->xrest;
        }
        if(s->pos) snprintf(s->mark, sizeof(s->mark), "Ready to receive %s in %s mode, from byte %lu", s->xpath,
                            s->ascii ? "ASCII" : "BINARY", s->pos);
        else snprintf(s->mark, sizeof(s->mark), "Ready to receive %s in %s mode", s->xpath, s->ascii ? "ASCII" : "BINARY");
        break;
    }
    if(fd < 0) {
        failed(s, s->xpath, errno);
        return -1;
    }
    s->file = fd;
    return 0;
}

/* A transfer begins, its command's checks passed and what it needs noted (xkind, xreal, xrest, a listing in mem,
   the 150's text): with its data connection there already (taken before the command), or begun for active mode
   (polled for: data_event() says when it is made), its file is opened and its 150 given now; a passive one waits
   for data_accept(). A file that cannot be opened, or a connection out that cannot even be begun, is the reply
   instead, and the data connection prepared stays (or goes, for a PORT that failed) */
static void begin_transfer(struct session *s, int kind, const char *what, const char *vp, int dir) {
    s->xkind = kind;
    snprintf(s->what, sizeof(s->what), "%s", what);
    snprintf(s->xpath, sizeof(s->xpath), "%s", vp);
    s->dir = dir;
    s->bytes = 0;
    s->cr = 0;
    s->conn = -1;
    s->xsince = uptime();
    if(s->dstate == D_LISTEN) {         /* the file and the 150 when the connection comes */
        s->state = S_XFER;
        s->began = 1;
        return;
    }
    if(open_transfer(s) < 0) {
        release_transfer(s);
        return;
    }
    if(s->dstate == D_PORT) {
        char ip[16];
        int fd = tcp_connect_start(s->dip, s->dport);
        if(fd < 0) {
            int e = errno;
            release_transfer(s);
            drop_data(s);
            reply(s, 425, "No data connection to %s port %u: %s", ipstr(s->dip, ip), s->dport, strerror(e));
            return;
        }
        s->conn = fd;
        s->connecting = 1;
        s->dstate = D_NONE;
    } else {                            /* D_READY */
        int fd = s->dfd;
        s->dfd = -1;
        start_moving(s, fd);
    }
    s->state = S_XFER;
    s->began = 1;
    reply(s, 150, "%s", s->mark);
}

/* a transfer needs a data connection prepared first */
static int data_ready(struct session *s) {
    if(s->dstate != D_NONE) return 1;
    reply(s, 425, "Use PASV, EPSV or PORT first");
    return 0;
}

static const char *mode_word(const struct session *s) {
    return s->ascii ? "ASCII" : "BINARY";
}

static void cmd_list(struct session *s, int kind, const char *arg) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX];
    const char *path = arg;
    struct buf b = { NULL, 0, 0 };
    int all = 0, count, e;
    if(kind != FTP_MLSD) ftp_list_args(arg, &all, &path);
    if(!data_ready(s) || resolve(s, path, vp, real) < 0) return;
    if((e = make_listing(kind, all, path, real, &b, &count)) != 0) {
        free(b.p);
        if(e == ENOTDIR) reply(s, 501, "%s is not a folder: MLSD lists folders (MLST describes a file)", vp);
        else failed(s, vp, e);
        return;
    }
    s->mem = b.p;
    s->mem_len = b.len;
    s->mem_off = 0;
    s->done_note[0] = 0;
    snprintf(s->mark, sizeof(s->mark), "Here comes the listing of %s (%d %s)", vp, count, count == 1 ? "entry" : "entries");
    begin_transfer(s, T_LIST, kind == FTP_LIST ? "LIST" : kind == FTP_NLST ? "NLST" : "MLSD", vp, X_SEND);
}

static void cmd_retr(struct session *s, const char *arg, long rest) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX];
    struct stat st;
    if(!data_ready(s) || resolve(s, arg, vp, real) < 0) return;
    if(stat(real, &st) < 0) {
        failed(s, vp, errno);
        return;
    }
    if(S_ISDIR(st.st_mode)) {
        reply(s, 550, "%s is a folder: LIST or MLSD lists it", vp);
        return;
    }
    if(st.st_size > FILE_MAX) {
        reply(s, 550, "%s is 2 GB or more, which SurfOS's programs cannot read", vp);
        return;
    }
    if((unsigned long)rest > st.st_size) {
        reply(s, 554, "REST %ld is past the end of %s (%u bytes)", rest, vp, st.st_size);
        return;
    }
    snprintf(s->xreal, sizeof(s->xreal), "%s", real);
    s->xrest = rest;
    s->done_note[0] = 0;
    if(rest) snprintf(s->mark, sizeof(s->mark), "Opening %s mode data connection for %s (%u bytes, from byte %ld)",
                      mode_word(s), vp, st.st_size, rest);
    else snprintf(s->mark, sizeof(s->mark), "Opening %s mode data connection for %s (%u bytes)", mode_word(s), vp,
                  st.st_size);
    begin_transfer(s, T_RETR, "RETR", vp, X_SEND);
}

/* STOR, APPE and STOU: the checks now, the file when the data connection is there (open_transfer()) */
static void cmd_store(struct session *s, int id, const char *arg, long rest) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX];
    struct stat st;
    int there;
    if(!data_ready(s) || resolve(s, id == FTP_STOU && !*arg ? "upload" : arg, vp, real) < 0) return;
    there = stat(real, &st) == 0;
    if(there && S_ISDIR(st.st_mode) && id != FTP_STOU) {
        reply(s, 550, "%s is a folder", vp);
        return;
    }
    /* what can be told now is, not once the connection is there: a read-only place, REST past the end */
    if(!writable_at(real)) {
        failed(s, vp, EROFS);
        return;
    }
    if(id == FTP_STOR && rest && (unsigned long)rest > (there ? st.st_size : 0)) {
        reply(s, 554, "REST %ld is past the end of %s (%u bytes)", rest, vp, there ? st.st_size : 0);
        return;
    }
    snprintf(s->xreal, sizeof(s->xreal), "%s", real);
    s->xrest = id == FTP_STOR ? rest : 0;
    s->done_note[0] = 0;
    begin_transfer(s, id == FTP_STOR ? T_STOR : id == FTP_APPE ? T_APPE : T_STOU,
                   id == FTP_STOR ? "STOR" : id == FTP_APPE ? "APPE" : "STOU", vp, X_RECV);
}

static void cmd_cwd(struct session *s, const char *arg) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX];
    struct stat st;
    if(resolve(s, arg, vp, real) < 0) return;
    if(stat(real, &st) < 0) {
        failed(s, vp, errno);
        return;
    }
    if(!S_ISDIR(st.st_mode)) {
        reply(s, 550, "%s is not a folder", vp);
        return;
    }
    strcpy(s->cwd, vp);
    reply(s, 250, "The folder is now %s", vp);
}

static void cmd_size(struct session *s, const char *arg) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX];
    struct stat st;
    if(resolve(s, arg, vp, real) < 0) return;
    if(stat(real, &st) < 0) failed(s, vp, errno);
    else if(S_ISDIR(st.st_mode)) reply(s, 550, "%s is a folder", vp);
    else reply(s, 213, "%u", st.st_size);
}

static void cmd_mlst(struct session *s, const char *arg) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX], line[FTP_PATH_MAX + 96];
    struct ftp_entry e;
    struct stat st;
    int n;
    if(resolve(s, arg, vp, real) < 0) return;
    if(stat(real, &st) < 0) {
        failed(s, vp, errno);
        return;
    }
    e.name = vp;
    e.dir = S_ISDIR(st.st_mode);
    e.size = st.st_size;
    e.mtime = st.st_mtime;
    if((n = ftp_mlsx_line(line, sizeof(line), &e, writable_at(real), 1)) < 0) {
        reply(s, 553, "The path is too long");
        return;
    }
    reply_first(s, 250, "Listing %s", vp);
    out_add(s, line, n);                /* the fact line as it is: its space first is RFC 3659's */
    reply(s, 250, "End");
}

static void cmd_stat(struct session *s, const char *arg) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX];
    const char *path;
    struct buf b = { NULL, 0, 0 };
    int all, count, e;
    unsigned int i;
    if(s->state == S_XFER) {            /* during a transfer: how far it is */
        reply(s, 213, "%s %s: %lu bytes so far", s->what, s->xpath, s->bytes);
        return;
    }
    if(!*arg) {
        reply_first(s, 211, "SurfOS ftpd status");
        reply_line(s, "Connected from %s", s->peer_str);
        if(s->logged) reply_line(s, "Logged in; the folder served is %s, %s", conf->root,
                                 conf->readonly ? "read-only" : "to read and write");
        else reply_line(s, "Not logged in");
        reply_line(s, "TYPE: %s; STRUcture: File; transfer MODE: Stream", s->ascii ? "ASCII (text)" : "BINARY");
        reply_line(s, "%s", s->dstate == D_LISTEN ? "Passive mode: listening" : s->dstate == D_READY ?
                   "Passive mode: the data connection is open" : s->dstate == D_PORT ? "Active mode: a PORT is set" :
                   "No data connection");
        reply(s, 211, "End of status");
        return;
    }
    if(!s->logged) {
        reply(s, 530, "Log in with USER and PASS first");
        return;
    }
    ftp_list_args(arg, &all, &path);
    if(resolve(s, path, vp, real) < 0) return;
    if((e = make_listing(FTP_LIST, all, path, real, &b, &count)) != 0) {
        free(b.p);
        failed(s, vp, e);
        return;
    }
    reply_first(s, 213, "Status of %s:", vp);
    for(i = 0; i < b.len; i++)                      /* 10/25/26 (review): a CR or LF in a name would end a line */
        if(((unsigned char)b.p[i] < 0x20 || b.p[i] == 0x7f) && !(b.p[i] == '\r' && i + 1 < b.len && b.p[i + 1] == '\n') &&
           !(b.p[i] == '\n' && i && b.p[i - 1] == '\r'))
            b.p[i] = '?';
    if(b.len) out_add(s, b.p, (int)b.len);         /* LIST's lines: none starts with a digit */
    free(b.p);
    reply(s, 213, "End of status (%d %s)", count, count == 1 ? "entry" : "entries");
}

/* RMD's 550 for a folder that is not empty: hidden files said, which LIST does not show */
static void not_empty(struct session *s, const char *vp, const char *real) {
    struct dirent *de;
    int shown = 0, hidden = 0;
    DIR *d = opendir(real);
    if(d) {
        while((de = readdir(d)) != NULL) {
            if(de->d_name[0] == '.') hidden++;
            else shown++;
        }
        closedir(d);
    }
    if(d && !shown && hidden)
        reply(s, 550, "%s is not empty: it holds %d hidden %s (names starting with a dot: LIST -a shows them)", vp,
              hidden, hidden == 1 ? "file" : "files");
    else reply(s, 550, "%s: Directory not empty", vp);
}

static void cmd_change(struct session *s, int id, const char *arg) {
    char vp[FTP_PATH_MAX], real[FTP_PATH_MAX], q[2 * FTP_PATH_MAX + 2];
    struct stat st;
    int e;
    if(resolve(s, arg, vp, real) < 0) return;
    switch(id) {
    case FTP_DELE:
        if(stat(real, &st) == 0 && S_ISDIR(st.st_mode)) reply(s, 550, "%s is a folder: RMD removes folders", vp);
        else if(unlink(real) < 0) failed(s, vp, errno);
        else reply(s, 250, "Deleted %s", vp);
        return;
    case FTP_MKD:
        if(mkdir(real, 0755) < 0) failed(s, vp, errno);
        else if(ftp_quote(q, sizeof(q), vp) < 0) reply(s, 257, "Created");
        else reply(s, 257, "%s created", q);
        return;
    case FTP_RMD:
        if(!strcmp(vp, "/")) reply(s, 550, "The folder served cannot be removed");
        else if(rmdir(real) == 0) reply(s, 250, "Removed %s", vp);
        else if((e = errno) == ENOTEMPTY) not_empty(s, vp, real);
        else failed(s, vp, e);
        return;
    case FTP_RNFR:
        if(!strcmp(vp, "/")) reply(s, 550, "The folder served cannot be renamed");
        else if(stat(real, &st) < 0) failed(s, vp, errno);
        else {
            strcpy(s->rnfr, real);
            strcpy(s->rnfr_vp, vp);
            reply(s, 350, "%s is ready to be renamed: send RNTO", vp);
        }
        return;
    case FTP_RNTO:
        if(!s->rnfr[0]) reply(s, 503, "RNFR first");
        else if(!strcmp(vp, "/")) reply(s, 550, "/ is the folder served");
        else if(rename(s->rnfr, real) < 0) {
            e = errno;
            if(e == EXDEV) reply(s, 553, "%s and %s are on different volumes: download and upload instead",
                                 s->rnfr_vp, vp);
            else failed(s, vp, e);
        } else reply(s, 250, "Renamed %s to %s", s->rnfr_vp, vp);
        s->rnfr[0] = 0;
        return;
    }
}

static void cmd_type(struct session *s, const char *arg) {
    if(!strcasecmp(arg, "A") || !strcasecmp(arg, "A N")) {
        s->ascii = 1;
        reply(s, 200, "Type set to A: text, its line ends converted");
    } else if(!strcasecmp(arg, "I") || !strcasecmp(arg, "L 8")) {
        s->ascii = 0;
        reply(s, 200, "Type set to I: the bytes as they are");
    } else if(!strcasecmp(arg, "E") || !strncasecmp(arg, "A ", 2) || !strncasecmp(arg, "E ", 2) || !strncasecmp(arg, "L ", 2))
        reply(s, 504, "Type %s is not served: A and I are", arg);
    else reply(s, 501, "Type %s is not understood", arg);
}

static void cmd_port(struct session *s, int id, const char *arg) {
    unsigned int ip;
    unsigned short port;
    char ip_text[16], peer[16];
    int r;
    if(s->epsv_all) {
        reply(s, 503, "EPSV ALL was sent: only EPSV now");
        return;
    }
    r = id == FTP_EPRT ? ftp_parse_eprt(arg, &ip, &port) : ftp_parse_port(arg, &ip, &port);
    if(r == -2) {
        reply(s, 522, "Network protocol not supported, use (1)");
        return;
    }
    if(r < 0) {
        reply(s, 501, "%s %s is not understood", id == FTP_EPRT ? "EPRT" : "PORT", arg);
        return;
    }
    if(ip != s->peer) {
        reply(s, 504, "A data connection to %s is refused: only to the client's own address (%s)", ipstr(ip, ip_text),
              ipstr(s->peer, peer));
        return;
    }
    if(port < 1024) {
        reply(s, 504, "A data connection to port %u is refused: only to ports from 1024", port);
        return;
    }
    drop_data(s);
    s->dstate = D_PORT;
    s->dip = ip;
    s->dport = port;
    s->dsince = uptime();
    reply(s, 200, "%s command successful", id == FTP_EPRT ? "EPRT" : "PORT");
}

static void cmd_passive(struct session *s, int id, const char *arg) {
    char text[80];
    int port;
    if(id == FTP_EPSV && !strcasecmp(arg, "ALL")) {
        s->epsv_all = 1;
        reply(s, 200, "EPSV ALL: only EPSV from now on");
        return;
    }
    if(id == FTP_EPSV && *arg && strcmp(arg, "1")) {
        if(arg[0] >= '0' && arg[0] <= '9') reply(s, 522, "Network protocol not supported, use (1)");
        else reply(s, 501, "EPSV %s is not understood", arg);
        return;
    }
    if(id == FTP_PASV && s->epsv_all) {
        reply(s, 503, "EPSV ALL was sent: only EPSV now");
        return;
    }
    if((port = open_passive(s)) < 0) return;
    if(id == FTP_PASV) {                /* the address the client reached, unless -a gave the router's */
        unsigned int ip = conf->pasv_addr ? conf->pasv_addr : s->local;
        struct netinfo ni;
        if(!ip && netinfo(&ni) == 0) ip = ni.ip;
        ftp_pasv_text(text, sizeof(text), ip, (unsigned short)port);
        reply(s, 227, "%s", text);
    } else {
        ftp_epsv_text(text, sizeof(text), (unsigned short)port);
        reply(s, 229, "%s", text);
    }
}

static void cmd_help(struct session *s) {
    reply_first(s, 214, "The commands SurfOS's ftpd knows:");
    reply_line(s, "USER PASS ACCT QUIT REIN NOOP SYST FEAT OPTS HELP STAT SITE");
    reply_line(s, "PWD CWD CDUP TYPE MODE STRU ALLO PASV EPSV PORT EPRT REST ABOR");
    reply_line(s, "LIST NLST MLSD MLST SIZE RETR STOR STOU APPE DELE MKD RMD RNFR RNTO");
    reply_line(s, "XPWD XCWD XCUP XMKD XRMD HOST CLNT");
    reply(s, 214, "No logins and no encryption: any user name and password are taken");
}

/* the session as a new login finds it (REIN, and a new session) */
static void fresh(struct session *s) {
    s->user = s->logged = s->ascii = s->epsv_all = 0;
    s->rest = 0;
    strcpy(s->cwd, "/");
    s->rnfr[0] = 0;
    drop_data(s);
}

/* the command line (as it came, without its end): run */
static void execute(struct session *s, const char *line) {
    char verb[8];
    const char *arg;
    const struct ftp_verb *v;
    long rest;
    int id;
    s->since = uptime();
    s->code = 0;
    s->began = 0;
    if(ftp_split(line, verb, &arg) < 0) {
        reply(s, 500, "Syntax error: no command");
        note(s, "%s -> %d", line, s->code);
        return;
    }
    v = ftp_find(verb);
    id = v ? v->id : 0;
    if(id != FTP_RNTO) s->rnfr[0] = 0;                      /* RNTO must come right after RNFR */
    if(v && (v->flags & FTP_F_DATA)) {                      /* REST is for the transfer command after it, only */
        rest = s->rest;
        s->rest = 0;
    } else rest = 0;
    if(!v) reply(s, 500, "%s is not understood", verb);
    else if(id == FTP_NOTIMPL) reply(s, 502, "%s is not implemented", verb);
    else if((v->flags & FTP_F_LOGIN) && !s->logged) reply(s, 530, "Log in with USER and PASS first");
    else if((v->flags & FTP_F_ARG) && !*arg) reply(s, 501, "%s needs an argument", verb);
    else if((v->flags & FTP_F_WRITE) && conf->readonly) reply(s, 550, "%s: this folder is served read-only", verb);
    else switch(id) {
    case FTP_USER:
        s->user = 1;
        s->logged = 0;
        reply(s, 331, "Any password will do: SurfOS's ftpd has no logins");
        break;
    case FTP_PASS:
        if(!s->user) reply(s, 503, "USER first");
        else if(s->logged) reply(s, 230, "Logged in already");
        else {
            s->logged = 1;
            reply(s, 230, "Logged in: %s, %s", conf->root, conf->readonly ? "read-only" : "to read and write");
        }
        break;
    case FTP_ACCT: reply(s, 202, "No accounts are needed"); break;
    case FTP_QUIT:
        reply(s, 221, "Goodbye");
        closing(s, "the client left");
        break;
    case FTP_REIN:
        fresh(s);
        reply(s, 220, "Ready for a new login");
        break;
    case FTP_NOOP: reply(s, 200, "NOOP ok"); break;
    case FTP_SYST: reply(s, 215, "UNIX Type: L8"); break;
    case FTP_FEAT: {
        int i;
        reply_first(s, 211, "Features:");
        for(i = 0; feat_lines[i]; i++) reply_line(s, "%s", feat_lines[i]);
        reply(s, 211, "End");
        break;
    }
    case FTP_OPTS:
        if(!strncasecmp(arg, "UTF8", 4) && (!arg[4] || arg[4] == ' ')) reply(s, 200, "Names are UTF-8 always");
        else reply(s, 501, "OPTS %s is not understood", arg);
        break;
    case FTP_HELP: cmd_help(s); break;
    case FTP_STAT: cmd_stat(s, arg); break;
    case FTP_SITE:
        if(!strcasecmp(arg, "HELP")) reply(s, 214, "No SITE commands here");
        else reply(s, 502, "SITE %s is not implemented", arg);
        break;
    case FTP_PWD: {
        char q[2 * FTP_PATH_MAX + 2];
        if(ftp_quote(q, sizeof(q), s->cwd) < 0) reply(s, 257, "\"/\"");
        else reply(s, 257, "%s is the current folder", q);
        break;
    }
    case FTP_CWD: cmd_cwd(s, arg); break;
    case FTP_CDUP: cmd_cwd(s, ".."); break;
    case FTP_TYPE: cmd_type(s, arg); break;
    case FTP_MODE:
        if(!strcasecmp(arg, "S")) reply(s, 200, "Mode S (stream)");
        else if(!strcasecmp(arg, "B") || !strcasecmp(arg, "C")) reply(s, 504, "Mode %s is not served: S is", arg);
        else reply(s, 501, "Mode %s is not understood", arg);
        break;
    case FTP_STRU:
        if(!strcasecmp(arg, "F")) reply(s, 200, "Structure F (file)");
        else if(!strcasecmp(arg, "R") || !strcasecmp(arg, "P")) reply(s, 504, "Structure %s is not served: F is", arg);
        else reply(s, 501, "Structure %s is not understood", arg);
        break;
    case FTP_ALLO: reply(s, 202, "ALLO is not needed"); break;
    case FTP_PASV: case FTP_EPSV: cmd_passive(s, id, arg); break;
    case FTP_PORT_CMD: case FTP_EPRT: cmd_port(s, id, arg); break;
    case FTP_LIST: case FTP_NLST: case FTP_MLSD: cmd_list(s, id, arg); break;
    case FTP_MLST: cmd_mlst(s, arg); break;
    case FTP_SIZE: cmd_size(s, arg); break;
    case FTP_RETR: cmd_retr(s, arg, rest); break;
    case FTP_STOR: case FTP_APPE: case FTP_STOU: cmd_store(s, id, arg, rest); break;
    case FTP_REST: {
        long off;
        if(ftp_parse_offset(arg, &off) < 0) reply(s, 501, "REST %s: an offset is digits, below 2 GB", arg);
        else {
            s->rest = off;
            reply(s, 350, "Restarting at %ld: send RETR or STOR", off);
        }
        break;
    }
    case FTP_ABOR:
        if(s->state == S_XFER) {        /* the transfer's own reply, then ABOR's */
            unsigned long moved = s->bytes;
            release_transfer(s);
            drop_data(s);
            s->state = S_CMD;
            reply(s, 426, "The transfer was aborted: %lu bytes moved", moved);
            note(s, "%s %s -> 426, aborted, %lu bytes", s->what, s->xpath, moved);
            reply(s, 226, "ABOR: the data connection is closed");
        } else {
            drop_data(s);
            reply(s, 225, "No transfer to abort");
        }
        break;
    case FTP_DELE: case FTP_MKD: case FTP_RMD: case FTP_RNFR: case FTP_RNTO: cmd_change(s, id, arg); break;
    case FTP_HOST: reply(s, 220, "Host noted: SurfOS's ftpd serves one site"); break;
    case FTP_CLNT: reply(s, 200, "Noted"); break;
    }
    if(s->began && id != FTP_STAT) return;  /* logged at the transfer's end */
    if(id == FTP_PASS) note(s, "PASS **** -> %d", s->code);
    else note(s, "%s -> %d", line, s->code);
}

/* is line a command answered during a transfer? ABOR and STAT */
static int urgent(const char *line) {
    char verb[8];
    const char *arg;
    return ftp_split(line, verb, &arg) == 0 && (!strcmp(verb, "ABOR") || !strcmp(verb, "STAT"));
}

/* the commands in raw[], one at a time, as far as they may run now: during a transfer ABOR and STAT, the
   others held (HELD_MAX) to run in their order after it */
static void run_commands(struct session *s) {
    while(s->state == S_CMD || s->state == S_XFER) {
        int got, k, i;
        if(s->broken) {
            end_session(s, "out of memory for its replies");
            return;
        }
        if(s->out_len - s->out_off >= OUT_WAIT) return;         /* the replies go first */
        if(s->state == S_CMD && s->held_n) {
            i = s->held_first;
            s->held_first = (i + 1) % HELD_MAX;
            s->held_n--;
            if(s->held_long[i]) reply(s, 500, "The command line is longer than %d bytes", FTP_LINE_MAX);
            else execute(s, s->held[i]);
            continue;
        }
        if(s->state == S_XFER && s->held_n == HELD_MAX) return;  /* the rest after the transfer */
        if(s->raw_off == s->raw_len) {
            s->raw_off = s->raw_len = 0;
            if(s->eof && s->state == S_CMD) closing(s, "the client left");     /* everything it sent is answered */
            return;
        }
        k = ftp_ctl_feed(&s->ctl, s->raw + s->raw_off, s->raw_len - s->raw_off, &got);
        s->raw_off += k;
        if(got == FTP_MORE) continue;
        if(s->state == S_XFER && (got == FTP_LONG || !urgent(s->ctl.line))) {
            i = (s->held_first + s->held_n++) % HELD_MAX;
            s->held_long[i] = got == FTP_LONG;
            strcpy(s->held[i], got == FTP_LONG ? "" : s->ctl.line);
            continue;
        }
        if(got == FTP_LONG) reply(s, 500, "The command line is longer than %d bytes", FTP_LINE_MAX);
        else execute(s, s->ctl.line);
    }
}

/* the control connection has bytes, or its end (POLLIN) */
static void control_read(struct session *s) {
    ssize_t n;
    if(s->raw_off) {
        memmove(s->raw, s->raw + s->raw_off, (size_t)(s->raw_len - s->raw_off));
        s->raw_len -= s->raw_off;
        s->raw_off = 0;
    }
    n = read(s->fd, s->raw + s->raw_len, (size_t)(RAW_SIZE - s->raw_len));
    if(n < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        end_session(s, "the connection failed");
        return;
    }
    if(n == 0) s->eof = 1;              /* what it sent is run and answered, then the session closes */
    else s->raw_len += (int)n;
    run_commands(s);
}

/* what poll() said of the control connection (asked: the events it was asked for) */
static void control_event(struct session *s, short asked, short ev) {
    unsigned int epoch = s->epoch;
    if(ev & POLLOUT) to_client(s);
    if(s->state == S_FREE || s->epoch != epoch) return;
    if(ev & POLLNVAL) end_session(s, "its connection is gone");
    else if((asked & POLLIN) && (ev & (POLLIN | POLLHUP | POLLERR))) control_read(s);
    else if(ev & POLLERR) end_session(s, "the connection failed");
    else if((ev & POLLHUP) && !(ev & POLLOUT)) {    /* half-closed, and nothing to do now: rest, never spin */
        s->crest = 1;
        s->crest_end = uptime() + REST_MS;
    }
}

/* what poll() said of a transfer's data connection */
static void data_event(struct session *s, short ev) {
    if(s->connecting) {                 /* active mode's: made (POLLOUT), or failed (read() says why) */
        char ip[16], b;
        int e;
        if(ev & POLLOUT) {
            s->connecting = 0;
            s->xsince = uptime();
            return;                     /* the next poll() moves the bytes */
        }
        e = read(s->conn, &b, 1) < 0 ? errno : ECONNRESET;
        end_transfer(s, 425, "No data connection to %s port %u: %s", ipstr(s->dip, ip), s->dport, strerror(e));
        return;
    }
    if(s->dir == X_RECV) {
        recv_step(s);                   /* the read says which: data, the end, an error */
        return;
    }
    if(ev & POLLOUT) send_step(s);
    else if(ev & (POLLERR | POLLNVAL)) end_transfer(s, 426, "The data connection failed: the transfer is aborted");
    else if(ev & POLLHUP) {             /* a client that closed its side and takes nothing now: rest */
        s->drest = 1;
        s->drest_end = uptime() + REST_MS;
    }
}

/**** the loop ****/

/* a free place, or NULL */
static struct session *free_place(void) {
    int i;
    for(i = 0; i < MAX_SESSIONS; i++) if(sessions[i].state == S_FREE) return &sessions[i];
    return NULL;
}

static void new_session(int lfd) {
    static const char full[] = "421 Too many sessions are open; try again later\r\n";
    struct netaddr from, here;
    struct session *s = free_place();
    char ip[16];
    int fd = tcp_accept(lfd, &from);
    if(fd < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        if(errno == EBADF || errno == EINVAL) {         /* descriptor 0 is no listening socket after all */
            say("ftpd: accept: %s; the server ends", strerror(errno));
            _exit(1);
        }
        say("ftpd: accept: %s; new sessions wait a moment", strerror(errno));     /* ENOMEM, EMFILE, ENFILE */
        resting = 1;
        rest_end = uptime() + PAUSE_MS;
        return;
    }
    if(!s) {                            /* a new connection's send buffer is empty: this write does not wait */
        write(fd, full, sizeof(full) - 1);
        close(fd);
        say("ftpd: a session from %s refused: %d are open", ipstr(from.ip, ip), MAX_SESSIONS);
        if(conf->verbose) {
            ttyctl(TTY_PAGE, 0);        /* as note() */
            printf("%s refused: %d sessions are open\n", ip, MAX_SESSIONS);
        }
        return;
    }
    memset(s, 0, offsetof(struct session, dbuf));
    s->fd = fd;
    s->dfd = s->conn = s->file = -1;
    s->peer = from.ip;
    s->peer_port = from.port;
    ipstr(from.ip, s->peer_str);
    if(tcp_addrs(fd, &here, NULL) == 0) s->local = here.ip;
    s->id = ++next_id;
    s->state = S_CMD;
    s->since = uptime();
    fresh(s);
    say("ftpd: session %d from %s:%u", s->id, s->peer_str, s->peer_port);
    note(s, "session %d from port %u", s->id, s->peer_port);
    reply(s, 220, "SurfOS ftpd: the files of %s, %s. No passwords, no encryption", conf->root,
          conf->readonly ? "read-only" : "to read and write");
}

/* sessions and transfers whose time is up */
static void expire(unsigned int now) {
    int i;
    for(i = 0; i < MAX_SESSIONS; i++) {
        struct session *s = &sessions[i];
        if(s->state == S_FREE) continue;
        if(s->crest && !before(now, s->crest_end)) s->crest = 0;
        if(s->drest && !before(now, s->drest_end)) s->drest = 0;
        if(s->out_len > s->out_off && now - s->out_since >= STALL_MS) {
            end_session(s, "its replies were not taken for 60 s");
            continue;
        }
        if(s->state == S_CLOSING) {
            if(s->out_len == s->out_off) end_session(s, s->why);
        } else if(s->state == S_CMD) {
            if(now - s->since >= IDLE_MS) {
                reply(s, 421, "No command for 5 minutes: the session is closed");
                closing(s, "no command for 5 minutes");
            } else if((s->dstate == D_LISTEN || s->dstate == D_READY) && now - s->dsince >= DATA_MS) drop_data(s);
        } else if((s->conn < 0 || s->connecting) && now - s->xsince >= DATA_MS)
            end_transfer(s, 425, "No data connection %s within %d s", s->connecting ? "was made" : "came", DATA_MS / 1000);
        else if(s->conn >= 0 && !s->connecting && now - s->xsince >= STALL_MS)
            end_transfer(s, 426, "Nothing moved for %d s: the transfer is aborted", STALL_MS / 1000);
    }
}

/* the time by which something is due for s */
static unsigned int deadline(const struct session *s, unsigned int now) {
    unsigned int end = now + IDLE_MS;
    if(s->state == S_CMD) {
        end = s->since + IDLE_MS;
        if((s->dstate == D_LISTEN || s->dstate == D_READY) && before(s->dsince + DATA_MS, end)) end = s->dsince + DATA_MS;
    } else if(s->state == S_XFER) end = s->xsince + (s->conn < 0 || s->connecting ? DATA_MS : STALL_MS);
    if(s->out_len > s->out_off && before(s->out_since + STALL_MS, end)) end = s->out_since + STALL_MS;
    if(s->crest && before(s->crest_end, end)) end = s->crest_end;
    if(s->drest && before(s->drest_end, end)) end = s->drest_end;
    return end;
}

/* how long poll() may wait: until the first deadline, or the listener's rest */
static int wait_ms(unsigned int now) {
    int i, ms = -1, left;
    for(i = 0; i < MAX_SESSIONS; i++) {
        if(sessions[i].state == S_FREE) continue;
        left = (int)(deadline(&sessions[i], now) - now);
        if(left < 0) left = 0;
        if(ms < 0 || left < ms) ms = left;
    }
    if(resting) {
        left = (int)(rest_end - now) > 0 ? (int)(rest_end - now) : 0;
        if(ms < 0 || left < ms) ms = left;
    }
    return ms;
}

enum { P_LISTENER, P_CONTROL, P_DATA };

int serve(int lfd, const struct ftpd_conf *c) {
    struct pollfd fds[1 + 2 * MAX_SESSIONS];
    struct session *who[1 + 2 * MAX_SESSIONS];
    unsigned int epochs[1 + 2 * MAX_SESSIONS];
    int kinds[1 + 2 * MAX_SESSIONS];
    int i, pass;
    conf = c;
    for(i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].state = S_FREE;
        sessions[i].fd = sessions[i].dfd = sessions[i].conn = sessions[i].file = -1;
    }
    if(!conf->verbose) {
        char at[24] = "";
        kmsg = open("/dev/kmsg", O_WRONLY);
        if(detach() < 0) say("ftpd: detach: %s", strerror(errno));
        if(conf->pasv_addr) {
            char ip[16];
            snprintf(at, sizeof(at), " at %s", ipstr(conf->pasv_addr, ip));
        }
        say("ftpd: serving %s on port %d (%s, passive ports %d-%d%s)", conf->root, conf->port,
            conf->readonly ? "read-only" : "read-write", conf->pasv_first, conf->pasv_last, at);
    }
    chdir("/");                                 /* no hold on a stick through the current directory */
    for(;;) {
        unsigned int now = uptime();
        int n = 0, r;
        expire(now);
        if(resting && !before(now, rest_end)) resting = 0;
        if(!resting) {
            fds[n].fd = lfd;
            fds[n].events = POLLIN;
            who[n] = NULL;
            kinds[n++] = P_LISTENER;
        }
        for(i = 0; i < MAX_SESSIONS; i++) {
            struct session *s = &sessions[i];
            short ev = 0;
            if(s->state == S_FREE) continue;
            if(s->out_len > s->out_off) ev |= POLLOUT;
            if(!s->eof && s->raw_len < RAW_SIZE && s->out_len - s->out_off < OUT_WAIT &&
               (s->state == S_CMD || (s->state == S_XFER && s->held_n < HELD_MAX))) ev |= POLLIN;
            if(!s->crest && (ev || !s->eof)) {  /* POLLHUP and POLLERR come whatever is asked */
                fds[n].fd = s->fd;
                fds[n].events = ev;
                who[n] = s;
                epochs[n] = s->epoch;
                kinds[n++] = P_CONTROL;
            }
            if(s->state == S_CLOSING) continue;
            if(s->dstate == D_LISTEN) {
                fds[n].fd = s->dfd;
                fds[n].events = POLLIN;
            } else if(s->state == S_XFER && s->conn >= 0 && !s->drest) {
                fds[n].fd = s->conn;
                fds[n].events = s->connecting || s->dir == X_SEND ? POLLOUT : POLLIN;
            } else continue;
            who[n] = s;
            epochs[n] = s->epoch;
            kinds[n++] = P_DATA;
        }
        for(i = 0; i < n; i++) fds[i].revents = 0;
        r = poll(fds, (nfds_t)n, wait_ms(now));
        if(r < 0) {
            if(errno == EINTR) continue;        /* a kill ends this task when the call does */
            say("ftpd: poll: %s; the server ends", strerror(errno));
            return 1;
        }
        /* the data connections first, then the control connections (10/25/26): a passive connection that came
           with its RETR is taken before the RETR runs, which then starts at once */
        for(pass = 0; pass < 2; pass++) {
            for(i = 0; i < n; i++) {
                struct session *s = who[i];
                short ev = fds[i].revents;
                if(!ev || kinds[i] == P_LISTENER || (kinds[i] == P_CONTROL) != pass) continue;
                if(s->state == S_FREE || s->epoch != epochs[i]) continue;  /* it changed since: the next poll() looks again */
                if(kinds[i] == P_CONTROL) control_event(s, fds[i].events, ev);
                else if(s->dstate == D_LISTEN && fds[i].fd == s->dfd) data_accept(s);
                else if(s->state == S_XFER && fds[i].fd == s->conn) data_event(s, ev);
            }
        }
        if(n && kinds[0] == P_LISTENER && fds[0].revents) new_session(lfd);
        for(i = 0; i < MAX_SESSIONS; i++) {     /* commands that waited for replies to go, or for a transfer */
            struct session *s = &sessions[i];
            if((s->state == S_CMD && (s->held_n || s->raw_off < s->raw_len || s->eof)) ||
               (s->state == S_XFER && s->raw_off < s->raw_len))        /* an ABOR behind replies not taken */
                run_commands(s);
        }
    }
}
