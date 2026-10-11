/*
SurfOS Undertow: what every part of the browser shares
----------------------
File: undertow.h  Date: 10/11/26
----------------------
The limits of the design (docs/superpowers/specs/2026-10-10-undertow-design.md),
the deadlines of one load, and the helpers every module uses (util.c): the
monotonic clock, bounded error messages, UTF-8 decoding and text that is safe
to print on any terminal. Each module includes the standard headers it needs
and <surf.h> only for SurfOS's own calls; the host tests put
tests/host/undertow/surf.h in its place.

Conventions: functions return 0 (or a count) on success and -1 on failure, with
a message in the caller's err buffer (cap bytes, UT_ERR_MAX is enough for any);
outputs are unchanged on failure; nothing calls exit(). Large structures (a
struct ut_url is 6 KB) live on the heap, never on a nested stack.
*/

#ifndef UNDERTOW_H
#define UNDERTOW_H

#include <stddef.h>
#include <stdarg.h>

#define UT_VERSION       "1.0"
#define UT_USER_AGENT    "Undertow/" UT_VERSION " (SurfOS 1.0; text)"

/* sizes, in bytes unless they say otherwise */
#define UT_ERR_MAX       2400u              /* an error buffer: a sentence and one whole URL */
#define UT_HEADERS_MAX   (32u * 1024)       /* a response's header lines, interim responses and trailers together */
#define UT_BODY_MAX      (1024u * 1024)     /* a response body as received (after chunking is undone) */
#define UT_TEXT_MAX      (512u * 1024)      /* the rendered text of one page */
#define UT_LINKS_MAX     256u               /* links on one page */
#define UT_ANCHORS_MAX   256u               /* fragment targets on one page */
#define UT_FIELD_MAX     1024u              /* a title, a link's label, an anchor's name */
#define UT_HISTORY_MAX   16u                /* pages kept for back and forward */
#define UT_HISTORY_BYTES (8u * 1024 * 1024) /* what those pages may own together */
#define UT_COMMAND_MAX   4096u              /* one line typed at the undertow> prompt */
#define UT_SEARCH_MAX    1024u              /* the text of find */
#define UT_REDIRECTS_MAX 5                  /* redirects followed in one load */
#define UT_INTERIM_MAX   8                  /* 1xx responses accepted before the real one */

/* deadlines, in milliseconds of ut_now() */
#define UT_DNS_MS        5000u              /* SYS_resolve's own bound */
#define UT_CONNECT_MS    10000u             /* SYS_connect's own bound */
#define UT_HANDSHAKE_MS  20000u             /* a TLS handshake */
#define UT_IDLE_MS       15000u             /* a read or a write that makes no progress */
#define UT_LOAD_MS       60000u             /* one whole load, every redirect included */

/* files in the initrd */
#define UT_CONF_PATH     "/initrd/etc/undertow.conf"
#define UT_CA_PATH       "/initrd/etc/ssl/cert.pem"

/* The monotonic clock: uptime() in milliseconds, which wraps after 49.7 days. A deadline is a
   ut_now() value; compare deadlines only through these two, which stay right across the wrap
   (for spans under 24 days). */
unsigned int ut_now(void);
int ut_expired(unsigned int deadline, unsigned int now);            /* now is at or past deadline */
unsigned int ut_remaining(unsigned int deadline, unsigned int now); /* milliseconds left, 0 once expired */

/* printf into err: cap bytes, always terminated when err is not NULL and cap > 0 (nothing
   is written otherwise). Every byte that is not printable ASCII becomes '?', so a message is
   one line that is safe on any terminal, whatever a server sent. Returns -1, so a failing
   function can end with "return ut_error(err, cap, ...);". */
int ut_error(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int ut_verror(char *err, size_t cap, const char *fmt, va_list ap);

/* UTF-8. ut_utf8_decode() reads one character from s (len > 0 bytes): it returns how many
   bytes it used (1-4) and the code point in *cp. Anything that is not well-formed UTF-8
   (a stray continuation byte, a truncated or overlong sequence, a surrogate, a value above
   U+10FFFF) uses 1 byte and gives U+FFFD. ut_utf8_encode() writes cp (at most 4 bytes) and
   returns how many; a surrogate or a value above U+10FFFF is written as U+FFFD. */
#define UT_REPLACEMENT 0xfffdu
size_t ut_utf8_decode(const unsigned char *s, size_t len, unsigned int *cp);
size_t ut_utf8_encode(unsigned int cp, unsigned char out[4]);

/* Characters that must never reach a terminal: C0 controls, DEL, C1 controls, the
   bidirectional marks, overrides and isolates (U+061C, U+200E, U+200F, U+202A-U+202E,
   U+2066-U+2069), the line and paragraph separators U+2028 and U+2029, and U+FEFF. */
int ut_unsafe_char(unsigned int cp);

/* The ASCII stand-in for a character, for a terminal without UTF-8: itself for printable
   ASCII; a transliteration for common punctuation and Latin letters (U+201C to "\"", U+2014
   to "-", U+2026 to "...", U+00E9 to "e", U+00DF to "ss", U+00A0 to " "); "?" otherwise. */
const char *ut_ascii_for(unsigned int cp);

/* Copy len bytes of in, UTF-8 or not, to out as one line that is safe on any terminal:
   unsafe characters are dropped, except that tabs and line breaks become spaces, and
   ill-formed UTF-8 becomes U+FFFD. With utf8 0 only printable ASCII is written
   (ut_ascii_for()). out (cap > 0) is always terminated; a character that does not fit
   whole is left out, with everything after it. Returns the bytes written. */
size_t ut_safe_text(char *out, size_t cap, const char *in, size_t len, int utf8);

#endif
