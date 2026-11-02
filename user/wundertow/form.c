/*
SurfOS Undertow's window: forms
----------------------
File: form.c  Date: 11/1/26
----------------------
form.h: the model of a page's forms, a person's doing to them, and what a submit sends. Every string a form holds comes
out of WD_FORM_BYTES; a control that does not fit marks its form, which is then never sent: better no request than
one that says less than the page does.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "html_scan.h"
#include "doc.h"
#include "form.h"

/**** building ****/

void fm_begin(struct fm_build *f) {
    memset(f, 0, sizeof(*f));
    f->open = -1;
    f->select = -1;
}

/* Room for one more of an array: 0, or -1 (failed set) */
static int more(struct fm_build *f, void **arr, size_t count, size_t *cap, size_t size) {
    size_t n;
    void *p;
    if(count < *cap) return 0;
    n = *cap ? *cap * 2 : 8;
    p = realloc(*arr, n * size);
    if(!p) {
        f->failed = 1;
        return -1;
    }
    *arr = p;
    *cap = n;
    return 0;
}

/* A string out of the budget: malloc'ed, n bytes and a NUL. NULL past the budget, or out of memory (failed set). */
static char *keep(struct fm_build *f, const char *s, size_t n) {
    char *p;
    if(n + 1 > WD_FORM_BYTES - f->bytes) return NULL;
    p = malloc(n + 1);
    if(!p) {
        f->failed = 1;
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = 0;
    f->bytes += n + 1;
    return p;
}

/* A control is not kept, so its form says less than the page and is not sent. A form= attribute may name a form only the
   page's end knows, or the one that follows: then it may be any. */
void fm_lost(struct wd_document *d, struct fm_build *f, const char *owner) {
    if(owner) {
        if(*owner) f->lost = 1;
    } else if(f->open >= 0) d->forms[f->open].too_much = 1;
}

/* accept-charset, by its first label: a UT_CS_* + 1, where UT_CS_ASCII is a charset this browser does not write; 0: none */
static int accept_of(const char *s) {
    char label[UT_HTML_LABEL_MAX], unknown[UT_HTML_LABEL_MAX];
    size_t n, start = 0;
    if(!s) return 0;
    for(;;) {                                   /* whitespace or commas: HTML 4's list, as browsers read it */
        s += strspn(s, " \t\n\f\r,");
        n = strcspn(s, " \t\n\f\r,");
        if(!n) return 0;
        if(n >= sizeof(label)) return UT_CS_ASCII + 1;
        memcpy(label, s, n);
        label[n] = 0;
        if(fm_differs(label, "unknown")) break; /* HTML 4's word for the page's own */
        s += n;
    }
    if(!fm_differs(label, "utf-16") || !fm_differs(label, "utf-16le") || !fm_differs(label, "utf-16be")) return UT_CS_UTF8 + 1;
    return ut_html_charset((const unsigned char *)"", 0, label, 0, &start, unknown, sizeof(unknown)) + 1;
}

int fm_form(struct wd_document *d, struct fm_build *f, const char *id, const char *action, int action_cut, const char *method,
            const char *accept) {
    struct wd_form *x;
    if(f->open >= 0) return 0;                  /* the standard: a form in a form is no form */
    if(d->form_count >= WD_FORMS_MAX) return 0;
    if(more(f, (void **)&d->forms, d->form_count, &f->forms_cap, sizeof(*d->forms))) return 0;
    x = &d->forms[d->form_count];
    memset(x, 0, sizeof(*x));
    if(action && *action) x->action = keep(f, action, strlen(action));
    else x->here = 1;                           /* an empty action: the page's own address */
    if(action && *action && !x->action) x->too_much = 1;
    x->action_cut = (unsigned char)action_cut;
    if(id && *id && !(x->id = keep(f, id, strlen(id)))) f->lost = 1;  /* a form= naming it would not find it */
    x->method = WM_GET;
    if(method && !fm_differs(method, "post")) x->method = WM_POST;
    else if(method && !fm_differs(method, "dialog")) x->method = WM_DIALOG;
    x->accept = (unsigned char)accept_of(accept);
    f->open = (int)d->form_count++;
    return 1;
}

void fm_form_end(struct fm_build *f) { f->open = -1; }

int fm_differs(const char *s, const char *w) {
    for(; *s && *w; s++, w++) if((*s >= 'A' && *s <= 'Z' ? *s + 32 : *s) != *w) return 1;
    return *s || *w;
}

/* A one-line field holds no line ends, as the standard's value sanitization has it: the length left */
static size_t one_line(char *s) {
    size_t i, n = 0;
    for(i = 0; s[i]; i++) if(s[i] != '\n' && s[i] != '\r') s[n++] = s[i];
    s[n] = 0;
    return n;
}

int fm_control(struct wd_document *d, struct fm_build *f, const struct fm_new *n) {
    struct wd_control *c;
    size_t vlen = n->value ? strlen(n->value) : 0;
    if(d->control_count >= WD_CONTROLS_MAX) {
        fm_lost(d, f, n->owner);
        return -1;
    }
    if(more(f, (void **)&d->controls, d->control_count, &f->controls_cap, sizeof(*d->controls))) return -1;
    c = &d->controls[d->control_count];
    memset(c, 0, sizeof(*c));
    c->kind = (unsigned char)n->kind;
    c->role = (unsigned char)n->role;
    c->form = n->owner && !*n->owner ? -1 : f->open;
    c->run = -1;
    c->checked = c->dflt_checked = n->checked != 0;
    c->disabled = n->disabled || f->off;
    c->readonly = n->readonly != 0;
    c->multiple = n->multiple != 0;
    c->image = n->image != 0;
    c->shaped = n->shaped != 0;
    c->cut = n->cut != 0;
    c->maxlength = n->maxlength;
    c->rows = n->rows;
    c->cols = n->cols;
    c->size = n->size;
    if(n->name && *n->name && !(c->name = keep(f, n->name, strlen(n->name)))) goto cut;
    c->has_value = n->value != NULL;
    c->value = keep(f, n->value ? n->value : "", vlen);
    if(!c->value) goto cut;
    c->dflt = keep(f, n->value ? n->value : "", vlen);
    if(!c->dflt) {
        free(c->value);
        c->value = NULL;
        goto cut;
    }
    c->cap = vlen + 1;
    if(n->kind == WF_TEXT || n->kind == WF_PASSWORD) {
        vlen = one_line(c->value);
        one_line(c->dflt);
    }
    c->len = c->caret = vlen;
    if(n->owner && *n->owner && !(c->owner = keep(f, n->owner, strlen(n->owner)))) goto drop;
    if(n->formaction && *n->formaction && !(c->formaction = keep(f, n->formaction, strlen(n->formaction)))) goto drop;
    c->here = n->formaction && !*n->formaction;
    c->action_cut = n->formaction_cut != 0;
    if(n->formmethod)                           /* of no such method: GET, the standard's for a method that is none */
        c->formmethod = !fm_differs(n->formmethod, "post") ? WM_POST + 1 : !fm_differs(n->formmethod, "dialog") ? WM_DIALOG + 1 : WM_GET + 1;
    if(n->kind == WF_SELECT) f->select = (int)d->control_count;
    return (int)d->control_count++;
drop:
    free(c->value);
    free(c->dflt);
    c->value = c->dflt = NULL;
cut:
    free(c->name);
    free(c->owner);
    c->name = c->owner = NULL;
    fm_lost(d, f, n->owner);
    return -1;
}

void fm_option(struct wd_document *d, struct fm_build *f, const char *value, const char *text, const char *label, int selected,
               int disabled, int cut) {
    struct wd_control *c;
    struct wd_option *o;
    const char *shown = label && *label ? label : text, *sent = value ? value : text;
    if(f->select < 0) return;
    c = &d->controls[f->select];                /* (what is cut shows at the page's end: this control's form may be any) */
    if(cut) c->cut = 1;
    if(d->option_count >= WD_OPTIONS_MAX ||
       more(f, (void **)&d->options, d->option_count, &f->options_cap, sizeof(*d->options))) {
        c->cut = 1;
        return;
    }
    o = &d->options[d->option_count];
    memset(o, 0, sizeof(*o));
    o->label = keep(f, shown, strlen(shown));
    o->value = o->label ? keep(f, sent, strlen(sent)) : NULL;
    if(!o->label || !o->value) {
        free(o->label);
        free(o->value);
        c->cut = 1;
        return;
    }
    if(!c->noptions) c->first_option = (unsigned int)d->option_count;
    o->on = o->dflt = selected != 0;
    o->disabled = disabled != 0;
    if(selected && !c->multiple) {              /* one of many: the last selected is the one */
        unsigned int i;
        for(i = 0; i < c->noptions; i++) d->options[c->first_option + i].on = d->options[c->first_option + i].dflt = 0;
    }
    c->noptions++;
    d->option_count++;
}

/* A one-of-many of one row with nothing chosen shows its first option that can be: the standard's "ask for a reset" */
static void first_chosen(struct wd_document *d, struct wd_control *c) {
    unsigned int i;
    if(c->multiple || c->size > 1 || !c->noptions) return;
    for(i = 0; i < c->noptions; i++) if(d->options[c->first_option + i].on) return;
    for(i = 0; i < c->noptions; i++)
        if(!d->options[c->first_option + i].disabled) {
            d->options[c->first_option + i].on = 1;
            return;
        }
}

void fm_select_end(struct wd_document *d, struct fm_build *f) {
    if(f->select >= 0) first_chosen(d, &d->controls[f->select]);
    f->select = -1;
}

/* "&name;" left in a string */
static int unread_ref(const char *s) {
    static const char word[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for(; (s = strchr(s, '&')); s++) {
        size_t n = strspn(s + 1, word);
        if(n && s[1 + n] == ';') return 1;
    }
    return 0;
}

int fm_unsure(const struct wd_document *d, const char *s) {
    if(!s) return 0;
    if(unread_ref(s)) return 1;
    if(d->form_charset != UT_CS_ASCII) return 0;
    for(; *s; s++) if(*s == '?' || (unsigned char)*s >= 0x80) return 1;
    return 0;
}

/* Can this control's name go, whatever is done with it? (An image button's goes without a name too, but it adds to it.) */
static int may_go(const struct wd_control *c) {
    return c->name && !c->disabled && (c->kind != WF_BUTTON || c->role == WC_SUBMIT);
}

void fm_finish(struct wd_document *d, struct fm_build *f, int page_cut) {
    size_t i, k;
    for(i = 0; i < d->control_count; i++) {
        struct wd_control *c = &d->controls[i];
        if(c->owner) {                          /* form=: the form of that id, or none */
            if(fm_unsure(d, c->owner)) f->lost = 1;
            c->form = -1;
            for(k = 0; k < d->form_count; k++)
                if(d->forms[k].id && !strcmp(d->forms[k].id, c->owner)) {
                    c->form = (int)k;
                    break;
                }
        }
        if(c->form < 0) continue;
        if(c->kind == WF_PASSWORD) d->forms[c->form].password = 1;
        if(c->cut) d->forms[c->form].too_much = 1;
        if(!may_go(c)) continue;
        if(fm_unsure(d, c->name)) d->forms[c->form].too_much = 1;
        if(c->image || c->kind == WF_FILE) continue;        /* their values do not go */
        if(fm_unsure(d, c->dflt)) d->forms[c->form].too_much = 1;
        for(k = 0; k < c->noptions; k++) {
            const struct wd_option *o = &d->options[c->first_option + k];
            if(!o->disabled && fm_unsure(d, o->value)) d->forms[c->form].too_much = 1;
        }
        if(d->form_charset == UT_CS_ASCII && c->kind == WC_HIDDEN && c->name && !fm_differs(c->name, "_charset_"))
            d->forms[c->form].too_much = 1;     /* it would name the page's charset */
    }
    for(i = 0; i < d->control_count; i++) {     /* of a radio group, the last checked is the one */
        struct wd_control *c = &d->controls[i];
        if(c->kind != WF_RADIO || !c->checked) continue;
        for(k = 0; k < i; k++) {
            struct wd_control *e = &d->controls[k];
            if(e->kind == WF_RADIO && e->checked && e->form == c->form && e->name && c->name && !strcmp(e->name, c->name))
                e->checked = e->dflt_checked = 0;
        }
    }
    if(page_cut || f->lost) for(k = 0; k < d->form_count; k++) d->forms[k].too_much = 1;
    d->form_bytes = f->bytes + d->form_count * sizeof(*d->forms) + d->control_count * sizeof(*d->controls) +
                    d->option_count * sizeof(*d->options);
}

/* Nothing of a password stays in memory */
static void wipe(struct wd_control *c) {
    if(c->value) memset(c->value, 0, c->cap);
    if(c->dflt) memset(c->dflt, 0, strlen(c->dflt));
}

void fm_free(struct wd_document *d) {
    size_t i;
    for(i = 0; d->forms && i < d->form_count; i++) {
        free(d->forms[i].action);
        free(d->forms[i].id);
    }
    free(d->forms);
    for(i = 0; d->controls && i < d->control_count; i++) {
        struct wd_control *c = &d->controls[i];
        if(c->kind == WF_PASSWORD) wipe(c);
        free(c->name);
        free(c->value);
        free(c->dflt);
        free(c->owner);
        free(c->formaction);
    }
    free(d->controls);
    for(i = 0; d->options && i < d->option_count; i++) {
        free(d->options[i].value);
        free(d->options[i].label);
    }
    free(d->options);
}

/**** what a box shows ****/

const char *fm_shown(const struct wd_document *d, const struct wd_control *c, char *stars, size_t *n) {
    if(c->kind == WF_SELECT) {
        unsigned int i, on = 0;
        const struct wd_option *first = NULL;
        for(i = 0; i < c->noptions; i++) {
            const struct wd_option *o = &d->options[c->first_option + i];
            if(!o->on) continue;
            if(!first) first = o;
            on++;
        }
        if(c->multiple && on > 1) {
            *n = (size_t)snprintf(stars, 64, "%u chosen", on);
            return stars;
        }
        *n = first ? strlen(first->label) : 0;
        return first ? first->label : "";
    }
    if(c->kind == WF_PASSWORD) {
        size_t i, cells = 0;                    /* a star a character */
        for(i = c->first; i < c->len; i++) if(((unsigned char)c->value[i] & 0xc0) != 0x80) cells++;
        *n = cells > 64 ? 64 : cells;
        memset(stars, '*', *n);
        return stars;
    }
    {
        size_t first = c->first > c->len ? c->len : c->first;
        *n = c->len - first;
        return c->value + first;
    }
}

/**** a person's doing ****/

void fm_click(struct wd_document *d, int control) {
    struct wd_control *c = &d->controls[control];
    size_t i;
    if(c->kind == WF_CHECKBOX) c->checked = !c->checked;
    else if(c->kind == WF_RADIO) {
        for(i = 0; i < d->control_count; i++) {
            struct wd_control *e = &d->controls[i];
            if(e->kind == WF_RADIO && e->form == c->form && e->name && c->name && !strcmp(e->name, c->name)) e->checked = 0;
        }
        c->checked = 1;
    }
}

void fm_reset(struct wd_document *d, int form) {
    size_t i;
    unsigned int k;
    for(i = 0; i < d->control_count; i++) {
        struct wd_control *c = &d->controls[i];
        if(c->form != form) continue;
        c->checked = c->dflt_checked;
        for(k = 0; k < c->noptions; k++) d->options[c->first_option + k].on = d->options[c->first_option + k].dflt;
        first_chosen(d, c);
        c->len = c->caret = strlen(c->dflt);    /* (it came out of the value's first room, which only grows) */
        memcpy(c->value, c->dflt, c->len + 1);
        c->first = 0;
        c->top = 0;
    }
}

void fm_pick(struct wd_document *d, int control, unsigned int option) {
    struct wd_control *c = &d->controls[control];
    unsigned int i;
    if(option >= c->noptions || d->options[c->first_option + option].disabled) return;
    if(c->multiple) {
        d->options[c->first_option + option].on ^= 1;
        return;
    }
    for(i = 0; i < c->noptions; i++) d->options[c->first_option + i].on = i == option;
}

int fm_chosen(const struct wd_document *d, int control) {
    const struct wd_control *c = &d->controls[control];
    unsigned int i;
    for(i = 0; i < c->noptions; i++) if(d->options[c->first_option + i].on) return (int)i;
    return -1;
}

static int char_start(const char *s, size_t i) { return ((unsigned char)s[i] & 0xc0) != 0x80; }

/* Where the character after the one at i starts */
static size_t after(const struct wd_control *c, size_t i) {
    for(i++; i < c->len && !char_start(c->value, i); i++) ;
    return i;
}

/* Room for one more byte. The old block is emptied: of a password nothing stays behind. */
static int room(struct wd_control *c) {
    char *p;
    size_t cap;
    if(c->len + 1 < c->cap) return 0;
    cap = c->cap * 2 + 16;
    if(cap > WD_VALUE_MAX + 1) cap = WD_VALUE_MAX + 1;
    if(c->len + 1 >= cap) return -1;
    p = malloc(cap);
    if(!p) return -1;
    memcpy(p, c->value, c->len + 1);
    memset(c->value, 0, c->cap);
    free(c->value);
    c->value = p;
    c->cap = cap;
    return 0;
}

/* maxlength counts characters */
static size_t characters(const struct wd_control *c) {
    size_t i, n = 0;
    for(i = 0; i < c->len; i++) if(char_start(c->value, i)) n++;
    return n;
}

static int insert(struct wd_control *c, char ch) {
    if(c->readonly || (c->maxlength >= 0 && characters(c) >= (size_t)c->maxlength) || room(c)) return 0;
    memmove(c->value + c->caret + 1, c->value + c->caret, c->len - c->caret + 1);
    c->value[c->caret++] = ch;
    c->len++;
    return 1;
}

static int edit(struct wd_control *c, unsigned int key) {
    int line, col;
    size_t k;
    switch(key) {
    case 127: case 8:
        if(c->readonly || !c->caret) return 0;
        k = c->caret - 1;
        while(k && !char_start(c->value, k)) k--;
        memmove(c->value + k, c->value + c->caret, c->len - c->caret + 1);
        c->len -= c->caret - k;
        c->caret = k;
        return 1;
    case 0x108:                                 /* Delete */
        if(c->readonly || c->caret >= c->len) return 0;
        k = after(c, c->caret);
        memmove(c->value + c->caret, c->value + k, c->len - k + 1);
        c->len -= k - c->caret;
        return 1;
    case 0x102:                                 /* Left */
        if(!c->caret) return 0;
        while(c->caret && !char_start(c->value, --c->caret)) ;
        return 1;
    case 0x103:                                 /* Right */
        if(c->caret >= c->len) return 0;
        c->caret = after(c, c->caret);
        return 1;
    case 0x104:                                 /* Home: the line's start */
        if(c->kind == WF_AREA) {
            fm_where(c, c->caret, &line, &col);
            c->caret = fm_at(c, line, 0);
        } else c->caret = 0;
        return 1;
    case 0x105:                                 /* End: the line's end */
        if(c->kind == WF_AREA) {
            fm_where(c, c->caret, &line, &col);
            c->caret = fm_on_line(c, line, 1 << 30);
        } else c->caret = c->len;
        return 1;
    case 0x100: case 0x101:                     /* Up, Down: a line of an area */
        if(c->kind != WF_AREA) return 0;
        fm_where(c, c->caret, &line, &col);
        if(key == 0x100 && !line) return 0;
        c->caret = fm_on_line(c, key == 0x100 ? line - 1 : line + 1, col);
        return 1;
    case 13: case 10:
        return c->kind == WF_AREA ? insert(c, '\n') : 0;
    }
    if(key < 0x20 || key > 0x7e) return 0;      /* the keyboard types ASCII */
    return insert(c, (char)key);
}

int fm_key(struct wd_control *c, unsigned int key) {
    int changed = edit(c, key);
    if(c->first > c->caret) c->first = c->caret;    /* what a text shows from stays inside it */
    return changed;
}

/* An area's width in characters: its cols where layout.c gives its box that many, else the least it gives it */
static int area_cols(const struct wd_control *c) { return c->rows && c->cols ? c->cols : 20; }

void fm_where(const struct wd_control *c, size_t at, int *line, int *col) {
    int cols = area_cols(c), l = 0, n = 0;
    size_t i, k;
    for(i = 0; i < at && i < c->len; i = k) {
        k = after(c, i);
        if(c->value[i] == '\n') {
            l++;
            n = 0;
        } else if(++n == cols && k < c->len && c->value[k] != '\n') {
            l++;
            n = 0;
        }
    }
    *line = l;
    *col = n;
}

size_t fm_at(const struct wd_control *c, int line, int col) {
    int cols = area_cols(c), l = 0, n = 0;
    size_t i, k;
    for(i = 0; i < c->len; i = k) {
        if(l == line && (n == col || c->value[i] == '\n')) return i;
        k = after(c, i);
        if(c->value[i] == '\n') {
            l++;
            n = 0;
        } else if(++n == cols && k < c->len && c->value[k] != '\n') {
            if(l == line) return k;             /* (past the line's last character: the next line's start) */
            l++;
            n = 0;
        }
    }
    return c->len;
}

size_t fm_on_line(const struct wd_control *c, int line, int col) {
    size_t at = fm_at(c, line, col);
    int l, n;
    fm_where(c, at, &l, &n);
    if(l > line) while(at && !char_start(c->value, --at)) ;
    return at;
}

size_t fm_byte_at(const struct wd_control *c, int cell) {
    size_t i = c->first > c->len ? c->len : c->first;
    int n = 0;
    for(; i < c->len && n < cell; n++) i = after(c, i);
    return i;
}

void fm_keep_in_view(struct wd_control *c, int cells) {
    size_t i;
    int n;
    if(cells < 1) cells = 1;
    if(c->caret < c->first) c->first = c->caret;
    for(;;) {
        n = 0;
        for(i = c->first; i < c->caret; i++) if(char_start(c->value, i)) n++;
        if(n < cells) break;
        c->first = after(c, c->first);
    }
}

/**** what is sent ****/

struct query {
    char *out;
    size_t n, cap;
    int over;
    int cs;                             /* UT_CS_1252, else UTF-8 */
};

static void q_put(struct query *q, char c) {
    if(q->n + 1 >= q->cap) {
        q->over = 1;
        return;
    }
    q->out[q->n++] = c;
}

/* The urlencoded byte serializer: a space is a plus, these stay, the rest is %XX */
static void q_byte(struct query *q, unsigned char c) {
    static const char hex[] = "0123456789ABCDEF";
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_')
        q_put(q, (char)c);
    else if(c == ' ') q_put(q, '+');
    else {
        q_put(q, '%');
        q_put(q, hex[c >> 4]);
        q_put(q, hex[c & 15]);
    }
}

/* windows-1252's byte for a character, or -1 */
static int cp1252(unsigned int cp) {
    static const unsigned short high[32] = {
        0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
        0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
    };
    int i;
    if(cp < 0x80 || (cp >= 0xa0 && cp <= 0xff)) return (int)cp;
    if(cp < 0xa0 && !high[cp - 0x80]) return (int)cp;   /* its five holes stand for themselves */
    for(i = 0; i < 32; i++) if(high[i] == cp) return 0x80 + i;
    return -1;
}

static void q_char(struct query *q, unsigned int cp) {
    unsigned char u[4];
    size_t k, i;
    int b;
    if(q->cs != UT_CS_1252) {
        k = ut_utf8_encode(cp, u);
        for(i = 0; i < k; i++) q_byte(q, u[i]);
        return;
    }
    b = cp1252(cp);
    if(b >= 0) q_byte(q, (unsigned char)b);
    else {                                      /* what the page's charset cannot say: &#NNN;, as the standard has it */
        char r[16];
        snprintf(r, sizeof(r), "&#%u;", cp);
        for(i = 0; r[i]; i++) q_byte(q, (unsigned char)r[i]);
    }
}

/* A name or a value, in UTF-8: every line ending a CR LF */
static void q_text(struct query *q, const char *s) {
    size_t n = strlen(s), i = 0;
    while(i < n) {
        unsigned int cp;
        i += ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        if(cp == '\n' || cp == '\r') {
            if(cp == '\r' && i < n && s[i] == '\n') i++;
            q_char(q, '\r');
            q_char(q, '\n');
        } else q_char(q, cp);
    }
}

static void q_entry(struct query *q, const char *name, const char *value) {
    if(q->n) q_put(q, '&');
    q_text(q, name);
    q_put(q, '=');
    q_text(q, value);
}

/* The standard's entry list of a form, submitter being what submitted it */
static void entries(const struct wd_document *d, int form, int submitter, int x, int y, struct query *q) {
    size_t i;
    unsigned int k;
    for(i = 0; i < d->control_count; i++) {
        const struct wd_control *c = &d->controls[i];
        if(c->form != form || c->disabled) continue;
        if(c->kind == WF_BUTTON && (int)i != submitter) continue;
        if((c->kind == WF_CHECKBOX || c->kind == WF_RADIO) && !c->checked) continue;
        if(c->image) {
            char name[UT_FIELD_MAX + 8], v[16];
            snprintf(name, sizeof(name), "%s%sx", c->name ? c->name : "", c->name ? "." : "");
            snprintf(v, sizeof(v), "%d", x);
            q_entry(q, name, v);
            snprintf(name, sizeof(name), "%s%sy", c->name ? c->name : "", c->name ? "." : "");
            snprintf(v, sizeof(v), "%d", y);
            q_entry(q, name, v);
            continue;
        }
        if(!c->name) continue;
        if(c->kind == WF_SELECT) {
            for(k = 0; k < c->noptions; k++) {
                const struct wd_option *o = &d->options[c->first_option + k];
                if(o->on && !o->disabled) q_entry(q, c->name, o->value);
            }
            continue;
        }
        if(c->kind == WF_CHECKBOX || c->kind == WF_RADIO) {
            q_entry(q, c->name, c->has_value ? c->value : "on");
            continue;
        }
        if(c->kind == WC_HIDDEN && !fm_differs(c->name, "_charset_")) {
            q_entry(q, c->name, q->cs == UT_CS_1252 ? "windows-1252" : "UTF-8");
            continue;
        }
        q_entry(q, c->name, c->kind == WF_FILE ? "" : c->value);
    }
}

const char *fm_submit(const struct wd_document *d, int form, int submitter, int x, int y, char *out, size_t cap) {
    const struct wd_control *s = submitter >= 0 ? &d->controls[submitter] : NULL;
    const struct wd_form *f;
    const char *action, *mark;
    struct query q;
    size_t base;
    int method;
    if(form < 0 || (size_t)form >= d->form_count) return "This button belongs to no form: nothing was sent";
    f = &d->forms[form];
    method = s && s->formmethod ? s->formmethod - 1 : f->method;
    q.cs = f->accept ? f->accept - 1 : d->form_charset;
    if(method == WM_POST) return "This form sends by POST, which this browser does not do: nothing was sent";
    if(method == WM_DIALOG) return "This form closes a dialog, which this browser does not have: nothing was sent";
    if(f->password) return "This form has a password field: nothing was sent, as a password must never go into an address";
    if(f->too_much) return "This form is more than this browser keeps of a form: nothing was sent";
    if(f->accept == UT_CS_ASCII + 1) return "This form asks for a charset this browser does not write: nothing was sent";
    action = s && s->formaction ? s->formaction : f->action;
    if(!action || (s && s->action_cut))
        return "This form's address is not one this browser can use: nothing was sent";
    mark = action + strcspn(action, "?#");      /* the query is the form's now; a fragment stays */
    base = (size_t)(mark - action);
    if(base + 2 >= cap) return "This form's address is too long: nothing was sent";
    memcpy(out, action, base);
    out[base] = '?';
    q.out = out + base + 1;
    q.n = 0;
    q.cap = cap - base - 1;
    q.over = 0;
    entries(d, form, submitter, x, y, &q);
    if(!q.over && action[base + strcspn(action + base, "#")]) {
        const char *hash = action + base + strcspn(action + base, "#");
        size_t i;
        for(i = 0; hash[i]; i++) q_put(&q, hash[i]);
    }
    if(q.over) return "This form's fields do not fit in an address: nothing was sent";
    q.out[q.n] = 0;
    return NULL;
}

int fm_default_button(const struct wd_document *d, int form) {
    size_t i;
    int fields = 0;
    for(i = 0; i < d->control_count; i++) {
        const struct wd_control *c = &d->controls[i];
        if(c->form != form) continue;
        if(c->kind == WF_BUTTON && c->role == WC_SUBMIT) return c->disabled ? -2 : (int)i;
        if((c->kind == WF_TEXT && !c->shaped) || c->kind == WF_PASSWORD) fields++;
    }
    return fields > 1 ? -2 : -1;
}

void fm_forget(struct wd_document *d) {
    size_t i;
    for(i = 0; i < d->control_count; i++) {
        struct wd_control *c = &d->controls[i];
        if(c->kind != WF_PASSWORD || !c->value) continue;
        wipe(c);
        c->len = c->caret = c->first = 0;
    }
}
