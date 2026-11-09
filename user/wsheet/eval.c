/*
SurfOS user program: wsheet
--------------------
File: eval.c    Date: 11/05/26
--------------------
A formula's value: its program (formula.c) run on a stack of values, and the functions.

Values and how they convert follow OpenFormula (OASIS, ODF 1.2 part 2) and what spreadsheets
agree on:
- an empty cell is 0 to arithmetic, "" to text and FALSE to logic; =A1 of an empty A1 shows 0;
- TRUE and FALSE are 1 and 0 to arithmetic; text that reads as a number (" 12 ", "50%") is that
  number, other text is #VALUE!; a number becomes text with its 15 digits, as General shows it;
- an error is passed on by whatever meets it, the left operand's first;
- comparisons put numbers before text before FALSE before TRUE; text compares in either case
  (ASCII and the Latin-1 letters), numbers equal to 15 digits are equal (number.c);
- a range is a value only to a function; anywhere else it is #VALUE!, unless it is one cell;
- a result that is not a finite number is #NUM!, a division by zero #DIV/0!.

The functions, and what their arguments take: a reference or a range gives a function the
values of its cells, and a value written in the call is taken as it is.
- SUM, AVERAGE, MIN, MAX: the numbers of ranges (text, TRUE and FALSE there are left out); in
  the call, TRUE and FALSE and text that reads as a number count too, other text is #VALUE!. SUM
  and AVERAGE add with Neumaier's compensated sum (1974), so that ten 0.1 make 1. MIN and MAX of
  no numbers are 0, AVERAGE of none #DIV/0!.
- COUNT counts numbers (and, in the call, TRUE, FALSE and text that reads as one); COUNTA counts
  every value but empty cells.
- IF(test, then, [else]): the else is FALSE when it is left out. AND, OR (TRUE, FALSE and
  numbers; text in ranges is left out; none at all is #VALUE!), NOT.
- ROUND(x, [places]) a half away from zero at x's 15 digits; INT rounds down; ABS; SQRT (of a
  negative number #NUM!); POWER(x, y), as x^y (0^-1 #DIV/0!, a negative x to a fraction #NUM!,
  0^0 is 1, as C's pow()); MOD(a, b) with the divisor's sign (MOD(-1, 3) is 2).
- LEN, LEFT(text, [n]), RIGHT(text, [n]), MID(text, start, n) count characters of UTF-8, a byte
  that is no UTF-8 one each; UPPER and LOWER change ASCII's letters and Latin-1's; TRIM takes the
  spaces from both ends and leaves one between words; CONCAT joins text, ranges cell by cell.
Text a formula makes is SH_TEXT_MAX bytes at most: longer is #VALUE!.

Every instruction, every cell a range shows, and every 8 bytes of text compared, scanned or made
is a step: past sh_steps_max ev_formula() stops, and recalc.c shows #LIMIT! where it could not
finish.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sheetpriv.h"

enum { V_EMPTY, V_NUM, V_STR, V_BOOL, V_ERR, V_REF, V_RANGE };

struct val {
    unsigned char t, e;                 /* V_*; V_BOOL its truth, V_ERR its error */
    unsigned char own;                  /* s was allocated for this value */
    int len;
    double n;
    const char *s;
    int r1, c1, r2, c2;                 /* V_REF r1, c1; V_RANGE */
};

struct ev {
    const struct sheet *s;
    long *steps;
    int over;                           /* past sh_steps_max */
    int nomem;
};

static void drop(struct val *v) {
    if(v->own) mem_free((char *)v->s);
    v->own = 0;
    v->s = NULL;
}

static void set_err(struct val *v, int e) {
    memset(v, 0, sizeof(*v));
    v->t = V_ERR;
    v->e = (unsigned char)e;
}

static void set_num(struct val *v, double n) {
    memset(v, 0, sizeof(*v));
    if(!isfinite(n)) {
        v->t = V_ERR;
        v->e = SH_NUM;
        return;
    }
    v->t = V_NUM;
    v->n = n == 0 ? 0 : n;              /* no -0 */
}

static void set_bool(struct val *v, int b) {
    memset(v, 0, sizeof(*v));
    v->t = V_BOOL;
    v->e = (unsigned char)(b != 0);
}

/* text work as steps: every 8 bytes compared, scanned or made is one, so that formulas working on long texts
   are bounded as those working on ranges are (11/09/26, the review: 25,000 formulas comparing 8 KB texts took
   seconds and counted 100,000 steps) */
static void text_work(struct ev *x, long bytes) {
    *x->steps += bytes >> 3;
}

/* len bytes of new text for v, to be filled: NULL when there is no memory (v is then #LIMIT!) or it is too long
   (#VALUE!) */
static char *new_text(struct ev *x, struct val *v, int len) {
    char *p;
    memset(v, 0, sizeof(*v));
    text_work(x, len);
    if(len > SH_TEXT_MAX) {
        set_err(v, SH_VALUE);
        return NULL;
    }
    p = mem_alloc((size_t)len + 1);
    if(!p) {
        x->nomem = 1;
        set_err(v, SH_LIMIT);
        return NULL;
    }
    p[len] = 0;
    v->t = V_STR;
    v->own = 1;
    v->s = p;
    v->len = len;
    return p;
}

static void from_value(struct val *v, const struct sh_value *sv) {
    memset(v, 0, sizeof(*v));
    switch(sv->type) {
    case SH_NUMBER: v->t = V_NUM; v->n = sv->num; break;
    case SH_TEXT: v->t = V_STR; v->s = sv->text; v->len = sv->len; break;
    case SH_BOOL: v->t = V_BOOL; v->e = (unsigned char)sv->b; break;
    case SH_ERROR: v->t = V_ERR; v->e = (unsigned char)sv->b; break;
    default: v->t = V_EMPTY; break;
    }
}

static void cell_val(struct ev *x, int r, int c, struct val *v) {
    struct cell *cl = cell_at(x->s, r, c);
    struct sh_value sv;
    if(!cl) {
        memset(v, 0, sizeof(*v));
        v->t = V_EMPTY;
        return;
    }
    cell_value(x->s, cl, &sv);
    from_value(v, &sv);
}

/* a reference becomes its cell's value, a range of one cell too; any other range is #VALUE! */
static void scalar(struct ev *x, struct val *v) {
    if(v->t == V_REF) cell_val(x, v->r1, v->c1, v);
    else if(v->t == V_RANGE) {
        if(v->r1 == v->r2 && v->c1 == v->c2) cell_val(x, v->r1, v->c1, v);
        else set_err(v, SH_VALUE);
    }
}

static int is_space(int c) { return c == ' '; }

/* text as a number: spaces around it, and the forms one is typed in (50%, 1e3); 1 with *d */
static int text_num(const char *s, int len, double *d) {
    int pct;
    while(len && is_space((unsigned char)*s)) {
        s++;
        len--;
    }
    while(len && is_space((unsigned char)s[len - 1])) len--;
    return num_read(s, len, d, &pct);
}

static int text_bool(const char *s, int len, int *b) {
    if(len == 4 && !strncasecmp(s, "TRUE", 4)) *b = 1;
    else if(len == 5 && !strncasecmp(s, "FALSE", 5)) *b = 0;
    else return 0;
    return 1;
}

/* the value as a number: 0, or the error it is */
static int to_num(struct ev *x, struct val *v, double *d) {
    scalar(x, v);
    switch(v->t) {
    case V_EMPTY: *d = 0; return 0;
    case V_NUM: *d = v->n; return 0;
    case V_BOOL: *d = v->e; return 0;
    case V_STR:
        text_work(x, v->len);
        return text_num(v->s, v->len, d) ? 0 : SH_VALUE;
    case V_ERR: return v->e;
    }
    return SH_VALUE;
}

/* as text: *s and *len, which may be buf (32 bytes); 0, or the error */
static int to_text(struct ev *x, struct val *v, char *buf, const char **s, int *len) {
    scalar(x, v);
    switch(v->t) {
    case V_EMPTY: *s = ""; *len = 0; return 0;
    case V_NUM: *len = num_general(v->n, 0, buf, 32); *s = buf; return 0;
    case V_BOOL: *s = v->e ? "TRUE" : "FALSE"; *len = v->e ? 4 : 5; return 0;
    case V_STR: *s = v->s; *len = v->len; return 0;
    case V_ERR: return v->e;
    }
    return SH_VALUE;
}

static int to_bool(struct ev *x, struct val *v, int *b) {
    scalar(x, v);
    switch(v->t) {
    case V_EMPTY: *b = 0; return 0;
    case V_NUM: *b = v->n != 0; return 0;
    case V_BOOL: *b = v->e; return 0;
    case V_STR: return text_bool(v->s, v->len, b) ? 0 : SH_VALUE;
    case V_ERR: return v->e;
    }
    return SH_VALUE;
}

static int over(struct ev *x) {
    if(*x->steps > sh_steps_max) x->over = 1;
    return x->over;
}

/**** UTF-8 and letters ****/

/* the character at s[i] (RFC 3629's well-formed sequences): the index after it; a byte that begins none is one
   character, 0x110000 + the byte, after every code point */
static int u8_next(const char *s, int len, int i, unsigned *cp) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[i], lo = 0x80, hi = 0xBF;
    int n, k;
    if(c < 0x80) {
        *cp = c;
        return i + 1;
    }
    if(c >= 0xC2 && c <= 0xDF) n = 1;
    else if(c >= 0xE0 && c <= 0xEF) {
        n = 2;
        if(c == 0xE0) lo = 0xA0;
        if(c == 0xED) hi = 0x9F;
    } else if(c >= 0xF0 && c <= 0xF4) {
        n = 3;
        if(c == 0xF0) lo = 0x90;
        if(c == 0xF4) hi = 0x8F;
    } else {
        *cp = 0x110000 + c;
        return i + 1;
    }
    if(i + n >= len) {                          /* cut short */
        *cp = 0x110000 + c;
        return i + 1;
    }
    *cp = c & (n == 1 ? 0x1F : n == 2 ? 0x0F : 0x07);
    for(k = 1; k <= n; k++) {
        unsigned b = u[i + k];
        if(b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF)) {
            *cp = 0x110000 + c;
            return i + 1;
        }
        *cp = (*cp << 6) | (b & 0x3F);
    }
    return i + n + 1;
}

int u8_chars(const char *s, int len) {
    unsigned cp;
    int i = 0, n = 0;
    while(i < len) {
        i = u8_next(s, len, i, &cp);
        n++;
    }
    return n;
}

/* the byte where character k (from 0) begins, len past the end */
static int u8_offset(const char *s, int len, int k) {
    unsigned cp;
    int i = 0;
    while(i < len && k-- > 0) i = u8_next(s, len, i, &cp);
    return i;
}

static unsigned fold(unsigned c) {
    if(c >= 'A' && c <= 'Z') return c + 32;
    if(c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if(c == 0x178) return 0xFF;
    return c;
}

static unsigned unfold(unsigned c) {
    if(c >= 'a' && c <= 'z') return c - 32;
    if(c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if(c == 0xFF) return 0x178;
    return c;
}

int text_fold_compare(const char *a, int alen, const char *b, int blen) {
    unsigned x, y;
    int i = 0, j = 0;
    while(i < alen && j < blen) {
        i = u8_next(a, alen, i, &x);
        j = u8_next(b, blen, j, &y);
        x = fold(x);
        y = fold(y);
        if(x != y) return x < y ? -1 : 1;
    }
    if(i < alen) return 1;
    if(j < blen) return -1;
    return 0;
}

/* UPPER and LOWER: every letter they change keeps its number of bytes, so the text keeps its length */
static void change_case(const char *s, int len, char *out, int up) {
    unsigned cp, to;
    int i = 0, k;
    while(i < len) {
        k = u8_next(s, len, i, &cp);
        to = up ? unfold(cp) : fold(cp);
        if(to == cp || cp >= 0x110000) memcpy(out + i, s + i, (size_t)(k - i));
        else if(to < 0x80) out[i] = (char)to;
        else {                                  /* both two bytes: U+00C0..U+017F */
            out[i] = (char)(0xC0 | (to >> 6));
            out[i + 1] = (char)(0x80 | (to & 0x3F));
        }
        i = k;
    }
}

/**** comparisons ****/

static int rank(int t) {
    return t == V_NUM ? 0 : t == V_STR ? 1 : 2;
}

/* -1, 0 or 1; or *err */
static int compare(struct ev *x, struct val *a, struct val *b, int *err) {
    struct val *v[2];
    int i;
    *err = 0;
    scalar(x, a);
    scalar(x, b);
    if(a->t == V_ERR) return *err = a->e, 0;
    if(b->t == V_ERR) return *err = b->e, 0;
    if(a->t == V_EMPTY && b->t == V_EMPTY) return 0;
    v[0] = a;
    v[1] = b;
    for(i = 0; i < 2; i++) {                    /* an empty cell is the other side's nothing: 0, "" or FALSE */
        struct val *o = v[1 - i];
        if(v[i]->t != V_EMPTY) continue;
        v[i]->t = o->t;
        v[i]->n = 0;
        v[i]->e = 0;
        v[i]->s = "";
        v[i]->len = 0;
    }
    if(rank(a->t) != rank(b->t)) return rank(a->t) < rank(b->t) ? -1 : 1;
    if(a->t == V_NUM) return num_equal(a->n, b->n) ? 0 : a->n < b->n ? -1 : 1;
    if(a->t == V_STR) {
        text_work(x, (long)a->len + b->len);
        return text_fold_compare(a->s, a->len, b->s, b->len);
    }
    return (a->e > b->e) - (a->e < b->e);
}

/**** operators ****/

static void arith(struct ev *x, int op, struct val *a, struct val *b, struct val *out) {
    double p, q;
    int e = to_num(x, a, &p);
    if(!e) e = to_num(x, b, &q);
    if(e) {
        set_err(out, e);
        return;
    }
    switch(op) {
    case I_ADD: set_num(out, p + q); break;
    case I_SUB: set_num(out, p - q); break;
    case I_MUL: set_num(out, p * q); break;
    case I_DIV:
        if(q == 0) set_err(out, SH_DIV0);
        else set_num(out, p / q);
        break;
    case I_POW:
        if(p == 0 && q < 0) set_err(out, SH_DIV0);
        else if(p < 0 && q != floor(q)) set_err(out, SH_NUM);
        else set_num(out, pow(p, q));
        break;
    }
}

static void concat2(struct ev *x, struct val *a, struct val *b, struct val *out) {
    char ba[32], bb[32], *p;
    const char *sa, *sb;
    int la, lb, e = to_text(x, a, ba, &sa, &la);
    if(!e) e = to_text(x, b, bb, &sb, &lb);
    if(e) {
        set_err(out, e);
        return;
    }
    p = new_text(x, out, la + lb);
    if(!p) return;
    memcpy(p, sa, (size_t)la);
    memcpy(p + la, sb, (size_t)lb);
}

static void binary(struct ev *x, int op, struct val *a, struct val *b, struct val *out) {
    int c, e;
    switch(op) {
    case I_CAT:
        concat2(x, a, b, out);
        break;
    case I_EQ: case I_NE: case I_LT: case I_LE: case I_GT: case I_GE:
        c = compare(x, a, b, &e);
        if(e) set_err(out, e);
        else set_bool(out, op == I_EQ ? c == 0 : op == I_NE ? c != 0 : op == I_LT ? c < 0 : op == I_LE ? c <= 0 :
                      op == I_GT ? c > 0 : c >= 0);
        break;
    default:
        arith(x, op, a, b, out);
        break;
    }
}

/**** functions ****/

/* the values a function's arguments give, one at a time: a reference's or a range's cells that hold something,
   row by row, or the argument itself (*direct set) */
struct items {
    struct ev *x;
    struct val *argv;
    int argc, i, inrange;
    struct walk w;
};

static int items_next(struct items *it, struct val *v, int *direct) {
    struct ev *x = it->x;
    struct cell *c;
    struct sh_value sv;
    int row;
    for(;;) {
        if(it->inrange) {
            c = walk_next(x->s, &it->w, &row, x->steps);
            if(over(x)) return 0;
            if(c) {
                cell_value(x->s, c, &sv);
                from_value(v, &sv);
                *direct = 0;
                return 1;
            }
            it->inrange = 0;
            it->i++;
            continue;
        }
        if(it->i >= it->argc) return 0;
        if(it->argv[it->i].t == V_REF || it->argv[it->i].t == V_RANGE) {
            struct val *a = &it->argv[it->i];
            walk_start(x->s, &it->w, a->r1, a->c1, a->t == V_REF ? a->r1 : a->r2, a->t == V_REF ? a->c1 : a->c2, 0);
            it->inrange = 1;
            continue;
        }
        *v = it->argv[it->i++];
        v->own = 0;                             /* still the argument's */
        *direct = 1;
        return 1;
    }
}

static void items_start(struct items *it, struct ev *x, struct val *argv, int argc) {
    it->x = x;
    it->argv = argv;
    it->argc = argc;
    it->i = 0;
    it->inrange = 0;
}

static void nsum_add(double *total, double *comp, double v) {
    double t = *total + v;
    if(fabs(*total) >= fabs(v)) *comp += (*total - t) + v;
    else *comp += (v - t) + *total;
    *total = t;
}

/* SUM, AVERAGE, MIN, MAX, COUNT, COUNTA */
static void aggregate(struct ev *x, int fn, struct val *argv, int argc, struct val *out) {
    struct items it;
    struct val v;
    double total = 0, comp = 0, lo = 0, hi = 0, d;
    int direct, n = 0, values = 0, counted;
    items_start(&it, x, argv, argc);
    while(items_next(&it, &v, &direct)) {
        if(v.t == V_EMPTY) continue;
        values++;
        counted = 0;
        if(v.t == V_ERR) {
            if(fn == F_COUNT || fn == F_COUNTA) continue;
            set_err(out, v.e);
            return;
        }
        if(v.t == V_NUM) {
            d = v.n;
            counted = 1;
        } else if(direct && v.t == V_BOOL) {
            d = v.e;
            counted = 1;
        } else if(direct && v.t == V_STR) {
            if(text_num(v.s, v.len, &d)) counted = 1;
            else if(fn != F_COUNT && fn != F_COUNTA) {
                set_err(out, SH_VALUE);
                return;
            }
        }
        if(!counted) continue;
        if(!n || d < lo) lo = d;
        if(!n || d > hi) hi = d;
        n++;
        nsum_add(&total, &comp, d);
    }
    if(x->over) return;
    switch(fn) {
    case F_SUM: set_num(out, total + comp); break;
    case F_AVERAGE:
        if(!n) set_err(out, SH_DIV0);
        else set_num(out, (total + comp) / n);
        break;
    case F_MIN: set_num(out, lo); break;
    case F_MAX: set_num(out, hi); break;
    case F_COUNT: set_num(out, n); break;
    case F_COUNTA: set_num(out, values); break;
    }
}

static void logic(struct ev *x, int fn, struct val *argv, int argc, struct val *out) {
    struct items it;
    struct val v;
    int direct, seen = 0, all = 1, any = 0, b;
    items_start(&it, x, argv, argc);
    while(items_next(&it, &v, &direct)) {
        switch(v.t) {
        case V_ERR: set_err(out, v.e); return;
        case V_NUM: b = v.n != 0; break;
        case V_BOOL: b = v.e; break;
        case V_STR:
            if(!direct) continue;
            if(!text_bool(v.s, v.len, &b)) {
                set_err(out, SH_VALUE);
                return;
            }
            break;
        default: continue;
        }
        seen = 1;
        all = all && b;
        any = any || b;
    }
    if(x->over) return;
    if(!seen) set_err(out, SH_VALUE);
    else set_bool(out, fn == F_AND ? all : any);
}

static void concat_all(struct ev *x, struct val *argv, int argc, struct val *out) {
    struct items it;
    struct val v;
    char buf[32], *p = NULL;
    const char *s;
    int direct, len, total = 0, e, pass;
    for(pass = 0; pass < 2; pass++) {           /* the length, then the text */
        if(pass && !(p = new_text(x, out, total))) return;
        total = 0;
        items_start(&it, x, argv, argc);
        while(items_next(&it, &v, &direct)) {
            e = to_text(x, &v, buf, &s, &len);
            if(e) {
                if(pass) drop(out);
                set_err(out, e);
                return;
            }
            if(pass) memcpy(p + total, s, (size_t)len);
            total += len;
            if(total > SH_TEXT_MAX) {
                if(pass) drop(out);
                set_err(out, SH_VALUE);
                return;
            }
        }
        if(x->over) {
            if(pass) drop(out);
            return;
        }
    }
}

/* a count from a number: whole, at least 0 (-1 when it is below), at most a bound past any text's length */
static int whole(double d) {
    if(d < 0) return -1;
    if(d > 1e9) return 1000000000;
    return (int)d;
}

static void text_part(struct ev *x, int fn, struct val *argv, int argc, struct val *out) {
    char buf[32], *p;
    const char *s;
    double d = 1, start = 1;
    int len, e, n, from, to, chars;
    e = to_text(x, &argv[0], buf, &s, &len);
    if(!e && fn == F_MID) {
        e = to_num(x, &argv[1], &start);
        if(!e) e = to_num(x, &argv[2], &d);
    } else if(!e && argc > 1) e = to_num(x, &argv[1], &d);
    if(e) {
        set_err(out, e);
        return;
    }
    text_work(x, len);                          /* the scans for characters */
    n = whole(d);
    if(n < 0 || (fn == F_MID && start < 1)) {
        set_err(out, SH_VALUE);
        return;
    }
    chars = u8_chars(s, len);
    switch(fn) {
    case F_LEFT:
        from = 0;
        to = u8_offset(s, len, n);
        break;
    case F_RIGHT:
        from = u8_offset(s, len, n >= chars ? 0 : chars - n);
        to = len;
        break;
    default:
        from = u8_offset(s, len, whole(start) - 1);
        to = from + u8_offset(s + from, len - from, n);
        break;
    }
    p = new_text(x, out, to - from);
    if(p) memcpy(p, s + from, (size_t)(to - from));
}

static void text_fn(struct ev *x, int fn, struct val *arg, struct val *out) {
    char buf[32], *p;
    const char *s;
    int len, e, i, k;
    e = to_text(x, arg, buf, &s, &len);
    if(e) {
        set_err(out, e);
        return;
    }
    text_work(x, len);
    switch(fn) {
    case F_LEN:
        set_num(out, u8_chars(s, len));
        break;
    case F_UPPER: case F_LOWER:
        p = new_text(x, out, len);
        if(p) change_case(s, len, p, fn == F_UPPER);
        break;
    case F_TRIM:                                /* spaces kept only one between words */
        p = new_text(x, out, len);
        if(!p) return;
        for(i = k = 0; i < len; i++) {
            if(s[i] != ' ') p[k++] = s[i];
            else if(k && p[k - 1] != ' ') p[k++] = ' ';
        }
        if(k && p[k - 1] == ' ') k--;
        p[k] = 0;
        out->len = k;
        break;
    }
}

static void math_fn(struct ev *x, int fn, struct val *argv, int argc, struct val *out) {
    double a, b = 0, r;
    int e = to_num(x, &argv[0], &a);
    if(!e && argc > 1) e = to_num(x, &argv[1], &b);
    if(e) {
        set_err(out, e);
        return;
    }
    switch(fn) {
    case F_ABS: set_num(out, fabs(a)); break;
    case F_INT: set_num(out, floor(a)); break;
    case F_SQRT:
        if(a < 0) set_err(out, SH_NUM);
        else set_num(out, sqrt(a));
        break;
    case F_ROUND:
        set_num(out, num_round(a, b > 1000 ? 1000 : b < -1000 ? -1000 : (int)b));
        break;
    case F_POWER:
        arith(x, I_POW, &argv[0], &argv[1], out);
        break;
    case F_MOD:
        if(b == 0) {
            set_err(out, SH_DIV0);
            break;
        }
        r = fmod(a, b);
        if(r != 0 && (r < 0) != (b < 0)) r += b;
        set_num(out, r);
        break;
    }
}

/* the call's value into out; argv's values go */
static void call(struct ev *x, int fn, struct val *argv, int argc, struct val *out) {
    int b, e;
    memset(out, 0, sizeof(*out));
    switch(fn) {
    case F_SUM: case F_AVERAGE: case F_MIN: case F_MAX: case F_COUNT: case F_COUNTA:
        aggregate(x, fn, argv, argc, out);
        break;
    case F_AND: case F_OR:
        logic(x, fn, argv, argc, out);
        break;
    case F_NOT:
        e = to_bool(x, &argv[0], &b);
        if(e) set_err(out, e);
        else set_bool(out, !b);
        break;
    case F_IF:
        e = to_bool(x, &argv[0], &b);
        if(e) set_err(out, e);
        else if(b) {
            *out = argv[1];
            argv[1].own = 0;                    /* the value is out's now */
        } else if(argc > 2) {
            *out = argv[2];
            argv[2].own = 0;
        } else set_bool(out, 0);
        break;
    case F_CONCAT:
        concat_all(x, argv, argc, out);
        break;
    case F_LEFT: case F_RIGHT: case F_MID:
        text_part(x, fn, argv, argc, out);
        break;
    case F_LEN: case F_UPPER: case F_LOWER: case F_TRIM:
        text_fn(x, fn, &argv[0], out);
        break;
    case F_ABS: case F_INT: case F_SQRT: case F_ROUND: case F_POWER: case F_MOD:
        math_fn(x, fn, argv, argc, out);
        break;
    default:
        set_err(out, SH_NAME);
        break;
    }
}

/**** the program ****/

void ev_set_error(struct formula *f, int e) {
    mem_free(f->vtext);
    f->vtext = NULL;
    f->vlen = 0;
    f->vtype = SH_ERROR;
    f->vb = (unsigned char)e;
}

/* the final value into f: a reference is its cell's value, an empty cell 0 */
static void store(struct ev *x, struct formula *f, struct val *v) {
    char *t;
    scalar(x, v);
    mem_free(f->vtext);
    f->vtext = NULL;
    f->vlen = 0;
    f->vnum = 0;
    f->vb = 0;
    switch(v->t) {
    case V_EMPTY:
        f->vtype = SH_NUMBER;
        break;
    case V_NUM:
        f->vtype = SH_NUMBER;
        f->vnum = v->n;
        break;
    case V_BOOL:
        f->vtype = SH_BOOL;
        f->vb = v->e;
        break;
    case V_STR:
        if(v->own) {                            /* its own text: kept as it is */
            t = (char *)v->s;
            v->own = 0;
        } else if(!(t = mem_dup(v->s, v->len))) {
            x->nomem = 1;
            ev_set_error(f, SH_LIMIT);
            return;
        }
        f->vtype = SH_TEXT;
        f->vtext = t;
        f->vlen = v->len;
        break;
    default:
        ev_set_error(f, v->t == V_ERR ? v->e : SH_VALUE);
        break;
    }
}

int ev_formula(const struct sheet *s, struct formula *f, long *steps) {
    struct ev x;
    struct val *st, res;
    const struct ins *in;
    double d;
    int sp = 0, i, e;
    if(!f->code) {
        ev_set_error(f, SH_SYNTAX);
        return 0;
    }
    x.s = s;
    x.steps = steps;
    x.over = 0;
    x.nomem = 0;
    st = mem_alloc((size_t)(f->depth + 1) * sizeof(*st));
    if(!st) {
        ev_set_error(f, SH_LIMIT);
        return -2;
    }
    for(i = 0; i < f->ncode && !x.over; i++) {
        in = &f->code[i];
        ++*steps;
        if(over(&x)) break;
        switch(in->op) {
        case I_NUM: set_num(&st[sp++], in->num); break;
        case I_STR:
            memset(&st[sp], 0, sizeof(st[sp]));
            st[sp].t = V_STR;
            st[sp].s = FORMULA_POOL(f) + in->r1;
            st[sp++].len = in->r2;
            break;
        case I_BOOL: set_bool(&st[sp++], in->fn); break;
        case I_ERR: set_err(&st[sp++], in->fn); break;
        case I_REF: case I_RANGE:
            memset(&st[sp], 0, sizeof(st[sp]));
            st[sp].t = in->op == I_REF ? V_REF : V_RANGE;
            st[sp].r1 = in->r1;
            st[sp].c1 = in->c1;
            st[sp].r2 = in->r2;
            st[sp++].c2 = in->c2;
            break;
        case I_NEG: case I_PCT:
            e = to_num(&x, &st[sp - 1], &d);
            drop(&st[sp - 1]);
            if(e) set_err(&st[sp - 1], e);
            else set_num(&st[sp - 1], in->op == I_NEG ? -d : d / 100);
            break;
        case I_PLUS:
            scalar(&x, &st[sp - 1]);
            break;
        case I_CALL:
            call(&x, in->fn, &st[sp - in->argc], in->argc, &res);
            for(e = 1; e <= in->argc; e++) drop(&st[sp - e]);   /* what IF passed on is res's, not theirs */
            sp -= in->argc;
            st[sp++] = res;
            break;
        default:
            binary(&x, in->op, &st[sp - 2], &st[sp - 1], &res);
            drop(&st[sp - 2]);
            drop(&st[sp - 1]);
            sp -= 2;
            st[sp++] = res;
            break;
        }
    }
    if(x.over || sp != 1) {                     /* a program leaves one value: anything else stopped early */
        while(sp > 0) drop(&st[--sp]);
        mem_free(st);
        if(x.over) return -1;
        ev_set_error(f, SH_VALUE);
        return 0;
    }
    store(&x, f, &st[0]);
    drop(&st[0]);
    mem_free(st);
    return x.nomem ? -2 : 0;
}
