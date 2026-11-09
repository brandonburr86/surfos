/*
SurfOS window toolkit: modal boxes in the window
--------------------
File: gkbox.c   Date: 11/05/26
--------------------
gk_message(), gk_confirm() and gk_ask() (gk.h): a box over the window's content, drawn and worked
as the desktop's own dialogs are (shell/guidialog.c): its shadow, a teal title bar with the wave
mark, a line in ink and one muted, or a line to type, and its buttons at the bottom right, the one
Enter answers lit in teal. Left, Right and Tab choose among the buttons of a question, Enter or Space
answers, Esc cancels (-1), and a click on a button answers it. While a box is open it has every key
and click of the window, and the widgets behind it hear nothing; the program still hears its timers,
its descriptors and the close button.

A box is answered once: GK_EV_ANSWER with its id and the button (0 the first, 1 the second, -1
Cancel or Esc) and, from gk_ask(), the text typed, which the toolkit wipes after the next gk_next().
A line to type a path in stands in for the desktop's chooser where the desktop has none (gkdesk.c's
gk_choose()): its answer is GK_EV_CHOSEN, the path made absolute from the folder it started in.
A box opened over another answers the other -1 first, so that a program waiting for an answer
always gets one. The box is a widget tree of its own (ui->box), laid out in the middle of the window
and kept on it; gk_ask()'s line is a field (gkfield.c), edited as every field is.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

enum { BOX_MESSAGE = 1, BOX_CONFIRM, BOX_ASK };

#define TITLE_H 28                      /* the desktop's title bar and dialog layout (shell/guipriv.h, guidialog.c) */
#define BUTTON_H 28

struct box {
    int kind, id;
    char title[64], line[2][192];
    char label[3][40];                  /* "" for none: 0 the first answer, 1 the second, 2 Cancel */
    int choice;                         /* the button Enter answers */
    int press;                          /* the button a press went down on, -1 */
    int bw, bh;                         /* the box, without its shadow */
    struct gk_widget *field;            /* gk_ask()'s line */
    int chooser;                        /* it stands in for a chooser: its answer is GK_EV_CHOSEN */
    char dir[WIN_PATH_MAX];             /* and a path typed without a / at its start is in this folder */
};

static void copy_to(char *to, size_t n, const char *s) {
    size_t k;
    if(!s) s = "";
    k = strlen(s);
    if(k >= n) {                                        /* cut at a character's start */
        k = n - 1;
        while(k && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    }
    memcpy(to, s, k);
    to[k] = 0;
}

/* where button i is (those with a label, from the right): 1 and its x and width, or 0 */
static int button_place(const struct gk_widget *w, int i, int *bx, int *bw) {
    const struct box *b = w->k;
    int k, x = w->x + b->bw - 16, width;
    if(i < 0 || i > 2 || !b->label[i][0]) return 0;
    for(k = 2; k >= i; k--) {
        if(!b->label[k][0]) continue;
        width = gw_text_width(b->label[k]) + 32;
        if(width < 88) width = 88;
        x -= width;
        if(k == i) {
            *bx = x;
            *bw = width;
            return 1;
        }
        x -= 8;
    }
    return 0;
}

static void box_measure(struct gk_widget *w) {
    const struct box *b = w->k;
    w->base_w = b->bw + 6;
    w->base_h = b->bh + 7;
}

static void box_arrange(struct gk_widget *w) {
    struct box *b = w->k;
    if(b->field) gk_lay(b->field, w->x + 16, w->y + 66, b->bw - 32, GK_LINE_H);
}

static void box_paint(struct gk_widget *w, struct gw *win) {
    const struct box *b = w->k;
    const struct gk_theme *t = &w->ui->theme;
    int x = w->x, y = w->y, cells = (b->bw - 32) / 8, i, bx, bwid;
    gw_rect(win, x + 6, y + 7, b->bw, b->bh, t->shadow);
    gw_rect(win, x, y, b->bw, b->bh, t->frame);
    gw_rect(win, x + 1, y + 1, b->bw - 2, b->bh - 2, t->paper);
    gw_rect(win, x + 1, y + 1, b->bw - 2, TITLE_H - 1, t->teal);
    gw_glyph(win, x + 12, y + 6, 0xF7, t->foam, 1);     /* the wave, as the desktop's dialogs have it */
    gw_text_cut(win, x + 28, y + 6, b->title, (b->bw - 40) / 8, t->on_teal);
    if(b->kind == BOX_ASK) gw_text_cut(win, x + 16, y + 42, b->line[0], cells, t->ink);
    else {
        gw_text_cut(win, x + 16, y + 44, b->line[0], cells, t->ink);
        gw_text_cut(win, x + 16, y + 66, b->line[1], cells, t->muted);
    }
    for(i = 0; i < 3; i++) {
        if(!button_place(w, i, &bx, &bwid)) continue;
        gw_rect(win, bx, y + b->bh - 40, bwid, BUTTON_H, i == b->press ? t->frame : i == b->choice ? t->teal : t->shell);
        gw_text(win, bx + (bwid - gw_text_width(b->label[i])) / 2, y + b->bh - 40 + 6, b->label[i],
                i == b->choice || i == b->press ? t->on_teal : t->ink);
    }
}

/* the box goes (its field wiped), and the focus goes back where it was */
static void close_box(struct gk *ui) {
    struct gk_widget *w = ui->box, *back = ui->saved;
    if(!w) return;
    gk_dirty(w);
    ui->box = NULL;
    ui->saved = NULL;
    gk_free_tree(w);                                    /* the focus in it is forgotten with it */
    gk_set_focus(ui, back && gk_reached(back) ? back : NULL);
}

static void answer(struct gk *ui, int button) {
    struct box *b = ui->box->k;
    char *text = NULL, path[WIN_PATH_MAX];
    int len = 0, n;
    if(b->field && button == 0) gk_field_take(b->field, &text, &len);
    if(b->chooser) {                                    /* a chooser's stand-in: the path, absolute, or nothing */
        ui->choosing = 0;
        n = len <= 0 ? -1 : text[0] == '/' ? snprintf(path, sizeof(path), "%.*s", len, text) :
            snprintf(path, sizeof(path), "%s%s%.*s", b->dir, b->dir[strlen(b->dir) - 1] == '/' ? "" : "/", len, text);
        if(n > 0 && n < (int)sizeof(path)) gk_post_text(ui, GK_EV_CHOSEN, b->id, 1, path, n);
        else gk_post_text(ui, GK_EV_CHOSEN, b->id, 0, NULL, 0);
    } else gk_post_text(ui, GK_EV_ANSWER, b->id, button == 2 ? -1 : button, text, len);
    close_box(ui);
}

static void box_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct box *b = w->k;
    int i, bx, bwid, on = -1;
    (void)buttons;
    for(i = 0; i < 3; i++)
        if(button_place(w, i, &bx, &bwid) && x >= bx && x < bx + bwid && y >= w->y + b->bh - 40 &&
           y < w->y + b->bh - 40 + BUTTON_H) on = i;
    if(pressed & WIN_BUTTON_LEFT) {
        b->press = on;
        gk_dirty(w);
    }
    if((released & WIN_BUTTON_LEFT) && b->press >= 0) {
        i = b->press;
        b->press = -1;
        gk_dirty(w);
        if(i == on) answer(w->ui, i);
    }
}

static const struct gk_class box_class = {
    .measure = box_measure, .arrange = box_arrange, .paint = box_paint, .pointer = box_pointer,
};

void gk_box_layout(struct gk *ui) {
    struct gk_widget *w = ui->box;
    struct box *b = w->k;
    int x, y;
    b->bw = ui->win->w - 16 < 470 ? ui->win->w - 16 : 470;
    b->bh = b->kind == BOX_ASK ? 160 : 146;
    if(b->bw < 120) b->bw = 120;
    x = (ui->win->w - b->bw) / 2;
    y = (ui->win->h - b->bh) / 2;
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    gk_lay(w, x, y, b->bw + 6, b->bh + 7);
}

/* a box's own keys: Esc, Enter, Space, the arrows and Tab between its buttons */
static int own_key(unsigned key) {
    return key == 27 || key == '\r' || key == '\n' || key == ' ' || key == WIN_KEY_LEFT || key == WIN_KEY_RIGHT ||
           key == 9 || key == WIN_KEY_BACKTAB;
}

/* the labelled button next to the one chosen, left (step -1) or right; the chosen one at an end */
static int next_button(const struct box *b, int step) {
    int k;
    for(k = b->choice + step; k >= 0 && k <= 2; k += step) if(b->label[k][0]) return k;
    return b->choice;
}

int gk_box_key(struct gk *ui, unsigned key, unsigned mods) {
    struct box *b = ui->box->k;
    if((mods & WIN_MOD_CTRL) && key != 27) return 0;   /* Ctrl and a key: the line's (Ctrl+V), or nobody's */
    if((mods & WIN_MOD_REPEAT) && own_key(key))         /* a key held down since before (a repeat): answers nothing, a
                                                           button's Enter held over its own question say */
        return b->kind != BOX_ASK || !(key == ' ' || key == WIN_KEY_LEFT || key == WIN_KEY_RIGHT) ? 1 : 0;
    if(key == 27) {                                     /* Esc (and Ctrl+[, which is Esc on a terminal) */
        answer(ui, 2);
        return 1;
    }
    switch(b->kind) {
    case BOX_MESSAGE:
        if(key == '\r' || key == '\n' || key == ' ') answer(ui, 0);
        return 1;
    case BOX_CONFIRM:                                   /* Left and Right stop at the ends: from the safe end, never
                                                           round to the other */
        if(key == WIN_KEY_LEFT || key == WIN_KEY_RIGHT || key == 9 || key == WIN_KEY_BACKTAB) {
            b->choice = next_button(b, key == WIN_KEY_LEFT || key == WIN_KEY_BACKTAB ? -1 : 1);
            gk_dirty(ui->box);
        } else if(key == '\r' || key == '\n' || key == ' ') answer(ui, b->choice);
        return 1;
    default:                                            /* gk_ask(): Enter answers, the rest is the line's */
        if(key == '\r' || key == '\n') {
            answer(ui, 0);
            return 1;
        }
        return key == 9 || key == WIN_KEY_BACKTAB;
    }
}

void gk_box_free(struct gk *ui) {
    if(!ui->box) return;
    gk_free_tree(ui->box);
    ui->box = NULL;
}

/* a new box: the one open answers -1 and goes; the pointer let go of what it held behind */
static struct box *open_box(struct gk *ui, int kind, int id, const char *title, int chooser) {
    struct gk_widget *w;
    struct box *b;
    if(ui->painting) return NULL;
    if(ui->box) answer(ui, 2);
    if(ui->capture) {
        ui->capture->flags &= ~GKW_PRESSED;
        gk_dirty(ui->capture);
        ui->capture = NULL;
    }
    if(!(w = gk_make_in(ui, &box_class, sizeof(struct box)))) {    /* no memory for it: answered as cancelled */
        if(chooser) gk_post_text(ui, GK_EV_CHOSEN, id, 0, NULL, 0);
        else gk_post_text(ui, GK_EV_ANSWER, id, -1, NULL, 0);
        return NULL;
    }
    b = w->k;
    b->kind = kind;
    b->id = id;
    b->chooser = chooser;
    b->press = -1;
    copy_to(b->title, sizeof(b->title), title);
    ui->saved = ui->focus;
    ui->box = w;
    gk_set_focus(ui, NULL);
    ui->layout = 1;
    return b;
}

void gk_message(struct gk *ui, int id, const char *title, const char *line1, const char *line2) {
    struct box *b = open_box(ui, BOX_MESSAGE, id, title, 0);
    if(!b) return;
    copy_to(b->line[0], sizeof(b->line[0]), line1);
    copy_to(b->line[1], sizeof(b->line[1]), line2);
    copy_to(b->label[0], sizeof(b->label[0]), "OK");
}

void gk_confirm_default(struct gk *ui, int id, const char *title, const char *line1, const char *line2,
                        const char *yes, const char *no, const char *cancel, int def) {
    struct box *b = open_box(ui, BOX_CONFIRM, id, title, 0);
    if(!b) return;
    copy_to(b->line[0], sizeof(b->line[0]), line1);
    copy_to(b->line[1], sizeof(b->line[1]), line2);
    copy_to(b->label[0], sizeof(b->label[0]), yes ? yes : "OK");
    copy_to(b->label[1], sizeof(b->label[1]), no);
    copy_to(b->label[2], sizeof(b->label[2]), cancel);
    if(def >= 0 && def <= 2 && b->label[def][0]) b->choice = def;   /* the button Enter answers at first */
}

void gk_confirm(struct gk *ui, int id, const char *title, const char *line1, const char *line2,
                const char *yes, const char *no, const char *cancel) {
    gk_confirm_default(ui, id, title, line1, line2, yes, no, cancel, 0);
}

void gk_ask(struct gk *ui, int id, const char *title, const char *prompt, const char *text, unsigned flags) {
    struct box *b = open_box(ui, BOX_ASK, id, title, 0);
    if(!b) return;
    copy_to(b->line[0], sizeof(b->line[0]), prompt);
    copy_to(b->label[0], sizeof(b->label[0]), "OK");
    copy_to(b->label[2], sizeof(b->label[2]), "Cancel");
    if(!(b->field = gk_field(ui->box, 0, 1024, flags & GK_PASSWORD))) {
        answer(ui, 2);
        return;
    }
    gk_set_text(b->field, text);
    gk_field_select(b->field, 0, -1);                   /* what was put there is replaced by the first key */
    gk_set_focus(ui, b->field);
}

/* gkdesk.c's gk_choose() on a desktop without choosers: the path to type, starting as dir and the name offered */
void gk_box_chooser(struct gk *ui, int id, const char *title, const char *dir, const char *path) {
    struct box *b = open_box(ui, BOX_ASK, id, title, 1);
    if(!b) return;
    ui->choosing = 1;
    copy_to(b->dir, sizeof(b->dir), dir);
    copy_to(b->line[0], sizeof(b->line[0]), "The path (this desktop has no chooser):");
    copy_to(b->label[0], sizeof(b->label[0]), "OK");
    copy_to(b->label[2], sizeof(b->label[2]), "Cancel");
    if(!(b->field = gk_field(ui->box, 0, WIN_PATH_MAX - 1, 0))) {
        answer(ui, 2);
        return;
    }
    gk_set_text(b->field, path);                        /* the cursor at its end: a name typed goes after the folder */
    gk_set_focus(ui, b->field);
}

int gk_asking(const struct gk *ui) {
    return ui->box ? ((const struct box *)ui->box->k)->id : 0;
}

int gk_box_open(const struct gk *ui) {
    return ui->box != NULL;
}

void gk_dismiss(struct gk *ui) {
    if(ui->box && !ui->painting) answer(ui, 2);
}
