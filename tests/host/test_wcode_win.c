/* Host tests of the Code Editor's window (user/wcode/window.c, 11/05/26, the desktop applications, track code) on the fake
   desktop of tests/host/gk_fake.c, with the model as it is and files in memory: the window as it opens (the tab, the
   line numbers, the status bar); typing, and only the rows that change presented; a C file opened through the
   desktop's chooser into a tab of its own, its keywords in their colour; Save by the menu; the find bar (Enter, Shift+Enter,
   Esc); Go to line's box; the clipboard both ways; the mouse (a click, a drag, a double click, the line numbers) and the
   wheel; a new size; find in files, its list and a line of it opened; the unsaved-changes guard on Close tab, on the
   close button and on Quit (a file never saved asks its name through the chooser); a desktop without choosers (a path
   typed instead); the theme followed. Built and run by `make unittest` (tools/gui_apps_code.py has the screen's checks). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <gk.h>
#include "gk_fake.h"
#include "window.h"

/* libsurf's regex.c, built for the host, allocates under these names */
void *surf_malloc(size_t n) { return malloc(n); }
void surf_free(void *p) { free(p); }
void *surf_calloc(size_t n, size_t k) { return calloc(n, k); }
void *surf_realloc(void *p, size_t n) { return realloc(p, n); }

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static struct wc_window w;
static int ended;

/**** files in memory ****/

struct mfile { char path[64]; char data[4096]; };
static struct mfile disk[16];
static int ndisk;

static struct mfile *mfind(const char *path) {
    int i;
    for(i = 0; i < ndisk; i++)
        if(!strcmp(disk[i].path, path)) return &disk[i];
    return NULL;
}

static void mput(const char *path, const char *data) {
    struct mfile *f = mfind(path);
    if(!f) f = &disk[ndisk++];
    snprintf(f->path, sizeof(f->path), "%s", path);
    snprintf(f->data, sizeof(f->data), "%s", data);
}

static char *bigtext;                   /* 11/09/26: /m/big.c, 4 MB of C whose first line opens a comment */
static unsigned bign;
static int disk_changes;                /* saves to refuse as a file another program saved since */

static int m_read(struct wc_doc *d, const char *path) {
    struct mfile *f = mfind(path);
    unsigned n;
    if(!strcmp(path, "/m/big.c") && bigtext) {
        memcpy(wc_text_load_begin(&d->t, bign), bigtext, bign);
        wc_text_load_end(&d->t, bign);
        return wc_doc_loaded(d, path) == 0 ? 0 : -1;
    }
    if(!f) {
        snprintf(d->msg, sizeof(d->msg), "Cannot open %s", path);
        return -1;
    }
    n = (unsigned)strlen(f->data);
    memcpy(wc_text_load_begin(&d->t, n), f->data, n);
    wc_text_load_end(&d->t, n);
    return wc_doc_loaded(d, path) == 0 ? 0 : -1;
}

static int m_save(struct wc_doc *d, const char *to) {
    char b[4096];
    if(disk_changes && !to && !d->overwrite) {
        disk_changes--;
        d->disk_changed = 1;
        snprintf(d->msg, sizeof(d->msg), "Not saved: it changed on the disk since it was read here");
        return -1;
    }
    d->overwrite = 0;
    unsigned n = d->t.len < sizeof(b) - 1 ? d->t.len : (unsigned)sizeof(b) - 1;
    wc_text_get(&d->t, 0, b, n);
    b[n] = 0;
    mput(to ? to : d->path, b);
    if(to) {
        snprintf(d->path, sizeof(d->path), "%s", to);
        if(d->lang == WC_LANG_TEXT) wc_doc_set_lang(d, wc_lang_guess(to, NULL, 0));
    }
    wc_doc_saved(d);
    snprintf(d->msg, sizeof(d->msg), "Saved %.150s", to ? to : d->path);
    return 0;
}

static const struct wc_app_io mio = { m_read, m_save };

/**** find in files' file system: the files in memory, under folders named in their paths ****/

static int listing, at;

static int f_opendir(void *ctx, const char *path) {
    (void)ctx;
    if(listing) return -1;
    listing = 1;
    at = 0;
    return !strcmp(path, "/m") ? 0 : -1;
}

static int f_readdir(void *ctx, int h, char *name, unsigned size, int *dir, unsigned *bytes) {
    (void)ctx;
    (void)h;
    for(; at < ndisk; at++)
        if(!strncmp(disk[at].path, "/m/", 3)) {
            snprintf(name, size, "%s", disk[at].path + 3);
            *dir = 0;
            *bytes = (unsigned)strlen(disk[at].data);
            at++;
            return 1;
        }
    return 0;
}

static void f_closedir(void *ctx, int h) {
    (void)ctx;
    (void)h;
    listing = 0;
}

static int reading = -1, rpos;

static int f_open(void *ctx, const char *path) {
    struct mfile *f = mfind(path);
    (void)ctx;
    if(!f || reading >= 0) return -1;
    reading = (int)(f - disk);
    rpos = 0;
    return 0;
}

static int f_read(void *ctx, int h, char *buf, unsigned n) {
    unsigned left = (unsigned)strlen(disk[reading].data) - (unsigned)rpos;
    (void)ctx;
    (void)h;
    if(n > left) n = left;
    memcpy(buf, disk[reading].data + rpos, n);
    rpos += (int)n;
    return (int)n;
}

static void f_close(void *ctx, int h) {
    (void)ctx;
    (void)h;
    reading = -1;
}

static const struct wc_fsops mfs = { NULL, f_opendir, f_readdir, f_closedir, f_open, f_read, f_close };

/**** driving it ****/

/* 11/09/26: the events waiting, through the window, and no idle work (a search stays between its steps) */
static void events_only(void) {
    struct gk_event ev;
    while(gk_next(w.ui, &ev, 0) > 0)
        if(!wc_window_event(&w, &ev)) ended = 1;
}

/* every event the window has, through it, and its idle work; 0 once it ended */
static int run(void) {
    struct gk_event ev;
    int r, k;
    for(k = 0; k < 10000 && !ended; k++) {
        r = gk_next(w.ui, &ev, 0);
        if(r > 0) {
            if(!wc_window_event(&w, &ev)) ended = 1;
        } else if(r == 0 && w.idle) wc_window_idle(&w);
        else break;
    }
    return !ended;
}

static int tx, ty, tw, th;                      /* the text area */

static void where(void) {
    gk_bounds(w.text, &tx, &ty, &tw, &th);
}

/* text drawn on row r of the text from column c, in colour */
static int row_says(int r, int c, const char *s, unsigned color) {
    where();
    return gkf_text(tx + (int)w.gutter * WC_CW + c * WC_CW, ty + r * WC_CH, s, color);
}

static int status_says(const char *s) {
    return gkf_text(10, WC_WIN_H - GK_STATUS_H + 3, s, GW_INK);
}

static struct wc_doc *doc(void) {
    return wc_app_doc(&w.app);
}

static int text_is(const char *want) {
    static char b[8192];
    wc_text_get(&doc()->t, 0, b, doc()->t.len);
    b[doc()->t.len] = 0;
    return !strcmp(b, want);
}

static void key(unsigned k, unsigned mods) {
    gkf_key(k, mods);
}

static void ctrl(char letter) {
    gkf_key((unsigned)(letter & 0x1f), WIN_MOD_CTRL);
}

/* the cell (row, col) of the text, clicked */
static void click_cell(int r, int c) {
    where();
    gkf_click(tx + (int)w.gutter * WC_CW + c * WC_CW + 1, ty + r * WC_CH + 8);
}

int main(void) {
    int i, n, x, y, ww, hh, big;
    char b[4096];
    setvbuf(stdout, NULL, _IOLBF, 0);
    mput("/m/a.c", "int main(void)\n{\n    return 0;\n}\n");
    mput("/m/notes.txt", "needle in a haystack\nno match\nanother needle\n");
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(wc_window_open(&w, &mio, &mfs) < 0 && errno == ENODEV);
    CHECK(wc_window_open(&w, &mio, &mfs) == 0 && gkf_w() == WC_WIN_W && gkf_h() == WC_WIN_H && gkf_resizable());
    gkf_focus(1);
    CHECK(run());
    CHECK(!strcmp(gkf_title(), "Code Editor - Untitled 1"));
    where();
    CHECK(ty == GK_TABS_H && w.gutter == 5 && w.rows == (unsigned)th / WC_CH);
    CHECK(th % WC_CH && gkf_pixel(tx + 2, ty + th - 1) == GW_BAND);  /* the gutter to the area's foot, past the rows */
    CHECK(gkf_text(8 + 7, 7, "Untitled 1", GW_PAPER));                /* the tab, lit: the one shown */
    CHECK(gkf_text(tx + 3 * WC_CW, ty, "1", GW_INK));                 /* line 1's number, the caret's line */
    CHECK(gkf_find("Ln 1, Col 1  Plain text  LF  Spaces: 4", GW_MUTED, &x, &y));
    /* typing: the row, the status bar, the tab and the title; never the whole text again */
    gkf_presented(&x, &y, &ww, &hh);
    gkf_type("hello");
    CHECK(run() && text_is("hello") && row_says(0, 0, "hello", GW_INK));
    CHECK(!strcmp(gkf_title(), "Code Editor - Untitled 1 *") && gkf_find("Ln 1, Col 6", GW_MUTED, &x, &y));
    n = gkf_presented(&x, &y, &ww, &hh);
    for(i = 0, big = 0; i < n; i++) {
        gkf_presented_rect(i, &x, &y, &ww, &hh);
        if(y >= ty && y < ty + th && hh > 2 * WC_CH) big++;
    }
    CHECK(n > 0 && !big);
    key(13, 0);
    gkf_type("x");
    CHECK(run() && text_is("hello\nx") && row_says(1, 0, "x", GW_INK) && gkf_text(tx + 3 * WC_CW, ty + WC_CH, "2", GW_INK));
    /* a C file through the desktop's Open chooser: a tab of its own (the first has changes), in colour */
    ctrl('O');
    CHECK(run() && gkf_dialog() && gkf_dialog()->kind == WIN_DLG_OPEN);
    gkf_dialog_answer("/m/a.c");
    CHECK(run() && w.app.n == 2 && w.app.cur == 1 && !strcmp(gkf_title(), "Code Editor - a.c"));
    CHECK(gk_tab_count(w.tabs) == 2 && gk_value(w.tabs) == 1);
    CHECK(row_says(0, 0, "int", 0x1E5C96) && row_says(0, 4, "main", GW_INK) && row_says(2, 4, "return", 0x6B3A8E) &&
          row_says(2, 11, "0", 0xA44A22));
    CHECK(gkf_find("C  LF  Spaces: 4", GW_MUTED, &x, &y));
    /* the menus in the desktop's bar */
    CHECK(gk_menus_shown(w.ui) && gkf_menu_find("Find in files" "\xe2\x88\x99") >= 0);
    CHECK(!strcmp(gkf_menu_entry(gkf_menu_find("Save as" "\xe2\x88\x99"))->keys, "Ctrl+Shift+S"));
    CHECK(!strcmp(gkf_menu_entry(gkf_menu_find("Find next"))->keys, "F5"));
    CHECK(gkf_menu_entry(gkf_menu_find("C"))->flags & WIN_MENU_RADIO);    /* View > Language: C */
    CHECK(gkf_menu_entry(gkf_menu_find("Undo"))->flags & WIN_MENU_DISABLED);
    key(WIN_KEY_END, WIN_MOD_CTRL);
    gkf_type("// end");
    CHECK(run() && !(gkf_menu_entry(gkf_menu_find("Undo"))->flags & WIN_MENU_DISABLED));
    CHECK(gkf_menu_choose(WC_CMD_SAVE));
    CHECK(run() && !strcmp(mfind("/m/a.c")->data, "int main(void)\n{\n    return 0;\n}\n// end") &&
          !strcmp(gkf_title(), "Code Editor - a.c"));
    CHECK(row_says(4, 0, "// end", GW_MUTED));
    /* the find bar: Enter finds the next, Shift+Enter the one before, Esc closes it */
    ctrl('F');
    CHECK(run() && gk_shown(w.findbar) && gk_focused(w.ui) == w.find);
    gkf_type("r");
    key(13, 0);
    CHECK(run() && doc()->anchor == 21 && doc()->caret == 22);       /* the r of return, round from the end */
    key(13, 0);
    CHECK(run() && doc()->anchor == 25 && doc()->caret == 26);       /* return's other r */
    key(13, WIN_MOD_SHIFT);                     /* the one before: selected from its end, the caret at its start */
    CHECK(run() && doc()->anchor == 22 && doc()->caret == 21);
    key(27, 0);
    CHECK(run() && !gk_shown(w.findbar) && gk_focused(w.ui) == w.text);
    key(WIN_KEY_F5, 0);
    CHECK(run() && doc()->anchor == 25);                              /* F5: the next, with the bar closed */
    /* Go to line, in a box */
    ctrl('L');
    CHECK(run() && gk_box_open(w.ui) && gk_asking(w.ui) == BOX_GOTO);
    gkf_type("3:5\n");
    CHECK(run() && !gk_box_open(w.ui) && wc_doc_line(doc()) == 2 && wc_doc_col(doc()) == 4);
    /* the clipboard */
    ctrl('A');
    ctrl('C');
    CHECK(run() && !strcmp(gkf_clip(), "int main(void)\n{\n    return 0;\n}\n// end"));
    key(WIN_KEY_END, WIN_MOD_CTRL);
    gkf_set_paste("\n/* more */");
    ctrl('V');
    CHECK(run() && row_says(5, 0, "/* more */", GW_MUTED));
    gkf_focus(0);
    ctrl('C');                                  /* (the desktop sends no key to a window without the focus) */
    gkf_focus(1);
    CHECK(run());
    /* the mouse: a click places the caret, a drag selects, a double click takes a word, the numbers a line */
    gkf_advance(1000);
    click_cell(0, 6);
    CHECK(run() && doc()->caret == 6 && doc()->anchor == 6);
    gkf_advance(1000);
    where();
    gkf_drag(tx + 40 + 1, ty + 8, tx + 40 + 3 * WC_CW + 1, ty + WC_CH + 8);
    CHECK(run() && doc()->anchor == 0 && doc()->caret == 16);       /* line 2 is "{": its end */
    gkf_advance(1000);
    click_cell(2, 6);
    click_cell(2, 6);
    CHECK(run() && doc()->anchor == 21 && doc()->caret == 27);       /* "return" */
    gkf_advance(1000);
    gkf_click(tx + 4, ty + 3 * WC_CH + 8);
    CHECK(run() && doc()->anchor == 31 && doc()->caret == 33);       /* the line "}\n" */
    /* a long file: the wheel scrolls it, the numbers with it */
    for(i = 0, n = 0; i < 200; i++) n += snprintf(b + n, sizeof(b) - (size_t)n, "line %d\n", i + 1);
    mput("/m/long.txt", b);
    ctrl('O');
    CHECK(run() && gkf_dialog());
    gkf_dialog_answer("/m/long.txt");
    CHECK(run() && w.app.n == 3 && row_says(0, 1, "ine 1", GW_INK) && row_says(1, 0, "line 2", GW_INK));   /* (the caret) */
    where();
    gkf_wheel(tx + 100, ty + 100, 3);
    CHECK(run() && doc()->top == 3 && row_says(0, 0, "line 4", GW_INK) && gkf_text(tx + 3 * WC_CW, ty, "4", GW_MUTED));
    key(WIN_KEY_END, WIN_MOD_CTRL);
    CHECK(run() && doc()->top + w.rows >= 200 && w.crow >= 0);
    /* a new size: the rows follow */
    gkf_resize(500, 300);
    CHECK(run() && gkf_w() == 500 && w.rows == (unsigned)(300 - GK_TABS_H - GK_STATUS_H) / WC_CH);
    where();
    CHECK(tw == 500 && w.crow >= 0);
    gkf_resize(WC_WIN_W, WC_WIN_H);
    CHECK(run());
    /* find in files: the band, its list, a line of it opened at its match */
    key(6, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    CHECK(run() && gk_shown(w.files) && gk_focused(w.ui) == w.fwhat && !strcmp(gk_text(w.fin), "/m"));
    key(1, WIN_MOD_CTRL);
    gkf_type("needle\n");
    CHECK(run() && w.app.grep.state == WC_GREP_DONE && gk_list_count(w.flist) == 2);
    CHECK(!strcmp(gk_list_text(w.flist, 0), "notes.txt:1: needle in a haystack"));
    CHECK(gkf_find("2 lines found in", GW_INK, &x, &y));
    gk_focus(w.flist);
    gk_set_value(w.flist, 1);
    key(13, 0);
    CHECK(run() && !strcmp(doc()->path, "/m/notes.txt") && doc()->anchor == 38 && doc()->caret == 44);
    CHECK(gk_focused(w.ui) == w.text);
    /* the guard: Close tab with changes asks; Cancel keeps it; Don't save closes it */
    gkf_type("Z");
    ctrl('W');
    CHECK(run() && gk_box_open(w.ui) && gk_asking(w.ui) == BOX_SAVE);
    key(27, 0);
    CHECK(run() && !gk_box_open(w.ui) && w.app.n == 4);
    ctrl('W');
    CHECK(run() && gk_box_open(w.ui));
    key(WIN_KEY_RIGHT, 0);                      /* Don't save */
    key(13, 0);
    CHECK(run() && w.app.n == 3 && !strcmp(mfind("/m/notes.txt")->data, "needle in a haystack\nno match\nanother needle\n"));
    /* a desktop without choosers: the path typed instead */
    gkf_lacks(1u << WIN_DIALOG, ENOSYS);
    ctrl('O');
    CHECK(run() && gk_box_open(w.ui));
    ctrl('A');                                  /* what the line starts with (the folder), replaced */
    gkf_type("/m/notes.txt\n");
    CHECK(run() && w.app.n == 4 && !strcmp(doc()->path, "/m/notes.txt"));
    gkf_lacks(0, 0);
    ctrl('O');
    CHECK(run() && gkf_dialog());
    gkf_dialog_answer("/m/missing.c");          /* a file that cannot be read: said, and no tab for it */
    CHECK(run() && w.app.n == 4 && status_says("Cannot open /m/missing.c"));
    gkf_lacks(1u << WIN_DIALOG, EBUSY);         /* the desktop busy (a chooser up, or an answer not yet taken): said */
    ctrl('O');
    CHECK(run() && !gkf_dialog() && !gk_box_open(w.ui) && status_says("No chooser yet: the desktop is busy; try again"));
    gkf_lacks(0, 0);
    /* 11/09/26 (the review): a jump in a large file draws with a guess and has the colouring catch up between events
       (L1); a search goes on between events, and Esc stops it (C3); a save over a file another program saved since
       asks first, in a box (H1) */
    {
        unsigned k, n = 0;
        const char *line = "\nint x = 1; char *s = \"text\";";
        bign = 4u << 20;
        bigtext = malloc(bign);
        bigtext[n++] = '/';
        bigtext[n++] = '*';
        while(n < bign)
            for(k = 0; line[k] && n < bign; k++) bigtext[n++] = line[k];
        ended = 0;
        ctrl('O');
        CHECK(run() && gkf_dialog());
        gkf_dialog_answer("/m/big.c");
        CHECK(run() && !strcmp(doc()->path, "/m/big.c"));
        key(WIN_KEY_END, WIN_MOD_CTRL);
        events_only();
        CHECK(!w.app.view.exact && w.idle);     /* drawn with a guess, the catch-up asked for */
        CHECK(run() && w.app.view.exact && row_says(0, 0, "int x", GW_MUTED));    /* a comment, as it is */
        ctrl('F');
        CHECK(run());
        ctrl('A');
        gkf_type("not in it");
        key(13, 0);
        events_only();
        CHECK(w.app.job == WC_JOB_FIND && w.idle && status_says("Searching: Esc stops it"));
        key(27, 0);
        events_only();
        CHECK(!w.app.job && status_says("Stopped searching") && gk_shown(w.findbar));
        key(27, 0);                             /* the bar's own Esc, now */
        CHECK(run() && !gk_shown(w.findbar));
        free(bigtext);
        bigtext = NULL;
        ctrl('W');
        CHECK(run());
        ctrl('O');
        CHECK(run() && gkf_dialog());
        gkf_dialog_answer("/m/notes.txt");
        CHECK(run() && !strcmp(doc()->path, "/m/notes.txt"));
        key(WIN_KEY_END, WIN_MOD_CTRL);
        gkf_type("Q");
        disk_changes = 1;
        ctrl('S');
        CHECK(run() && gk_box_open(w.ui) && gk_asking(w.ui) == BOX_DISK && gkf_find("notes.txt changed on the disk", GW_PAPER, &x, &y));
        key(27, 0);                             /* Cancel: not saved, said */
        CHECK(run() && !gk_box_open(w.ui) && status_says("Not saved: the file on the disk stays as it is"));
        disk_changes = 1;
        ctrl('S');
        CHECK(run() && gk_asking(w.ui) == BOX_DISK);
        key(13, 0);                             /* Save over it */
        CHECK(run() && !gk_box_open(w.ui) && !strcmp(mfind("/m/notes.txt")->data, "needle in a haystack\nno match\nanother needle\nQ"));
    }
    /* 11/09/26 (the second review): Replace all too large to undo asks first, in a box; while it changes the file the
       find bar's fields are greyed (typing there changes nothing) and only the status bar is drawn between its steps;
       the find bar changed while a search goes on stops it */
    {
        unsigned k;
        int i2, outside;
        bign = 200000 * 4;
        bigtext = malloc(bign);
        for(k = 0; k < 200000; k++) memcpy(bigtext + k * 4, "foo\n", 4);
        ctrl('O');
        CHECK(run() && gkf_dialog());
        gkf_dialog_answer("/m/big.c");
        CHECK(run() && !strcmp(doc()->path, "/m/big.c"));
        w.app.step = 16u << 10;
        key(8, WIN_MOD_CTRL);                   /* Ctrl+H: the find bar with the replacement's line */
        CHECK(run() && gk_shown(w.replace_row));
        gk_set_text(w.find, "");                /* (the fields as a person empties them; Ctrl+A is the menu's) */
        gk_set_text(w.with, "");
        gk_focus(w.find);
        gkf_type("foo");
        CHECK(run());
        gk_focus(w.with);
        gkf_type("bar");
        CHECK(run() && !strcmp(w.app.find.what, "foo") && !strcmp(w.app.find.with, "bar"));
        CHECK(gkf_find("Replace all", GW_INK, &x, &y));
        gkf_click(x + 4, y + 4);                /* its button */
        CHECK(run() && gk_asking(w.ui) == BOX_ALL && gkf_find("Replace 200000 matches?", GW_PAPER, &x, &y) &&
              !wc_app_busy(&w.app));
        key(13, 0);                             /* Replace all */
        events_only();
        CHECK(wc_app_busy(&w.app) && !gk_enabled(w.with) && !gk_enabled(w.find) && !gk_enabled(w.regex));
        gkf_presented(&x, &y, &ww, &hh);
        wc_window_idle(&w);                     /* a step: the status bar only */
        events_only();
        n = gkf_presented(&x, &y, &ww, &hh);
        for(i2 = 0, outside = 0; i2 < n; i2++) {
            gkf_presented_rect(i2, &x, &y, &ww, &hh);
            if(y < WC_WIN_H - GK_STATUS_H) outside++;
        }
        CHECK(!outside && status_says("Replacing: Esc stops it"));     /* (nothing at all when the status is as it was) */
        gkf_type("X");                          /* into the greyed Replace field: nothing */
        CHECK(run() && !w.app.job && !strcmp(gk_text(w.with), "bar") && gk_enabled(w.with) &&
              doc()->t.len == bign && wc_text_byte(&doc()->t, bign - 4) == 'b' && status_says("Replaced 200000"));
        gk_set_text(w.find, "");
        gk_focus(w.find);
        gkf_type("zzz");
        key(13, 0);
        events_only();
        CHECK(w.app.job == WC_JOB_FIND);
        gkf_type("q");                          /* the find bar changed: the search with "zzz" stops */
        events_only();
        CHECK(!w.app.job && status_says("Stopped searching") && !strcmp(w.app.find.what, "zzzq"));
        w.app.step = 0;
        key(27, 0);
        CHECK(run() && !gk_shown(w.findbar));
        ctrl('W');                              /* big.c, replaced: Don't save */
        CHECK(run() && gk_asking(w.ui) == BOX_SAVE);
        key(WIN_KEY_RIGHT, 0);
        key(13, 0);
        CHECK(run());
        free(bigtext);
        bigtext = NULL;
        ctrl('O');
        CHECK(run() && gkf_dialog());
        gkf_dialog_answer("/m/notes.txt");
        CHECK(run() && !strcmp(doc()->path, "/m/notes.txt"));
    }
    /* the theme: drawn again in its colours */
    gkf_set_theme(WIN_THEME_DUSK, NULL);
    CHECK(run() && row_says(0, 1, "eedle", GW_INK));
    /* quitting: the close button asks about each file with changes; Untitled 1 asks its name through the chooser */
    gkf_close();
    CHECK(run() && gk_box_open(w.ui) && gk_asking(w.ui) == BOX_SAVE && w.app.cur == 0);
    gkf_close();                                /* pressed again while it asks: the question stands, */
    CHECK(gkf_menu_choose(WC_CMD_NEW));         /* and the bar's items wait for its answer */
    CHECK(run() && gk_box_open(w.ui) && gk_asking(w.ui) == BOX_SAVE && w.app.asking == 0 && w.app.n == 4);
    key(13, 0);                                 /* Save */
    CHECK(run() && gkf_dialog() && gkf_dialog()->kind == WIN_DLG_SAVE && !strcmp(gkf_dialog()->name, "untitled.txt"));
    gkf_dialog_answer("/m/hello.txt");
    CHECK(run() && !strcmp(mfind("/m/hello.txt")->data, "hello\nx"));
    CHECK(gk_box_open(w.ui) && w.app.asking == 1 && w.app.cur == 1);   /* then a.c, with the paste since its save */
    key(WIN_KEY_RIGHT, 0);                      /* Don't save: the quit goes on, and ends */
    key(13, 0);
    CHECK(!run() && !strcmp(mfind("/m/a.c")->data, "int main(void)\n{\n    return 0;\n}\n// end"));
    wc_window_close(&w);
    CHECK(!gkf_open());
    /* the window let go meanwhile: the loop ends (main.c then puts unsaved text beside its files) */
    ended = 0;
    CHECK(wc_window_open(&w, &mio, &mfs) == 0);
    gkf_focus(1);
    gkf_type("q");
    CHECK(run());
    gkf_gone();
    {
        struct gk_event ev;
        CHECK(gk_next(w.ui, &ev, 0) < 0 && errno == EPIPE && wc_app_unsafe(&w.app) == 1);
    }
    wc_window_close(&w);
    gkf_reset(0, 0);
    printf("test_wcode_win: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
