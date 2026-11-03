/*
SurfOS file transfer client: ftp
--------------------
File: ftpclient.h  Date: 11/01/26
--------------------
The client's side of FTP, with no system calls, so tests/host/test_ftpc.c runs it as it is. It
adds to what the server's module has (user/ftpd/ftp.c, linked as it is: the control connection's
lines with Telnet taken out, PORT's numbers, a path's last name, text mode's line ends) what
only a client needs: replies, the commands it sends, the passive replies, the 257 and 213
replies, what is shown on a terminal, the words of a typed line, listings' lines and the checks
on their names, wildcards, and a password as it is typed.

Everything a server sends is hostile input. Every length here is bounded, whatever comes.
*/

#ifndef _FTPCLIENT_H
#define _FTPCLIENT_H

#include "../ftpd/ftp.h"

#define FTPC_REPLY_LINES 1000           /* lines of one reply; more is not FTP */
#define FTPC_REPLY_BYTES 65536          /* its bytes */
#define FTPC_LIST_LINE   1024           /* a line of a listing */
#define FTPC_NAME_MAX    255            /* a name from a listing, FAT's longest */
#define FTPC_SECRET_MAX  128            /* a password */
#define FTPC_PORT_MIN    1024           /* the lowest port a passive reply may name */

/**** replies ****/

/* RFC 959: one line, "123 text", or several: "123-text", any lines, "123 text". */
struct ftpc_reply {
    int code;                           /* 0 until the first line */
    int lines;                          /* lines so far */
    int bytes;
    int multi;                          /* the first line said 123-: the reply ends at 123 and a space */
    char text[FTP_LINE_MAX + 1];        /* the last line's text, after the code; "" when there is none */
};

enum { FTPC_R_MORE, FTPC_R_DONE, FTPC_R_BAD };

void ftpc_reply_init(struct ftpc_reply *r);

/* One line of a reply, as ftp_ctl_feed() gives it; toolong: FTP_LONG, a line that was dropped.
   FTPC_R_MORE, FTPC_R_DONE, or FTPC_R_BAD: this is no reply (no code on the first line, or too
   long a reply), and the connection is not to be trusted any further */
int ftpc_reply_line(struct ftpc_reply *r, const char *line, int toolong);

/**** commands ****/

/* "VERB arg" and CR LF into out (arg may be NULL): its length, or -1 when it does not fit or arg
   holds a control character, which could end the line and start another command. A byte 255 is
   doubled (the control connection is Telnet: RFC 959). */
int ftpc_command(char *out, int size, const char *verb, const char *arg);

/**** what replies say ****/

/* 229: the port in "(|||port|)", with any delimiter: 0, or -1 when there is no such text */
int ftpc_parse_epsv(const char *text, unsigned short *port);

/* 227: the port of "h1,h2,h3,h4,p1,p2", found anywhere in the text (RFC 1123): 0, or -1. The address
   is not given back: a data connection goes to the server's own address, whatever a reply names */
int ftpc_parse_pasv(const char *text, unsigned short *port);

/* may a passive reply name this port? Not one below FTPC_PORT_MIN, a service's: a server's reply
   does not get to aim this client at its mail port, say */
int ftpc_port_ok(unsigned short port);

/* 257: the path in double quotes, each "" a quote, into out: 0, or -1 */
int ftpc_parse_path(const char *text, char *out, int size);

/* 213: digits and nothing else after them but a blank: 0, or -1 (none, or past 64 bits) */
int ftpc_parse_size(const char *text, unsigned long long *size);

/**** what is shown ****/

/* text from a server (a reply, a name) as it may be shown on a terminal, into out: control
   characters, C1 controls and bytes that are no UTF-8 become '?', a tab a blank; cut at a whole
   character when it does not fit */
void ftpc_text(char *out, int size, const char *text);

/* every copy of secret in text becomes stars: a server that says the password back is not
   believed to be allowed to print it */
void ftpc_scrub(char *text, const char *secret);

/**** what is typed ****/

/* The words of line, in place: blanks between them, double quotes around a word that has blanks
   in it, \" and \\ inside the quotes. How many; -1 for more than max, -2 for a quote not closed */
int ftpc_words(char *line, char **word, int max);

/* The last name of a remote or local path, "" when it ends in a '/' */
const char *ftpc_last_name(const char *path);

/* dir and name, a '/' between them unless dir is "" or ends in one, into out: 0, or -1 */
int ftpc_join(char *out, int size, const char *dir, const char *name);

/* path split at its last '/': the folder into dir ("" for none; "/" kept) and *name after it */
int ftpc_split(const char *path, char *dir, int size, const char **name);

/**** names from a server ****/

/* A name from a listing, to become a local file by mget: one name, no '/', not "." or "..", no
   control character, nothing FAT refuses (\ : * ? " < > |), valid UTF-8, no blank or dot at its
   end (FAT drops those, so ".. " could come out as something else), no '-' at its start (it would
   be an option to the next tool) */
int ftpc_safe_name(const char *name);

/* Does name match pattern? * any characters, ? one character (a whole UTF-8 sequence), [a-z] and
   [!a-z] one of or none of those; \ takes the next character as it is. A wildcard does not match a
   name's leading dot. Bounded by the two lengths' product: there is no recursion. icase: FAT's rule */
int ftpc_glob(const char *pattern, const char *name, int icase);

/* a listing's lines: a line ends with LF, a CR before it goes; a NUL becomes DEL, so a name with
   one in it is refused rather than cut short */
struct ftpc_lines {
    int len;
    int toolong;
    char line[FTPC_LIST_LINE + 1];
};

/* Takes bytes of in up to a line's end: how many. *got: FTP_LINE, l->line holds it; FTP_LONG, a
   line past FTPC_LIST_LINE ended; FTP_MORE, all n were taken */
int ftpc_lines_feed(struct ftpc_lines *l, const char *in, int n, int *got);

/* the end of the listing: FTP_LINE when a last line had no end, else FTP_MORE */
int ftpc_lines_end(struct ftpc_lines *l);

/**** a password as it is typed ****/

struct ftpc_secret {
    char text[FTPC_SECRET_MAX + 1];
    int len;
    int esc;                            /* inside the sequence of a special key */
    int over;                           /* more was typed than fits */
};

enum { FTPC_S_MORE, FTPC_S_DONE, FTPC_S_CANCEL };

/* one key: Return ends it (FTPC_S_DONE), Ctrl+C and Ctrl+D cancel it (FTPC_S_CANCEL), Backspace and
   Ctrl+U edit it; a special key's sequence and other control characters are dropped */
int ftpc_secret_key(struct ftpc_secret *s, unsigned char c);

/* the bytes are gone, whatever the compiler thinks of a buffer nobody reads again */
void ftpc_wipe(void *p, int n);

#endif
