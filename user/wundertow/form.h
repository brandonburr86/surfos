/*
SurfOS Undertow's window: forms
----------------------
File: form.h  Date: 11/1/26
----------------------
What doc.h's forms do: the building of them, which doc.c calls as it reads a page; what a person does to a control; and
what is sent when a person submits. Nothing here sends anything by itself: fm_submit() makes an address, and only
browser.c's handling of a key or a click calls it. No page can: there are no scripts.

A submit is a GET, encoded as the HTML standard's application/x-www-form-urlencoded (UTF-8, or windows-1252 on a page
in it, where a character it does not have goes as a numeric reference, as browsers do), with the entries of the
standard's list: not what is disabled, not a box not ticked, not a button but the one that submitted. What it does not
do it says, and sends nothing:

- a form that POSTs (or a button with formmethod=post) is not sent, nor one with method=dialog;
- a form with a password field is not sent at all: a password never goes into an address, a history or a server's log;
- a form that lost anything to a limit is not sent: it would be another form than the page's;
- what would not fit in an address is not sent.

Password fields are shown as stars, never otherwise, and what was typed in them goes when their page is left
(fm_forget()), so the history holds none.

Limits: WD_FORMS_MAX forms, WD_CONTROLS_MAX controls, WD_OPTIONS_MAX options, WD_FORM_BYTES of names, values and labels
together, a page; WD_VALUE_MAX bytes in a field.
*/

#ifndef WUNDERTOW_FORM_H
#define WUNDERTOW_FORM_H

#include <stddef.h>
#include "doc.h"

struct fm_new {                         /* a control as a page has it */
    int kind;                           /* WF_*, or WC_HIDDEN */
    int role;                           /* a button's: WC_SUBMIT, WC_RESET, WC_PUSH */
    const char *name, *value;           /* NULL: none */
    int value_cut;                      /* its value was longer than was kept */
    int checked, disabled, readonly, multiple, image;
    int maxlength;                      /* -1: none */
    int rows, cols, size;               /* 0: none */
    const char *owner;                  /* its form= attribute, or NULL */
    const char *formaction;             /* a button's, or NULL */
    int formaction_cut;
    const char *formmethod;             /* a button's, or NULL */
};

struct fm_build {                       /* doc.c's, while it reads */
    size_t forms_cap, controls_cap, options_cap;
    size_t bytes;                       /* of WD_FORM_BYTES */
    int open;                           /* the form the next control is in, -1: none */
    int select;                         /* the open select's control, -1: none */
    int failed;                         /* out of memory: the page fails */
    int off;                            /* open fieldsets that are disabled */
};

void fm_begin(struct fm_build *f);

/* 0 when s is the lowercase word w, in any case */
int fm_differs(const char *s, const char *w);

/* <form>: 1 when it opened one, 0 for one inside another, which the standard ignores */
int fm_form(struct wd_document *d, struct fm_build *f, const char *id, const char *action, int action_cut, const char *method);
void fm_form_end(struct fm_build *f);

/* A control: its number, or -1 when it is not kept (a limit: its form is marked, and is never sent) */
int fm_control(struct wd_document *d, struct fm_build *f, const struct fm_new *n);

/* An option of the open select, whose label is label (collapsed text); value NULL: the label */
void fm_option(struct wd_document *d, struct fm_build *f, const char *value, const char *label, int selected, int disabled);

/* The open select's end: the first option is chosen when a one-of-many has none */
void fm_select_end(struct wd_document *d, struct fm_build *f);

/* The page's end: form= attributes, radio groups, what a form holds. page_cut: the page stopped at a limit, so that no
   form can be known to be whole. */
void fm_finish(struct wd_document *d, struct fm_build *f, int page_cut);

/* doc.c's wd_free() */
void fm_free(struct wd_document *d);

/* What a control's box says: its text, or a select's choice (n bytes; a password's stars in stars, which holds 64) */
const char *fm_shown(const struct wd_document *d, const struct wd_control *c, char *stars, size_t *n);

/* A person's doing */
void fm_click(struct wd_document *d, int control);      /* a checkbox ticked or not; a radio button chosen */
void fm_reset(struct wd_document *d, int form);
void fm_pick(struct wd_document *d, int control, unsigned int option);    /* one of a select's; multiple: on or off */
int fm_chosen(const struct wd_document *d, int control);    /* the first chosen, from 0, or -1 */
int fm_key(struct wd_control *c, unsigned int key);     /* typing in a text field or area: 1 when something changed */

/* An area's text as its box shows it: the line (from 0) and column of a byte, and the byte at a line and column; lines
   end at a line feed and at cols characters */
void fm_where(const struct wd_control *c, size_t at, int *line, int *col);
size_t fm_at(const struct wd_control *c, int line, int col);

/* The byte in a text field at a pixel x from its text's start, with cells of cw pixels; and the first byte to show so
   that the caret stays within cells cells */
size_t fm_byte_at(const struct wd_control *c, int cell);
void fm_keep_in_view(struct wd_control *c, int cells);

/* The address a submit goes to: NULL and out (cap bytes), or the sentence that says why nothing is sent. submitter: the
   control that did it, or -1 (Enter with no button); x, y where an image button was pressed. */
const char *fm_submit(const struct wd_document *d, int form, int submitter, int x, int y, char *out, size_t cap);

/* What Enter in a field submits with: the form's first submit button, -1 when it has none, -2 when it may not submit
   by itself (the standard: no button, and more than one field that takes text) */
int fm_default_button(const struct wd_document *d, int form);

/* What was typed in a page's password fields goes */
void fm_forget(struct wd_document *d);

#endif
