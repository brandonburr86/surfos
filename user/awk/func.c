/*
SurfOS user program: awk
--------------------
File: func.c    Date: 11/01/26
--------------------
Regular expressions and the built-in functions.

libsurf's regex.c takes POSIX's syntax, and awk's is that with a few differences, so a pattern
goes through awk_ere() first. awk's escapes (\n, \t, \/, \" and \ddd) become what they stand for.
In a bracket expression \ is itself in POSIX but an escape in awk, so [\]a] is rewritten, with
[.].] for an escaped ] or the like, which POSIX has for this. Everything else with a \ goes to the
library as it is: \. and the like, and its \w \s \< \>. A compiled pattern is kept: a string
that a program uses as a pattern is compiled once. The kept ones are bounded, as a program can
make any number of different strings.

A match's offsets are bytes. length, substr, index, match, RSTART and RLENGTH count characters.
*/

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include "awk.h"

#define KEPT_MAX 512                    /* compiled strings kept */

struct regex {
    regex_t re;
    struct str *text;
    struct regex *next;
};

char regex_error[128];

static struct regex *kept;
static unsigned nkept;

/**** the patterns ****/

static int octal(const char *s, unsigned n, unsigned *used) {
    int v = 0;
    unsigned k = 0;
    while(k < 3 && k < n && s[k] >= '0' && s[k] <= '7') v = v * 8 + (s[k++] - '0');
    *used = k;
    return v;
}

/* the byte an escape of awk's stands for, or -1; *used: how many bytes after the backslash */
static int escape_value(const char *s, unsigned n, unsigned *used) {
    *used = 1;
    switch(s[0]) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return 7;
    case 'b': return 8;
    case '/': return '/';
    case '"': return '"';
    }
    if(s[0] >= '0' && s[0] <= '7') return octal(s, n, used);
    return -1;
}

static void put_literal(struct buf *b, int c) {
    if(c && strchr("\\^$.[]|()*+?{}", c)) buf_ch(b, '\\');
    if(c) buf_ch(b, c);
}

static void put_in_bracket(struct buf *b, int c) {
    if(c == ']' || c == '[' || c == '^' || c == '-') {
        buf_add(b, "[.", 2);
        buf_ch(b, c);
        buf_add(b, ".]", 2);
    } else if(c) {
        buf_ch(b, c);
    }
}

/* from the [ at s[i]: the bracket expression copied, *i past it */
static void copy_bracket(struct buf *b, const char *s, unsigned n, unsigned *i) {
    unsigned k = *i + 1;
    buf_ch(b, '[');
    if(k < n && s[k] == '^') {
        buf_ch(b, '^');
        k++;
    }
    if(k < n && s[k] == ']') {
        buf_ch(b, ']');
        k++;
    }
    while(k < n && s[k] != ']') {
        if(s[k] == '[' && k + 1 < n && (s[k + 1] == ':' || s[k + 1] == '.' || s[k + 1] == '=')) {
            char kind = s[k + 1];
            unsigned e = k + 2;
            while(e + 1 < n && !(s[e] == kind && s[e + 1] == ']')) e++;
            if(e + 1 < n) {
                buf_add(b, s + k, e + 2 - k);
                k = e + 2;
                continue;
            }
        }
        if(s[k] == '\\' && k + 1 < n) {
            unsigned used;
            int v = escape_value(s + k + 1, n - k - 1, &used);
            if(v >= 0) {
                put_in_bracket(b, v);
                k += 1 + used;
            } else {
                put_in_bracket(b, s[k + 1]);
                k += 2;
            }
            continue;
        }
        buf_ch(b, s[k++]);
    }
    if(k < n) {
        buf_ch(b, ']');
        k++;
    }
    *i = k;
}

static void awk_ere(struct buf *b, const char *s, unsigned n) {
    unsigned i = 0;
    buf_add(b, "", 0);
    while(i < n) {
        if(s[i] == '[') {
            copy_bracket(b, s, n, &i);
        } else if(s[i] == '\\' && i + 1 < n) {
            unsigned used;
            int v = escape_value(s + i + 1, n - i - 1, &used);
            if(v >= 0) {
                put_literal(b, v);
                i += 1 + used;
            } else {
                buf_add(b, s + i, 2);
                i += 2;
            }
        } else {
            buf_ch(b, s[i++]);
        }
    }
}

static struct regex *regex_compile(const char *text, unsigned len) {
    struct regex *r = xalloc(sizeof(*r));
    struct buf b = { NULL, 0, 0 };
    int err;
    awk_ere(&b, text, len);
    err = regcomp(&r->re, b.p, REG_EXTENDED);
    free(b.p);
    if(err) {
        regerror(err, &r->re, regex_error, sizeof(regex_error));
        free(r);
        return NULL;
    }
    r->text = str_new(text, len);
    r->next = NULL;
    return r;
}

const struct regex *regex_static(const char *text, unsigned len) {
    return regex_compile(text, len);
}

const struct regex *regex_of(const struct value *v) {
    struct regex *r, **p;
    struct str *s;
    if(v->kind == V_REGEX) return v->u.re;
    s = to_str(v);
    for(p = &kept; (r = *p); p = &r->next) {
        if(r->text->len == s->len && !memcmp(r->text->s, s->s, s->len)) {
            *p = r->next;               /* the last used first: a loop has one or two */
            r->next = kept;
            kept = r;
            str_put(s);
            return r;
        }
    }
    if(nkept == KEPT_MAX) {
        while(kept) {
            r = kept;
            kept = r->next;
            regfree(&r->re);
            str_put(r->text);
            free(r);
        }
        nkept = 0;
    }
    r = regex_compile(s->s, s->len);
    if(!r) fatal("/%.40s/: %s", s->s, regex_error);
    str_put(s);
    r->next = kept;
    kept = r;
    nkept++;
    return r;
}

/* s[from..to) searched, what comes before seen by ^ and the like: 1 and the match, or 0 */
int regex_find(const struct regex *re, const char *s, unsigned from, unsigned to, regoff_t *so, regoff_t *eo) {
    regmatch_t m;
    int err;
    if(to > 0x7fffffffu) fatal("a string too long for a regular expression");
    m.rm_so = (regoff_t)from;
    m.rm_eo = (regoff_t)to;
    err = regexec(&re->re, s, 1, &m, REG_STARTEND);
    if(!err) {
        *so = m.rm_so;
        *eo = m.rm_eo;
        return 1;
    }
    if(err != REG_NOMATCH) {
        char t[80];
        regerror(err, &re->re, t, sizeof(t));
        fatal("/%.40s/: %s", re->text->s, t);
    }
    return 0;
}

/**** sub and gsub ****/

/* The replacement for one match, by POSIX's text: & is the match, a backslash before & or before a backslash
   makes that character plain, and any other backslash is itself. */
static void put_replacement(struct buf *b, const char *r, unsigned n, const char *m, unsigned mlen) {
    const char *end = r + n;
    while(r < end) {
        char c = *r++;
        if(c == '&') {
            buf_add(b, m, mlen);
        } else {
            if(c == '\\' && r < end && (*r == '&' || *r == '\\')) c = *r++;
            buf_ch(b, c);
        }
    }
}

/* the number of replacements; *result is a new string when there were any. An empty match does
   not come straight after the match before it, and the text goes on a whole character at a time */
int substitute(const struct value *rev, struct str *repl, struct str *text, int global, struct str **result) {
    const struct regex *re = regex_of(rev);
    struct buf b = { NULL, 0, 0 };
    unsigned pos = 0, n = text->len, count = 0, after = 0xffffffffu;
    regoff_t so, eo;
    while(pos <= n && regex_find(re, text->s, pos, n, &so, &eo)) {
        buf_add(&b, text->s + pos, (unsigned)so - pos);
        if(eo == so) {
            if((unsigned)so != after) {
                put_replacement(&b, repl->s, repl->len, text->s + so, 0);
                count++;
            }
            pos = (unsigned)so;
            if(pos < n) {
                unsigned k = char_len(text->s + pos, n - pos);
                buf_add(&b, text->s + pos, k);
                pos += k;
            } else {
                pos = n + 1;
            }
        } else {
            put_replacement(&b, repl->s, repl->len, text->s + so, (unsigned)(eo - so));
            count++;
            pos = (unsigned)eo;
            after = pos;
        }
        if(!global) break;
    }
    if(!count) {
        free(b.p);
        return 0;
    }
    if(pos < n) buf_add(&b, text->s + pos, n - pos);
    *result = str_new(b.p, b.len);
    free(b.p);
    return (int)count;
}

/**** the functions ****/

#define ARG(i) (A.stack[A.sp - (unsigned)nargs + (unsigned)(i)])

/* the arguments go and the result takes their place */
static void result_num(int nargs, double d) {
    int i;
    for(i = 0; i < nargs; i++) val_put(&ARG(i));
    A.sp -= (unsigned)nargs;
    A.stack[A.sp].kind = V_NUM;
    A.stack[A.sp++].u.num = d;
}

static void result_str(int nargs, struct str *s) {
    int i;
    for(i = 0; i < nargs; i++) val_put(&ARG(i));
    A.sp -= (unsigned)nargs;
    A.stack[A.sp].kind = V_STR;
    A.stack[A.sp++].u.s = s;
}

/* POSIX: at most n characters of s, from position m. It says nothing of a position below 1 or of a number that is not
   whole. Here each is cut to a whole number, and a start below 1 is 1 with the count running from there, so that
   substr("hello", 0, 2) is he and substr("hello", -1, 3) hel. The answer is a window of positions, from up to but not
   including to, and whatever is not a window is empty: a NaN starts at 1 as m and gives nothing as n. */
static struct str *substring(struct str *s, double m, double n) {
    double chars = str_chars(s), from = trunc(m), to;
    unsigned first, last, at = 0, k;
    if(!(from >= 1)) from = 1;
    to = from + trunc(n);
    if(to > chars + 1) to = chars + 1;
    if(!(to > from)) return str_new("", 0);
    first = (unsigned)from - 1;
    last = (unsigned)to - 1;
    if(chars == s->len) return str_new(s->s + first, last - first);     /* all ASCII: no need to walk */
    for(k = 0; k < first; k++) at += char_len(s->s + at, s->len - at);
    first = at;
    for(; k < last; k++) at += char_len(s->s + at, s->len - at);
    return str_new(s->s + first, at - first);
}

static double index_of(struct str *s, struct str *t) {
    unsigned i;
    if(!t->len) return 1;               /* it stands at the start, as match(s, "") says too */
    if(t->len > s->len) return 0;
    for(i = 0; i + t->len <= s->len; i++) {
        const char *p = memchr(s->s + i, t->s[0], s->len - t->len - i + 1);
        if(!p) return 0;
        i = (unsigned)(p - s->s);
        if(!memcmp(p, t->s, t->len)) return (double)chars_in(s->s, i) + 1;
    }
    return 0;
}

struct split_to {
    struct array *a;
    struct str *text;
    unsigned count;
};

static void split_add(void *ctx, unsigned start, unsigned len) {
    struct split_to *t = ctx;
    struct value *e;
    struct str *key;
    t->count++;
    key = num_str(t->count, S_CONVFMT);
    e = array_elem(t->a, key, 1);
    str_put(key);
    val_put(e);
    e->kind = V_INPUT;
    e->u.s = str_new(t->text->s + start, len);
}

static double do_split(int nargs) {
    struct split_to t;
    struct str *rs;
    int paragraph;
    t.text = to_str(&ARG(0));
    t.a = ARG(1).u.a;
    t.count = 0;
    rs = to_str(&A.global[S_RS]);
    paragraph = !rs->len;
    str_put(rs);
    array_clear(t.a);
    if(nargs > 2 && ARG(2).kind == V_REGEX) {
        split_text(t.text, NULL, ARG(2).u.re, 0, split_add, &t);
    } else {
        struct str *sep = to_str(nargs > 2 ? &ARG(2) : &A.global[S_FS]);
        const struct regex *re = NULL;
        if(sep->len > 1) {
            struct value v;
            v.kind = V_STR;
            v.u.s = sep;
            re = regex_of(&v);
        }
        split_text(t.text, sep, re, nargs > 2 ? 0 : paragraph, split_add, &t);
        str_put(sep);
    }
    str_put(t.text);
    return t.count;
}

static double do_match(void) {
    int nargs = 2;
    struct str *s = to_str(&ARG(0));
    const struct regex *re = regex_of(&ARG(1));
    regoff_t so, eo;
    double start = 0, len = -1;
    if(regex_find(re, s->s, 0, s->len, &so, &eo)) {
        start = (double)chars_in(s->s, (unsigned)so) + 1;
        len = chars_in(s->s + so, (unsigned)(eo - so));
    }
    str_put(s);
    val_put(&A.global[S_RSTART]);
    val_put(&A.global[S_RLENGTH]);
    A.global[S_RSTART].kind = A.global[S_RLENGTH].kind = V_NUM;
    A.global[S_RSTART].u.num = start;
    A.global[S_RLENGTH].u.num = len;
    return start;
}

static struct str *case_of(struct str *s, int upper) {
    struct str *r = str_alloc(s->len);
    unsigned i;
    for(i = 0; i < s->len; i++) {
        char c = s->s[i];
        if(upper ? c >= 'a' && c <= 'z' : c >= 'A' && c <= 'Z') c = (char)(c ^ 0x20);
        r->s[i] = c;
    }
    return r;
}

static double next_rand(void) {
    A.rand_state = A.rand_state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(unsigned)(A.rand_state >> 32) / 4294967296.0;
}

void builtin_call(int b, int nargs) {
    struct str *s, *t;
    double d;
    switch(b) {
    case B_LENGTH:
        if(!nargs) {
            result_num(0, str_chars(record_get()));
        } else if(ARG(0).kind == V_ARRAY) {
            result_num(1, array_count(ARG(0).u.a));
        } else {
            s = to_str(&ARG(0));
            d = str_chars(s);
            str_put(s);
            result_num(1, d);
        }
        break;
    case B_SUBSTR:
        s = to_str(&ARG(0));
        t = substring(s, to_num(&ARG(1)), nargs > 2 ? to_num(&ARG(2)) : 1e300);
        str_put(s);
        result_str(nargs, t);
        break;
    case B_INDEX:
        s = to_str(&ARG(0));
        t = to_str(&ARG(1));
        d = index_of(s, t);
        str_put(s);
        str_put(t);
        result_num(2, d);
        break;
    case B_SPLIT:
        result_num(nargs, do_split(nargs));
        break;
    case B_MATCH:
        result_num(2, do_match());
        break;
    case B_SPRINTF: {
        struct buf out = { NULL, 0, 0 };
        s = to_str(&ARG(0));
        format_text(&out, s->s, s->len, &ARG(1), nargs - 1);
        str_put(s);
        t = str_new(out.p, out.len);
        free(out.p);
        result_str(nargs, t);
        break;
    }
    case B_SIN: result_num(1, sin(to_num(&ARG(0)))); break;
    case B_COS: result_num(1, cos(to_num(&ARG(0)))); break;
    case B_ATAN2: result_num(2, atan2(to_num(&ARG(0)), to_num(&ARG(1)))); break;
    case B_EXP: result_num(1, exp(to_num(&ARG(0)))); break;
    case B_LOG: result_num(1, log(to_num(&ARG(0)))); break;
    case B_SQRT: result_num(1, sqrt(to_num(&ARG(0)))); break;
    case B_INT: result_num(1, trunc(to_num(&ARG(0)))); break;
    case B_RAND: result_num(0, next_rand()); break;
    case B_SRAND:
        d = A.rand_seed;
        A.rand_seed = nargs ? to_num(&ARG(0)) : (double)time(NULL);
        {
            double low = fmod(A.rand_seed, 9007199254740992.0);
            A.rand_state = isnan(low) ? 0 : (unsigned long long)(long long)low;
        }
        result_num(nargs, d);
        break;
    case B_TOLOWER: case B_TOUPPER:
        s = to_str(&ARG(0));
        t = case_of(s, b == B_TOUPPER);
        str_put(s);
        result_str(1, t);
        break;
    case B_SYSTEM:
        s = to_str(&ARG(0));
        fflush(NULL);
        if(strlen(s->s) != s->len) fatal("a command with a NUL in it");
        d = system(s->s);
        str_put(s);
        result_num(1, d);
        break;
    case B_CLOSE:
        s = to_str(&ARG(0));
        d = stream_close(s);
        str_put(s);
        result_num(1, d);
        break;
    case B_FFLUSH:
        if(!nargs) {
            result_num(0, stream_flush(NULL));
        } else {
            s = to_str(&ARG(0));
            d = stream_flush(s);
            str_put(s);
            result_num(1, d);
        }
        break;
    default:
        fatal("no such function");
    }
}
