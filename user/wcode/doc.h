/*
SurfOS user program: wcode
--------------------
File: doc.h     Date: 11/05/26
--------------------
A file open in the Code Editor (doc.c): its text, its history, its selection, what it was read with
(line ends, indentation, language) and what moving and editing do to it. Every command here is
what a key or a menu item asks; main.c only says which. Nothing here makes a system call or draws.

The selection runs between anchor and caret, empty when they meet. Up and Down keep to the column
they started from. Line ends are kept as the file had them: a new one (Enter, a paste) is the
file's own kind, CR LF when most of its lines end so, LF otherwise. The clipboard's text has LF
line ends; a paste gets the file's.

Indentation: a file indented with tabs is indented with tabs, one indented with spaces with as
many as its steps are (2, 4 or 8, the commonest), a Makefile with tabs always, anything else with
four spaces. Enter keeps the line's indentation, and adds a step after an opening bracket (and
after Python's colon, a shell's then, do, else and {, a Makefile's rule); a line left with nothing
but its indentation loses it. Typing a closing brace on a line of its own takes the line back to
the indentation of the line its opening brace is on. Backspace in the spaces of an indentation
takes a step back. Tab and Shift+Tab indent and take back every line of a selection.
*/
#ifndef _WCODE_DOC_H
#define _WCODE_DOC_H

#include "text.h"
#include "undo.h"
#include "syntax.h"

#define WC_PATH     256                 /* a path with its NUL: the kernel's limit (255 bytes) */
#define WC_TABW     8                   /* a tab reaches the next multiple of 8 columns, as in edit and Notepad */
#define WC_MSG      160
#define WC_LINE_MAX 4096                /* an indentation or a line comment's room looked at, at most */

enum { WC_EOL_NONE, WC_EOL_LF, WC_EOL_CRLF, WC_EOL_MIXED };     /* the line ends a file came with */

/* the kinds of changes, for the history: runs of the same kind are one step */
enum { WC_K_OTHER, WC_K_TYPE, WC_K_BACK, WC_K_DEL };

/* moving the caret */
enum { WC_LEFT, WC_RIGHT, WC_WORD_LEFT, WC_WORD_RIGHT, WC_UP, WC_DOWN, WC_PGUP, WC_PGDN, WC_HOME, WC_END,
       WC_DOC_START, WC_DOC_END };

struct wc_doc {
    struct wc_text t;
    struct wc_undo u;
    unsigned anchor, caret;
    int want_col;                       /* the column Up and Down keep to, -1 for the caret's own */
    char path[WC_PATH];                 /* "" for a new file */
    unsigned untitled;                  /* its number when it has no path: "Untitled 2" */
    int eols;                           /* the line ends it came with */
    int crlf;                           /* new line ends are CR LF */
    int lang;
    int indent_tab;                     /* indent with a tab */
    int indent;                         /* or with this many spaces */
    unsigned saved;                     /* the history's number of the text as it was last saved or read */
    /* edit's care after a rename that may have half happened (user/bin/edit.c): the file and the new one beside it
       may share their data, so nothing is written over it until the window closes; each save puts a copy beside */
    int unsure;
    char copied[WC_PATH];               /* the last such copy */
    unsigned copied_serial;             /* the text it holds */
    char twin[WC_PATH];                 /* the name beside the file found to look like its other half */
    unsigned top, left;                 /* the view: the first line and the first column shown */
    char msg[WC_MSG];                   /* for the status bar, until the next key */
    /* 11/09/26 (the review, H1): the file as it was read or last saved (its size, time and number in its file
       system), so that a save over it that changed since (another wcode, Notepad) asks first; disk_changed: the
       last save was refused for that; overwrite: the next save goes over it all the same (the person said so) */
    int disk_known, disk_changed, overwrite;
    unsigned disk_size, disk_mtime, disk_ino;
};

int wc_doc_init(struct wc_doc *d, struct wc_pool *pool);     /* an empty new file; -1 without memory */
void wc_doc_free(struct wc_doc *d);
/* after its bytes were read in (wc_text_load_*): the line ends, the indentation and the language it came with; 0, or
   WC_NOT_TEXT for a file with a NUL byte, which is no text (Notepad's rule too) */
#define WC_NOT_TEXT (-2)
int wc_doc_loaded(struct wc_doc *d, const char *path);
void wc_doc_set_lang(struct wc_doc *d, int lang);
int wc_doc_modified(const struct wc_doc *d);
int wc_doc_safe(const struct wc_doc *d);                     /* saved, or the text is in the last copy beside it */
void wc_doc_saved(struct wc_doc *d);                         /* the text as it is now is the file's */
void wc_doc_title(const struct wc_doc *d, char *out, unsigned size);    /* its name, or "Untitled N" */

/* the selection */
unsigned wc_doc_sel(const struct wc_doc *d, unsigned *from, unsigned *to);  /* its bytes, from <= to */
void wc_doc_select(struct wc_doc *d, unsigned anchor, unsigned caret);
void wc_doc_select_all(struct wc_doc *d);
void wc_doc_select_word(struct wc_doc *d, unsigned pos);
void wc_doc_select_line(struct wc_doc *d, unsigned line, int extend);       /* a line with its line end */

/* moving (extend: the selection grows, Shift); page: the rows a page has */
void wc_doc_move(struct wc_doc *d, int how, int extend, unsigned page);
int wc_doc_goto(struct wc_doc *d, unsigned line, unsigned col);   /* both from 1, col 0 for the line's start */
int wc_doc_bracket(struct wc_doc *d, int extend);                 /* to the matching bracket: 1, or 0 */
unsigned wc_doc_line(struct wc_doc *d);                           /* the caret's line, from 0 */
unsigned wc_doc_col(struct wc_doc *d);                            /* its column, from 0 */

/* editing: 0, or -1 with d->msg saying why (no memory, a file too large) and the text as it was */
int wc_doc_type(struct wc_doc *d, const char *s, unsigned n);     /* typed: UTF-8, no line end */
int wc_doc_enter(struct wc_doc *d);
int wc_doc_tab(struct wc_doc *d, int back);
int wc_doc_indent(struct wc_doc *d, int back);                    /* the selection's lines (or the caret's) a step */
int wc_doc_backspace(struct wc_doc *d, int word);
int wc_doc_delete(struct wc_doc *d, int word);
int wc_doc_paste(struct wc_doc *d, const char *s, unsigned n);    /* to a NUL, line ends made the file's */
int wc_doc_erase(struct wc_doc *d);                               /* the selection goes (Cut after Copy) */
int wc_doc_comment(struct wc_doc *d);                             /* line comments on the lines, or off */
int wc_doc_undo(struct wc_doc *d, int redo);
/* the general change, a step of its own: [from, to) becomes s[0..n), the selection then on what went in (select)
   or after it */
int wc_doc_replace(struct wc_doc *d, unsigned from, unsigned to, const char *s, unsigned n, int select);
/* the selection's text with LF line ends into out: its length; 0 for none, or when it is larger than size - 1
   (d->msg says so) */
unsigned wc_doc_copy(struct wc_doc *d, char *out, unsigned size);

/* 11/09/26 (the review, H1 and H2): an absolute path in its one form, without empty, . and .. parts (.. above / is
   /) or a / at its end: 0, or -1 for one that is not absolute or does not fit size; and whether two such name one
   file as FAT names it, case aside (the files a person edits are on FAT; the initrd is read-only) */
int wc_path_canon(const char *in, char *out, unsigned size);
int wc_path_same(const char *a, const char *b);

/* for the changes find.c makes: one step of several pieces; 11/09/26, room first for what it comes to at most
   (wc_doc_room(): 0, or -1 with d->msg and nothing done), so that no put fails after a cut (the review, C1) */
int wc_doc_room(struct wc_doc *d, unsigned long len);
void wc_doc_begin(struct wc_doc *d, int kind);
int wc_doc_put(struct wc_doc *d, unsigned pos, const char *s, unsigned n);
void wc_doc_cut(struct wc_doc *d, unsigned pos, unsigned n);
void wc_doc_end(struct wc_doc *d);

#endif
