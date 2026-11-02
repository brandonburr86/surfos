/*
SurfOS web server: httpd
--------------------
File: server.c  Date: 10/17/26
--------------------
The server. httpd on starts it with the listening descriptor as its descriptor 0 and
/dev/null as 1 and 2; it detach()es from the shell that started it (init is its parent
from then on, and Ctrl+C there does not reach it) and says what it does in the kernel's
log. httpd serve runs the same loop on its own console instead and prints each request
there. Either way it serves the files of one folder (root) in one loop: poll() over the
listening descriptor and up to MAX_CONNS connections. Nothing here waits for one client.

A connection reads its request head (http.c, at most HTTP_HEAD_MAX bytes), then sends the
response: the header, then the body, from the file or from memory (a folder's listing),
a piece of up to OUT_SIZE bytes at a time, written as far as the connection takes it
whenever poll() says POLLOUT. While a response is sent the connection is not read: a
request behind it (pipelining) waits in the kernel and is answered after it. Then the
connection waits for the next request (HTTP/1.1 keep-alive), or closes; a close never
loses the response (the FIN goes out behind it, and what the client still sends is dropped
by the kernel, never answered with a reset).

The request's path names a file below root (http.c has decoded and checked it). A
folder asked without its '/' is redirected to it (301), since the relative links of its
pages need it; a folder's index.html (or index.htm, an 8.3 name) is its page, and a
folder without one is listed: folders first, without names starting with a dot, at most
LIST_MAX entries. A file is opened for each request and read as it is sent, so an edited
page shows at the next reload; its size and time make its ETag, and a request that names
it in If-None-Match gets 304 and no body. A file that ends before the length its header
promised (it shrank meanwhile, or its stick was pulled) ends the connection, which tells
the client that the body is short.

Places: a new connection takes a free one; when there is none, the place of the
connection idle longest (waiting for a request with nothing of it read yet, for
IDLE_GRACE_MS at least: a client's request comes just behind its connection, so one made or
answered a moment ago is not idle yet); when no connection is idle, the listening
descriptor is left out of poll() and new connections wait in the kernel's backlog until a
response ends or a connection's grace does. Times: a connection has REQUEST_MS to
begin a request and REQUEST_MS from its first byte to finish the head (then 408 when part
of a request came, else a plain close); a response the client has taken nothing of for
STALL_MS is given up. A client that has closed its side while its response is still
written (a half-close) has poll() say POLLHUP at once, every time, while the connection
takes nothing more: the connection then rests for HUP_REST_MS before it is polled again,
so the loop never spins.

httpd off kills the server: the kill ends it at the end of its poll(), and its
descriptors close with it, so the port is free and every client gets its FIN.
*/

#include "httpd.h"

#define OUT_SIZE    8192                /* a response's header, then its body, a piece at a time */
#define REQUEST_MS  10000               /* to begin a request, and from its first byte to finish its head */
#define IDLE_GRACE_MS 1000              /* a connection waits this long for a request before it can lose its place */
#define STALL_MS    30000               /* a response the client has taken nothing of for this long is given up */
#define HUP_REST_MS 20                  /* a half-closed client that takes nothing now is polled again after this */
#define PAUSE_MS    1000                /* the listening descriptor rests this long after accept() failed */
#define LIST_MAX    512                 /* the entries of a listing; the rest are counted */

enum { FREE, READING, SENDING };

struct conn {
    int fd, state;
    unsigned int since;                 /* READING: when the wait or the request began; SENDING: the last progress */
    unsigned int rest_end;              /* resting (a half-closed client) until then, when rest is set */
    int rest;
    int keep;                           /* the connection stays open after this response */
    int file;                           /* the file the body comes from, or -1 */
    char *mem;                          /* or the body made in memory (a listing), from malloc() */
    unsigned int mem_off, left;         /* where the body goes on in mem; the body's bytes still to send */
    int in_len;
    char in[HTTP_HEAD_MAX];             /* the request head as it arrives (and what came behind it) */
    int out_len, out_off;
    char out[OUT_SIZE];                 /* out[out_off..out_len): bytes the connection has not taken yet */
    char peer[16];                      /* the client's address, for the log */
};

static struct conn conns[MAX_CONNS];
static const char *root;                /* the folder served: absolute, no '/' at its end (but "/" itself) */
static int verbose, kmsg = -1;
static int resting;                     /* the listening descriptor is left out of poll() until rest_end */
static unsigned int rest_end;

/* a line for the kernel's log (httpd on's server) */
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

/* 10/24/26: the user shell pages every command it runs, and httpd serve held at --More-- after a screenful of
   lines answered no request until a key came: each line starts a new screenful (TTY_PAGE 0 does for a program
   the shell pages, and changes nothing for one it does not) */
static void console_line(void) {
    ttyctl(TTY_PAGE, 0);
}

static void drop(struct conn *c, const char *why) {
    if(verbose && why && c->state == SENDING) {
        console_line();
        printf("%s: the response was cut short: %s\n", c->peer, why);
    }
    if(c->file >= 0) close(c->file);
    free(c->mem);
    close(c->fd);
    c->fd = c->file = -1;
    c->mem = NULL;
    c->state = FREE;
    c->in_len = c->out_len = c->out_off = 0;
    c->left = c->mem_off = 0;
    c->rest = 0;
}

/* the response r begins: its header goes first; the body (if any) is set up by the caller */
static void begin(struct conn *c, const struct http_req *req, const struct http_resp *r) {
    int n = http_header(c->out, OUT_SIZE, r);
    c->out_len = n > 0 ? n : 0;
    c->out_off = 0;
    c->keep = r->keep;
    c->state = SENDING;
    c->since = uptime();
    if(verbose) {
        long body = req->method == HTTP_HEAD || r->status == 304 || r->length < 0 ? 0 : r->length;
        console_line();
        printf("%s %s %s %d %ld\n", c->peer, req->method_name[0] ? req->method_name : "-",
               req->target[0] ? req->target : "-", r->status, body);
    }
}

/* an error page; the connection closes after it, but after a 404 the client may keep it */
static void error(struct conn *c, const struct http_req *req, int status) {
    char page[1024];
    int n = http_error_page(page, sizeof(page), status);
    struct http_resp r = { status, "text/html; charset=utf-8", n, NULL, NULL, status == 404 && req->keep };
    begin(c, req, &r);
    if(req->method != HTTP_HEAD && c->out_len + n <= OUT_SIZE) {
        memcpy(c->out + c->out_len, page, (size_t)n);
        c->out_len += n;
    }
}

/* the file path of a request's path with tail after it ("" or "/index.html") into out: 0, or -1 when it
   is too long */
static int file_of(char *out, int size, const char *path, const char *tail) {
    int n = snprintf(out, (size_t)size, "%s%s%s", strcmp(root, "/") ? root : "", strcmp(path, "/") ? path : "", tail);
    if(n < 0 || n >= size) return -1;
    if(!out[0]) strcpy(out, "/");       /* the root of everything, as itself */
    return 0;
}

static void send_file(struct conn *c, const struct http_req *req, const char *file, const struct stat *st) {
    char tag[32];
    struct http_resp r = { 200, http_type(file), (long)st->st_size, tag, NULL, req->keep };
    int fd;
    if(st->st_size > 0x7fffffffu) {     /* 2 GB and more (FAT32 holds 4): no Content-Length for it here */
        error(c, req, 500);
        return;
    }
    http_etag(tag, sizeof(tag), st->st_size, st->st_mtime);
    if(req->tag[0] && http_etag_match(req->tag, tag)) {
        r.status = 304;
        r.type = NULL;
        r.length = -1;
        begin(c, req, &r);
        return;
    }
    if(req->method == HTTP_HEAD) {      /* the header is enough: no file to open */
        begin(c, req, &r);
        return;
    }
    if((fd = open(file, O_RDONLY)) < 0) {
        error(c, req, errno == ENOENT || errno == ENOTDIR ? 404 : 500);
        return;
    }
    begin(c, req, &r);
    c->file = fd;
    c->left = st->st_size;
}

/* the listing of the folder dir, the request's path */
static void send_listing(struct conn *c, const struct http_req *req, const char *dir) {
    struct http_entry *e;
    struct dirent *de;
    struct http_resp r = { 200, "text/html; charset=utf-8", 0, NULL, NULL, req->keep };
    DIR *d = opendir(dir);
    int n = 0, more = 0, len, i;
    char *page = NULL;
    if(!d) {
        error(c, req, errno == ENOENT || errno == ENOTDIR ? 404 : 500);
        return;
    }
    if(!(e = malloc(LIST_MAX * sizeof(*e)))) {
        closedir(d);
        error(c, req, 500);
        return;
    }
    while((de = readdir(d)) != NULL) {
        if(de->d_name[0] == '.') continue;          /* hidden: macOS's ._ files, .DS_Store, .Trashes, ... */
        if(n == LIST_MAX) {
            more++;
            continue;
        }
        if(!(e[n].name = strdup(de->d_name))) break;
        e[n].dir = de->d_type == DT_DIR;
        e[n].size = de->size;
        e[n].mtime = de->mtime;
        n++;
    }
    closedir(d);
    http_sort(e, n);
    len = http_listing(req->path, e, n, more, &page);
    for(i = 0; i < n; i++) free((char *)e[i].name);
    free(e);
    if(len < 0) {
        error(c, req, 500);
        return;
    }
    r.length = len;
    begin(c, req, &r);
    if(req->method == HTTP_HEAD) free(page);
    else {
        c->mem = page;
        c->left = (unsigned int)len;
    }
}

/* the response to a request http_parse() gave status for (0: it asks for a file) */
static void respond(struct conn *c, const struct http_req *req, int status) {
    char file[HTTP_PATH_MAX], loc[3 * HTTP_PATH_MAX + 2];
    struct stat st;
    if(status) {
        error(c, req, status);
        return;
    }
    if(file_of(file, sizeof(file), req->path, "") < 0) {
        error(c, req, 414);
        return;
    }
    if(stat(file, &st) < 0) {
        error(c, req, errno == ENOENT || errno == ENOTDIR ? 404 : 500);
        return;
    }
    if(!S_ISDIR(st.st_mode)) {
        send_file(c, req, file, &st);
        return;
    }
    if(!req->slash && strcmp(req->path, "/")) {     /* a folder without its '/': its relative links need it */
        struct http_resp r = { 301, NULL, 0, NULL, loc, req->keep };
        if(http_encode_path(loc, sizeof(loc) - 1, req->path) < 0) {
            error(c, req, 414);
            return;
        }
        strcat(loc, "/");
        begin(c, req, &r);
        return;
    }
    if((file_of(file, sizeof(file), req->path, "/index.html") == 0 && stat(file, &st) == 0 && S_ISREG(st.st_mode)) ||
       (file_of(file, sizeof(file), req->path, "/index.htm") == 0 && stat(file, &st) == 0 && S_ISREG(st.st_mode))) {
        send_file(c, req, file, &st);
        return;
    }
    file_of(file, sizeof(file), req->path, "");
    send_listing(c, req, file);
}

/* a request too long for the head buffer, or one that did not come whole in time: an error, then the close */
static void refuse(struct conn *c, int status) {
    struct http_req req;
    http_parse(c->in, c->in_len, &req);         /* as far as it goes: the method and target for the log, HEAD */
    req.keep = 0;
    c->in_len = 0;
    error(c, &req, status);
}

/* the request head in c->in, when it is whole: answered (its response begins) */
static void next_request(struct conn *c) {
    struct http_req req;
    int end = http_head_end(c->in, c->in_len), status, i = 0;
    if(!end) {
        if(c->in_len < HTTP_HEAD_MAX) return;   /* more is to come */
        while(i < c->in_len && (c->in[i] == '\r' || c->in[i] == '\n')) i++;
        refuse(c, memchr(c->in + i, '\n', (size_t)(c->in_len - i)) ? 431 : 414);
        return;
    }
    status = http_parse(c->in, end, &req);
    /* what came behind the head is the next request; but a body is never read, and after one the
       connection closes, so what came of it is dropped */
    c->in_len = req.body ? 0 : c->in_len - end;
    memmove(c->in, c->in + end, (size_t)c->in_len);
    respond(c, &req, status);
}

/* the response has gone: the connection waits for the next request, or closes */
static void finished(struct conn *c) {
    if(c->file >= 0) close(c->file);
    free(c->mem);
    c->file = -1;
    c->mem = NULL;
    c->mem_off = c->left = 0;
    c->out_len = c->out_off = 0;
    if(!c->keep) {
        drop(c, NULL);
        return;
    }
    c->state = READING;
    c->since = uptime();
    if(c->in_len) next_request(c);      /* a request came behind it already */
}

/* the next piece of the body into out (which is empty): 0, or -1 when the body cannot be read */
static int refill(struct conn *c) {
    unsigned int want = c->left < OUT_SIZE ? c->left : OUT_SIZE;
    if(c->file >= 0) {
        ssize_t n = read(c->file, c->out, want);
        if(n <= 0) return -1;           /* shorter than its header said, or the read failed */
        want = (unsigned int)n;
    } else if(c->mem) {
        memcpy(c->out, c->mem + c->mem_off, want);
        c->mem_off += want;
    } else return -1;
    c->out_off = 0;
    c->out_len = (int)want;
    c->left -= want;
    return 0;
}

/* the connection takes bytes (POLLOUT): as many as it will */
static void to_client(struct conn *c) {
    ssize_t n;
    if(c->out_off == c->out_len && c->left && refill(c) < 0) {
        drop(c, c->file >= 0 ? "the file ended before its length, or could not be read" : "no body");
        return;
    }
    n = write(c->fd, c->out + c->out_off, (size_t)(c->out_len - c->out_off));
    if(n < 0) {
        if(errno != EINTR && errno != EAGAIN) drop(c, strerror(errno));
        return;
    }
    if(n > 0) c->since = uptime();      /* progress: the stall clock starts again */
    c->out_off += (int)n;
    if(c->out_off == c->out_len && !c->left) finished(c);
}

/* what the client sent (the read does not wait: poll() said so): more of a request, or the end */
static void from_client(struct conn *c) {
    ssize_t n = read(c->fd, c->in + c->in_len, (size_t)(HTTP_HEAD_MAX - c->in_len));
    if(n <= 0) {
        if(n < 0 && (errno == EINTR || errno == EAGAIN)) return;
        drop(c, NULL);                  /* the client left, or the connection failed */
        return;
    }
    if(!c->in_len) c->since = uptime(); /* a request begins: its head has REQUEST_MS from now */
    c->in_len += (int)n;
    next_request(c);
}

/* connections whose time is up */
static void expire(unsigned int now) {
    int i;
    for(i = 0; i < MAX_CONNS; i++) {
        struct conn *c = &conns[i];
        if(c->state == READING && now - c->since >= REQUEST_MS) {
            if(c->in_len) refuse(c, 408);       /* part of a request came: say why it ends */
            else drop(c, NULL);                 /* idle */
        } else if(c->state == SENDING && now - c->since >= STALL_MS)
            drop(c, "the client took nothing for 30 s");
        if(c->rest && (int)(now - c->rest_end) >= 0) c->rest = 0;
    }
}

/* a free place, or NULL */
static struct conn *free_place(void) {
    int i;
    for(i = 0; i < MAX_CONNS; i++) if(conns[i].state == FREE) return &conns[i];
    return NULL;
}

/* how long poll() may wait: until the first connection's time is up, its rest ends, its grace ends with every
   place taken (a new connection may wait for that), or the listening descriptor's rest ends; -1: until
   something happens */
static int wait_ms(unsigned int now) {
    int i, ms = -1, left, full = !free_place();
    for(i = 0; i < MAX_CONNS; i++) {
        const struct conn *c = &conns[i];
        if(c->state == FREE) continue;
        left = (int)(c->since + (c->state == READING ? REQUEST_MS : STALL_MS) - now);
        if(c->rest && (int)(c->rest_end - now) < left) left = (int)(c->rest_end - now);
        if(left < 0) left = 0;
        if(ms < 0 || left < ms) ms = left;
        if(full && c->state == READING && !c->in_len) {
            left = (int)(c->since + IDLE_GRACE_MS - now);
            if(left > 0 && left < ms) ms = left;
        }
    }
    if(resting) {
        left = (int)(rest_end - now) > 0 ? (int)(rest_end - now) : 0;
        if(ms < 0 || left < ms) ms = left;
    }
    return ms;
}

/* the connection idle longest (waiting for a request, nothing of it read), or NULL. 11/1/26 (audit B90): one
   that has waited less than IDLE_GRACE_MS is not idle yet: its request may be on its way */
static struct conn *idle_longest(unsigned int now) {
    struct conn *best = NULL;
    int i;
    for(i = 0; i < MAX_CONNS; i++) {
        struct conn *c = &conns[i];
        if(c->state != READING || c->in_len || now - c->since < IDLE_GRACE_MS) continue;
        if(!best || now - c->since > now - best->since) best = c;
    }
    return best;
}

static void new_conn(int lfd) {
    struct netaddr from;
    struct conn *c = free_place();
    int fd;
    if(!c && (c = idle_longest(uptime())) != NULL) drop(c, NULL);  /* it makes room */
    if(!c) return;
    fd = tcp_accept(lfd, &from);
    if(fd < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        if(errno == EBADF || errno == EINVAL) {         /* descriptor 0 is no listening socket after all */
            say("httpd: accept: %s; the server ends", strerror(errno));
            _exit(1);
        }
        say("httpd: accept: %s; new connections wait a moment", strerror(errno));   /* ENOMEM, EMFILE */
        resting = 1;
        rest_end = uptime() + PAUSE_MS;
        return;
    }
    c->fd = fd;
    c->state = READING;
    c->since = uptime();
    c->in_len = c->out_len = c->out_off = 0;
    c->file = -1;
    c->mem = NULL;
    c->left = c->mem_off = 0;
    c->keep = c->rest = 0;
    ipstr(from.ip, c->peer);
}

int serve(int lfd, int port, const char *folder, int loud) {
    struct pollfd fds[1 + MAX_CONNS];
    struct conn *who[1 + MAX_CONNS];            /* whose descriptor each is (NULL: the listener) */
    int i;
    root = folder;
    verbose = loud;
    for(i = 0; i < MAX_CONNS; i++) {
        conns[i].fd = conns[i].file = -1;
        conns[i].state = FREE;
    }
    if(!verbose) {
        kmsg = open("/dev/kmsg", O_WRONLY);
        if(detach() < 0) say("httpd: detach: %s", strerror(errno));
        say("httpd: serving %s on port %d", root, port);
    }
    chdir("/");                                 /* no hold on a stick through the current directory */
    for(;;) {
        unsigned int now = uptime();
        int n = 0, r, l = -1;
        expire(now);
        if(resting && (int)(now - rest_end) >= 0) resting = 0;
        if(!resting && (free_place() || idle_longest(now))) {
            fds[n].fd = lfd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            who[n] = NULL;
            l = n++;
        }
        for(i = 0; i < MAX_CONNS; i++) {
            struct conn *c = &conns[i];
            if(c->state == FREE) continue;
            fds[n].fd = c->rest ? -1 : c->fd;   /* a resting connection is skipped this time */
            fds[n].events = c->state == READING ? POLLIN : POLLOUT;     /* POLLHUP, POLLERR come anyway */
            fds[n].revents = 0;
            who[n++] = c;
        }
        r = poll(fds, (nfds_t)n, wait_ms(now));
        if(r < 0) {
            if(errno == EINTR) continue;        /* a kill ends this task when the call does */
            say("httpd: poll: %s; the server ends", strerror(errno));
            return 1;
        }
        for(i = 0; i < n; i++) {
            struct conn *c = who[i];
            short ev = fds[i].revents;
            if(!ev || !c || c->state == FREE) continue;    /* nothing, the listener, or a connection ended just now */
            if(c->state == READING) {
                if(ev & (POLLIN | POLLHUP | POLLERR)) from_client(c);   /* the read says which: data, the end, an error */
                else if(ev & POLLNVAL) drop(c, NULL);
            } else if(ev & POLLOUT) to_client(c);
            else if(ev & (POLLERR | POLLNVAL)) drop(c, "the connection failed");
            else if(ev & POLLHUP) {             /* half-closed, and nothing fits now: rest, never spin */
                c->rest = 1;
                c->rest_end = uptime() + HUP_REST_MS;
            }
        }
        if(l >= 0 && fds[l].revents) new_conn(lfd);
    }
}
