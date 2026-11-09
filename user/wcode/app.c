/*
SurfOS user program: wcode
--------------------
File: app.c     Date: 11/05/26
--------------------
The Code Editor's tabs and what reaches them (app.h). Each call clears the last message, does
what was asked of the file in front, keeps the caret in view, and says in WC_DO_* bits what the
window must do next. The guard over unsaved changes is a small state machine: closing a tab, or
everything (quitting), asks about one file at a time (the tab comes to the front), and each
answer either goes on to the next or ends it; a file never saved goes through the Save as
chooser first. Nothing is closed by a Don't save until every question is answered, so a Cancel
at the third file loses nothing of the first two.

The keys are the desktop's (surfos/win.h, 11/05/26): a Ctrl+letter is its control byte with Ctrl's bit, Tab, Enter,
Esc and Backspace are 9, 13, 27 and 127 and with Ctrl keys of their own (Ctrl+Backspace deletes a word, Ctrl+H is
no Backspace), and every key carries its modifiers' bits, which are tested one at a time. The window's menus take
their shortcuts before a key reaches here (gk.h); the Ctrl+letters below are for a window without them.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

#ifdef WC_APP_MALLOC                            /* 11/09/26: a test's, which fails on request (tests/host/test_wcode.c) */
void *WC_APP_MALLOC(size_t n);
#else
#define WC_APP_MALLOC malloc
#endif

#define CTRL(c) ((c) & 0x1f)

int wc_app_init(struct wc_app *a, const struct wc_app_io *io, const struct wc_fsops *fs) {
    memset(a, 0, sizeof(*a));
    a->pool.limit = WC_POOL_MAX;
    a->io = io;
    a->fs = fs;
    a->asking = -1;
    a->rows = 24;
    a->cols = 80;
    a->untitled = 1;
    wc_find_init(&a->find);
    wc_grep_init(&a->grep);
    wc_view_init(&a->view);
    a->docs[0] = malloc(sizeof(struct wc_doc));
    if(!a->docs[0] || wc_doc_init(a->docs[0], &a->pool) < 0) {
        free(a->docs[0]);
        return -1;
    }
    a->docs[0]->untitled = a->untitled++;
    a->n = 1;
    return 0;
}

void wc_app_free(struct wc_app *a) {
    int i;
    for(i = 0; i < a->n; i++) {
        wc_doc_free(a->docs[i]);
        free(a->docs[i]);
    }
    a->n = 0;
    wc_find_free(&a->find);
    wc_grep_free(&a->grep);
}

struct wc_doc *wc_app_doc(struct wc_app *a) {
    return a->docs[a->cur];
}

void wc_app_size(struct wc_app *a, unsigned rows, unsigned cols) {
    a->rows = rows ? rows : 1;
    a->cols = cols ? cols : 1;
    wc_view_follow(wc_app_doc(a), a->rows, a->cols);
}

int wc_app_unsafe(const struct wc_app *a) {
    int i, n = 0;
    for(i = 0; i < a->n; i++) n += !wc_doc_safe(a->docs[i]);
    return n;
}

/* a new file nobody has touched: an Open takes its place */
static int pristine(const struct wc_doc *d) {
    return !d->path[0] && !d->t.len && !wc_doc_modified(d);
}

static int follow(struct wc_app *a) {
    wc_view_follow(wc_app_doc(a), a->rows, a->cols);
    return WC_DO_DRAW;
}

static int new_tab(struct wc_app *a) {
    struct wc_doc *d;
    if(a->n == WC_TABS) {
        snprintf(a->status, sizeof(a->status), "%d files are open: close one first", WC_TABS);
        return WC_DO_DRAW;
    }
    d = WC_APP_MALLOC(sizeof(*d));
    if(!d || wc_doc_init(d, &a->pool) < 0) {
        free(d);
        snprintf(a->status, sizeof(a->status), "There is not enough memory for another file");
        return WC_DO_DRAW;
    }
    d->untitled = a->untitled++;
    a->docs[a->n] = d;
    a->cur = a->n++;
    return WC_DO_DRAW | WC_DO_TITLE;
}

/* tab i goes; there is always one, so the last gives its place to a new file, made first (11/09/26, the review: when
   there is no memory for it, the tab stays and the status says why): 1 gone, 0 kept */
static int drop_tab(struct wc_app *a, int i) {
    if(a->n == 1 && !(new_tab(a) & WC_DO_TITLE)) return 0;
    wc_doc_free(a->docs[i]);
    free(a->docs[i]);
    memmove(a->docs + i, a->docs + i + 1, (size_t)(a->n - i - 1) * sizeof(a->docs[0]));
    memmove(a->discard + i, a->discard + i + 1, (size_t)(a->n - i - 1) * sizeof(a->discard[0]));
    a->n--;
    a->discard[a->n] = 0;
    if(a->cur > i || a->cur >= a->n) a->cur = a->cur > 0 ? a->cur - 1 : 0;
    return 1;
}

/**** the guard ****/

/* the next file a quit asks about, or the end */
static int ask_next(struct wc_app *a) {
    int i;
    for(i = 0; i < a->n; i++)
        if(!a->discard[i] && !wc_doc_safe(a->docs[i])) {
            a->asking = a->cur = i;
            return WC_DO_ASK | WC_DO_DRAW | WC_DO_TITLE;
        }
    a->closing = WC_CLOSE_NONE;
    a->asking = -1;
    return WC_DO_QUIT;
}

static int close_tab(struct wc_app *a, int i) {
    if(wc_doc_safe(a->docs[i])) {
        drop_tab(a, i);
        return follow(a) | WC_DO_TITLE | WC_DO_DRAW;
    }
    a->closing = WC_CLOSE_TAB;
    a->asking = a->cur = i;
    return WC_DO_ASK | WC_DO_DRAW | WC_DO_TITLE;
}

/* the file asked about is dealt with: on to what comes next */
static int proceed(struct wc_app *a) {
    int i = a->asking;
    if(a->closing == WC_CLOSE_TAB) {
        a->closing = WC_CLOSE_NONE;
        a->asking = -1;
        drop_tab(a, i);
        return follow(a) | WC_DO_TITLE;
    }
    a->discard[i] = 1;
    return ask_next(a);
}

static int stop(struct wc_app *a) {
    a->closing = WC_CLOSE_NONE;
    a->asking = -1;
    a->after_save_as = 0;
    memset(a->discard, 0, sizeof(a->discard));
    return WC_DO_DRAW | WC_DO_TITLE;
}

int wc_app_answer(struct wc_app *a, int answer) {
    struct wc_doc *d;
    if(a->asking < 0) return 0;
    d = a->docs[a->asking];
    if(answer == WC_ANSWER_CANCEL) return stop(a);
    if(answer == WC_ANSWER_DISCARD) return proceed(a);
    if(!d->path[0]) {                           /* never saved: its name first */
        a->after_save_as = 1;
        return WC_DO_SAVE_AS;
    }
    if(a->io->save(d, NULL) < 0) {              /* not saved: everything stays open, the message says why */
        if(d->disk_changed) return WC_DO_DISK | WC_DO_DRAW;    /* 11/09/26: asked first; the guard waits for it */
        snprintf(a->status, sizeof(a->status), "%s", d->msg);
        return stop(a);
    }
    return proceed(a);
}

int wc_app_disk(struct wc_app *a, int answer) {
    struct wc_doc *d = wc_app_doc(a);
    int guard = a->asking >= 0;
    d->disk_changed = 0;
    if(answer == WC_DISK_SAVE_AS) {
        a->after_save_as = guard;
        return WC_DO_SAVE_AS;
    }
    if(answer != WC_DISK_OVER) {
        snprintf(d->msg, sizeof(d->msg), "Not saved: the file on the disk stays as it is");
        return guard ? stop(a) : WC_DO_DRAW;
    }
    d->overwrite = 1;
    if(a->io->save(d, NULL) < 0) {
        if(guard) {
            snprintf(a->status, sizeof(a->status), "%s", d->msg);
            return stop(a);
        }
        return WC_DO_DRAW | WC_DO_TITLE;
    }
    return guard ? proceed(a) : WC_DO_DRAW | WC_DO_TITLE;
}

int wc_app_asking(struct wc_app *a, char *question, unsigned size) {
    char name[WC_PATH];
    if(a->asking < 0) return -1;
    wc_doc_title(a->docs[a->asking], name, sizeof(name));
    snprintf(question, size, "Save the changes to %s?", name);
    return a->asking;
}

int wc_app_save_as(struct wc_app *a, const char *path) {
    struct wc_doc *d = a->after_save_as && a->asking >= 0 ? a->docs[a->asking] : wc_app_doc(a);
    char full[WC_PATH];
    int guard = a->after_save_as, r, i;
    a->after_save_as = 0;
    if(!path || !path[0]) return guard ? stop(a) : WC_DO_DRAW;
    if(wc_app_path(a, path, full, sizeof(full)) < 0) {
        snprintf(d->msg, sizeof(d->msg), "Not saved: the name is too long");
        return guard ? stop(a) : WC_DO_DRAW;
    }
    for(i = 0; i < a->n; i++)                   /* 11/09/26: two tabs of one file would save over each other */
        if(a->docs[i] != d && wc_path_same(a->docs[i]->path, full)) {
            snprintf(d->msg, sizeof(d->msg), "Not saved: %.100s is open in another tab", full);
            return guard ? stop(a) : WC_DO_DRAW;
        }
    path = full;
    r = a->io->save(d, path);
    if(guard) {
        if(r < 0) {
            snprintf(a->status, sizeof(a->status), "%s", d->msg);
            return stop(a);
        }
        return proceed(a);
    }
    return WC_DO_DRAW | WC_DO_TITLE;
}

/**** opening ****/

int wc_app_path(const struct wc_app *a, const char *path, char *out, unsigned size) {
    char joined[2 * WC_PATH];
    if(path[0] == '/') return wc_path_canon(path, out, size);
    snprintf(joined, sizeof(joined), "%s/%s", a->cwd[0] ? a->cwd : "", path);
    return wc_path_canon(joined, out, size);
}

int wc_app_open(struct wc_app *a, const char *path) {
    struct wc_doc *d;
    char full[WC_PATH];
    int i;
    if(!path || !path[0]) return WC_DO_DRAW;
    if(wc_app_path(a, path, full, sizeof(full)) < 0) {
        snprintf(a->status, sizeof(a->status), "Cannot open %.100s: the name is too long", path);
        return WC_DO_DRAW;
    }
    path = full;                                /* 11/09/26 (the review, H1): one form, compared as FAT compares */
    for(i = 0; i < a->n; i++)
        if(wc_path_same(a->docs[i]->path, path)) {      /* open already: to its tab */
            a->cur = i;
            return follow(a) | WC_DO_TITLE;
        }
    if(a->n == WC_TABS && !pristine(wc_app_doc(a))) {
        snprintf(a->status, sizeof(a->status), "%d files are open: close one first", WC_TABS);
        return WC_DO_DRAW;
    }
    d = malloc(sizeof(*d));
    if(!d || wc_doc_init(d, &a->pool) < 0) {
        free(d);
        snprintf(a->status, sizeof(a->status), "There is not enough memory for another file");
        return WC_DO_DRAW;
    }
    if(a->io->read(d, path) < 0) {
        snprintf(a->status, sizeof(a->status), "%s", d->msg);
        wc_doc_free(d);
        free(d);
        return WC_DO_DRAW;
    }
    if(d->msg[0]) snprintf(a->status, sizeof(a->status), "%s", d->msg);    /* (an earlier refusal stays said) */
    if(pristine(wc_app_doc(a))) {               /* in place of the untouched new file */
        wc_doc_free(a->docs[a->cur]);
        free(a->docs[a->cur]);
        a->docs[a->cur] = d;
    } else {
        a->docs[a->n] = d;
        a->cur = a->n++;
    }
    return follow(a) | WC_DO_TITLE;
}

int wc_app_new_file(struct wc_app *a, const char *path) {
    struct wc_doc *d;
    char full[WC_PATH];
    int i;
    if(!path || !path[0]) return WC_DO_DRAW;
    if(wc_app_path(a, path, full, sizeof(full)) < 0) {
        snprintf(a->status, sizeof(a->status), "Cannot open %.100s: the name is too long", path);
        return WC_DO_DRAW;
    }
    path = full;
    for(i = 0; i < a->n; i++)
        if(wc_path_same(a->docs[i]->path, path)) {      /* open already: to its tab */
            a->cur = i;
            return follow(a) | WC_DO_TITLE;
        }
    if(!pristine(wc_app_doc(a)) && !(new_tab(a) & WC_DO_TITLE)) return WC_DO_DRAW;    /* no tab free: said */
    d = wc_app_doc(a);
    snprintf(d->path, sizeof(d->path), "%s", path);
    wc_doc_set_lang(d, wc_lang_guess(path, NULL, 0));
    snprintf(a->status, sizeof(a->status), "A new file: it is made when it is saved");
    return WC_DO_DRAW | WC_DO_TITLE;
}

/**** commands ****/

void wc_app_find_seed(struct wc_app *a) {
    struct wc_doc *d = wc_app_doc(a);
    unsigned from, to, n = wc_doc_sel(d, &from, &to);
    if(!n || n >= WC_FIND_MAX || wc_text_count_nl(&d->t, from, to)) return;
    wc_text_get(&d->t, from, a->find.what, n);
    a->find.what[n] = 0;
}

/**** searches going on between events (11/09/26, the review, C3) ****/

int wc_app_busy(const struct wc_app *a) {
    return a->job == WC_JOB_ALL && wc_find_all_changing(&a->find);
}

/* a step of the search going on: more to do (it shows), or its end */
static int run_job(struct wc_app *a, int job) {
    struct wc_doc *d = wc_app_doc(a);
    unsigned long step = a->step ? a->step : WC_FIND_STEP;
    int r = job == WC_JOB_FIND ? wc_find_doc_go(&a->find, d, step) : wc_find_all_go(&a->find, d, step);
    if(r == 2) {
        a->job = job;
        snprintf(a->status, sizeof(a->status), "%s: Esc stops it", job == WC_JOB_FIND ? "Searching" :
                 wc_find_all_changing(&a->find) ? "Replacing" : "Counting the matches");
        return WC_DO_DRAW | WC_DO_IDLE;
    }
    a->job = WC_JOB_NONE;
    a->status[0] = 0;
    return follow(a) | WC_DO_TITLE;
}

int wc_app_interrupt(struct wc_app *a, int esc) {
    struct wc_doc *d = wc_app_doc(a);
    if(!a->job) return 0;
    if(wc_app_busy(a)) {
        if(!esc) {
            snprintf(a->status, sizeof(a->status), "Replacing: wait for it, or Esc stops it");
            return -1;
        }
        if(!wc_find_all_stop(&a->find, d)) {
            snprintf(a->status, sizeof(a->status), "Replacing goes on: too large a change to undo, it cannot stop");
            return -1;
        }
    } else if(a->job == WC_JOB_ALL) wc_find_all_stop(&a->find, d);     /* counting: nothing changed */
    else {
        wc_find_stop(&a->find);
        snprintf(d->msg, sizeof(d->msg), "Stopped searching");
    }
    a->job = WC_JOB_NONE;
    a->status[0] = 0;
    return follow(a) | WC_DO_TITLE;
}

/* the job going on stops before another command; 1 when it cannot (the command waits) */
static int stop_job(struct wc_app *a) {
    return a->job && wc_app_interrupt(a, 0) < 0;
}

static int find(struct wc_app *a, int backward) {
    if(!a->find.what[0]) return WC_DO_FIND | WC_DO_DRAW;
    if(wc_find_doc_begin(&a->find, wc_app_doc(a), backward) < 0) return WC_DO_DRAW;
    return run_job(a, WC_JOB_FIND);
}

int wc_app_cmd(struct wc_app *a, int cmd) {
    struct wc_doc *d;
    unsigned from, to;
    if(stop_job(a)) return WC_DO_DRAW | WC_DO_IDLE;     /* Replace all changing the file: the command waits */
    d = wc_app_doc(a);
    a->status[0] = 0;
    d->msg[0] = 0;
    switch(cmd) {
    case WC_CMD_NEW: return new_tab(a);
    case WC_CMD_OPEN: return WC_DO_OPEN;
    case WC_CMD_SAVE:
        if(!d->path[0]) return WC_DO_SAVE_AS;
        if(a->io->save(d, NULL) < 0 && d->disk_changed) return WC_DO_DISK | WC_DO_DRAW;   /* changed since: asked */
        return WC_DO_DRAW | WC_DO_TITLE;
    case WC_CMD_SAVE_AS: return WC_DO_SAVE_AS;
    case WC_CMD_CLOSE: return close_tab(a, a->cur);
    case WC_CMD_QUIT:
        a->closing = WC_CLOSE_ALL;
        memset(a->discard, 0, sizeof(a->discard));
        return ask_next(a);
    case WC_CMD_UNDO: wc_doc_undo(d, 0); return follow(a) | WC_DO_TITLE;
    case WC_CMD_REDO: wc_doc_undo(d, 1); return follow(a) | WC_DO_TITLE;
    case WC_CMD_CUT: case WC_CMD_COPY:
        if(!wc_doc_sel(d, &from, &to)) {
            snprintf(a->status, sizeof(a->status), "Nothing is selected");
            return WC_DO_DRAW;
        }
        a->cut = cmd == WC_CMD_CUT;
        return WC_DO_COPY;
    case WC_CMD_PASTE: return WC_DO_PASTE;
    case WC_CMD_DELETE: wc_doc_delete(d, 0); return follow(a) | WC_DO_TITLE;
    case WC_CMD_SELECT_ALL: wc_doc_select_all(d); return WC_DO_DRAW;
    case WC_CMD_FIND: wc_app_find_seed(a); return WC_DO_FIND | WC_DO_DRAW;
    case WC_CMD_REPLACE: wc_app_find_seed(a); return WC_DO_REPLACE | WC_DO_DRAW;
    case WC_CMD_FIND_NEXT: return find(a, 0);
    case WC_CMD_FIND_PREV: return find(a, 1);
    case WC_CMD_REPLACE_ONE:
        if(wc_find_replace_sel(&a->find, d) < 0) return WC_DO_DRAW;
        if(wc_find_doc_begin(&a->find, d, 0) < 0) return follow(a) | WC_DO_TITLE;
        return run_job(a, WC_JOB_FIND);
    case WC_CMD_REPLACE_ALL:
        if(wc_find_all_begin(&a->find, d) < 0) return WC_DO_DRAW;
        return run_job(a, WC_JOB_ALL);
    case WC_CMD_FIND_FILES:
        if(!a->grep.f.what[0]) snprintf(a->grep.f.what, sizeof(a->grep.f.what), "%s", a->find.what);
        return WC_DO_FILES | WC_DO_DRAW;
    case WC_CMD_GOTO: return WC_DO_GOTO;
    case WC_CMD_BRACKET: wc_doc_bracket(d, 0); return follow(a);
    case WC_CMD_COMMENT: wc_doc_comment(d); return follow(a) | WC_DO_TITLE;
    case WC_CMD_INDENT: case WC_CMD_OUTDENT: wc_doc_indent(d, cmd == WC_CMD_OUTDENT); return follow(a) | WC_DO_TITLE;
    case WC_CMD_NEXT_TAB: a->cur = (a->cur + 1) % a->n; return WC_DO_DRAW | WC_DO_TITLE;
    case WC_CMD_PREV_TAB: a->cur = (a->cur + a->n - 1) % a->n; return WC_DO_DRAW | WC_DO_TITLE;
    }
    if(cmd >= WC_CMD_LANG && cmd < WC_CMD_LANG + WC_LANGS) {
        wc_doc_set_lang(d, cmd - WC_CMD_LANG);
        return WC_DO_DRAW;
    }
    if(cmd >= WC_CMD_TAB && cmd < WC_CMD_TAB + a->n) {
        a->cur = cmd - WC_CMD_TAB;
        return WC_DO_DRAW | WC_DO_TITLE;
    }
    return 0;
}

int wc_app_paste(struct wc_app *a, const char *text, unsigned n) {
    struct wc_doc *d = wc_app_doc(a);
    if(!n || !text[0]) {
        snprintf(a->status, sizeof(a->status), "The clipboard is empty");
        return WC_DO_DRAW;
    }
    wc_doc_paste(d, text, n);
    return follow(a) | WC_DO_TITLE;
}

unsigned wc_app_clip(struct wc_app *a, char *out, unsigned size) {
    unsigned n = wc_doc_copy(wc_app_doc(a), out, size);
    if(!n) {
        snprintf(a->status, sizeof(a->status), "%s", wc_app_doc(a)->msg);
        a->cut = 0;
    }
    return n;
}

int wc_app_copied(struct wc_app *a, int ok) {
    struct wc_doc *d = wc_app_doc(a);
    if(ok && a->cut) wc_doc_erase(d);
    if(!ok) snprintf(a->status, sizeof(a->status), "Not copied: the window must have the focus");
    a->cut = 0;
    return follow(a) | WC_DO_TITLE;
}

int wc_app_goto(struct wc_app *a, const char *text) {
    struct wc_doc *d = wc_app_doc(a);
    unsigned long line = 0, col = 0;
    const char *p = text;
    while(*p == ' ') p++;
    if(*p < '0' || *p > '9') goto bad;
    while(*p >= '0' && *p <= '9' && line < 100000000) line = line * 10 + (unsigned)(*p++ - '0');
    if(*p == ':' || *p == ',') {
        p++;
        if(*p < '0' || *p > '9') goto bad;
        while(*p >= '0' && *p <= '9' && col < 100000000) col = col * 10 + (unsigned)(*p++ - '0');
    }
    while(*p == ' ') p++;
    if(*p || !line) goto bad;
    wc_doc_goto(d, (unsigned)line, (unsigned)col);
    return follow(a);
bad:
    snprintf(a->status, sizeof(a->status), "Not a line number: %.40s (a line, or line:column)", text);
    return WC_DO_DRAW;
}

int wc_app_files(struct wc_app *a, const char *folder) {
    char full[WC_PATH];
    a->status[0] = 0;
    if(folder[0] && folder[0] != '/' && !wc_app_path(a, folder, full, sizeof(full))) folder = full;  /* from a->cwd */
    if(wc_grep_start(&a->grep, a->fs, folder) < 0) {
        snprintf(a->status, sizeof(a->status), "%s", a->grep.why);
        return WC_DO_DRAW | WC_DO_FILES;
    }
    return WC_DO_DRAW | WC_DO_FILES;
}

int wc_app_hit(struct wc_app *a, unsigned i) {
    struct wc_doc *d;
    struct wc_line l;
    const struct wc_hit *h;
    unsigned ls, from, to;
    int r;
    if(i >= a->grep.nhits) return 0;
    h = &a->grep.hits[i];
    r = wc_app_open(a, wc_grep_path(&a->grep, h));
    d = wc_app_doc(a);
    if(strcmp(d->path, wc_grep_path(&a->grep, h))) return r;    /* it would not open: the status says why */
    ls = wc_text_line_start(&d->t, h->line - 1);
    wc_text_line(&d->t, ls, &l);
    from = ls + h->col < l.end ? ls + h->col : l.end;
    to = from + (unsigned)(h->b - h->a) < l.end ? from + (unsigned)(h->b - h->a) : l.end;
    wc_doc_select(d, from, to);
    return r | follow(a);
}

/**** keys ****/

/* a character typed: a UTF-8 sequence may come a byte at a time */
static int typed(struct wc_app *a, unsigned key) {
    struct wc_doc *d = wc_app_doc(a);
    unsigned char c = (unsigned char)key;
    int need;
    if(a->nutf8 && (c & 0xC0) == 0x80) {
        a->utf8[a->nutf8++] = (char)c;
        need = wc_utf8_len((const unsigned char *)a->utf8, a->nutf8);
        if(need > 1 || a->nutf8 == 4) {         /* complete (or no sequence after all): in it goes */
            wc_doc_type(d, a->utf8, a->nutf8);
            a->nutf8 = 0;
        }
        return follow(a) | WC_DO_TITLE;
    }
    if(a->nutf8) {                              /* a sequence cut short: what came of it, as it is */
        wc_doc_type(d, a->utf8, a->nutf8);
        a->nutf8 = 0;
    }
    if(c >= 0xC2 && c <= 0xF4) {
        a->utf8[a->nutf8++] = (char)c;
        return WC_DO_DRAW;
    }
    wc_doc_type(d, (const char *)&c, 1);
    return follow(a) | WC_DO_TITLE;
}

int wc_app_key(struct wc_app *a, unsigned key, unsigned mods) {
    struct wc_doc *d;
    int shift = (mods & WC_MOD_SHIFT) != 0, ctrl = (mods & WC_MOD_CTRL) != 0, alt = (mods & WC_MOD_ALT) != 0, r;
    if(a->job) {                                /* a search going on: stopped by the key (Esc used up), or it waits */
        r = wc_app_interrupt(a, key == 0x1B && !ctrl);
        if(r < 0) return WC_DO_DRAW | WC_DO_IDLE;
        if(key == 0x1B && !ctrl) return r;
    }
    d = wc_app_doc(a);
    a->status[0] = 0;
    d->msg[0] = 0;
    if(key >= 0x20 && key < 0x100 && key != 0x7F && !ctrl) return alt ? 0 : typed(a, key);  /* Alt+a key types nothing */
    if(a->nutf8) {
        wc_doc_type(d, a->utf8, a->nutf8);
        a->nutf8 = 0;
    }
    switch(key) {
    case 0x7F: wc_doc_backspace(d, 0); return follow(a) | WC_DO_TITLE;
    case WC_KEY_BACKSPACE: wc_doc_backspace(d, 1); return follow(a) | WC_DO_TITLE;
    case '\t':
        if(ctrl) return 0;                      /* Ctrl+I */
        wc_doc_tab(d, 0);
        return follow(a) | WC_DO_TITLE;
    case WC_KEY_BACKTAB: wc_doc_tab(d, 1); return follow(a) | WC_DO_TITLE;
    case WC_KEY_TAB: return wc_app_cmd(a, shift ? WC_CMD_PREV_TAB : WC_CMD_NEXT_TAB);
    case '\r': case '\n': case WC_KEY_ENTER:
        if(ctrl && key != WC_KEY_ENTER) return 0;      /* Ctrl+M, Ctrl+J */
        wc_doc_enter(d);
        return follow(a) | WC_DO_TITLE;
    case 0x1B:
        if(ctrl) return wc_app_cmd(a, WC_CMD_OUTDENT);     /* Ctrl+[ */
        wc_doc_select(d, d->caret, d->caret);
        return WC_DO_DRAW;
    case WC_KEY_LEFT: wc_doc_move(d, ctrl ? WC_WORD_LEFT : WC_LEFT, shift, a->rows); return follow(a);
    case WC_KEY_RIGHT: wc_doc_move(d, ctrl ? WC_WORD_RIGHT : WC_RIGHT, shift, a->rows); return follow(a);
    case WC_KEY_UP: case WC_KEY_DOWN:
        if(ctrl) {                              /* the view a line, the caret where it is */
            wc_view_scroll(d, key == WC_KEY_UP ? -1 : 1, 0, a->rows);
            return WC_DO_DRAW;
        }
        wc_doc_move(d, key == WC_KEY_UP ? WC_UP : WC_DOWN, shift, a->rows);
        return follow(a);
    case WC_KEY_PGUP: case WC_KEY_PGDN:
        if(ctrl) return wc_app_cmd(a, key == WC_KEY_PGUP ? WC_CMD_PREV_TAB : WC_CMD_NEXT_TAB);
        /* the view goes a page with the caret, so the caret keeps its row */
        wc_view_scroll(d, key == WC_KEY_PGUP ? -(int)(a->rows > 1 ? a->rows - 1 : 1) : (int)(a->rows > 1 ? a->rows - 1 : 1),
                       0, a->rows);
        wc_doc_move(d, key == WC_KEY_PGUP ? WC_PGUP : WC_PGDN, shift, a->rows);
        return follow(a);
    case WC_KEY_HOME: wc_doc_move(d, ctrl ? WC_DOC_START : WC_HOME, shift, a->rows); return follow(a);
    case WC_KEY_END: wc_doc_move(d, ctrl ? WC_DOC_END : WC_END, shift, a->rows); return follow(a);
    case WC_KEY_DELETE: wc_doc_delete(d, ctrl); return follow(a) | WC_DO_TITLE;
    case WC_KEY_F(5): return wc_app_cmd(a, shift ? WC_CMD_FIND_PREV : WC_CMD_FIND_NEXT);
    }
    if(!ctrl) return 0;
    switch(key) {                               /* the Ctrl+letters, as their control bytes */
    case CTRL('a'): return wc_app_cmd(a, WC_CMD_SELECT_ALL);
    case CTRL('b'): wc_doc_bracket(d, shift); return follow(a);
    case CTRL('c'): return wc_app_cmd(a, WC_CMD_COPY);
    case CTRL('f'): return wc_app_cmd(a, shift ? WC_CMD_FIND_FILES : WC_CMD_FIND);
    case CTRL('h'): return wc_app_cmd(a, WC_CMD_REPLACE);
    case CTRL('l'): return wc_app_cmd(a, WC_CMD_GOTO);
    case CTRL('n'): return wc_app_cmd(a, WC_CMD_NEW);
    case CTRL('o'): return wc_app_cmd(a, WC_CMD_OPEN);
    case CTRL('q'): return wc_app_cmd(a, WC_CMD_QUIT);
    case CTRL('s'): return wc_app_cmd(a, shift ? WC_CMD_SAVE_AS : WC_CMD_SAVE);
    case CTRL('v'): return wc_app_cmd(a, WC_CMD_PASTE);
    case CTRL('w'): return wc_app_cmd(a, WC_CMD_CLOSE);
    case CTRL('x'): return wc_app_cmd(a, WC_CMD_CUT);
    case CTRL('y'): return wc_app_cmd(a, WC_CMD_REDO);
    case CTRL('z'): return wc_app_cmd(a, shift ? WC_CMD_REDO : WC_CMD_UNDO);
    case 0x1D: case ']': return wc_app_cmd(a, WC_CMD_INDENT);      /* Ctrl+] */
    case 0x1F: case '/': return wc_app_cmd(a, WC_CMD_COMMENT);     /* Ctrl+/ */
    }
    return 0;
}

/**** the pointer ****/

int wc_app_press(struct wc_app *a, int row, int col, unsigned clicks, int extend) {
    struct wc_doc *d;
    unsigned pos, line;
    if(stop_job(a)) return WC_DO_DRAW | WC_DO_IDLE;
    d = wc_app_doc(a);
    a->status[0] = 0;
    d->msg[0] = 0;
    if(row < 0) row = 0;
    line = d->top + (unsigned)row;
    if(col < 0) {                               /* the line numbers: whole lines */
        if(line > d->t.nl) line = d->t.nl;
        wc_doc_select_line(d, line, extend);
        a->clicks = 3;
        return WC_DO_DRAW;
    }
    pos = wc_view_hit(d, (unsigned)row, (unsigned)col);
    a->clicks = clicks;
    if(clicks == 2) wc_doc_select_word(d, pos);
    else if(clicks >= 3) wc_doc_select_line(d, line > d->t.nl ? d->t.nl : line, 0);
    else wc_doc_select(d, extend ? d->anchor : pos, pos);
    return WC_DO_DRAW;
}

int wc_app_drag(struct wc_app *a, int row, int col) {
    struct wc_doc *d = wc_app_doc(a);
    unsigned pos, line;
    if(row < 0) {                               /* past the top or bottom: the text comes to the pointer */
        wc_view_scroll(d, -1, 0, a->rows);
        row = 0;
    } else if(row >= (int)a->rows) {
        wc_view_scroll(d, 1, 0, a->rows);
        row = (int)a->rows - 1;
    }
    if(col < 0 && a->clicks != 3) {
        if(d->left) wc_view_scroll(d, 0, -1, a->rows);
        col = 0;
    } else if(col >= (int)a->cols) {
        wc_view_scroll(d, 0, 1, a->rows);
        col = (int)a->cols - 1;
    }
    line = d->top + (unsigned)row;
    if(a->clicks == 3) {                        /* from the line numbers, or a triple click: lines */
        wc_doc_select_line(d, line > d->t.nl ? d->t.nl : line, 1);
        return WC_DO_DRAW;
    }
    pos = wc_view_hit(d, (unsigned)row, (unsigned)(col < 0 ? 0 : col));
    d->caret = pos;
    d->want_col = -1;
    return WC_DO_DRAW;
}

int wc_app_wheel(struct wc_app *a, int rows) {
    wc_view_scroll(wc_app_doc(a), rows, 0, a->rows);
    return WC_DO_DRAW;
}

int wc_app_idle(struct wc_app *a) {
    struct wc_doc *d = wc_app_doc(a);
    int r = 0, exact;
    unsigned hits = a->grep.nhits;
    if(a->job) r |= run_job(a, a->job);         /* 11/09/26: a search's next step */
    if(a->grep.state == WC_GREP_RUN) {
        wc_grep_step(&a->grep, 256u << 10);
        if(a->grep.nhits != hits || a->grep.state != WC_GREP_RUN) r |= WC_DO_DRAW | WC_DO_FILES;
        if(a->grep.state == WC_GREP_RUN) r |= WC_DO_IDLE;
    }
    if(!a->view.exact) {                        /* the screenful was drawn with a guess: on towards its first line */
        wc_syn_state(&d->t, d->lang, d->top, 256u << 10, &exact);
        r |= exact ? WC_DO_DRAW : WC_DO_IDLE;
    }
    return r;
}

/**** what the window shows ****/

void wc_app_tab_label(struct wc_app *a, int i, char *out, unsigned size) {
    char name[WC_PATH];
    if(i < 0 || i >= a->n) {
        if(size) out[0] = 0;
        return;
    }
    wc_doc_title(a->docs[i], name, sizeof(name));
    snprintf(out, size, "%s%s", name, wc_doc_safe(a->docs[i]) ? "" : " *");
}

void wc_app_title(struct wc_app *a, char *out, unsigned size) {
    char name[WC_PATH];
    wc_doc_title(wc_app_doc(a), name, sizeof(name));
    snprintf(out, size, "Code Editor - %s%s", name, wc_doc_safe(wc_app_doc(a)) ? "" : " *");
}

void wc_app_status(struct wc_app *a, char *out, unsigned size) {
    struct wc_doc *d = wc_app_doc(a);
    if(a->status[0]) snprintf(out, size, "%s", a->status);
    else if(d->msg[0]) snprintf(out, size, "%s", d->msg);
    else if(d->unsure) snprintf(out, size, "%s may share this file's data: check the disk (fsck) first",
                                d->twin[0] ? d->twin : d->copied[0] ? d->copied : "A file beside it");
    else wc_view_status(d, out, size);
}
