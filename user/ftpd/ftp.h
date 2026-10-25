/*
SurfOS file transfer server: ftpd
--------------------
File: ftp.h     Date: 10/24/26
--------------------
The FTP protocol of ftpd (ftp.c): RFC 959, with what clients use today from RFC 2428
(EPSV, EPRT), RFC 3659 (SIZE, REST STREAM, MLSD, MLST, TVFS) and RFC 2640 (UTF-8 names).
The control connection's lines, the commands, a session's paths and the file system's,
the data addresses, the lines of the listings, text mode's line ends, a path quoted for a
257 reply and the reply to a failed file call. ftp.c makes no system calls, so it is
tested on the host as it is (tests/host/test_ftp.c); the server (server.c) does the
connections and the files.
*/

#ifndef _FTP_H
#define _FTP_H

#define FTP_LINE_MAX 512                /* a command line, its end not counted (RFC 959 sets none) */
#define FTP_PATH_MAX 256                /* a path with its NUL: the kernel's VFS_PATH_MAX */
#define FTP_PORT     21

/**** the control connection ****/

/* RFC 959 has the control connection speak Telnet (RFC 854): its commands (IAC, 255, and
   what follows it: the IP and Synch some clients send before ABOR, option negotiations) are
   taken out, IAC IAC is a byte 255, CR NUL is a CR, and a line ends with CR LF or LF. Urgent
   data (Python's ftplib sends ABOR so) comes in line with the rest: SurfOS's TCP has no urgent
   mode. A NUL in a line becomes a DEL, which no path takes, rather than cutting it short. */
struct ftp_ctl {
    int len;                            /* bytes of the line so far */
    unsigned char state;                /* the Telnet decoder's (ftp.c) */
    unsigned char cr;                   /* a CR came last: it ends the line if an LF follows */
    unsigned char toolong;              /* the line passed FTP_LINE_MAX: the rest of it is dropped */
    char line[FTP_LINE_MAX + 1];
};

enum { FTP_MORE, FTP_LINE, FTP_LONG };

/* Takes bytes of buf (n of them) up to the end of a line: how many it took. *got says what
   ended: FTP_LINE, a line in c->line (NUL-terminated, without its end, valid until the next
   call); FTP_LONG, a line longer than FTP_LINE_MAX (dropped); FTP_MORE, all n bytes were
   taken and no line ended. */
int ftp_ctl_feed(struct ftp_ctl *c, const char *buf, int n, int *got);

/**** commands ****/

enum {
    FTP_USER = 1, FTP_PASS, FTP_ACCT, FTP_QUIT, FTP_REIN, FTP_NOOP, FTP_SYST, FTP_FEAT, FTP_OPTS, FTP_HELP,
    FTP_STAT, FTP_SITE, FTP_PWD, FTP_CWD, FTP_CDUP, FTP_TYPE, FTP_MODE, FTP_STRU, FTP_ALLO, FTP_PASV,
    FTP_EPSV, FTP_PORT_CMD, FTP_EPRT, FTP_LIST, FTP_NLST, FTP_MLSD, FTP_MLST, FTP_SIZE, FTP_RETR, FTP_STOR,
    FTP_STOU, FTP_APPE, FTP_REST, FTP_ABOR, FTP_DELE, FTP_MKD, FTP_RMD, FTP_RNFR, FTP_RNTO, FTP_HOST,
    FTP_CLNT, FTP_NOTIMPL
};

#define FTP_F_LOGIN 1                   /* only after USER and PASS (530 before) */
#define FTP_F_ARG   2                   /* needs an argument (501 without) */
#define FTP_F_WRITE 4                   /* changes files: refused when the folder is served read-only */
#define FTP_F_DATA  8                   /* moves something over the data connection */

struct ftp_verb {
    const char *name;
    int id;
    int flags;
};

/* The verb of a command line, in capitals, into verb (8 bytes), and where its argument begins
   (*arg: after the one space that follows the verb; "" when there is none): 0, or -1 when the
   line does not start with a verb (letters, at most 7, then a space or the end) */
int ftp_split(const char *line, char *verb, const char **arg);

/* The entry of a verb ftpd knows (USER, ..., the X forms XPWD XCWD XCUP XMKD XRMD), or NULL.
   Verbs it knows but does not do (AUTH, MDTM, ...) have the id FTP_NOTIMPL. */
const struct ftp_verb *ftp_find(const char *verb);

/**** paths ****/

/* A session's path for arg as a client sends it (from the folder served when it starts with
   '/', else from cwd, the session's directory) into out (size bytes): "/" or "/a/b", without
   "." or "..", where ".." above "/" stays at "/". 0; -1 when it does not fit; -2 when arg holds
   a control character, which no name may have */
int ftp_path(const char *cwd, const char *arg, char *out, int size);

/* The file system's path for the session's path vpath under the folder served (root, absolute):
   0, or -1 when it does not fit in size bytes */
int ftp_real(const char *root, const char *vpath, char *out, int size);

/* Is real (a file system path) one ftpd never touches? /dev, the devices, and /.netman, where
   the network manager mounts the stick of its settings for a moment (kernel/netman.c), and what
   is under them */
int ftp_reserved(const char *real);

/* The last name of a path: "/a/b" gives "b", "/" gives "/" */
const char *ftp_basename(const char *path);

/* path in double quotes, each '"' in it doubled (RFC 959's 257 replies), into out: 0, or -1
   when it does not fit */
int ftp_quote(char *out, int size, const char *path);

/* text for a reply (a path, a name): controls become '?', so no line end can get into a reply */
void ftp_clean(char *out, int size, const char *text);

/**** the data connection's addresses (IPv4, network byte order; ports in host order) ****/

/* PORT's argument, "h1,h2,h3,h4,p1,p2": 0 with *ip and *port, -1 when it is no such text */
int ftp_parse_port(const char *arg, unsigned int *ip, unsigned short *port);

/* EPRT's argument, "|1|a.b.c.d|port|" with any delimiter from '!' to '~': 0, -1 when it is
   no such text, -2 for another network protocol than 1 (IPv4) */
int ftp_parse_eprt(const char *arg, unsigned int *ip, unsigned short *port);

/* The text of PASV's 227 reply ("Entering Passive Mode (h1,h2,h3,h4,p1,p2).") and of EPSV's
   229 ("Entering Extended Passive Mode (|||port|)"), into out */
void ftp_pasv_text(char *out, int size, unsigned int ip, unsigned short port);
void ftp_epsv_text(char *out, int size, unsigned short port);

/* REST's argument: digits only, below 2 GB (SurfOS's file offsets): 0 with *off, else -1 */
int ftp_parse_offset(const char *arg, long *off);

/**** listings ****/

struct ftp_entry {
    const char *name;
    int dir;
    unsigned int size, mtime;           /* mtime: SurfOS's clock, local time with no zone */
};

/* LIST's and NLST's argument: the options first (words starting with '-'; a or A asks for the
   names starting with a dot too), then the path (*path, "" for none) */
void ftp_list_args(const char *arg, int *all, const char **path);

/* A line of LIST, as ls -l prints it ("-rw-r--r--   1 surfos surfos   1234 Oct 24 10:00 name"
   and CR LF; a time more than six months back or an hour ahead of now has its year instead):
   its length, or -1 when it does not fit. writable: the permissions show w */
int ftp_list_line(char *out, int size, const struct ftp_entry *e, unsigned int now, int writable);

/* A line of MLSD ("type=file;size=1234;perm=adfrw; name" and CR LF) or, with lead, of MLST
   (a space before it, and the path as e->name): its length, or -1. No modify fact: RFC 3659
   gives it in UTC, which SurfOS's clock does not know */
int ftp_mlsx_line(char *out, int size, const struct ftp_entry *e, int writable, int lead);

/* A line of NLST: prefix (the argument's folder, "" for none) and the name, and CR LF */
int ftp_nlst_line(char *out, int size, const char *prefix, const char *name);

/**** text mode (TYPE A) ****/

/* To the network: each LF without a CR before it becomes CR LF. in (n bytes) into out (room for
   2n): its length. *cr carries "the last byte was a CR" from one piece to the next */
int ftp_to_crlf(const char *in, int n, char *out, int *cr);

/* From the network: CR LF becomes LF, and a CR alone stays. in (n bytes) into out (room for n + 1):
   its length. A CR at the end of a piece is held (*held) until the next piece shows what follows
   it; at the end of the transfer a call with n 0 gives it back */
int ftp_from_crlf(const char *in, int n, char *out, int *held);

/**** replies ****/

/* The reply to a file call that failed with errno e: its code (450, 451, 550, 552, 553), and
   its text in *text (errno's own words, as Linux has them) */
int ftp_errno_reply(int e, const char **text);

#endif
