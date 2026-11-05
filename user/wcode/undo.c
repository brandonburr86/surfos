/*
SurfOS user program: wcode
--------------------
File: undo.c    Date: 11/05/26
--------------------
The Code Editor's history (undo.h). A step is an array of ops, each the bytes a piece of a change
put in or took out, with the bytes kept one after another in the step's blob; a run of typing
adds its bytes to its last op, so typing a line is one op and not one a key. Memory is counted
as the blocks are sized; past WC_UNDO_BYTES the oldest steps go, and when the step being made is
itself too large, or memory runs out, the history is forgotten and the change carries on without
it: a history with a hole in it would undo into a text that never was.
*/
#include <stdlib.h>
#include <string.h>
#include "undo.h"

static unsigned long step_bytes(const struct wc_step *s) {
    return sizeof(struct wc_step) + (unsigned long)s->opcap * sizeof(struct wc_op) + s->bcap;
}

static void free_step(struct wc_undo *u, struct wc_step *s) {
    u->bytes -= step_bytes(s);
    free(s->ops);
    free(s->blob);
    memset(s, 0, sizeof(*s));
}

void wc_undo_init(struct wc_undo *u) {
    memset(u, 0, sizeof(*u));
}

void wc_undo_free(struct wc_undo *u) {
    while(u->n) free_step(u, &u->s[--u->n]);
    free(u->s);
    memset(u, 0, sizeof(*u));
}

/* nothing left to undo or redo; the text as it is gets a number no undo gives back */
void wc_undo_forget(struct wc_undo *u) {
    while(u->n) free_step(u, &u->s[--u->n]);
    u->cur = 0;
    u->open = 0;
    u->base = ++u->serials;
}

unsigned wc_undo_serial(const struct wc_undo *u) {
    return u->cur ? u->s[u->cur - 1].serial : u->base;
}

unsigned wc_undo_can(const struct wc_undo *u, int redo) {
    return redo ? u->n - u->cur : u->cur;
}

/* the oldest step goes (it is never the one being made): undo stops at the text after it */
static void drop_oldest(struct wc_undo *u) {
    u->base = u->s[0].serial;
    free_step(u, &u->s[0]);
    memmove(u->s, u->s + 1, (u->n - 1) * sizeof(struct wc_step));
    u->n--;
    u->cur--;
    memset(&u->s[u->n], 0, sizeof(struct wc_step));
}

static void give_up(struct wc_undo *u) {
    wc_undo_forget(u);
    u->forgot = 1;
}

void wc_undo_begin(struct wc_undo *u, int kind, unsigned anchor, unsigned caret) {
    struct wc_step *s, *ns;
    unsigned cap;
    u->forgot = 0;
    if(u->open && kind && u->n && u->cur == u->n) {
        s = &u->s[u->n - 1];
        if(s->kind == kind && s->c1 == caret && s->a1 == anchor) return;     /* the run goes on */
    }
    u->open = 0;
    while(u->n > u->cur) free_step(u, &u->s[--u->n]);      /* what was undone cannot be done again */
    if(u->n == WC_UNDO_STEPS) drop_oldest(u);
    if(u->n == u->cap) {
        cap = u->cap ? u->cap * 2 : 64;
        if(cap > WC_UNDO_STEPS) cap = WC_UNDO_STEPS;
        ns = realloc(u->s, cap * sizeof(struct wc_step));
        if(!ns) {
            give_up(u);
            return;
        }
        u->s = ns;
        u->cap = cap;
    }
    s = &u->s[u->n++];
    u->cur = u->n;
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->a0 = s->a1 = anchor;
    s->c0 = s->c1 = caret;
    s->serial = ++u->serials;
    u->bytes += step_bytes(s);
    u->open = 1;
}

/* room in the open step for n more bytes, and an op more when op: 0, or -1 with the history forgotten */
static int room(struct wc_undo *u, struct wc_step *s, unsigned n, int op) {
    unsigned long grow = 0, before = step_bytes(s);
    unsigned bcap = s->bcap, opcap = s->opcap;
    char *b;
    struct wc_op *o;
    if((unsigned long)s->used + n > WC_UNDO_BYTES) {
        give_up(u);
        return -1;
    }
    while(s->used + n > bcap) bcap = bcap ? bcap * 2 : 64;
    if(bcap > WC_UNDO_BYTES) bcap = WC_UNDO_BYTES;
    if(op && s->nops == opcap) opcap = opcap ? opcap * 2 : 4;
    grow = (unsigned long)(bcap - s->bcap) + (unsigned long)(opcap - s->opcap) * sizeof(struct wc_op);
    while(u->bytes + grow > WC_UNDO_BYTES && u->n > 1) drop_oldest(u);
    if(u->bytes + grow > WC_UNDO_BYTES) {
        give_up(u);
        return -1;
    }
    s = &u->s[u->n - 1];                        /* the steps may have moved down */
    if(bcap != s->bcap) {
        b = realloc(s->blob, bcap);
        if(!b) {
            give_up(u);
            return -1;
        }
        s->blob = b;
        s->bcap = bcap;
    }
    if(opcap != s->opcap) {
        o = realloc(s->ops, opcap * sizeof(struct wc_op));
        if(!o) {
            give_up(u);
            return -1;
        }
        s->ops = o;
        s->opcap = opcap;
    }
    u->bytes += step_bytes(s) - before;
    return 0;
}

int wc_undo_op(struct wc_undo *u, int ins, unsigned pos, const char *bytes, unsigned n) {
    struct wc_step *s;
    struct wc_op *last;
    if(!u->open || !u->n || u->cur != u->n) return -1;     /* no step: forgotten during this change */
    s = &u->s[u->n - 1];
    last = s->nops ? &s->ops[s->nops - 1] : NULL;
    if(last && n && last->ins == ins &&
       (ins ? last->pos + last->n == pos : (pos + n == last->pos || pos == last->pos))) {
        if(room(u, s, n, 0) < 0) return -1;     /* typing at its end, Backspace before it, Delete where it was */
        s = &u->s[u->n - 1];
        last = &s->ops[s->nops - 1];
        if(!ins && pos + n == last->pos) {      /* the bytes Backspace took come before those it took already */
            memmove(s->blob + last->off + n, s->blob + last->off, last->n);
            memcpy(s->blob + last->off, bytes, n);
            last->pos = pos;
        } else memcpy(s->blob + last->off + last->n, bytes, n);
        last->n += n;
        s->used += n;
        return 0;
    }
    if(room(u, s, n, 1) < 0) return -1;
    s = &u->s[u->n - 1];
    last = &s->ops[s->nops++];
    last->ins = (unsigned char)(ins != 0);
    last->pos = pos;
    last->n = n;
    last->off = s->used;
    if(n) memcpy(s->blob + s->used, bytes, n);
    s->used += n;
    return 0;
}

void wc_undo_end(struct wc_undo *u, unsigned anchor, unsigned caret) {
    struct wc_step *s;
    if(!u->open || !u->n || u->cur != u->n) return;
    s = &u->s[u->n - 1];
    if(!s->nops) {                              /* nothing changed after all: no step */
        free_step(u, s);
        u->n--;
        u->cur--;
        u->open = 0;
        return;
    }
    s->a1 = anchor;
    s->c1 = caret;
}

void wc_undo_close(struct wc_undo *u) {
    u->open = 0;
}

const struct wc_step *wc_undo_back(struct wc_undo *u) {
    u->open = 0;
    return u->cur ? &u->s[--u->cur] : NULL;
}

const struct wc_step *wc_undo_forward(struct wc_undo *u) {
    u->open = 0;
    return u->cur < u->n ? &u->s[u->cur++] : NULL;
}
