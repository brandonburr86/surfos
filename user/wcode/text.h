/*
SurfOS user program: wcode
--------------------
File: text.h    Date: 11/05/26
--------------------
The Code Editor's text (text.c): a file's bytes exactly as they were read, in a gap buffer, with a
sparse index of line starts that also keeps the syntax colouring's states. Nothing here makes a
system call or draws, so tests/host/test_wcode.c runs it on the host as it is.

A position is a byte offset, 0 to len. A line ends at a line feed; a carriage return right before
it belongs to the line end (CR LF), never to the line's text, so a file keeps the line ends it came
with, LF, CR LF or both. A character is a UTF-8 sequence, or a byte that does not start one, which
counts as a character of its own (WC_BAD | the byte); a CR LF pair is one character too, so that
nothing ever stops between its two bytes.

The index: marks at line starts, about one in WC_MARK_EVERY lines, sorted, the first at line 0.
They are made where a lookup or the colouring scans text that has none (one in WC_MARK_EVERY lines
or WC_MARK_BYTES bytes, whichever comes first), and moved with every change, so a lookup scans at
most from the mark before it. Each mark also holds the state of
the colouring's tokenizer at its line's start (syntax.c), with whether that state is still
right; dirty_lo and dirty_hi say what changed since the colouring last looked.
*/
#ifndef _WCODE_TEXT_H
#define _WCODE_TEXT_H

#define WC_TEXT_MAX   (16u << 20)       /* a file's bytes, at most */
#define WC_POOL_MAX   (32u << 20)       /* the buffers of all open files, gaps included */
#define WC_GAP        (64u << 10)       /* the room a buffer gets beyond its text, at least */
#ifndef WC_MARK_EVERY                   /* (a test builds them small, to work the index hard) */
#define WC_MARK_EVERY 128               /* lines between the marks a scan leaves, */
#define WC_MARK_BYTES (16u << 10)       /* or bytes, whichever comes first */
#endif
#define WC_BAD        0x80000000u       /* a code point for a byte that starts no UTF-8 sequence: WC_BAD | the byte */

/* the memory every open file's buffer takes, together */
struct wc_pool { unsigned long used, limit; };

struct wc_mark {                        /* a line start the index knows */
    unsigned pos, line;
    unsigned state;                     /* the tokenizer's state at the line's start */
    unsigned char ok;                   /* state is that of the text as it is now */
};

struct wc_text {
    char *b;                            /* the bytes: [0, gs) then [ge, cap) */
    unsigned gs, ge, cap;
    unsigned len;                       /* the text's bytes */
    unsigned nl;                        /* its line feeds: it has nl + 1 lines */
    struct wc_mark *m;                  /* the index, m[0] line 0 */
    unsigned nm, mcap;
    unsigned dirty_lo, dirty_hi;        /* changed since the colouring looked: [lo, hi], none when lo > hi */
    unsigned edits;                     /* changes since the colouring looked */
    unsigned keep;                      /* 11/09/26: the room wc_text_reserve() promised: no shrinking meanwhile */
    struct wc_pool *pool;
};

/* a line: its text [start, end), then eol bytes of line end (0 for the last line, 1 for LF, 2 for CR LF) */
struct wc_line { unsigned start, end, eol; };

int wc_utf8_len(const unsigned char *s, unsigned n);     /* the bytes of the character at s, 1 for a bad byte */
unsigned wc_utf8_decode(const unsigned char *s, unsigned n, unsigned *cp);  /* bytes and code point (WC_BAD | b) */
int wc_utf8_put(unsigned cp, char *out);                /* the UTF-8 bytes of cp (0 for none), at most 4 */

int wc_text_init(struct wc_text *t, struct wc_pool *pool);  /* empty; -1 without memory */
void wc_text_free(struct wc_text *t);
/* reading a file in: room for n bytes, read into what this returns (NULL and errno EFBIG or ENOMEM); then the
   count that came. The text must be empty. */
char *wc_text_load_begin(struct wc_text *t, unsigned n);
void wc_text_load_end(struct wc_text *t, unsigned n);

/* changes: 0, or -1 with errno (EFBIG past WC_TEXT_MAX, ENOMEM past the pool or without memory) and nothing done */
int wc_text_insert(struct wc_text *t, unsigned pos, const char *s, unsigned n);
void wc_text_delete(struct wc_text *t, unsigned pos, unsigned n);
/* 11/09/26 (the review, C1): before a change that cuts and then puts, room for the text to reach len bytes at most
   meanwhile, so that no put of it can fail after a cut: 0, or -1 with errno (EFBIG past WC_TEXT_MAX, ENOMEM) and
   nothing done. The room stays (a deletion gives no memory back) until wc_text_release(). */
int wc_text_reserve(struct wc_text *t, unsigned long len);
void wc_text_release(struct wc_text *t);

/* reading */
unsigned char wc_text_byte(const struct wc_text *t, unsigned pos);
void wc_text_get(const struct wc_text *t, unsigned pos, char *out, unsigned n);
const char *wc_text_span(struct wc_text *t, unsigned pos, unsigned n);  /* n bytes in a row; may move the gap */
void wc_text_parts(const struct wc_text *t, const char **a, unsigned *na, const char **b, unsigned *nb);
unsigned wc_text_find_nl(const struct wc_text *t, unsigned from, unsigned to);    /* the first LF in [from, to), or to */
unsigned wc_text_count_nl(const struct wc_text *t, unsigned from, unsigned to);
unsigned wc_text_line_begin(const struct wc_text *t, unsigned pos);  /* the start of pos's line, by scanning back */
void wc_text_line(const struct wc_text *t, unsigned start, struct wc_line *l);   /* the line that starts at start */

/* the index */
unsigned wc_text_lines(const struct wc_text *t);
unsigned wc_text_line_start(struct wc_text *t, unsigned line);      /* past the last line: the last line's */
unsigned wc_text_line_of(struct wc_text *t, unsigned pos);
unsigned wc_text_mark_before(const struct wc_text *t, unsigned line);   /* the last mark at or before the line */
unsigned wc_text_mark_of(const struct wc_text *t, unsigned pos);        /* the last mark at or before the position */
int wc_text_mark_add(struct wc_text *t, unsigned at, unsigned pos, unsigned line);  /* a mark as m[at]: 0, or -1 */
void wc_text_drop_marks(struct wc_text *t, unsigned pos);   /* the marks after pos go (before many changes there) */

/* characters: the bytes of the one at pos (2 for a CR LF), 0 at the end; *cp its code point ('\n' for a line end) */
unsigned wc_text_char(const struct wc_text *t, unsigned pos, unsigned *cp);
unsigned wc_text_next(const struct wc_text *t, unsigned pos);
unsigned wc_text_prev(const struct wc_text *t, unsigned pos);
unsigned wc_text_snap(const struct wc_text *t, unsigned pos);       /* back to a character's start */

/* screen columns: a character is one cell, a tab reaches the next multiple of tabw */
unsigned wc_text_col(const struct wc_text *t, unsigned start, unsigned pos, unsigned tabw);
unsigned wc_text_at_col(const struct wc_text *t, const struct wc_line *l, unsigned col, unsigned tabw);

#endif
