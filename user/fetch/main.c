/*
SurfOS downloader: fetch
--------------------
File: main.c    Date: 10/24/26
--------------------
fetch, files from the web onto SurfOS: an http:// or https:// address downloaded into a file.

    fetch [-o file|folder|-] [-c] [-f] [-q] [-t 'YYYY-MM-DD HH:MM:SS'] http[s]://address

The file is named after the last name of the address's path (index.html for a folder), in
the current directory or in the folder -o names; -o can also name the file itself, "-"
(standard output) or a device (/dev/null). A file that is there already is not replaced
unless -f says so; -c goes on with it instead, asking for the rest (a Range request). The
part that comes (206) must start where the file ends and be as long as its range says; one
that ends before the file does is added and fetch says so (fetch -c again asks for the
rest). A server that sends all of it again (200) leaves the file as it was, as curl and
wget refuse it too (a network's login page never takes the place of half a download): -f
downloads it anew. One that answers that the range starts at the file's end (416) has it
whole already. Nothing is written before a 2xx response with something in it has come, so
an error status (404, 500), a 204 or a 205 leaves no file and changes none, and a file that
appears meanwhile is not replaced; a download cut short keeps what came, and says that -c
goes on with it. Over HTTPS a body that only the end of the connection delimits counts as
whole only when TLS's close_notify ended it (RFC 9112 section 9.8): the end of a TCP
connection is anyone's to send.

Redirects (301, 302, 303, 307, 308) are followed, 10 at most, never in a loop and never
from https: to http:; the file keeps the first address's name. HTTPS is Undertow's
transport (user/undertow/transport.c): the certificate chain checked against the roots in
/initrd/etc/ssl/cert.pem, the name and the dates too, with no way to skip any of it; it
needs a CPU with RDRAND and a clock that is right (/initrd/etc/undertow.conf's
rtc_offset_minutes; -t gives the UTC time for this run, as Undertow's clock command does,
when the hardware clock cannot be trusted). Every wait is bounded: 15 seconds without a
byte end a download, and 20 days the whole of it.
The response is read as strictly as Undertow reads it (fetchhttp.c).

On a terminal the progress is a line written over as it goes, from half a second on; at the
end, "fetch: 4.5 MB in 5.3 s (850.2 KB/s) -> name" (the rate only when it took 0.1 s or
more). -q says nothing but errors. Exit status 0, 1 after an error, 2 for a usage error.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <surf.h>
#include "undertow.h"
#include "url.h"
#include "config.h"
#include "transport.h"
#include "fetch.h"

#define PIECE       16384               /* read from the connection at a time */
#define REDIRECTS   10                  /* followed in one download */
#define INTERIM     8                   /* 1xx responses taken before the real one (Undertow's UT_INTERIM_MAX) */
#define DEADLINE_MS 1728000000u         /* a connection's deadline, 20 days (ut_now()'s differences hold 24): UT_IDLE_MS
                                           bounds every wait in it */
#define SHOW_MS     500                 /* the progress line, at most this often */
#define REQ_MAX     (3 * UT_URL_MAX + 512)
#define PATH_MAX_   512                 /* the file's path */

static int quiet, show, shown;          /* -q; the progress line is wanted; a progress line is on the screen */
static struct ut_transport *conn;       /* the connection, closed by main() */
static struct ut_url *cur, *next;       /* the address now, and a redirect's */
static struct fx_head h;
static char err[UT_ERR_MAX];
static char head[FX_HEAD_MAX];          /* the response's head, then what came behind it */
static char piece[PIECE], body[FX_HEAD_MAX];
static char seen[REDIRECTS + 1][UT_URL_MAX + 1];
static char path[PATH_MAX_];

static int usage(void) {
    fprintf(stderr, "usage: fetch [-o file|folder|-] [-c] [-f] [-q] [-t 'YYYY-MM-DD HH:MM:SS'] http[s]://address\n"
                    "  -o  where it goes: a file, a folder for it, - for standard output\n"
                    "  -c  go on with a file cut short: ask for the rest of it\n"
                    "  -f  replace a file that is there already\n"
                    "  -q  quiet: only errors\n"
                    "  -t  the time now, in UTC, for HTTPS's certificates (the hardware clock's otherwise)\n");
    return 2;
}

static void say(const char *fmt, va_list ap) {
    if(shown) fputc('\n', stderr);
    shown = 0;
    fputs("fetch: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

/* an error: 1, for "return fail(...)" */
static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
    return 1;
}

/* what -q leaves out: 0 */
static int note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int note(const char *fmt, ...) {
    va_list ap;
    if(quiet) return 0;
    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
    return 0;
}

/* text from a server or the command line, as one line that is safe on the terminal, cut at 300 bytes */
static const char *safe(const char *s) {
    static char out[320];
    size_t n = strlen(s);
    ut_safe_text(out, sizeof(out) - 4, s, n > 300 ? 300 : n, 1);
    if(n > 300) strcat(out, "...");
    return out;
}

/**** the request and the response's head ****/

/* the request for cur, written whole: the target's bytes above 0x7e percent-encoded, as Undertow sends them. It
   fits in REQ_MAX, Undertow's bound: url.c keeps a target to UT_URL_MAX bytes (3 each, encoded) and a host to 253 */
static int send_request(unsigned long from) {
    static const char hexdig[] = "0123456789ABCDEF";
    static char req[REQ_MAX];
    const unsigned char *s;
    int n, done = 0, w;
    n = snprintf(req, sizeof(req), "GET %s", cur->target[0] == '/' ? "" : "/");
    for(s = (const unsigned char *)cur->target; *s; s++) {
        if(*s <= 0x20 || *s == 0x7f) return fail("%s cannot be asked for: it has a blank or a control in it", cur->text);
        if(*s < 0x7f) req[n++] = (char)*s;
        else {
            req[n++] = '%';
            req[n++] = hexdig[*s >> 4];
            req[n++] = hexdig[*s & 15];
        }
    }
    n += snprintf(req + n, sizeof(req) - (size_t)n, " HTTP/1.1\r\nHost: %s", cur->host);
    if(cur->port != (cur->secure ? 443 : 80)) n += snprintf(req + n, sizeof(req) - (size_t)n, ":%u", cur->port);
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\nUser-Agent: fetch/1.0 (SurfOS 1.0)\r\nAccept: */*\r\n"
                  "Accept-Encoding: identity\r\nConnection: close\r\n");
    if(from) n += snprintf(req + n, sizeof(req) - (size_t)n, "Range: bytes=%lu-\r\n", from);
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
    while(done < n) {
        if((w = ut_transport_write(conn, req + done, (size_t)(n - done))) <= 0)
            return fail("could not send the request: %s", ut_transport_error(conn));
        done += w;
    }
    return 0;
}

/* The final response's head into h, after any interim (1xx) ones; what came behind it stays at
   the start of head[], *len bytes. 0, or 1 after a message. */
static int read_head(int *len) {
    const char *why;
    int interim = 0, end, r;
    *len = 0;
    for(;;) {
        while((end = fx_head_end(head, *len)) == 0) {
            r = ut_transport_read(conn, head + *len, sizeof(head) - (size_t)*len);
            if(r < 0) return fail("%s", ut_transport_error(conn));
            if(r == 0) return fail("%s closed the connection before its response's head ended", cur->host);
            *len += r;
        }
        if(end < 0) return fail("%s: the response's head is over %d bytes", cur->host, FX_HEAD_MAX);
        if(fx_parse_head(head, end, &h, &why) < 0) return fail("%s: %s", cur->host, why);
        memmove(head, head + end, (size_t)(*len - end));
        *len -= end;
        if(h.status >= 200) return 0;
        if(h.status == 101) return fail("%s switched protocols (101), which fetch did not ask for", cur->host);
        if(++interim > INTERIM) return fail("%s sent more than %d interim (1xx) responses", cur->host, INTERIM);
    }
}

/* The redirect in h from cur: next is where it goes, after the checks Undertow makes. hops counts
   the redirects before this one. 0, or 1 after a message. */
static int redirect(int hops) {
    static char ref[UT_URL_MAX + 1];
    char uerr[128];
    size_t i, n = 0, len;
    if(!h.locations || (!h.location_long && !h.location[0]))
        return fail("%s answered %d %s without a Location to go to", cur->host, h.status, h.reason);
    if(h.location_conflict) return fail("%s sent two different Locations in one redirect", cur->host);
    for(i = 0; !h.location_long && h.location[i]; i++) {   /* a space goes as %20, as Undertow sends it */
        int space = h.location[i] == ' ';
        if(n + (space ? 3 : 1) > UT_URL_MAX) break;
        if(space) {
            memcpy(ref + n, "%20", 3);
            n += 3;
        } else ref[n++] = h.location[i];
    }
    if(h.location_long || h.location[i])
        return fail("cannot follow the redirect from %s: its Location is over %u bytes", cur->host, UT_URL_MAX);
    ref[n] = 0;
    if(ut_url_resolve(cur, ref, next, uerr, sizeof(uerr)) < 0)
        return fail("cannot follow the redirect from %s to %s: %s", cur->host, safe(h.location), uerr);
    if(cur->secure && !next->secure)
        return fail("%s redirects from HTTPS to unencrypted HTTP, which fetch does not follow by itself; "
                    "if that is right, fetch %s", cur->host, safe(next->text));
    len = strcspn(next->text, "#");
    for(i = 0; i <= (size_t)hops; i++)
        if(strlen(seen[i]) == len && !memcmp(seen[i], next->text, len))
            return fail("the address redirects in a loop (back to %s)", safe(seen[i]));
    if(hops == REDIRECTS) return fail("more than %d redirects (the next was to %s)", REDIRECTS, safe(next->text));
    memcpy(seen[hops + 1], next->text, len);
    seen[hops + 1][len] = 0;
    return 0;
}

/**** the body ****/

/* the progress line, written over: from SHOW_MS on, at most every SHOW_MS; at the end (last) once more when one
   was shown, so it ends at the whole size (a download done sooner has the summary alone) */
static void progress(unsigned long at, long total, unsigned long got, unsigned int start, int last) {
    static unsigned int when;
    static int width;
    unsigned int now = uptime(), ms = now - start;
    char a[24], b[24], r[24], line[96];
    int n;
    if(!show || (last ? !shown : ms < SHOW_MS || (shown && now - when < SHOW_MS))) return;
    when = now;
    fx_size(a, sizeof(a), at);
    fx_size(r, sizeof(r), ms ? (unsigned long)((unsigned long long)got * 1000 / ms) : got);
    if(total > 0) {
        fx_size(b, sizeof(b), (unsigned long)total);
        n = snprintf(line, sizeof(line), "%s of %s (%lu%%), %s/s", a, b,
                     (unsigned long)((unsigned long long)at * 100 / (unsigned long)total), r);
    } else n = snprintf(line, sizeof(line), "%s, %s/s", a, r);
    fprintf(stderr, "\r%s%*s", line, width > n ? width - n : 0, "");
    width = n;
    shown = 1;
}

static int write_all(int fd, const char *p, int n) {
    while(n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) return -1;
        p += w;
        n -= (int)w;
    }
    return 0;
}

/**** the download ****/

/* where the file goes, into path: a device is written as it is; a file may be there already */
static int place(const char *out, int *device, int *exists, unsigned long *size) {
    char name[FX_NAME_MAX];
    struct stat st;
    fx_name_of(cur->target, name, sizeof(name));
    if(!out) snprintf(path, sizeof(path), "%s", name);
    else if(stat(out, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(path, sizeof(path), "%s%s%s", out, out[strlen(out) - 1] == '/' ? "" : "/", name);
    else snprintf(path, sizeof(path), "%s", out);
    *device = *exists = 0;
    *size = 0;
    if(stat(path, &st) < 0) return 0;
    if(S_ISDIR(st.st_mode)) return fail("%s is a folder", safe(path));
    if(!strcmp(st.fs, "devfs")) *device = 1;
    else {
        *exists = 1;
        *size = st.st_size;
    }
    return 0;
}

static int run(int argc, char **argv) {
    static struct ut_config cfg;      /* 4 KB: off the stack */
    struct fx_chunks ch;
    const char *addr = NULL, *out = NULL, *utc = NULL;
    unsigned long from = 0, got = 0, at, size;
    unsigned int start, deadline;
    int i, cont = 0, force = 0, hops = 0, len, fd, to_stdout, device, exists, done, r, part;
    long want, total;

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if(!strcmp(argv[i], "-c")) cont = 1;
        else if(!strcmp(argv[i], "-f")) force = 1;
        else if(!strcmp(argv[i], "-q")) quiet = 1;
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) utc = argv[++i];
        else if(argv[i][0] == '-') {
            fprintf(stderr, "fetch: there is no option %s\n", safe(argv[i]));
            return usage();
        } else if(addr) return usage();
        else addr = argv[i];
    }
    if(!addr) return usage();
    if(out && !*out) {
        fprintf(stderr, "fetch: -o names nothing\n");
        return 2;
    }
    if(strncmp(addr, "http://", 7) && strncmp(addr, "https://", 8)) {
        fprintf(stderr, "fetch: %s: the address starts with http:// or https://\n", safe(addr));
        return 2;
    }
    to_stdout = out && !strcmp(out, "-");
    if(cont && to_stdout) {
        fprintf(stderr, "fetch: -c goes on with a file, not with standard output\n");
        return 2;
    }
    cur = malloc(sizeof(*cur));
    next = malloc(sizeof(*next));
    if(!cur || !next) return fail("out of memory");
    if(ut_url_parse(addr, cur, err, sizeof(err)) < 0) return fail("%s", err);
    ut_config_init(&cfg);
    if(ut_config_load(&cfg, UT_CONF_PATH, err, sizeof(err)) < 0) note("%s: %s (the defaults are used)", UT_CONF_PATH, err);
    if(utc && ut_clock_set(&cfg.clock, utc, err, sizeof(err)) < 0) {
        fprintf(stderr, "fetch: -t: %s\n", err);
        return 2;
    }
    device = exists = 0;
    size = 0;
    if(!to_stdout && place(out, &device, &exists, &size)) return 1;
    if(cont && device) return fail("-c goes on with a file, not with the device %s", safe(path));
    if(exists && !cont && !force) return fail("%s is there already: -f replaces it, -c goes on with it", safe(path));
    if(exists && cont) from = size;
    if(from > (unsigned long)FX_FILE_MAX) return fail("%s is 2 GB or more already", safe(path));
    show = !quiet && !to_stdout && isatty(2);
    snprintf(seen[0], sizeof(seen[0]), "%.*s", (int)strcspn(cur->text, "#"), cur->text);

    for(;;) {                           /* a request a hop, the redirects followed */
        deadline = ut_now() + DEADLINE_MS;
        if(ut_transport_open(&conn, cur, &cfg, deadline, err, sizeof(err)) < 0) {
            conn = NULL;
            return hops ? fail("redirected to %s: %s", safe(cur->text), err) : fail("%s", err);
        }
        if(send_request(from) || read_head(&len)) return 1;
        if(h.status != 301 && h.status != 302 && h.status != 303 && h.status != 307 && h.status != 308) break;
        if(redirect(hops)) return 1;
        ut_transport_close(conn);       /* a redirect's body is never read */
        conn = NULL;
        memcpy(cur, next, sizeof(*cur));
        hops++;
        note("%d %s: to %s", h.status, h.reason, safe(cur->text));
    }

    if(h.status == 416 && from) {
        if(h.range_total == (long)from) return note("%s is whole already (%lu bytes)", safe(path), from);
        if(h.range_total >= 0)
            return fail("%s has %ld bytes there and %lu here: fetch -f writes it anew", safe(path), h.range_total, from);
    }
    if(h.status < 200 || h.status > 299) return fail("%s: %d %s", safe(cur->text), h.status, h.reason);
    if(h.status == 204 || h.status == 205)
        return fail("%s answered %d %s: there is nothing to save%s", cur->host, h.status, h.reason,
                    exists ? ", and the file there is kept as it was" : "");
    part = h.status == 206;
    if(part) {
        if(!from) return fail("%s sent a part of it (206) that was not asked for", cur->host);
        if(h.range_start != (long)from)
            return fail("%s sent another part of it than the one asked for (from byte %lu)", cur->host, from);
        if(h.length >= 0 && h.length != h.range_end - h.range_start + 1)
            return fail("%s sent a part of %ld bytes for a range of %ld (bytes %ld to %ld): %s is kept as it was",
                        cur->host, h.length, h.range_end - h.range_start + 1, h.range_start, h.range_end, safe(path));
    } else if(from)                     /* curl and wget refuse it too: a login page never replaces half a file */
        return fail("%s sent all of it (%d %s), not the rest: %s is kept as it was; fetch -f downloads it anew",
                    cur->host, h.status, h.reason, safe(path));
    want = h.chunked ? -1 : h.length;
    if(want >= 0 && want > FX_FILE_MAX - (long)from)     /* from is FX_FILE_MAX at most: no long overflows here */
        return fail("%s would be 2 GB or more, more than a file here can hold", safe(path));
    total = part && h.range_total >= 0 ? h.range_total : want >= 0 ? want + (long)from : -1;
    if(to_stdout) fd = 1;
    else if(device) fd = open(path, O_WRONLY);
    else if(from) fd = open(path, O_WRONLY | O_APPEND);
    else if(exists) fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);  /* -f (or an empty file gone on with) */
    else fd = open(path, O_WRONLY | O_CREAT | O_EXCL);           /* not there when asked: nor may it be now */
    if(fd < 0 && errno == EEXIST)
        return fail("%s appeared while it was being asked for (another download?): -f replaces it", safe(path));
    if(fd < 0) return fail("%s: %s", safe(path), strerror(errno));
    if(from) {                          /* as it was when its rest was asked for: nothing wrote it meanwhile */
        struct stat st;
        if(fstat(fd, &st) < 0 || (unsigned long)st.st_size != from)
            return fail("%s changed while its rest was being asked for: fetch -c again", safe(path));
    }
    if(h.encoded) note("%s sent it with Content-Encoding %s: it is saved as it came", cur->host, h.encoding);

    memset(&ch, 0, sizeof(ch));
    start = uptime();
    at = from;
    done = want == 0;
    while(!done) {                      /* what came behind the head first, then piece by piece */
        const char *in = head;
        int n = len, k = 0;
        if(!n) {
            r = ut_transport_read(conn, piece, sizeof(piece));
            /* cut short; or a body that only the end of the connection ends, over TLS without close_notify */
            if(r < 0 || (r == 0 && (h.chunked || want >= 0 || !ut_transport_eof_clean(conn)))) {
                static char what[UT_ERR_MAX + 400];             /* a host of 253 bytes and the words, whole */
                int unsure = r == 0 && !h.chunked && want < 0;  /* TLS's TCP ended without close_notify */
                size_t used;
                if(r < 0 && ut_expired(deadline, ut_now()))
                    snprintf(what, sizeof(what), "the download took more than %u days", DEADLINE_MS / 86400000u);
                else if(r < 0) snprintf(what, sizeof(what), "%s", ut_transport_error(conn));
                else if(unsure) snprintf(what, sizeof(what), "%s ended its TLS connection without close_notify",
                                         cur->host);
                else snprintf(what, sizeof(what), "%s closed the connection", cur->host);
                used = strlen(what);
                if(want >= 0) snprintf(what + used, sizeof(what) - used, " after %lu of %ld bytes", got, want);
                else snprintf(what + used, sizeof(what) - used, " after %lu bytes%s", got,
                              unsure ? ": it may be cut short" : "");
                if(to_stdout || device) return fail("%s", what);
                return fail("%s; fetch -c goes on with %s", what, safe(path));
            }
            if(r == 0) break;           /* a body that the end of the connection ends: all of it came */
            in = piece;
            n = r;
        }
        len = 0;
        if(h.chunked) {
            if(fx_chunks_feed(&ch, in, n, body, &k, &done) < 0) return fail("%s broke the chunked coding", cur->host);
        } else {                        /* in unsigned longs: got + n may pass what a long holds */
            unsigned long left = want >= 0 ? (unsigned long)want - got : 0;
            k = want >= 0 && left <= (unsigned long)n ? (int)left : n;
            memcpy(body, in, (size_t)k);
            done = want >= 0 && got + (unsigned long)k == (unsigned long)want;
        }
        if(k && at + (unsigned long)k > (unsigned long)FX_FILE_MAX)
            return fail("%s would be 2 GB or more, more than a file here can hold", safe(path));
        if(part && at + (unsigned long)k > (unsigned long)h.range_end + 1)   /* chunked, or the end ends it */
            return fail("%s sent more than the part it named (bytes %ld to %ld)", cur->host, h.range_start, h.range_end);
        if(k && write_all(fd, body, k) < 0) return fail("%s: %s", safe(path), strerror(errno));
        got += (unsigned long)k;
        at += (unsigned long)k;
        progress(at, total, got, start, 0);
    }
    if(!to_stdout && !device) {
        if(fsync(fd) < 0) return fail("%s: %s", safe(path), strerror(errno));
        if(close(fd) < 0) return fail("%s: %s", safe(path), strerror(errno));
    }
    progress(at, total, got, start, 1);
    if(part && at != (unsigned long)h.range_end + 1)        /* chunked, or the end ended it early */
        return fail("%s ended the part after %lu bytes, short of byte %ld; fetch -c goes on with %s", cur->host, got,
                    h.range_end, safe(path));
    if(part && h.range_total >= 0 && h.range_end + 1 < h.range_total)
        return fail("%s sent bytes %ld to %ld of %ld, not all of the rest; fetch -c goes on with %s", cur->host,
                    h.range_start, h.range_end, h.range_total, safe(path));
    if(!to_stdout) {                    /* the rate when there was time enough to tell one */
        char a[24], rate[40] = "";
        const char *tls = ut_transport_tls(conn);
        unsigned int ms = uptime() - start;
        fx_size(a, sizeof(a), got);
        if(ms >= 100) {
            rate[0] = ' ';
            rate[1] = '(';
            fx_size(rate + 2, sizeof(rate) - 6, (unsigned long)((unsigned long long)got * 1000 / ms));
            strcat(rate, "/s)");
        }
        note("%s in %u.%u s%s%s%s -> %s%s", a, ms / 1000, ms % 1000 / 100, rate, *tls ? ", " : "", tls, safe(path),
             from ? " (the rest of it)" : "");
    }
    return 0;
}

int main(int argc, char **argv) {
    int r = run(argc, argv);
    ut_transport_close(conn);
    ut_transport_cleanup();
    return r;
}
