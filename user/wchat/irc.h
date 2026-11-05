/*
SurfOS chat client: wchat
--------------------
File: irc.h     Date: 11/05/26
--------------------
IRC's messages on the wire, as a module with no system calls (irc.c), so that
tests/host/test_wchat.c runs it on the host as it is, with hostile, split and oversized lines.

A server's bytes are cut into lines at LF; a CR right before it is part of the line end, a CR
anywhere else stays in the line and is taken out with the other controls later (ircfmt.c), so
that no server can make one of its lines read as two here. A line is at most IRC_IN_MAX bytes:
RFC 1459 and RFC 2812 (section 2.3) give a message 512 bytes with its CR LF, and IRCv3's message
tags may come in front of it with 8191 more. A longer line is dropped whole, up to its LF, and
so is a line with a NUL in it, which no message may hold. Empty lines are passed over.

A line is parsed in place: the tags, the prefix (a server's name, or nick!user@host), the
command (letters, made upper case, or three digits) and up to 15 parameters, the last of which
may hold spaces after a ':' (RFC 2812 section 2.3.1; after 14 the rest is the 15th, with or
without its ':'). Spaces between the parts may be more than one, as RFC 1459's grammar allows.

Names are compared under the server's case mapping (the ISUPPORT draft's three: rfc1459, where
[ ] \ ^ are the upper case of { } | ~; strict-rfc1459, without ^ and ~; ascii). A line going
out is put together here and nowhere else: irc_compose() refuses a CR, an LF or a NUL in any
part, a space or a leading ':' in any part but the last, and a line over 512 bytes, so nothing
typed or received can add a command of its own to one of ours.
*/

#ifndef _WCHAT_IRC_H
#define _WCHAT_IRC_H

#define IRC_LINE_MAX   512              /* a message with its CR LF (RFC 1459, RFC 2812 section 2.3) */
#define IRC_TAGS_MAX   8191             /* IRCv3 message tags: the '@', the tags and the space after them */
#define IRC_IN_MAX     (IRC_TAGS_MAX + IRC_LINE_MAX)   /* a line from a server, its line end included */
#define IRC_PARAMS     15               /* a message's parameters at most (RFC 2812 section 2.3.1) */
#define IRC_COMMAND_MAX 32              /* a command's letters: no command IRC has is longer */

/* the line reader: room for one line at a time */
struct irc_reader {
    char line[IRC_IN_MAX + 1];
    int len;                            /* bytes of the line so far */
    int over;                           /* it has passed IRC_IN_MAX: dropped up to its LF */
    int ready;                          /* line holds a whole line, given out by the last call */
};

/* what irc_read() found */
enum { IRC_MORE, IRC_LINE, IRC_LONG, IRC_BAD };

/* Takes n bytes of in, up to the end of the next line: *used of them. IRC_LINE: r->line holds the
   line without its line end, NUL-terminated, until the next call; IRC_LONG: a line over IRC_IN_MAX
   ended and was dropped; IRC_BAD: a line with a NUL in it was dropped; IRC_MORE: every byte was
   taken (*used is n) and no line has ended. memset() starts a struct irc_reader. */
int irc_read(struct irc_reader *r, const char *in, int n, int *used);

/* a message, its parts pointing into the line it was parsed from */
struct irc_msg {
    char *tags;                         /* without the '@'; NULL when there are none */
    char *prefix;                       /* without the ':', the nick alone once split; NULL when none */
    char *nick, *user, *host;           /* the prefix's parts: nick (or a server's name), "" for those absent */
    int server;                         /* the prefix is a server's, or there is none */
    char *command;                      /* upper case letters, or three digits */
    int numeric;                        /* the digits' value, -1 for a word */
    int argc;
    char *argv[IRC_PARAMS];
};

/* Parses line in place (it writes NULs into it): 0, or -1 when it is no message (no command, a
   command of anything but letters or three digits, a prefix or tags with nothing after them). */
int irc_parse(char *line, struct irc_msg *m);

/* the case mappings (ISUPPORT CASEMAPPING) */
enum { IRC_CM_RFC1459, IRC_CM_STRICT, IRC_CM_ASCII };

int irc_lower(int cm, int c);                           /* a byte in lower case under cm */
int irc_casecmp(int cm, const char *a, const char *b);  /* <0, 0, >0 as strcmp() of both in lower case */
int irc_casemap_named(const char *name);                /* CASEMAPPING's value: its number (rfc1459 for any other) */

/* Can s be a nick or a channel's name here? Not empty, at most max bytes, no byte that would end
   or split a message's part or a list of them (controls, space, ',', DEL), and no ':' first. A nick
   is ASCII, as RFC 2812's are, has none of ! @ * ? (a mask's), and no channel type (chantypes)
   first. */
int irc_valid_nick(const char *s, int max, const char *chantypes);
int irc_valid_channel(const char *s, int max, const char *chantypes);

/* A line to send into out (size bytes): cmd, then mid[0..nmid-1] each as a middle parameter, then
   trail, when not NULL, as the last with its ':', and CR LF. Its length, or -1 when any part holds
   a CR, LF or NUL, a middle one is empty or holds a space or starts with ':', cmd is not letters,
   or the line would be over IRC_LINE_MAX bytes or not fit in size. */
int irc_compose(char *out, int size, const char *cmd, const char *const *mid, int nmid, const char *trail);

/* in (n bytes) as base64 (RFC 4648 section 4) into out (size bytes, its NUL included): its length,
   or -1 when it does not fit */
int irc_base64(const unsigned char *in, int n, char *out, int size);

/* A CTCP message (a PRIVMSG or NOTICE whose text starts with \x01): its command in upper case into
   cmd (size bytes; letters and digits, at most size - 1 of them, else no CTCP), and *arg, *arglen
   its argument after the space, up to the closing \x01 or the end. 1, or 0 when text is no CTCP. */
int irc_ctcp(const char *text, char *cmd, int size, const char **arg, int *arglen);

#endif
