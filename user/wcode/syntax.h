/*
SurfOS user program: wcode
--------------------
File: syntax.h  Date: 11/05/26
--------------------
The Code Editor's colouring: small tokenizers of its own, one a language (lex.c), and what keeps
their states along the text (syntax.c). A tokenizer reads one line at a time, from the state the
line starts in (inside a comment, a string, a here-document, a fenced block...) to the state it
ends in, and gives each byte a class, which the window draws in a colour. The states at the
text index's marks (text.h) are kept, so drawing a screenful tokenizes from the mark before it,
never from the top of the file, and a change only makes the states after it stale. A state is
one unsigned number, 0 at the top of a file in every language.

The languages: C and its headers; assembly in NASM's syntax and in GNU as's (AT&T, with the C
preprocessor of .S files); Python; shell (POSIX sh, as surfsh takes it); Makefiles; Markdown
(CommonMark's blocks, roughly its inline rules); JSON (with line and block comments, as many
configuration files have them). Plain text has no colours.
*/
#ifndef _WCODE_SYNTAX_H
#define _WCODE_SYNTAX_H

#include "text.h"

enum { WC_LANG_TEXT, WC_LANG_C, WC_LANG_NASM, WC_LANG_GAS, WC_LANG_PY, WC_LANG_SH, WC_LANG_MAKE, WC_LANG_MD,
       WC_LANG_JSON, WC_LANGS };

/* the classes of the bytes, which the window draws in colours */
enum { WC_C_TEXT, WC_C_COMMENT, WC_C_STRING, WC_C_NUMBER, WC_C_KEYWORD, WC_C_TYPE, WC_C_PREPROC, WC_C_VAR,
       WC_C_LABEL, WC_C_HEADING, WC_C_EMPH, WC_C_CODE, WC_C_LINK, WC_C_KEY, WC_C_BAD, WC_CLASSES };

/* 11/09/26 (the review, C2): the tokenizers read a line's first WC_LEX_MAX bytes, as if it ended there (the state
   it ends in is theirs), and the rest is drawn plain, so that no line costs more than these to colour */
#define WC_LEX_MAX     (8u << 10)
#define WC_LEX_CLASSES WC_LEX_MAX       /* a line's bytes past these are drawn uncoloured */

const char *wc_lang_name(int lang);                         /* "C", "Assembly (NASM)", ... */
/* the language of a file by its name (its last part is enough) and its first line (#! lines) */
int wc_lang_guess(const char *name, const char *first, unsigned n);
const char *wc_lang_comment(int lang);                      /* a line comment's start, NULL for none */
int wc_lang_braces(int lang);                               /* { } close blocks (auto-indent, the closing brace) */

/* one line, s[0..n) without its line end, from state: the state at its end, and the classes of its first ncls
   bytes into cls (NULL: the state only) */
unsigned wc_lex(int lang, const char *s, unsigned n, unsigned state, unsigned char *cls, unsigned ncls);

/* the state at the start of line, from the nearest mark with a known one; past budget bytes of tokenizing it stops
   there (having kept what it learned) and gives the best it has, with *exact 0 */
unsigned wc_syn_state(struct wc_text *t, int lang, unsigned line, unsigned long budget, int *exact);
/* known states further on, budget bytes of tokenizing at most: 1 when more is left to do */
int wc_syn_idle(struct wc_text *t, int lang, unsigned long budget);
void wc_syn_reset(struct wc_text *t);                       /* another language: every state is stale */
/* the classes of the line l (its first ncls bytes) and the state after it, from its start state */
unsigned wc_syn_classes(struct wc_text *t, int lang, const struct wc_line *l, unsigned state, unsigned char *cls,
                        unsigned ncls);

/* the bracket at pos (or else right before it) and its partner, brackets in comments and strings left out, within
   max_lines lines and max_bytes bytes: 1 with *at the bracket and *other its partner; 0 for none (*at is
   WC_NONE when no bracket is there) */
#define WC_NONE 0xFFFFFFFFu
int wc_syn_match(struct wc_text *t, int lang, unsigned pos, unsigned *at, unsigned *other, unsigned max_lines,
                 unsigned long max_bytes);

#endif
