/*
SurfOS chat client: wchat
--------------------
File: ircfmt.h  Date: 11/05/26
--------------------
What IRC's text is on the screen (ircfmt.c), with no system calls, host-tested as it is.

IRC has no character set. A server's text is kept as UTF-8 when all of it is valid UTF-8, and
read as windows-1252 otherwise (ISO 8859-1's superset, what older clients send), each byte one
character. Then what is not for the screen goes: the C0 controls, DEL, the C1 controls and the
invisible marks that change the order or the joining of what is shown (U+200B-U+200F,
U+202A-U+202E, U+2060-U+2064, U+2066-U+2069, U+FEFF). A TAB is a space. CTCP's \x01 goes with the
controls: chat.c has read it before. Only the formatting codes stay, for the window to show or
to leave out.

The formatting codes are those of the IRC formatting document of ircdocs
(modern.ircdocs.horse/formatting): \x02 bold, \x1D italics, \x1F underline, \x1E strikethrough,
\x11 monospace, \x16 reverse, \x0F all of them off; \x03 a colour (up to two digits, then a
comma and up to two more for the background, 99 the default; \x03 alone puts both back) and
\x04 one in hex (RRGGBB, then ",RRGGBB"). The colours' numbers are that document's (0 white,
1 black, 2 blue ... 15 light grey; 16-98 the extended ones); their shades are ours, the
document says that clients choose them.

Rows: the window draws the console's characters, one 8-pixel cell each, so a row of a line is
counted in characters, codes counting as none.
*/

#ifndef _WCHAT_IRCFMT_H
#define _WCHAT_IRCFMT_H

#define IRC_TEXT_MAX   2048             /* a text kept: 510 bytes read as windows-1252 are at most 1530 */

/* the codes */
#define IRC_BOLD       0x02
#define IRC_COLOR      0x03
#define IRC_HEXCOLOR   0x04
#define IRC_PLAIN      0x0f
#define IRC_MONO       0x11
#define IRC_REVERSE    0x16
#define IRC_ITALIC     0x1d
#define IRC_STRIKE     0x1e
#define IRC_UNDERLINE  0x1f

/* in (n bytes from a server) as text for the screen into out (size bytes): valid UTF-8, the
   formatting codes kept, cut between characters when it does not fit. Its length. */
int irc_text(char *out, int size, const char *in, int n);

/* A colour: IRC_DEFAULT, a number 0-98, or IRC_RGB(0xRRGGBB) for \x04's */
#define IRC_DEFAULT    (-1)
#define IRC_RGB(v)     (0x1000000 | (int)(v))

struct irc_style {
    int fg, bg;
    unsigned char bold, italic, underline, strike, mono, reverse;
};

void irc_style_reset(struct irc_style *st);

/* The next run of s, from *pos, whose characters share one style: 1 with its bytes at s + *start
   (*len of them) and *st the style they have; 0 at the end. *pos and *st go on to the next call:
   start with *pos 0 and irc_style_reset(). */
int irc_run(const char *s, int *pos, struct irc_style *st, int *start, int *len);

/* s without its formatting codes into out (size bytes): its length */
int irc_strip(char *out, int size, const char *s);

/* the bytes of the code at s (and of its digits), 0 when s is no code */
int irc_code_len(const char *s);
int irc_is_code(int c);                 /* c is one of the codes' bytes */

/* a colour as 0x00RRGGBB: a number 0-98 or an IRC_RGB(); 0 for any other */
unsigned irc_rgb(int color);

/* Can text of colour fg be read on bg (both 0x00RRGGBB)? Their brightness apart by enough. */
int irc_readable(unsigned fg, unsigned bg);

/* the characters of s that take a cell, its codes counting as none */
int irc_cells(const char *s);

/* Where the row of s that starts at byte at ends, with rows of cols cells (1 or more): the byte
   after its last character, after the last space in it when a word would be cut and there is one,
   or the end of s. *next is where the next row starts: past the spaces at the break. */
int irc_wrap(const char *s, int at, int cols, int *next);

#endif
