/*
SurfOS user programs: reading a patch
--------------------
File: phunk.c   Date: 11/01/26
--------------------
See patch.h. A patch is text from anyone, so nothing in it is trusted: every number is checked for size, a hunk
ends where its counts say and nowhere else (a line of it may well look like a header), and lines
around sections are skipped, since mail and commit messages surround them. A line is any length, up to
memory; running out is an error returned, never an exit, because user/bin/patch.c may hold a
half-written file that must be cleaned up.
*/

#include "patch.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/**** lines of the input ****/

int pi_start(struct pinput *p, struct dsrc *src) {
    memset(p, 0, sizeof(*p));
    return dr_start(&p->r, src, 0, src->stream ? DSRC_ALL : src->size);
}

void pi_stop(struct pinput *p) {
    dr_stop(&p->r);
    free(p->line);
    p->line = NULL;
}

static int bad(struct pinput *p, const char *what) {
    snprintf(p->why, sizeof(p->why), "line %lu: %s", p->lineno, what);
    return -1;
}

/* the next line: 1, 0 at the end, -1 */
static int next_line(struct pinput *p) {
    size_t n = 0;
    int c;
    if(p->again) {
        p->again = 0;
        return 1;
    }
    p->had_newline = 0;
    while((c = dr_getc(&p->r)) >= 0) {
        if(n + 2 > p->cap) {
            size_t cap = p->cap ? p->cap * 2 : 256;
            char *q = cap > p->cap ? realloc(p->line, cap) : NULL;
            if(!q) return bad(p, "a line too long for memory");
            p->line = q;
            p->cap = cap;
        }
        if(c == '\n') {
            p->had_newline = 1;
            break;
        }
        p->line[n++] = (char)c;
    }
    if(p->r.failed) {
        snprintf(p->why, sizeof(p->why), "%s", strerror(p->r.failed));
        return -1;
    }
    if(c < 0 && !n) return 0;
    p->line[n] = 0;
    p->len = n;
    p->lineno++;
    return 1;
}

static void read_again(struct pinput *p) {
    p->again = 1;
}

static int starts(const struct pinput *p, const char *word) {
    size_t n = strlen(word);
    return p->len >= n && !memcmp(p->line, word, n);
}

/* a number at *s: 1 and *s after it, or 0 for none or one that does not fit */
static int number(const char **s, unsigned *v) {
    const char *p = *s;
    unsigned n = 0;
    if(*p < '0' || *p > '9') return 0;
    for(; *p >= '0' && *p <= '9'; p++) {
        unsigned d = (unsigned)(*p - '0');
        if(n > (0xFFFFFFFFu - d) / 10) return 0;
        n = n * 10 + d;
    }
    *s = p;
    *v = n;
    return 1;
}

/**** file names ****/

static int digits_at(const char *s, int n) {
    int i;
    for(i = 0; i < n; i++)
        if(s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* in names, each 3 letters: is the word at s one? */
static int word_in(const char *s, const char *names) {
    for(; *names; names += 3)
        if(!strncmp(s, names, 3)) return 1;
    return 0;
}

/* the dates that follow a name with no TAB before them: 2026-11-01, or Sun Nov  1. Any other words are part of the name */
static int is_date_at(const char *s) {
    if(digits_at(s, 4) && s[4] == '-' && digits_at(s + 5, 2) && s[7] == '-' && digits_at(s + 8, 2)) return !s[10] || s[10] == ' ';
    return word_in(s, "SunMonTueWedThuFriSat") && s[3] == ' ' && word_in(s + 4, "JanFebMarAprMayJunJulAugSepOctNovDec") && s[7] == ' ';
}

/* A header's date that says the file was not there: diff -N writes the first moment of 1970, UTC, for a side that is missing,
   in the zone it runs in. 1969-12-31 18:00:00.000000000 -0600 is one; with no zone only 1970-01-01 00:00:00 is, and the
   older form's Thu Jan  1 00:00:00 1970 */
static int is_epoch(const char *s) {
    unsigned y, mo, d, hh, mi, se, zone;
    long minutes;
    const char *z;
    while(*s == ' ' || *s == '\t') s++;
    if(word_in(s, "SunMonTueWedThuFriSat") && s[3] == ' ')
        return !strncmp(s + 4, "Jan  1 00:00:00 1970", 20) && (!s[24] || s[24] == ' ' || s[24] == '\r');
    if(!number(&s, &y) || *s++ != '-' || !number(&s, &mo) || *s++ != '-' || !number(&s, &d) || *s++ != ' ') return 0;
    if(!number(&s, &hh) || *s++ != ':' || !number(&s, &mi) || *s++ != ':' || !number(&s, &se)) return 0;
    if(*s == '.')
        for(s++; *s == '0'; s++);               /* any fraction: nothing but zeros */
    if((*s >= '0' && *s <= '9') || se || hh > 23 || mi > 59) return 0;
    if(y == 1970 && mo == 1 && d == 1) minutes = (long)hh * 60 + mi;
    else if(y == 1969 && mo == 12 && d == 31) minutes = (long)hh * 60 + mi - 24 * 60;
    else return 0;
    while(*s == ' ') s++;
    if(*s == '+' || *s == '-') {
        long sign = *s == '-' ? -1 : 1;
        z = ++s;
        if(!number(&s, &zone) || s - z != 4 || zone % 100 > 59) return 0;
        minutes -= sign * (long)(zone / 100 * 60 + zone % 100);
    }
    return minutes == 0;
}

/* the name in a header's text: up to a TAB, or the date after it, or the line's end; quotes taken off when
   there is nothing in them to unescape. Too long for a path: none. *epoch: the date says the file was not there */
static void take_name(const char *s, char *out, int *epoch) {
    size_t n;
    const char *tab, *date = NULL;
    out[0] = 0;
    while(*s == ' ') s++;
    tab = strchr(s, '\t');
    n = tab ? (size_t)(tab - s) : strlen(s);
    if(tab) date = tab + 1;
    else {
        const char *p;
        for(p = s; (p = strchr(p, ' ')); p++) {
            if(is_date_at(p + 1)) {
                n = (size_t)(p - s);
                date = p + 1;
                break;
            }
        }
    }
    if(epoch) *epoch = date && is_epoch(date);
    while(n && (s[n - 1] == ' ' || s[n - 1] == '\r')) n--;     /* a patch with CR LF line ends: the CR is no part of a name */
    if(n >= 2 && s[0] == '"' && s[n - 1] == '"' && !memchr(s, '\\', n)) {
        s++;
        n -= 2;
    }
    if(n && n < PATCH_NAME_MAX) {
        memcpy(out, s, n);
        out[n] = 0;
    }
}

/* the last two words of a diff line, diff -r's: the names a normal diff has nowhere else */
static void names_of_command(const struct pinput *p, struct psection *s) {
    const char *end = p->line + p->len, *b1, *e1;
    const char *e2 = end, *b2;
    while(e2 > p->line && (e2[-1] == ' ' || e2[-1] == '\r')) e2--;
    for(b2 = e2; b2 > p->line && b2[-1] != ' '; b2--);
    for(e1 = b2; e1 > p->line && e1[-1] == ' '; e1--);
    for(b1 = e1; b1 > p->line && b1[-1] != ' '; b1--);
    s->old_name[0] = s->new_name[0] = 0;
    if(b1 == p->line || e1 - b1 >= PATCH_NAME_MAX || e2 - b2 >= PATCH_NAME_MAX) return;
    memcpy(s->old_name, b1, (size_t)(e1 - b1));
    s->old_name[e1 - b1] = 0;
    memcpy(s->new_name, b2, (size_t)(e2 - b2));
    s->new_name[e2 - b2] = 0;
}

/**** sections ****/

/* 5a6,7 and its kin */
static int is_command(const struct pinput *p) {
    const char *s = p->line;
    unsigned v;
    if(!number(&s, &v)) return 0;
    if(*s == ',') {
        s++;
        if(!number(&s, &v)) return 0;
    }
    if(*s != 'a' && *s != 'c' && *s != 'd') return 0;
    s++;
    if(!number(&s, &v)) return 0;
    if(*s == ',') {
        s++;
        if(!number(&s, &v)) return 0;
    }
    return !*s && p->len < sizeof(((struct psection *)0)->command);
}

int pp_next_section(struct pinput *p, struct psection *s) {
    char diff_old[PATCH_NAME_MAX], diff_new[PATCH_NAME_MAX];
    int r, have = 0;
    memset(s, 0, sizeof(*s));
    diff_old[0] = diff_new[0] = 0;
    for(;;) {
        if(!have) {
            if((r = next_line(p)) <= 0) return r;
        }
        have = 0;
        if(starts(p, "Index: ")) take_name(p->line + 7, s->index_name, NULL);
        else if(starts(p, "diff ") && !starts(p, "diff --git")) {
            names_of_command(p, s);
            memcpy(diff_old, s->old_name, sizeof(diff_old));
            memcpy(diff_new, s->new_name, sizeof(diff_new));
            s->old_name[0] = s->new_name[0] = 0;
        } else if(starts(p, "--- ")) {
            take_name(p->line + 4, s->old_name, &s->old_epoch);
            if((r = next_line(p)) <= 0) return r;
            if(starts(p, "+++ ")) {
                take_name(p->line + 4, s->new_name, &s->new_epoch);
                if((r = next_line(p)) <= 0) return r;
                if(starts(p, "@@ -")) {
                    read_again(p);
                    s->format = 'u';
                    p->started = 1;
                    return 1;
                }
            }
            s->old_name[0] = s->new_name[0] = 0;
            s->old_epoch = s->new_epoch = 0;
            have = 1;
        } else if(starts(p, "*** ")) {
            take_name(p->line + 4, s->old_name, &s->old_epoch);
            if((r = next_line(p)) <= 0) return r;
            if(starts(p, "--- ")) {
                take_name(p->line + 4, s->new_name, &s->new_epoch);
                if((r = next_line(p)) <= 0) return r;
                if(starts(p, "***************")) {
                    read_again(p);
                    s->format = 'c';
                    p->started = 1;
                    return 1;
                }
            }
            s->old_name[0] = s->new_name[0] = 0;
            s->old_epoch = s->new_epoch = 0;
            have = 1;
        } else if(starts(p, "@@ -")) {          /* hunks with no headers: a file named on the command line */
            read_again(p);
            s->format = 'u';
            p->started = 1;
            return 1;
        } else if(is_command(p)) {
            memcpy(s->command, p->line, p->len + 1);
            if((r = next_line(p)) <= 0) return r;
            if(starts(p, "< ") || starts(p, "> ")) {
                read_again(p);
                s->format = 'n';
                memcpy(s->old_name, diff_old, sizeof(diff_old));
                memcpy(s->new_name, diff_new, sizeof(diff_new));
                p->started = 1;
                return 1;
            }
            s->command[0] = 0;
            have = 1;
        }
    }
}

void ps_reverse(struct psection *s) {
    char t[PATCH_NAME_MAX];
    int e = s->old_epoch;
    memcpy(t, s->old_name, sizeof(t));
    memcpy(s->old_name, s->new_name, sizeof(t));
    memcpy(s->new_name, t, sizeof(t));
    s->old_epoch = s->new_epoch;
    s->new_epoch = e;
}

/**** a hunk's lines ****/

static int add_line(struct pinput *p, struct phunk *h, char kind, const char *text, size_t len) {
    struct pline *l;
    if(h->nlines == h->lines_cap) {
        unsigned cap = h->lines_cap ? h->lines_cap * 2 : 32;
        struct pline *q = cap > h->lines_cap && cap < 0x10000000u ? realloc(h->lines, cap * sizeof(*q)) : NULL;
        if(!q) return bad(p, "a hunk too big for memory");
        h->lines = q;
        h->lines_cap = cap;
    }
    if(!h->text || len > h->text_cap - h->text_len) {
        size_t cap = h->text_cap ? h->text_cap : 1024;
        char *q;
        while(cap - h->text_len < len && cap < 0x80000000u) cap *= 2;
        q = cap - h->text_len >= len ? realloc(h->text, cap) : NULL;
        if(!q) return bad(p, "a hunk too big for memory");
        h->text = q;
        h->text_cap = cap;
    }
    if(len) memcpy(h->text + h->text_len, text, len);
    l = &h->lines[h->nlines++];
    l->at = (unsigned)h->text_len;
    l->len = (unsigned)len;
    l->kind = kind;
    l->no_newline = 0;
    l->hash = 0;
    h->text_len += len;
    return 0;
}

static void reset_hunk(struct phunk *h) {
    h->nlines = 0;
    h->text_len = 0;
    h->lead = h->trail = 0;
}

void ph_free(struct phunk *h) {
    free(h->lines);
    free(h->text);
    memset(h, 0, sizeof(*h));
}

/* what the lines are, once all are in: their hashes and the context at each end */
static void finish_hunk(struct phunk *h) {
    unsigned i;
    for(i = 0; i < h->nlines; i++) {
        struct pline *l = &h->lines[i];
        const char *t = h->text + l->at;
        unsigned k, v = DH_START;
        for(k = 0; k < l->len; k++) v = DH_ADD(v, t[k]);
        if(!l->no_newline) v = DH_ADD(v, '\n');
        l->hash = v;
    }
    while(h->lead < h->nlines && h->lines[h->lead].kind == ' ') h->lead++;
    while(h->trail < h->nlines - h->lead && h->lines[h->nlines - 1 - h->trail].kind == ' ') h->trail++;
    if(h->lead == h->nlines) h->trail = h->lead;        /* no change in it: all context, and neither end is short */
}

/* a line counted as start,count: start names the line before when count is 0 */
static int settle(struct pinput *p, unsigned *start, unsigned count) {
    if(!count) {
        if(*start == 0xFFFFFFFFu) return bad(p, "a line number too big");
        ++*start;
    } else if(!*start) return bad(p, "a hunk at line 0");
    if(count && count - 1 > 0xFFFFFFFFu - *start) return bad(p, "a hunk past the largest file");
    return 0;
}

/* a line of a hunk must be whole: with no newline at the end of the input the patch was cut, and what is there is a part
   of a line. Only the \ marker may lack one: it is no line of the file */
static int whole_line(struct pinput *p) {
    return p->had_newline ? 0 : bad(p, "the patch ends inside a line");
}

/* a \ line: it speaks of the line before it */
static int no_newline(struct pinput *p, struct phunk *h) {
    if(!h->nlines) return bad(p, "a \\ line with no line before it");
    h->lines[h->nlines - 1].no_newline = 1;
    return 0;
}

/**** unified ****/

static int range_unified(const char **s, unsigned *start, unsigned *count) {
    *count = 1;
    if(!number(s, start)) return 0;
    if(**s == ',') {
        ++*s;
        if(!number(s, count)) return 0;
    }
    return 1;
}

static int read_unified(struct pinput *p, struct phunk *h) {
    const char *s = p->line + 4;
    unsigned old_left, new_left;
    int r;
    if(!range_unified(&s, &h->old_start, &h->old_count) || s[0] != ' ' || s[1] != '+') return bad(p, "not a hunk's header");
    s += 2;
    if(!range_unified(&s, &h->new_start, &h->new_count) || strncmp(s, " @@", 3)) return bad(p, "not a hunk's header");
    if(settle(p, &h->old_start, h->old_count) < 0 || settle(p, &h->new_start, h->new_count) < 0) return -1;
    old_left = h->old_count;
    new_left = h->new_count;
    while(old_left || new_left) {
        char c;
        if((r = next_line(p)) < 0) return -1;
        if(!r) return bad(p, "the patch ends inside a hunk");
        c = p->len ? p->line[0] : ' ';          /* a mailer took the space off an empty line */
        if(c == '\\') {
            if(no_newline(p, h) < 0) return -1;
            continue;
        }
        if(whole_line(p) < 0) return -1;
        if(c == ' ' ? !old_left || !new_left : c == '-' ? !old_left : c == '+' ? !new_left : 1)
            return bad(p, "a hunk's lines do not match its header");
        if(add_line(p, h, c, p->line + 1, p->len ? p->len - 1 : 0) < 0) return -1;
        if(c != '+') old_left--;
        if(c != '-') new_left--;
    }
    if((r = next_line(p)) < 0) return -1;
    if(r) {
        if(p->line[0] == '\\' && p->len) return no_newline(p, h);
        read_again(p);
    }
    return 0;
}

/**** context ****/

/* *** 3,5 **** and --- 3,5 ----: the numbers, the first and the last line */
static int range_context(const char *s, const char *tail, unsigned *start, unsigned *count) {
    unsigned last;
    if(!number(&s, start)) return 0;
    last = *start;
    if(*s == ',') {
        s++;
        if(!number(&s, &last)) return 0;
    }
    if(strcmp(s, tail)) return 0;
    if(last < *start) return 0;
    if(!*start && last) return 0;               /* from line 0, which is only for a side of no lines */
    if(*start == last && !*start) *count = 0;   /* 0: none, before line 1 */
    else *count = last - *start + 1;
    return 1;
}

/* a line of one side of a context hunk: its mark, or 0 when it is not one. A space may be gone from an empty line */
static char context_mark(const struct pinput *p) {
    if(!p->len) return ' ';
    if(p->len == 1) return p->line[0] == ' ' ? ' ' : 0;
    if(p->line[1] != ' ') return 0;
    return p->line[0] == ' ' || p->line[0] == '-' || p->line[0] == '+' || p->line[0] == '!' ? p->line[0] : 0;
}

/* count lines of one side, each mark other than a space being own or ! */
static int read_context_side(struct pinput *p, struct phunk *h, unsigned count, char own) {
    unsigned i;
    int r;
    for(i = 0; i < count; i++) {
        char c;
        if((r = next_line(p)) < 0) return -1;
        if(!r) return bad(p, "the patch ends inside a hunk");
        if(p->line[0] == '\\' && p->len) {
            if(no_newline(p, h) < 0) return -1;
            i--;
            continue;
        }
        if(whole_line(p) < 0) return -1;
        c = context_mark(p);
        if(c != ' ' && c != own && c != '!') return bad(p, "a hunk's lines do not match its header");
        if(add_line(p, h, c, p->len > 2 ? p->line + 2 : "", p->len > 2 ? p->len - 2 : 0) < 0) return -1;
    }
    if((r = next_line(p)) < 0) return -1;
    if(r) {
        if(p->line[0] == '\\' && p->len) return no_newline(p, h);
        read_again(p);
    }
    return 0;
}

/* the two sides as one list: changes of the old side, of the new, and the context they share. nold lines of the
   old side come first in h->lines */
static int merge_sides(struct pinput *p, struct phunk *h, unsigned nold) {
    unsigned nnew = h->nlines - nold, i = 0, j = 0, k = 0, old_seen = 0, new_seen = 0;
    struct pline *out, *o, *n;
    if(!nold || !nnew) {                        /* one side left out, it had no changes: the other has it all */
        for(i = 0; i < h->nlines; i++) {
            char c = h->lines[i].kind;
            if(c == '!') return bad(p, "a hunk with a ! on one side only");
            old_seen += c != '+';
            new_seen += c != '-';
        }
        /* with no context a side of no lines is written as one number, the line before it: *** 3 **** */
        if(!old_seen && h->old_count == 1) h->old_count = 0;
        if(!new_seen && h->new_count == 1) h->new_count = 0;
        if(old_seen != h->old_count || new_seen != h->new_count) return bad(p, "a hunk's lines do not match its header");
        return 0;
    }
    out = malloc(h->nlines * sizeof(*out));
    if(!out) return bad(p, "a hunk too big for memory");
    o = h->lines;
    n = h->lines + nold;
    while(i < nold || j < nnew) {
        while(i < nold && o[i].kind != ' ') {
            out[k] = o[i++];
            out[k++].kind = '-';
        }
        while(j < nnew && n[j].kind != ' ') {
            out[k] = n[j++];
            out[k++].kind = '+';
        }
        if(i < nold && j < nnew) {
            if(o[i].len != n[j].len || memcmp(h->text + o[i].at, h->text + n[j].at, o[i].len)) {
                free(out);
                return bad(p, "the two sides of a hunk differ in their context");
            }
            out[k++] = o[i++];
            j++;
        } else if(i < nold || j < nnew) {
            free(out);
            return bad(p, "the two sides of a hunk differ in their context");
        }
    }
    memcpy(h->lines, out, k * sizeof(*out));
    h->nlines = k;
    free(out);
    return 0;
}

static int read_context(struct pinput *p, struct phunk *h) {
    unsigned nold;
    int r;
    if((r = next_line(p)) < 0) return -1;
    if(!r) return bad(p, "the patch ends inside a hunk");
    if(!starts(p, "*** ") || !range_context(p->line + 4, " ****", &h->old_start, &h->old_count))
        return bad(p, "not a hunk's header");
    if((r = next_line(p)) < 0) return -1;
    if(!r) return bad(p, "the patch ends inside a hunk");
    if(!starts(p, "--- ") || !range_context(p->line + 4, " ----", &h->new_start, &h->new_count)) {
        read_again(p);                          /* the old side's lines come first */
        if(read_context_side(p, h, h->old_count, '-') < 0) return -1;
        if((r = next_line(p)) < 0) return -1;
        if(!r) return bad(p, "the patch ends inside a hunk");
        if(!starts(p, "--- ") || !range_context(p->line + 4, " ----", &h->new_start, &h->new_count))
            return bad(p, "not a hunk's header");
    }
    nold = h->nlines;
    if(h->new_count) {                          /* left out when the new side changed nothing */
        unsigned same = 0, i;
        int changes = 0;
        for(i = 0; i < nold; i++) {
            same += h->lines[i].kind == ' ';
            changes |= h->lines[i].kind == '!';
        }
        if((r = next_line(p)) < 0) return -1;
        if(r) {
            char c = context_mark(p);
            read_again(p);
            /* an empty line after a side that could be left out is the end of the hunk, not an empty line of it */
            if(c == '+' || c == '!' || (c == ' ' && (p->len || !nold || changes || same != h->new_count))) {
                if(read_context_side(p, h, h->new_count, '+') < 0) return -1;
            }
        }
    }
    if(merge_sides(p, h, nold) < 0) return -1;
    if(settle(p, &h->old_start, h->old_count) < 0 || settle(p, &h->new_start, h->new_count) < 0) return -1;
    return 0;
}

/**** normal ****/

/* the two sides of 5,7c8 or 5a6: their first lines and how many */
static int parse_command(struct pinput *p, const char *s, struct phunk *h) {
    unsigned a, b, c, d;
    char op;
    if(!number(&s, &a)) return bad(p, "not a hunk's header");
    b = a;
    if(*s == ',') {
        s++;
        if(!number(&s, &b)) return bad(p, "not a hunk's header");
    }
    op = *s++;
    if(!number(&s, &c)) return bad(p, "not a hunk's header");
    d = c;
    if(*s == ',') {
        s++;
        if(!number(&s, &d)) return bad(p, "not a hunk's header");
    }
    if(*s || b < a || d < c || (op == 'a' && a != b) || (op == 'd' && c != d)) return bad(p, "not a hunk's header");
    if((op != 'a' && !a) || (op != 'd' && !c)) return bad(p, "a hunk at line 0");
    h->old_start = a;
    h->old_count = op == 'a' ? 0 : b - a + 1;
    h->new_start = c;
    h->new_count = op == 'd' ? 0 : d - c + 1;
    if(settle(p, &h->old_start, h->old_count) < 0 || settle(p, &h->new_start, h->new_count) < 0) return -1;
    return 0;
}

static int read_normal_side(struct pinput *p, struct phunk *h, unsigned count, char mark, char kind) {
    unsigned i;
    int r;
    for(i = 0; i < count; i++) {
        if((r = next_line(p)) < 0) return -1;
        if(!r) return bad(p, "the patch ends inside a hunk");
        if(p->line[0] == '\\' && p->len) {
            if(no_newline(p, h) < 0) return -1;
            i--;
            continue;
        }
        if(whole_line(p) < 0) return -1;
        if(!p->len || p->line[0] != mark || (p->len > 1 && p->line[1] != ' '))
            return bad(p, "a hunk's lines do not match its header");
        if(add_line(p, h, kind, p->line + 2, p->len > 2 ? p->len - 2 : 0) < 0) return -1;
    }
    if(count) {
        if((r = next_line(p)) < 0) return -1;
        if(r) {
            if(p->line[0] == '\\' && p->len) return no_newline(p, h);
            read_again(p);
        }
    }
    return 0;
}

static int read_normal(struct pinput *p, struct psection *s, struct phunk *h, const char *command) {
    int r;
    if(parse_command(p, command, h) < 0) return -1;
    s->command[0] = 0;
    if(read_normal_side(p, h, h->old_count, '<', '-') < 0) return -1;
    if(h->old_count && h->new_count) {
        if((r = next_line(p)) < 0) return -1;
        if(!r || strcmp(p->line, "---")) return bad(p, "a hunk's lines do not match its header");
    }
    return read_normal_side(p, h, h->new_count, '>', '+');
}

/**** the next hunk ****/

/* The line after a hunk, which is not another hunk: text is passed over, but one that looks like more of the hunk may be a
   count gone wrong, and should not go unsaid. The next section's headers and a mail's "-- " are not that. */
static void note_stray(struct pinput *p, const struct psection *s) {
    int stray;
    if(!s->hunks || p->stray || !p->len) return;
    if(s->format == 'u') stray = (p->line[0] == ' ' || p->line[0] == '+' || p->line[0] == '-') && !starts(p, "--- ") && !starts(p, "+++ ") && !starts(p, "--");
    else if(s->format == 'c') stray = p->len >= 2 && p->line[1] == ' ' && (p->line[0] == ' ' || p->line[0] == '+' || p->line[0] == '-' || p->line[0] == '!');
    else stray = starts(p, "< ") || starts(p, "> ");
    if(stray) p->stray = p->lineno;
}

int pp_next_hunk(struct pinput *p, struct psection *s, struct phunk *h) {
    int r;
    reset_hunk(h);
    h->patch_line = p->lineno + 1;
    if(s->format == 'n') {
        if(s->command[0]) {
            char first[sizeof(s->command)];
            memcpy(first, s->command, sizeof(first));
            r = read_normal(p, s, h, first);
        } else {
            if((r = next_line(p)) <= 0) return r;
            if(!is_command(p)) {
                note_stray(p, s);
                read_again(p);
                return 0;
            }
            memcpy(s->command, p->line, p->len + 1);
            if((r = next_line(p)) < 0) return -1;
            if(!r || !(starts(p, "< ") || starts(p, "> "))) {
                s->command[0] = 0;              /* only a line that looked like one */
                if(r) read_again(p);
                return 0;
            }
            read_again(p);
            {
                char first[sizeof(s->command)];
                memcpy(first, s->command, sizeof(first));
                r = read_normal(p, s, h, first);
            }
        }
    } else {
        if((r = next_line(p)) <= 0) return r;
        if(s->format == 'u') {
            if(!starts(p, "@@ -")) {
                note_stray(p, s);
                read_again(p);
                return 0;
            }
            r = read_unified(p, h);
        } else {
            if(!starts(p, "***************")) {
                note_stray(p, s);
                read_again(p);
                return 0;
            }
            r = read_context(p, h);
        }
    }
    if(r < 0) return -1;
    s->hunks++;
    h->number = s->hunks;
    finish_hunk(h);
    return 1;
}

void ph_reverse(struct phunk *h) {
    unsigned i, t;
    t = h->old_start;
    h->old_start = h->new_start;
    h->new_start = t;
    t = h->old_count;
    h->old_count = h->new_count;
    h->new_count = t;
    for(i = 0; i < h->nlines; i++) {
        struct pline *l = &h->lines[i];
        if(l->kind == '-') l->kind = '+';
        else if(l->kind == '+') l->kind = '-';
    }
}
