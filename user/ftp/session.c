/*
SurfOS file transfer client: ftp
--------------------
File: session.c   Date: 11/01/26
--------------------
A session with an FTP server (ftpsession.h). The server is not trusted with anything:

- Every wait is bounded: a reply within REPLY_MS, a byte of data within DATA_MS, and a write to the
  control connection only after poll() says it will not wait. A reply is bounded in lines and bytes
  (ftpclient.c); what comes that is no reply ends the connection.
- A data connection is passive (EPSV, then PASV) and goes to the address the control connection
  reached, whatever a 227 reply names, and to a port from 1024.
- A file is good only when the server said 2xx after its last byte, and, in binary mode, when as
  many bytes came as SIZE said. Until then nothing the user had is touched: a file that was there
  is replaced through saveutil.h at the end, a file that was not is made with O_EXCL and deleted
  again. reget is the exception, by what it is for: it adds to a file and keeps what came, so the
  next reget goes on. It cuts the file back only when the server sent more than SIZE left room for,
  which means it did not go on from where it was told to.
- The password is typed with no echo (raw mode, on the terminal itself even when the commands come
  from a pipe), goes straight to the connection and is wiped, and is scrubbed from what the server
  says in answer. Nothing here prints what it sends.

Ctrl+C during a command (TTY_INTR): a transfer, once its command has gone, is aborted (ABOR, the
local file cleaned up as for any failure) and the session goes on, or ends if the server does not
answer; any other command
has no way back in step with the server, so the connection is closed. A kill leaves a partial file
or a .tmp where it was: SurfOS has no way to tell a program.
*/

#include <surf.h>
#include "../bin/saveutil.h"
#include "ftpclient.h"
#include "ftpsession.h"

#define CTL_PIECE    1024               /* read from the control connection at a time */
#define DATA_PIECE   16384              /* read from or written to a data connection at a time */
#define REPLY_MS     120000             /* a whole reply; a server may sync a big file first */
#define DATA_MS      60000              /* a data connection that moves no byte */
#define SEND_MS      30000              /* a command the server does not take */
#define ABORT_MS     5000               /* the answer to ABOR */
#define ABORT2_MS    1000               /* and a second reply, if it sends one */
#define QUIT_MS      5000
#define STEP_MS      500                /* a Ctrl+C just before a wait is seen this soon */
#define PRELIMS      16                 /* 1xx replies taken before the real one */
#define DRAIN_MAX    64                 /* replies nobody asked for, taken before a command */
#define ANON_USER    "anonymous"
#define ANON_PASS    "anonymous@surfos"
#define FILE_LIMIT   0xffffffffULL      /* the most a file may hold */

enum { L_STDOUT, L_NEW, L_SAVE, L_APPEND, L_DEVICE };   /* where a download goes */
enum { TO_FILE, TO_SHOW, TO_NAMES };

static struct {
    int ctl;                            /* the control connection, -1 without one */
    unsigned int ip;                    /* its other end: data connections go only there */
    int port;
    char peer[16];
    char user[64];
    int logged;
    int text;                           /* ascii */
    char type_sent;                     /* the TYPE in force: 'A', 'I', 0 for none yet */
    int no_epsv;                        /* the server does not know EPSV */
    int stopped;                        /* Ctrl+C */
    const char *secret;                 /* the password, while its answer is read */
    struct ftp_ctl lines;
    char in[CTL_PIECE];
    int in_off, in_len;
} s;

struct local {
    const char *path;
    int how;                            /* L_*: where it goes */
    int fd;
    int keep;                           /* reget: what came stays, also on a failure */
    unsigned int start;                 /* L_APPEND: the file's size before */
    struct su_save save;
};

struct sink {
    int kind;                           /* TO_* */
    struct local *file;                 /* TO_FILE */
    int text, held;                     /* TO_FILE, ascii: CR LF becomes LF; a CR held to see what follows */
    unsigned long long bytes;           /* what came */
    unsigned long long want;            /* what SIZE leaves to come, when have_want */
    int have_want;
    unsigned long long limit;           /* reget: the most that may come, 0 for no limit */
    struct ftpc_lines lines;            /* TO_SHOW, TO_NAMES */
    int long_lines;
    struct fs_names *names;
    int names_bytes;
    int failed;                         /* a write failed: errno said */
};

static char data_buf[DATA_PIECE], text_buf[2 * DATA_PIECE + 2];
static struct local dl;                 /* holds a struct su_save, too big for a stack it need not take */

void fs_init(void) {
    memset(&s, 0, sizeof(s));
    s.ctl = -1;
}

int fs_connected(void) {
    return s.ctl >= 0;
}

int fs_stopped(void) {
    int was = s.stopped;
    s.stopped = 0;
    return was;
}

void fs_set_text(int text) {
    s.text = text;
}

void fs_catch_interrupts(int on) {
    ttyctl(TTY_INTR, (unsigned int)on);
}

/* text from a server or a name, safe to show */
static const char *safe(const char *text) {
    static char out[2][FTP_LINE_MAX + 8];
    static int which;
    which = !which;
    ftpc_text(out[which], sizeof(out[which]), text);
    return out[which];
}

static void say_err(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));
static void say_err(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fputs("ftp: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/**** waiting ****/

/* has a Ctrl+C come since the last look? Once for each: the ABOR that follows one must not be stopped by it, and a
   second Ctrl+C stops that too */
static int interrupted(void) {
    if(ttyctl(TTY_INTR, 1) <= 0) return 0;
    s.stopped = 1;
    return 1;
}

/* poll within ms: what it says (0 for the time being up), -1 and errno, -2 for Ctrl+C. A Ctrl+C cuts poll() short,
   unless it came just before the call: so it waits in steps */
static int wait_ready(struct pollfd *p, int n, int ms) {
    unsigned int start = uptime();
    for(;;) {
        int left = ms - (int)(uptime() - start), r;
        if(interrupted()) return -2;
        if(left < 0) left = 0;
        r = poll(p, (nfds_t)n, left > STEP_MS ? STEP_MS : left);
        if(r > 0) return r;
        if(r < 0 && errno != EINTR) return -1;
        if(interrupted()) return -2;
        if(r == 0 && left <= STEP_MS) return 0;
    }
}

/**** the control connection ****/

/* the connection is over, for a reason said here */
static void drop(const char *why) {
    if(s.ctl < 0) return;
    say_err("%s: the connection is closed: %s", s.peer, why);
    close(s.ctl);
    s.ctl = -1;
    s.logged = 0;
    s.in_off = s.in_len = 0;
}

/* a reply wait that ended in r (reply_wait()): the status */
static int failed(int r) {
    if(r == -2) drop("stopped by Ctrl+C");
    return FS_TROUBLE;
}

/* what is in s.in, into r: 1 a reply is whole, 0 more is needed, -1 this is no FTP */
static int feed_lines(struct ftpc_reply *r, int show) {
    while(s.in_off < s.in_len) {
        int got, k = ftp_ctl_feed(&s.lines, s.in + s.in_off, s.in_len - s.in_off, &got), v;
        s.in_off += k;
        if(got == FTP_MORE) continue;
        if(got == FTP_LINE && s.secret) ftpc_scrub(s.lines.line, s.secret);
        v = ftpc_reply_line(r, s.lines.line, got == FTP_LONG);
        if(v == FTPC_R_BAD) return -1;
        if(show && got == FTP_LINE) printf("%s\n", safe(s.lines.line));
        if(v == FTPC_R_DONE) return 1;
    }
    return 0;
}

/* the next piece from the server into s.in, within ms: 1, 0 at its end, -1 an error, -2 Ctrl+C, -3 no time */
static int read_more(int ms) {
    struct pollfd p;
    ssize_t n;
    int r;
    p.fd = s.ctl;
    p.events = POLLIN;
    p.revents = 0;
    r = wait_ready(&p, 1, ms);
    if(r == 0) return -3;
    if(r < 0) return r;
    n = read(s.ctl, s.in, sizeof(s.in));
    if(n < 0) return errno == EINTR ? 1 : -1;
    s.in_off = 0;
    s.in_len = (int)n;
    return n ? 1 : 0;
}

/* the rest of the reply r has begun, within ms: its code; 0 the time was up (the connection stays); -1 the connection
   is gone (said); -2 Ctrl+C */
static int reply_wait(struct ftpc_reply *r, int ms, int show) {
    unsigned int start = uptime();
    for(;;) {
        int v = feed_lines(r, show), left;
        if(v > 0) return r->code;
        if(v < 0) {
            drop("what the server says is not FTP");
            return -1;
        }
        left = ms - (int)(uptime() - start);
        if(left <= 0) return 0;
        v = read_more(left);
        if(v == -3) return 0;
        if(v == -2) return -2;
        if(v == -1) {
            drop(strerror(errno));
            return -1;
        }
        if(v == 0) {
            drop("the server closed it");
            return -1;
        }
    }
}

/* the next reply: its code, -1 or -2 */
static int reply(struct ftpc_reply *r, int show) {
    int c;
    ftpc_reply_init(r);
    c = reply_wait(r, REPLY_MS, show);
    if(c == 0) {
        drop("no reply came");
        return -1;
    }
    return c;
}

/* the next reply that is not a 1xx */
static int final_reply(struct ftpc_reply *r, int show) {
    int i, c;
    for(i = 0; i < PRELIMS; i++) {
        c = reply(r, show);
        if(c < 100 || c >= 200) return c;
    }
    drop("no end to its replies");
    return -1;
}

/* what the server said on its own (421, going away) before a command, so that its answer is that command's */
static void drain(void) {
    struct ftpc_reply r;
    int n;
    for(n = 0; n < DRAIN_MAX && s.ctl >= 0; n++) {
        struct pollfd p;
        int v;
        if(s.in_off == s.in_len) {
            p.fd = s.ctl;
            p.events = POLLIN;
            p.revents = 0;
            if(poll(&p, 1, 0) <= 0) return;
            v = read_more(0);
            if(v == 0) drop("the server closed it");
            if(v <= 0) return;
        }
        ftpc_reply_init(&r);
        if(feed_lines(&r, 1) < 0) drop("what the server says is not FTP");
        else if(r.code == 421) drop("the server is going away");
    }
    if(s.ctl >= 0 && n == DRAIN_MAX) drop("the server will not stop talking");
}

/* a command, written whole: 0; -1 said. What it sends is never shown or kept: a password may be in it */
static int send_cmd(const char *verb, const char *arg) {
    char out[2 * FTP_LINE_MAX + 16];
    int n = ftpc_command(out, sizeof(out), verb, arg), off = 0, r = 0;
    if(n < 0) {
        say_err("%s: too long, or a control character is in it", arg ? safe(arg) : verb);
        return -1;
    }
    while(off < n) {
        struct pollfd p;
        ssize_t w;
        p.fd = s.ctl;
        p.events = POLLOUT;
        p.revents = 0;
        r = wait_ready(&p, 1, SEND_MS);
        if(r <= 0) {
            if(r == -2) drop("stopped by Ctrl+C");
            else drop(r ? strerror(errno) : "the server takes no command");
            r = -1;
            break;
        }
        w = write(s.ctl, out + off, (size_t)(n - off));
        r = 0;
        if(w < 0) {
            if(errno == EINTR) continue;
            drop(strerror(errno));
            r = -1;
            break;
        }
        off += (int)w;
    }
    ftpc_wipe(out, sizeof(out));
    return r;
}

/* said on stderr: what it was for, and the server's own words */
static int negative(const char *what, const struct ftpc_reply *r) {
    say_err("%s: %d %s", safe(what), r->code, safe(r->text));
    if(r->code == 421) drop("the server is going away");
    return FS_NO;
}

/* a command and its final reply: the code, -1 or -2 */
static int ask(const char *verb, const char *arg, struct ftpc_reply *r, int show) {
    drain();
    if(s.ctl < 0 || send_cmd(verb, arg) < 0) return -1;
    return final_reply(r, show);
}

/* a command whose success is a 2xx, shown */
static int simple(const char *verb, const char *arg, const char *what) {
    struct ftpc_reply r;
    int c = ask(verb, arg, &r, 0);
    if(c < 0) return failed(c);
    if(c >= 300) return negative(what, &r);
    printf("%d %s\n", c, safe(r.text));
    return FS_OK;
}

/**** logging in ****/

/* the password, from the terminal itself (the commands may come from a pipe): 0, -1 with nothing to send */
static int ask_secret(struct ftpc_secret *sec, const char *user) {
    char prompt[100];
    int fd, r = FTPC_S_MORE;
    if((fd = open("/dev/tty", O_RDWR)) < 0) {
        say_err("%s: a password is needed, and there is no terminal to ask on", safe(user));
        return -1;
    }
    fflush(stdout);
    snprintf(prompt, sizeof(prompt), "Password for %s: ", safe(user));
    /* raw mode first: a key that came between the prompt and raw mode would be echoed */
    if(ttyctl(TTY_RAW, 1) < 0 || write(fd, prompt, strlen(prompt)) < 0) {
        ttyctl(TTY_RAW, 0);
        say_err("%s: no way to ask for the password here", safe(user));
        close(fd);
        return -1;
    }
    memset(sec, 0, sizeof(*sec));
    while(r == FTPC_S_MORE) {
        unsigned char c;
        ssize_t n = read(fd, &c, 1);
        if(n < 0 && errno == EINTR) continue;
        r = n <= 0 ? FTPC_S_CANCEL : ftpc_secret_key(sec, c);
        c = 0;
    }
    ttyctl(TTY_RAW, 0);
    if(write(fd, "\n", 1) < 0) r = FTPC_S_CANCEL;
    close(fd);
    if(r == FTPC_S_DONE && !sec->over) return 0;
    r = sec->over;
    ftpc_wipe(sec, sizeof(*sec));
    say_err("%s", r ? "that password is too long" : "no password was given");
    return -1;
}

int fs_login(const char *user) {
    static struct ftpc_secret sec;
    struct ftpc_reply r;
    const char *name = user ? user : ANON_USER;
    int anonymous = !strcasecmp(name, ANON_USER) || !strcasecmp(name, "ftp"), c;
    if(s.ctl < 0) return FS_TROUBLE;
    s.logged = 0;
    c = ask("USER", name, &r, 1);
    if(c < 0) return failed(c);
    if(c == 331) {
        if(anonymous) c = ask("PASS", ANON_PASS, &r, 1);
        else {
            if(ask_secret(&sec, name) < 0) return FS_TROUBLE;
            s.secret = sec.text;
            c = ask("PASS", sec.text, &r, 1);
            s.secret = NULL;
            ftpc_wipe(&sec, sizeof(sec));
        }
        if(c < 0) return failed(c);
    }
    if(c == 332) {
        say_err("%s: the server wants an account, which this client does not send", safe(name));
        return FS_NO;
    }
    if(c < 200 || c >= 300) {
        if(c >= 400) {
            if(r.code == 421) drop("the server is going away");
            return FS_NO;
        }
        say_err("%s: the server did not log in: %d", safe(name), c);
        return FS_NO;
    }
    s.logged = 1;
    snprintf(s.user, sizeof(s.user), "%s", name);
    return FS_OK;
}

int fs_open(const char *host, int port) {
    struct ftpc_reply r;
    unsigned int ip;
    int c;
    if(s.ctl >= 0) fs_close();
    if(ipparse(host, &ip) < 0 && net_resolve(host, &ip) < 0) {
        say_err("%s: %s", safe(host), errno == ENOENT ? "no such host" : errno == EINVAL ? "not a host name" : strerror(errno));
        return FS_TROUBLE;
    }
    ipstr(ip, s.peer);
    s.ip = ip;
    s.port = port;
    if((s.ctl = tcp_connect(ip, (unsigned short)port)) < 0) {
        s.ctl = -1;
        say_err("%s port %d: %s", s.peer, port, strerror(errno));
        return FS_TROUBLE;
    }
    memset(&s.lines, 0, sizeof(s.lines));
    s.in_off = s.in_len = s.logged = s.no_epsv = 0;
    s.type_sent = 0;
    s.user[0] = 0;
    c = final_reply(&r, 1);
    if(c < 0) return failed(c);
    if(c != 220) {
        negative(s.peer, &r);
        drop("the server will not talk");
        return FS_TROUBLE;
    }
    return FS_OK;
}

int fs_close(void) {
    struct ftpc_reply r;
    int c;
    if(s.ctl < 0) return FS_OK;
    drain();
    if(s.ctl >= 0 && send_cmd("QUIT", NULL) == 0) {
        ftpc_reply_init(&r);
        c = reply_wait(&r, QUIT_MS, 1);
        if(c < 0) return failed(c);
    }
    if(s.ctl >= 0) close(s.ctl);
    s.ctl = -1;
    s.logged = 0;
    return FS_OK;
}

/**** simple commands ****/

int fs_cd(const char *path) {
    return simple("CWD", path, path);
}

int fs_verb(const char *verb, const char *arg) {
    return simple(verb, arg, arg);
}

int fs_pwd(void) {
    struct ftpc_reply r;
    char path[FTP_LINE_MAX];
    int c = ask("PWD", NULL, &r, 0);
    if(c < 0) return failed(c);
    if(c >= 300) return negative("pwd", &r);
    if(ftpc_parse_path(r.text, path, sizeof(path)) == 0) printf("%s\n", safe(path));
    else printf("%s\n", safe(r.text));
    return FS_OK;
}

int fs_rename(const char *from, const char *to) {
    struct ftpc_reply r;
    int c = ask("RNFR", from, &r, 0);
    if(c < 0) return failed(c);
    if(c != 350) return negative(from, &r);
    return simple("RNTO", to, to);
}

/* the type for the next transfer */
static int set_type(void) {
    struct ftpc_reply r;
    char want = s.text ? 'A' : 'I', arg[2];
    int c;
    if(s.type_sent == want) return FS_OK;
    arg[0] = want;
    arg[1] = 0;
    c = ask("TYPE", arg, &r, 0);
    if(c < 0) return failed(c);
    if(c >= 300) return negative("the transfer type", &r);
    s.type_sent = want;
    return FS_OK;
}

/* SIZE, in binary mode: 0 with *size; 1 the server does not say; FS_TROUBLE, the session is gone */
static int ask_size(const char *path, unsigned long long *size) {
    struct ftpc_reply r;
    int c = ask("SIZE", path, &r, 0);
    if(c < 0) {
        failed(c);
        return -1;
    }
    if(c == 421) negative(path, &r);
    return c == 213 && ftpc_parse_size(r.text, size) == 0 ? 0 : 1;
}

int fs_size(const char *path) {
    struct ftpc_reply r;
    unsigned long long size;
    int c;
    if((c = set_type()) != FS_OK) return c;
    if((c = ask("SIZE", path, &r, 0)) < 0) return failed(c);
    if(c != 213) return negative(path, &r);
    if(ftpc_parse_size(r.text, &size) < 0) {
        say_err("%s: the server's answer is no size: %s", safe(path), safe(r.text));
        return FS_TROUBLE;
    }
    printf("%s: %llu bytes\n", safe(path), size);
    return FS_OK;
}

/**** the data connection ****/

/* where a data connection goes: 0 with *port, a status when there is none */
static int passive_port(unsigned short *port) {
    struct ftpc_reply r;
    int c;
    if(!s.no_epsv) {
        c = ask("EPSV", NULL, &r, 0);
        if(c < 0) return failed(c);
        if(c == 229 && ftpc_parse_epsv(r.text, port) == 0) {
            if(ftpc_port_ok(*port)) return 0;
            say_err("the server names port %u for its data: a service's", (unsigned int)*port);
            return FS_TROUBLE;
        }
        if(c == 421) return negative("EPSV", &r);
        if(c == 500 || c == 501 || c == 502) s.no_epsv = 1;     /* it does not know it: PASV alone from now on */
    }
    c = ask("PASV", NULL, &r, 0);
    if(c < 0) return failed(c);
    if(c >= 400) return negative("no passive data connection", &r);
    if(c != 227) {
        say_err("the server's answer to PASV is no passive reply: %d %s", c, safe(r.text));
        return FS_TROUBLE;
    }
    if(ftpc_parse_pasv(r.text, port) < 0) {
        say_err("the server's passive reply cannot be read: %s", safe(r.text));
        return FS_TROUBLE;
    }
    if(!ftpc_port_ok(*port)) {
        say_err("the server names port %u for its data: a service's", (unsigned int)*port);
        return FS_TROUBLE;
    }
    return 0;
}

/* a data connection: the descriptor, or -1 with *status */
static int open_data(int *status) {
    unsigned short port;
    int c = passive_port(&port), fd;
    if(c) {
        *status = c;
        return -1;
    }
    fd = tcp_connect(s.ip, port);
    if(fd < 0) {
        if(errno == EINTR) interrupted();
        say_err("the data connection to %s port %u: %s", s.peer, (unsigned int)port, strerror(errno));
        *status = FS_TROUBLE;
        return -1;
    }
    return fd;
}

/* the transfer stops here: ABOR first, as the end of the data connection alone would tell a server taking an upload
   that the file is whole; then the data connection goes, which frees a server waiting to write to it, and then the
   server's answers. One that does not answer is left */
static void abort_transfer(struct ftpc_reply *r, int dfd) {
    int c = 0, i;
    if(s.ctl < 0 || send_cmd("ABOR", NULL) < 0) {
        close(dfd);
        return;
    }
    close(dfd);
    for(i = 0; i < PRELIMS; i++) {      /* a Ctrl+C before the 150: it comes first */
        ftpc_reply_init(r);
        c = reply_wait(r, ABORT_MS, 0);
        if(c < 100 || c >= 200) break;
    }
    if(c >= 100 && c < 200) {
        drop("no end to its replies");
        return;
    }
    if(c == 0) {
        drop("the server does not answer ABOR");
        return;
    }
    if(c < 0) {
        failed(c);
        return;
    }
    ftpc_reply_init(r);                 /* the transfer's own reply came first; ABOR's now, if it sends one */
    c = reply_wait(r, ABORT2_MS, 0);
    if(c < 0) failed(c);
}

/* what the control connection says during a transfer, into r: 1 a final reply is here (*early), 0 not yet, -1 the
   connection is gone, -2 Ctrl+C. A 2xx may come before the last of the data, a 4xx or 5xx means the transfer is over.
   readable: poll() said so; without it, only what an earlier read left in s.in, which poll() cannot know of (a reply
   can come in one piece with the 150 before it) */
static int ctl_during(struct ftpc_reply *r, int *early, int readable) {
    int v = 1;
    if(readable) v = read_more(0);
    if(v == 0 || v == -1) {
        drop(v ? strerror(errno) : "the server closed it");
        return -1;
    }
    if(v == -2) return -2;                      /* told once: the pump has to hear of it */
    if(v < 0) return 0;
    v = feed_lines(r, 0);
    if(v < 0) {
        drop("what the server says is not FTP");
        return -1;
    }
    if(!v) return 0;
    if(r->code < 200) {
        ftpc_reply_init(r);
        return 0;
    }
    *early = 1;
    return 1;
}

/**** downloads ****/

static int file_write(int fd, const char *p, int n) {
    while(n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) {
            if(!w) errno = ENOSPC;
            return -1;
        }
        p += w;
        n -= (int)w;
    }
    return 0;
}

static void names_add(struct sink *k, const char *line) {
    struct fs_names *l = k->names;
    const char *name = ftpc_last_name(line);        /* the prefix a server gives is its own business */
    char **v;
    char *copy;
    int n = (int)strlen(name);
    if(!n) return;
    if(l->count >= FS_NAMES_MAX || k->names_bytes + n + 1 > FS_NAMES_BYTES) {
        l->cut = 1;
        return;
    }
    if(!(l->count & 63)) {
        if(!(v = realloc(l->name, (size_t)(l->count + 64) * sizeof(char *)))) {
            l->cut = 1;
            return;
        }
        l->name = v;
    }
    if(!(copy = strdup(name))) {
        l->cut = 1;
        return;
    }
    l->name[l->count++] = copy;
    k->names_bytes += n + 1;
}

static void take_line(struct sink *k, int got) {
    if(got == FTP_LONG) k->long_lines++;
    else if(k->kind == TO_NAMES) names_add(k, k->lines.line);
    else printf("%s\n", safe(k->lines.line));
}

/* n bytes of data: 0, -1 when the file will not take them */
static int sink_take(struct sink *k, const char *p, int n) {
    int off = 0, got;
    k->bytes += (unsigned long long)n;
    if(k->kind == TO_FILE) {
        int m = n;
        if(k->limit && k->bytes > k->limit) {
            errno = EFBIG;
            return -1;
        }
        if(k->text) {
            m = ftp_from_crlf(p, n, text_buf, &k->held);
            p = text_buf;
        }
        return file_write(k->file->fd, p, m);
    }
    while(off < n) {
        off += ftpc_lines_feed(&k->lines, p + off, n - off, &got);
        if(got != FTP_MORE) take_line(k, got);
    }
    return 0;
}

/* the end of the data: a CR held, a last line with no end */
static int sink_end(struct sink *k) {
    if(k->kind == TO_FILE) {
        int m;
        if(!k->text || !(m = ftp_from_crlf(text_buf, 0, text_buf, &k->held))) return 0;
        return file_write(k->file->fd, text_buf, m);
    }
    if(ftpc_lines_end(&k->lines) == FTP_LINE) take_line(k, FTP_LINE);
    return 0;
}

/* the data, until its end: 0; -1 it failed, said; -2 Ctrl+C; 1 the file failed, said; *early when the final reply came
   (a negative one ends this at once) */
static int pump_in(int dfd, struct sink *k, struct ftpc_reply *r, int *early) {
    unsigned int last = uptime();
    for(;;) {
        struct pollfd p[2];
        int n = 1, left = DATA_MS - (int)(uptime() - last), v;
        if(!*early && s.in_off < s.in_len) {
            v = ctl_during(r, early, 0);
            if(v < 0) return -1;
            if(v > 0 && r->code >= 300) return 0;
            continue;
        }
        p[0].fd = dfd;
        p[0].events = POLLIN;
        p[0].revents = 0;
        if(!*early) {
            p[1].fd = s.ctl;
            p[1].events = POLLIN;
            p[1].revents = 0;
            n = 2;
        }
        if(left <= 0) {
            say_err("no data came for %d s", DATA_MS / 1000);
            return -1;
        }
        v = wait_ready(p, n, left);
        if(v == -2) return -2;
        if(v == 0) continue;
        if(v < 0) {
            say_err("the data connection: %s", strerror(errno));
            return -1;
        }
        if(n == 2 && p[1].revents) {
            v = ctl_during(r, early, 1);
            if(v < 0) return v;
            if(v > 0 && r->code >= 300) return 0;
        }
        if(p[0].revents) {
            ssize_t m = read(dfd, data_buf, sizeof(data_buf));
            if(m == 0) return 0;
            if(m < 0) {
                if(errno == EINTR) continue;
                say_err("the data connection: %s", strerror(errno));
                return -1;
            }
            last = uptime();
            if(sink_take(k, data_buf, (int)m) < 0) {
                if(k->limit && errno == EFBIG) say_err("the server sends more than it said the file holds");
                else say_err("%s: %s", safe(k->file->path), strerror(errno));
                return 1;
            }
        }
    }
}

/* the file a download goes to: 0, or a status, said */
static int local_open(struct local *l, int resume) {
    struct stat st;
    int there = stat(l->path, &st) == 0;
    if(!there && errno != ENOENT) {
        say_err("%s: %s", safe(l->path), strerror(errno));
        return FS_TROUBLE;
    }
    if(there && S_ISDIR(st.type)) {
        say_err("%s: is a folder", safe(l->path));
        return FS_TROUBLE;
    }
    if(there && !strcmp(st.fs, "devfs")) {
        l->how = L_DEVICE;
        l->fd = open(l->path, O_WRONLY);
    } else if(there && resume) {
        l->how = L_APPEND;
        l->start = st.size;
        l->fd = open(l->path, O_WRONLY | O_APPEND);
    } else if(there) {
        int r = su_begin(&l->save, l->path);
        l->how = L_SAVE;
        l->fd = l->save.fd;
        if(r < 0) {
            su_say("ftp", &l->save, r);
            return FS_TROUBLE;
        }
    } else {
        l->how = L_NEW;
        l->fd = open(l->path, O_WRONLY | O_CREAT | O_EXCL);
    }
    if(l->fd < 0) {
        say_err("%s: %s", safe(l->path), strerror(errno));
        return FS_TROUBLE;
    }
    return 0;
}

static int local_commit(struct local *l) {
    int r;
    if(l->how == L_SAVE) {
        r = su_commit(&l->save);
        if(r < 0) su_say("ftp", &l->save, r);
        return r < 0 ? FS_TROUBLE : FS_OK;
    }
    r = l->how == L_DEVICE ? 0 : fsync(l->fd);
    if(close(l->fd) < 0) r = -1;
    if(r < 0) {
        say_err("%s: %s", safe(l->path), strerror(errno));
        if(l->how == L_NEW && !l->keep) unlink(l->path);
        return FS_TROUBLE;
    }
    return FS_OK;
}

/* a download failed: the file as it was */
static void local_abort(struct local *l) {
    if(l->how == L_SAVE) su_abort(&l->save);
    else {
        if(l->how == L_APPEND) fsync(l->fd);
        close(l->fd);
        if(l->how == L_NEW && !l->keep) unlink(l->path);
    }
}

/* the rest of the final reply, of which r may hold the first lines */
static int final_from(struct ftpc_reply *r) {
    int i, c;
    for(i = 0; i < PRELIMS; i++) {
        c = reply_wait(r, REPLY_MS, 0);
        if(c == 0) {
            drop("no reply came");
            return -1;
        }
        if(c < 100 || c >= 200) return c;
        ftpc_reply_init(r);
    }
    drop("no end to its replies");
    return -1;
}

/* what the pump's end leaves: the status of a transfer stopped before its end. v: pump_in()'s or pump_out()'s */
static int stopped_early(int v, int dfd, struct ftpc_reply *r, int early) {
    if(early) close(dfd);
    else abort_transfer(r, dfd);
    if(v == -2) printf("stopped\n");
    return FS_TROUBLE;
}

/* the file, now that the transfer is over */
static int file_done(struct sink *k, int ok) {
    struct local *l = k->file;
    int short_read = ok && k->have_want && !k->text && k->bytes < k->want;
    if(l->how == L_STDOUT) {
        fflush(stdout);
        return ok && !short_read ? FS_OK : FS_TROUBLE;
    }
    if(short_read) say_err("%s: only %llu of the %llu bytes the server promised came", safe(l->path), k->bytes, k->want);
    if(!ok || short_read) {
        if(l->keep) printf("%s: %llu bytes kept: reget goes on from there\n", safe(l->path), k->bytes);
        if(l->keep && short_read) local_commit(l);
        else local_abort(l);
        return FS_TROUBLE;
    }
    return local_commit(l);
}

/* the server sent more than was left to come: this is no continuation of the file, so what was added goes */
static void cut_back(struct sink *k) {
    struct local *l = k->file;
    l->keep = 0;
    if(l->how != L_APPEND) return;
    if(ftruncate(l->fd, (off_t)l->start) < 0)
        say_err("%s: what came could not be cut off again: %s", safe(l->path), strerror(errno));
}

/* LIST, NLST or RETR arg into k. rest: REST's offset, 0 for none. A file is opened when the server has said yes, so
   that a 550 leaves nothing behind. The status; a file was committed for FS_OK, and is as it was otherwise */
static int transfer_in(const char *verb, const char *arg, struct sink *k, unsigned int rest, const char *what) {
    struct ftpc_reply r;
    int status, dfd, c, v, early = 0, to_file = k->kind == TO_FILE;
    if((status = set_type()) != FS_OK) return status;
    if((dfd = open_data(&status)) < 0) return status;
    if(rest) {
        char off[16];
        snprintf(off, sizeof(off), "%u", rest);
        c = ask("REST", off, &r, 0);
        if(c != 350) {
            close(dfd);
            if(c < 0) return failed(c);
            negative(what, &r);
            say_err("%s: the server will not go on from byte %u", safe(what), rest);
            return FS_NO;
        }
    }
    drain();
    if(s.ctl < 0 || send_cmd(verb, arg) < 0) {
        close(dfd);
        return FS_TROUBLE;
    }
    c = reply(&r, 0);
    if(c == -2) return stopped_early(-2, dfd, &r, 0);   /* a transfer already, for the server: nothing was opened here */
    if(c < 0) {
        close(dfd);
        return failed(c);
    }
    if(c >= 300) {
        close(dfd);
        return negative(what, &r);
    }
    if(c >= 200) early = 1;                     /* all over already: what the data connection holds comes all the same */
    else ftpc_reply_init(&r);
    if(to_file && k->file->how == L_STDOUT) fflush(stdout);     /* what was said comes before the file's bytes */
    if(to_file && k->file->how != L_STDOUT && (status = local_open(k->file, k->file->keep)) != 0) {
        stopped_early(1, dfd, &r, early);
        return status;
    }
    v = pump_in(dfd, k, &r, &early);
    if(v == 0 && sink_end(k) < 0) {
        say_err("%s: %s", safe(what), strerror(errno));
        v = 1;
    }
    if(v == 1 && to_file && k->limit && k->bytes > k->limit) cut_back(k);
    if(v) {
        stopped_early(v, dfd, &r, early);
        return to_file ? file_done(k, 0) : FS_TROUBLE;
    }
    close(dfd);
    if(!early) {
        c = final_from(&r);
        if(c < 0) {
            status = failed(c);
            if(to_file) file_done(k, 0);
            return status;
        }
    }
    if(r.code >= 300) {
        status = negative(what, &r);
        if(to_file) file_done(k, 0);
        return status;
    }
    return to_file ? file_done(k, 1) : FS_OK;
}

static int download(const char *remote, const char *local, int resume) {
    static struct sink k;
    struct stat st;
    unsigned long long size = 0;
    unsigned int began = uptime(), from = 0, ms;
    int have = 0, r;
    if(s.ctl < 0) return FS_TROUBLE;
    if(resume && s.text) {
        say_err("reget needs binary mode: a text file's offsets differ at the two ends");
        return FS_TROUBLE;
    }
    memset(&k, 0, sizeof(k));
    k.kind = TO_FILE;
    k.file = &dl;
    k.text = s.text;
    dl.path = local ? local : "-";
    dl.how = local ? L_NEW : L_STDOUT;
    dl.fd = 1;
    dl.keep = resume;
    dl.start = 0;
    if(!s.text) {                               /* SIZE means bytes only in binary mode */
        if((r = set_type()) != FS_OK) return r;
        r = ask_size(remote, &size);
        if(r < 0 || s.ctl < 0) return FS_TROUBLE;
        have = r == 0;
    }
    if(have && size > FILE_LIMIT && local) {
        say_err("%s: %llu bytes: a file here holds less than 4 GB", safe(remote), size);
        return FS_TROUBLE;
    }
    if(resume) {
        if(stat(local, &st) == 0) {
            if(S_ISDIR(st.type)) {
                say_err("%s: is a folder", safe(local));
                return FS_TROUBLE;
            }
            from = st.size;
            if(have && from == size) {
                printf("%s: whole already\n", safe(local));
                return FS_OK;
            }
            if(have && from > size) {
                say_err("%s: longer than the server's file: no part of it", safe(local));
                return FS_TROUBLE;
            }
        } else if(errno != ENOENT) {
            say_err("%s: %s", safe(local), strerror(errno));
            return FS_TROUBLE;
        }
    }
    if(have) {
        k.have_want = 1;
        k.want = size - from;
        if(resume) k.limit = k.want;
    }
    r = transfer_in("RETR", remote, &k, from, remote);
    ms = uptime() - began;
    if(r == FS_OK && local) printf("%s: %llu bytes in %u.%u s\n", safe(local), k.bytes, ms / 1000, ms % 1000 / 100);
    return r;
}

int fs_get(const char *remote, const char *local) {
    return download(remote, local, 0);
}

int fs_reget(const char *remote, const char *local) {
    return download(remote, local, 1);
}

int fs_list(const char *verb, const char *path) {
    static struct sink k;
    int r;
    if(s.ctl < 0) return FS_TROUBLE;
    memset(&k, 0, sizeof(k));
    k.kind = TO_SHOW;
    r = transfer_in(verb, path, &k, 0, path ? path : "the listing");
    if(k.long_lines) say_err("%d lines of the listing were too long: left out", k.long_lines);
    return r;
}

int fs_names(const char *dir, struct fs_names *out) {
    static struct sink k;
    int r;
    out->name = NULL;
    out->count = out->cut = 0;
    if(s.ctl < 0) return FS_TROUBLE;
    memset(&k, 0, sizeof(k));
    k.kind = TO_NAMES;
    k.names = out;
    r = transfer_in("NLST", dir, &k, 0, dir ? dir : "the listing");
    if(k.long_lines) out->cut = 1;
    return r;
}

void fs_names_free(struct fs_names *n) {
    while(n->count > 0) free(n->name[--n->count]);
    free(n->name);
    n->name = NULL;
}

/**** uploads ****/

/* the file, to the data connection: as pump_in() */
static int pump_out(int dfd, int fd, struct ftpc_reply *r, int *early, unsigned long long *sent) {
    const char *p = data_buf;
    unsigned int last = uptime();
    int have = 0, off = 0, cr = 0;
    for(;;) {
        struct pollfd f[2];
        int n = 1, left, v;
        if(off == have) {
            ssize_t m = read(fd, data_buf, sizeof(data_buf));
            if(m < 0) {
                if(errno == EINTR) continue;
                say_err("reading the file: %s", strerror(errno));
                return 1;
            }
            if(m == 0) return 0;
            *sent += (unsigned long long)m;
            p = data_buf;
            have = (int)m;
            off = 0;
            if(s.text) {
                have = ftp_to_crlf(data_buf, (int)m, text_buf, &cr);
                p = text_buf;
            }
        }
        if(!*early && s.in_off < s.in_len) {
            v = ctl_during(r, early, 0);
            if(v < 0) return -1;
            if(v > 0 && r->code >= 300) return 0;
            continue;
        }
        left = DATA_MS - (int)(uptime() - last);
        if(left <= 0) {
            say_err("the data connection took no byte for %d s", DATA_MS / 1000);
            return -1;
        }
        f[0].fd = dfd;
        f[0].events = POLLOUT;
        f[0].revents = 0;
        if(!*early) {
            f[1].fd = s.ctl;
            f[1].events = POLLIN;
            f[1].revents = 0;
            n = 2;
        }
        v = wait_ready(f, n, left);
        if(v == -2) return -2;
        if(v == 0) continue;
        if(v < 0) {
            say_err("the data connection: %s", strerror(errno));
            return -1;
        }
        if(n == 2 && f[1].revents) {
            v = ctl_during(r, early, 1);
            if(v < 0) return v;
            if(v > 0 && r->code >= 300) return 0;
        }
        if(f[0].revents & POLLOUT) {
            ssize_t w = write(dfd, p + off, (size_t)(have - off));
            if(w < 0) {
                if(errno == EINTR) continue;
                say_err("the data connection: %s", strerror(errno));
                return -1;
            }
            off += (int)w;
            if(w) last = uptime();
        } else if(f[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            say_err("the server closed the data connection before the end");
            return -1;
        }
    }
}

int fs_put(const char *local, const char *remote) {
    struct ftpc_reply r;
    struct stat st;
    unsigned long long sent = 0;
    unsigned int began = uptime(), ms;
    int status, fd, dfd, c, v, early = 0;
    if(s.ctl < 0) return FS_TROUBLE;
    if(stat(local, &st) < 0 || (fd = open(local, O_RDONLY)) < 0) {
        say_err("%s: %s", safe(local), strerror(errno));
        return FS_TROUBLE;
    }
    if(S_ISDIR(st.type)) {
        say_err("%s: is a folder", safe(local));
        close(fd);
        return FS_TROUBLE;
    }
    if((status = set_type()) != FS_OK || (dfd = open_data(&status)) < 0) {
        close(fd);
        return status;
    }
    drain();
    if(s.ctl < 0 || send_cmd("STOR", remote) < 0) {
        close(dfd);
        close(fd);
        return FS_TROUBLE;
    }
    c = reply(&r, 0);
    if(c == -2) {
        close(fd);
        return stopped_early(-2, dfd, &r, 0);
    }
    if(c < 0 || c >= 200) {
        close(dfd);
        close(fd);
        if(c < 0) return failed(c);
        if(c >= 300) return negative(remote, &r);
        say_err("%s: the server said %d before the data", safe(remote), c);     /* nothing took the file */
        return FS_TROUBLE;
    }
    ftpc_reply_init(&r);
    v = pump_out(dfd, fd, &r, &early, &sent);
    close(fd);
    if(v) return stopped_early(v, dfd, &r, early);
    close(dfd);
    if(!early && (c = final_from(&r)) < 0) return failed(c);
    if(r.code >= 300) return negative(remote, &r);
    ms = uptime() - began;
    printf("%s: %llu bytes in %u.%u s\n", safe(remote), sent, ms / 1000, ms % 1000 / 100);
    return FS_OK;
}

void fs_status(void) {
    char cwd[PATH_MAX];
    if(s.ctl >= 0) printf("Connected to %s port %d\n", s.peer, s.port);
    else printf("Not connected\n");
    if(s.ctl >= 0 && s.logged) printf("Logged in as %s\n", safe(s.user));
    else if(s.ctl >= 0) printf("Not logged in\n");
    printf("Transfers are %s\n", s.text ? "ascii: text, its line ends converted" : "binary: the bytes as they are");
    printf("Data connections are passive: %s\n", s.no_epsv ? "PASV (the server has no EPSV)" : "EPSV, then PASV");
    if(getcwd(cwd, sizeof(cwd))) printf("The local folder is %s\n", safe(cwd));
}
