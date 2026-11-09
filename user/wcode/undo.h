/*
SurfOS user program: wcode
--------------------
File: undo.h    Date: 11/05/26
--------------------
The Code Editor's history (undo.c): every change of the text is kept as steps of bytes put in and
taken out, so that Ctrl+Z undoes a step and Ctrl+Y does it again. A step is what a person thinks
of as one change: a run of typing, of Backspace or of Delete, an Enter with its indentation, a
paste, a cut, a replacement, Replace all, an indentation of several lines. The selection before
and after each step is kept with it. doc.c decides the kinds and when a run ends.

The text after each step is numbered (edit's way, user/bin/edit.c), so that undoing back to the
text as it was saved shows it unmodified again. The history holds at most WC_UNDO_STEPS steps and
WC_UNDO_BYTES of memory; past either the oldest steps go, and one change larger than all of it
(Replace all over a large file, say) forgets the whole history rather than keep part of it.
*/
#ifndef _WCODE_UNDO_H
#define _WCODE_UNDO_H

#define WC_UNDO_STEPS 4096
#define WC_UNDO_BYTES (3u << 20)

struct wc_op { unsigned pos, n, off; unsigned char ins; };  /* n bytes at pos put in (ins) or taken out; blob + off */

struct wc_step {
    struct wc_op *ops;
    unsigned nops, opcap;
    char *blob;                         /* the ops' bytes, one after another */
    unsigned used, bcap;
    unsigned a0, c0, a1, c1;            /* the selection, anchor and caret, before and after */
    unsigned serial;                    /* the text after the step, as a number */
    int kind;                           /* what made it (doc.c's WC_K_*): runs of a kind go together */
};

struct wc_undo {
    struct wc_step *s;                  /* [0, cur) can be undone, [cur, n) done again */
    unsigned n, cur, cap;
    unsigned serials;                   /* numbers given out */
    unsigned base;                      /* the text with nothing left to undo, as a number */
    unsigned long bytes;                /* the memory the steps hold */
    int open;                           /* the last step takes the next change of its kind */
    int forgot;                         /* the last change forgot the history: too large, or no memory */
};

void wc_undo_init(struct wc_undo *u);
void wc_undo_free(struct wc_undo *u);
void wc_undo_forget(struct wc_undo *u);
unsigned wc_undo_serial(const struct wc_undo *u);           /* the text as it is now */

/* a change of this kind begins, the selection as it is: it goes on the open step when that is of the same kind and
   ended where this one begins, otherwise it starts a step (what was undone can no longer be done again) */
void wc_undo_begin(struct wc_undo *u, int kind, unsigned anchor, unsigned caret);
/* each piece of the change, in order: 0, or -1 when the history was forgotten (u->forgot) */
int wc_undo_op(struct wc_undo *u, int ins, unsigned pos, const char *s, unsigned n);
void wc_undo_end(struct wc_undo *u, unsigned anchor, unsigned caret);  /* the selection after it */
void wc_undo_close(struct wc_undo *u);                      /* the next change is a step of its own */

/* the step to undo (cur goes back) or to do again (cur goes on), NULL for none; doc.c replays its ops */
const struct wc_step *wc_undo_back(struct wc_undo *u);
const struct wc_step *wc_undo_forward(struct wc_undo *u);
unsigned wc_undo_can(const struct wc_undo *u, int redo);    /* how many steps */
/* 11/09/26: whether one step of ops ops holding bytes of text would be kept (room()'s rules), so that a change can be
   asked about before it is made when it would be too large to undo */
int wc_undo_fits(unsigned long bytes, unsigned long ops);

#endif
