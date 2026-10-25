/*
SurfOS file transfer server: ftpd
--------------------
File: ftp.c     Date: 10/24/26
--------------------
The FTP protocol of ftpd (ftp.h), with no system calls: the control connection's lines,
the commands, a session's paths and the file system's, the data addresses of PORT, EPRT,
PASV and EPSV, the lines of LIST, NLST, MLSD and MLST, the line ends of text mode, a path
quoted for a 257 reply, and the reply to a failed file call. Host-tested as it is
(tests/host/test_ftp.c).
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "ftp.h"

/**** the control connection ****/

#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240

enum { T_DATA, T_IAC, T_OPTION, T_SB, T_SB_IAC };

/* a byte of the line; one past FTP_LINE_MAX makes the line too long, and the rest of it is dropped */
static void put(struct ftp_ctl *c, char ch) {
    if(c->toolong) return;
    if(c->len == FTP_LINE_MAX) {
        c->toolong = 1;
        return;
    }
    c->line[c->len++] = ch;
}

/* the line has ended: what it was, and the next one begins */
static int line_end(struct ftp_ctl *c) {
    int got = c->toolong ? FTP_LONG : FTP_LINE;
    c->line[c->toolong ? 0 : c->len] = 0;
    c->len = 0;
    c->toolong = 0;
    return got;
}

int ftp_ctl_feed(struct ftp_ctl *c, const char *buf, int n, int *got) {
    int i;
    for(i = 0; i < n; i++) {
        unsigned char b = (unsigned char)buf[i];
        switch(c->state) {
        case T_IAC:
            c->state = T_DATA;
            if(b == IAC) break;                 /* IAC IAC: a byte 255 of the line */
            if(b >= WILL) c->state = T_OPTION;  /* WILL, WONT, DO, DONT: their option byte follows */
            else if(b == SB) c->state = T_SB;
            continue;                           /* IP, DM (Synch), AYT, NOP, ...: nothing for a line */
        case T_OPTION:
            c->state = T_DATA;
            continue;
        case T_SB:
            if(b == IAC) c->state = T_SB_IAC;
            continue;
        case T_SB_IAC:
            c->state = b == SE ? T_DATA : T_SB;
            continue;
        default:
            if(b == IAC) {
                c->state = T_IAC;
                continue;
            }
        }
        if(c->cr) {                             /* what follows a CR says what it was */
            c->cr = 0;
            if(b == '\n') {
                *got = line_end(c);
                return i + 1;
            }
            put(c, '\r');                       /* a CR that ends nothing is part of the line */
            if(b == 0) continue;                /* CR NUL: Telnet's CR alone */
        }
        if(b == '\r') {
            c->cr = 1;
            continue;
        }
        if(b == '\n') {
            *got = line_end(c);
            return i + 1;
        }
        put(c, b ? (char)b : 0x7f);             /* a NUL would cut the line short: a DEL, which no path takes */
    }
    *got = FTP_MORE;
    return n;
}

/**** commands ****/

#define LOGIN FTP_F_LOGIN
#define ARG   FTP_F_ARG
#define WRITE FTP_F_WRITE
#define DATA  FTP_F_DATA

static const struct ftp_verb verbs[] = {
    { "USER", FTP_USER, ARG },          { "PASS", FTP_PASS, 0 },            { "ACCT", FTP_ACCT, 0 },
    { "QUIT", FTP_QUIT, 0 },            { "REIN", FTP_REIN, 0 },            { "NOOP", FTP_NOOP, 0 },
    { "SYST", FTP_SYST, 0 },            { "FEAT", FTP_FEAT, 0 },            { "OPTS", FTP_OPTS, ARG },
    { "HELP", FTP_HELP, 0 },            { "STAT", FTP_STAT, 0 },            { "SITE", FTP_SITE, LOGIN | ARG },
    { "PWD",  FTP_PWD, LOGIN },         { "XPWD", FTP_PWD, LOGIN },         { "CWD",  FTP_CWD, LOGIN | ARG },
    { "XCWD", FTP_CWD, LOGIN | ARG },   { "CDUP", FTP_CDUP, LOGIN },        { "XCUP", FTP_CDUP, LOGIN },
    { "TYPE", FTP_TYPE, LOGIN | ARG },  { "MODE", FTP_MODE, LOGIN | ARG },  { "STRU", FTP_STRU, LOGIN | ARG },
    { "ALLO", FTP_ALLO, LOGIN },        { "PASV", FTP_PASV, LOGIN },        { "EPSV", FTP_EPSV, LOGIN },
    { "PORT", FTP_PORT_CMD, LOGIN | ARG }, { "EPRT", FTP_EPRT, LOGIN | ARG },
    { "LIST", FTP_LIST, LOGIN | DATA }, { "NLST", FTP_NLST, LOGIN | DATA }, { "MLSD", FTP_MLSD, LOGIN | DATA },
    { "MLST", FTP_MLST, LOGIN },        { "SIZE", FTP_SIZE, LOGIN | ARG },  { "RETR", FTP_RETR, LOGIN | ARG | DATA },
    { "STOR", FTP_STOR, LOGIN | ARG | DATA | WRITE },                       { "STOU", FTP_STOU, LOGIN | DATA | WRITE },
    { "APPE", FTP_APPE, LOGIN | ARG | DATA | WRITE },                       { "REST", FTP_REST, LOGIN | ARG },
    { "ABOR", FTP_ABOR, LOGIN },        { "DELE", FTP_DELE, LOGIN | ARG | WRITE },
    { "MKD",  FTP_MKD, LOGIN | ARG | WRITE },  { "XMKD", FTP_MKD, LOGIN | ARG | WRITE },
    { "RMD",  FTP_RMD, LOGIN | ARG | WRITE },  { "XRMD", FTP_RMD, LOGIN | ARG | WRITE },
    { "RNFR", FTP_RNFR, LOGIN | ARG | WRITE }, { "RNTO", FTP_RNTO, LOGIN | ARG | WRITE },
    { "HOST", FTP_HOST, ARG },          { "CLNT", FTP_CLNT, 0 },
    /* known, not done: 502, and clients go on without them. AUTH and the rest of RFC 2228 (no
       encryption here), MDTM and MFMT (UTC times: ftp.h), the hashes, the long and spare forms */
    { "AUTH", FTP_NOTIMPL, 0 }, { "ADAT", FTP_NOTIMPL, 0 }, { "PBSZ", FTP_NOTIMPL, 0 }, { "PROT", FTP_NOTIMPL, 0 },
    { "CCC",  FTP_NOTIMPL, 0 }, { "MIC",  FTP_NOTIMPL, 0 }, { "CONF", FTP_NOTIMPL, 0 }, { "ENC",  FTP_NOTIMPL, 0 },
    { "MDTM", FTP_NOTIMPL, 0 }, { "MFMT", FTP_NOTIMPL, 0 }, { "MFCT", FTP_NOTIMPL, 0 }, { "MFF",  FTP_NOTIMPL, 0 },
    { "SMNT", FTP_NOTIMPL, 0 }, { "LANG", FTP_NOTIMPL, 0 }, { "LPRT", FTP_NOTIMPL, 0 }, { "LPSV", FTP_NOTIMPL, 0 },
    { "XCRC", FTP_NOTIMPL, 0 }, { "XMD5", FTP_NOTIMPL, 0 }, { "XSHA1", FTP_NOTIMPL, 0 }, { "HASH", FTP_NOTIMPL, 0 },
    { "AVBL", FTP_NOTIMPL, 0 }, { "DSIZ", FTP_NOTIMPL, 0 }, { "SPSV", FTP_NOTIMPL, 0 }, { "THMB", FTP_NOTIMPL, 0 },
};

static int letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int ftp_split(const char *line, char *verb, const char **arg) {
    int n = 0;
    while(n < 7 && letter(line[n])) {
        verb[n] = (char)(line[n] >= 'a' ? line[n] - 'a' + 'A' : line[n]);
        n++;
    }
    if(!n || (line[n] && line[n] != ' ')) return -1;
    verb[n] = 0;
    *arg = line[n] == ' ' ? line + n + 1 : line + n;
    return 0;
}

const struct ftp_verb *ftp_find(const char *verb) {
    unsigned int i;
    for(i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++) if(!strcmp(verbs[i].name, verb)) return &verbs[i];
    return NULL;
}

/**** paths ****/

/* an absolute path without ".", ".." (".." above "/" stays there), empty names or a '/' at its end:
   "/" or "/a/b" into out, as httpd's http_normalize(): 0, or -1 when it does not fit */
static int normalize(const char *path, char *out, int size) {
    int o = 0, i = 0, n;
    if(size < 2) return -1;
    while(path[i]) {
        while(path[i] == '/') i++;
        for(n = 0; path[i + n] && path[i + n] != '/'; n++);
        if(!n) break;
        if(n == 1 && path[i] == '.') ;                              /* "." */
        else if(n == 2 && path[i] == '.' && path[i + 1] == '.') {    /* "..": the last name and its slash go */
            while(o > 0 && out[o - 1] != '/') o--;
            if(o > 0) o--;
        } else {
            if(o + 1 + n >= size) return -1;
            out[o++] = '/';
            memcpy(out + o, path + i, (size_t)n);
            o += n;
        }
        i += n;
    }
    if(!o) out[o++] = '/';
    out[o] = 0;
    return 0;
}

int ftp_path(const char *cwd, const char *arg, char *out, int size) {
    char joined[2 * FTP_PATH_MAX + 2];
    const char *p;
    for(p = arg; *p; p++) if((unsigned char)*p < 0x20 || *p == 0x7f) return -2;
    if(strlen(cwd) + strlen(arg) + 2 > sizeof(joined)) return -1;
    if(arg[0] == '/') snprintf(joined, sizeof(joined), "%s", arg);
    else snprintf(joined, sizeof(joined), "%s/%s", cwd, arg);
    return normalize(joined, out, size);
}

int ftp_real(const char *root, const char *vpath, char *out, int size) {
    int n;
    if(!strcmp(root, "/")) n = snprintf(out, (size_t)size, "%s", vpath);
    else if(!strcmp(vpath, "/")) n = snprintf(out, (size_t)size, "%s", root);
    else n = snprintf(out, (size_t)size, "%s%s", root, vpath);
    return n < 0 || n >= size ? -1 : 0;
}

/* is path dir itself, or below it? Letters of either case: the mount table is matched as it is,
   but nothing is lost by refusing /DEV too */
static int under(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return !strncasecmp(path, dir, n) && (path[n] == 0 || path[n] == '/');
}

int ftp_reserved(const char *real) {
    return under(real, "/dev") || under(real, "/.netman");
}

const char *ftp_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

int ftp_quote(char *out, int size, const char *path) {
    int o = 0;
    if(size < 3) return -1;
    out[o++] = '"';
    for(; *path; path++) {
        if(o + (*path == '"' ? 2 : 1) + 2 > size) return -1;
        if(*path == '"') out[o++] = '"';
        out[o++] = *path;
    }
    out[o++] = '"';
    out[o] = 0;
    return 0;
}

void ftp_clean(char *out, int size, const char *text) {
    int o = 0;
    if(size < 1) return;
    for(; *text && o < size - 1; text++)
        out[o++] = (unsigned char)*text < 0x20 || *text == 0x7f ? '?' : *text;
    out[o] = 0;
}

/**** data addresses ****/

/* up to 3 digits, 0-255, at *s: the value, s moved past them; -1 when there are none or it is more */
static int octet(const char **s) {
    int v = 0, n = 0;
    while(**s >= '0' && **s <= '9' && n < 3) {
        v = v * 10 + (*(*s)++ - '0');
        n++;
    }
    return n && v <= 255 && !(**s >= '0' && **s <= '9') ? v : -1;
}

/* four bytes into an address in network byte order: the first byte first in memory, as libsurf's ipparse() */
static unsigned int addr_of(const int b[4]) {
    return (unsigned int)b[0] | (unsigned int)b[1] << 8 | (unsigned int)b[2] << 16 | (unsigned int)b[3] << 24;
}

int ftp_parse_port(const char *arg, unsigned int *ip, unsigned short *port) {
    int b[6], i;
    for(i = 0; i < 6; i++) {
        if((b[i] = octet(&arg)) < 0) return -1;
        if(i < 5 && *arg++ != ',') return -1;
    }
    if(*arg) return -1;
    *ip = addr_of(b);
    *port = (unsigned short)(b[4] * 256 + b[5]);
    return 0;
}

int ftp_parse_eprt(const char *arg, unsigned int *ip, unsigned short *port) {
    char d = arg[0];
    int b[4], i;
    long v = 0;
    if(d < '!' || d > '~' || (d >= '0' && d <= '9')) return -1;     /* 10/25/26: a digit would be read as one */
    arg++;
    for(i = 0; arg[i] >= '0' && arg[i] <= '9' && i < 3; i++) v = v * 10 + (arg[i] - '0');
    if(!i || arg[i] != d) return -1;
    if(v != 1) return -2;                       /* 2 is IPv6: RFC 2428's 522 names the one there is */
    arg += i + 1;
    v = 0;
    for(i = 0; i < 4; i++) {
        if((b[i] = octet(&arg)) < 0) return -1;
        if(*arg++ != (i < 3 ? '.' : d)) return -1;
    }
    for(i = 0; *arg >= '0' && *arg <= '9' && i < 5; i++) v = v * 10 + (*arg++ - '0');
    if(!i || v < 1 || v > 65535 || *arg++ != d || *arg) return -1;
    *ip = addr_of(b);
    *port = (unsigned short)v;
    return 0;
}

void ftp_pasv_text(char *out, int size, unsigned int ip, unsigned short port) {
    const unsigned char *b = (const unsigned char *)&ip;   /* network byte order: the first octet first */
    snprintf(out, (size_t)size, "Entering Passive Mode (%u,%u,%u,%u,%u,%u).", b[0], b[1], b[2], b[3],
             (unsigned int)port >> 8, (unsigned int)port & 255);
}

void ftp_epsv_text(char *out, int size, unsigned short port) {
    snprintf(out, (size_t)size, "Entering Extended Passive Mode (|||%u|)", (unsigned int)port);
}

int ftp_parse_offset(const char *arg, long *off) {
    unsigned long v = 0;
    int n = 0;
    for(; *arg >= '0' && *arg <= '9'; arg++, n++) {
        v = v * 10 + (unsigned long)(*arg - '0');
        if(v > 0x7fffffffUL) return -1;
    }
    if(!n || *arg) return -1;
    *off = (long)v;
    return 0;
}

/**** listings ****/

void ftp_list_args(const char *arg, int *all, const char **path) {
    *all = 0;
    while(*arg == ' ') arg++;
    while(*arg == '-') {
        for(arg++; *arg && *arg != ' '; arg++) if(*arg == 'a' || *arg == 'A') *all = 1;
        while(*arg == ' ') arg++;
    }
    *path = arg;
}

static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* seconds since 1970 as a date (the days to a civil date: Howard Hinnant's method) */
static void civil(unsigned int t, int *year, int *month, int *day, int *hour, int *minute) {
    long z = (long)(t / 86400) + 719468, era = z / 146097, y;
    unsigned long doe = (unsigned long)(z - era * 146097), yoe, doy, mp;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(y + (*month <= 2));
    *hour = (int)(t % 86400 / 3600);
    *minute = (int)(t % 3600 / 60);
}

int ftp_list_line(char *out, int size, const struct ftp_entry *e, unsigned int now, int writable) {
    char when[8];
    int year, month, day, hour, minute, recent, n;
    civil(e->mtime, &year, &month, &day, &hour, &minute);
    if(e->mtime > now) recent = e->mtime - now <= 3600u;            /* a little ahead: clocks differ */
    else recent = now - e->mtime < 15552000u;                       /* 180 days */
    if(recent) snprintf(when, sizeof(when), "%02d:%02d", hour, minute);
    else snprintf(when, sizeof(when), " %4d", year);
    n = snprintf(out, (size_t)size, "%s   1 surfos   surfos   %10u %s %2d %s %s\r\n",
                 e->dir ? (writable ? "drwxr-xr-x" : "dr-xr-xr-x") : (writable ? "-rw-r--r--" : "-r--r--r--"),
                 e->dir ? 0u : e->size, months[month - 1], day, when, e->name);
    return n < 0 || n >= size ? -1 : n;
}

int ftp_mlsx_line(char *out, int size, const struct ftp_entry *e, int writable, int lead) {
    char sz[24] = "";
    int n;
    if(!e->dir) snprintf(sz, sizeof(sz), "size=%u;", e->size);
    n = snprintf(out, (size_t)size, "%stype=%s;%sperm=%s; %s\r\n", lead ? " " : "", e->dir ? "dir" : "file", sz,
                 e->dir ? (writable ? "cdeflmp" : "el") : (writable ? "adfrw" : "r"), e->name);
    return n < 0 || n >= size ? -1 : n;
}

int ftp_nlst_line(char *out, int size, const char *prefix, const char *name) {
    size_t p = strlen(prefix);
    int n = snprintf(out, (size_t)size, "%s%s%s\r\n", prefix, p && prefix[p - 1] != '/' ? "/" : "", name);
    return n < 0 || n >= size ? -1 : n;
}

/**** text mode ****/

int ftp_to_crlf(const char *in, int n, char *out, int *cr) {
    int i, o = 0;
    for(i = 0; i < n; i++) {
        if(in[i] == '\n' && !*cr) out[o++] = '\r';
        out[o++] = in[i];
        *cr = in[i] == '\r';
    }
    return o;
}

int ftp_from_crlf(const char *in, int n, char *out, int *held) {
    int i, o = 0;
    if(*held && (n == 0 || in[0] != '\n')) out[o++] = '\r';         /* the CR held was a CR alone */
    *held = 0;
    for(i = 0; i < n; i++) {
        if(in[i] == '\r') {
            if(i + 1 == n) {                    /* what follows is in the next piece */
                *held = 1;
                break;
            }
            if(in[i + 1] == '\n') continue;     /* CR LF: the LF alone */
        }
        out[o++] = in[i];
    }
    return o;
}

/**** replies ****/

int ftp_errno_reply(int e, const char **text) {
    switch(e) {
    case ENOENT:       *text = "No such file or directory"; return 550;
    case ENOTDIR:      *text = "Not a directory"; return 550;
    case EISDIR:       *text = "Is a directory"; return 550;
    case EEXIST:       *text = "File exists"; return 550;
    case ENOTEMPTY:    *text = "Directory not empty"; return 550;
    case EROFS:        *text = "Read-only file system"; return 550;
    case EACCES:       *text = "Permission denied"; return 550;
    case EPERM:        *text = "Operation not permitted"; return 550;
    case EXDEV:        *text = "Invalid cross-device link"; return 550;
    case ENODEV:       *text = "No such device"; return 550;
    case EBUSY:        *text = "Device or resource busy"; return 450;
    case EMFILE:       *text = "Too many open files"; return 450;
    case ENFILE:       *text = "File table overflow"; return 450;
    case ENOSPC:       *text = "No space left on device"; return 552;
    case EFBIG:        *text = "File too large"; return 552;
    case ENAMETOOLONG: *text = "File name too long"; return 553;
    case EINVAL:       *text = "Invalid argument"; return 553;     /* a name the file system cannot hold */
    case EIO:          *text = "I/O error"; return 451;
    case ENOMEM:       *text = "Out of memory"; return 451;
    }
    *text = NULL;                               /* the caller says strerror()'s */
    return 451;
}
