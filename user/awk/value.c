/*
SurfOS user program: awk
--------------------
File: value.c   Date: 11/01/26
--------------------
Strings, numbers and arrays, and POSIX's rules for turning one into the other.

A string knows its length, so that a NUL in the input does no harm, and has a NUL after it for
the C library. A character is a UTF-8 sequence by the rule of libsurf's regex.c: overlong forms,
surrogates and anything above U+10FFFF are no sequences, and a byte in none counts as one
character. So that RSTART, which comes from the regular expression, and length() agree. The count
is kept in the string: a loop of substr() over a long string would otherwise be quadratic.

A value from input, V_INPUT, is a number where it looks like one, decided when asked: most fields
are never compared. What looks like a number is decimal only: strtod() also takes 0x10, inf and
nan, which POSIX's text allows, but no awk that is in use takes them from a file, as a word such as
"nan" or "infinity" in a column should stay a word.

Arrays are hash tables of chains. for-in works from a copy of the keys, so that the body may
delete what it likes. Nothing here recurses.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "awk.h"

/**** strings ****/

struct str *str_alloc(unsigned len) {
    struct str *s;
    if(len >= 0x7ff00000u) fatal("out of memory");
    s = xalloc(sizeof(*s) + (size_t)len + 1);
    s->refs = 1;
    s->len = len;
    s->chars = NOT_COUNTED;
    s->s[len] = 0;
    return s;
}

struct str *str_new(const char *p, unsigned len) {
    struct str *s = str_alloc(len);
    if(len) memcpy(s->s, p, len);
    return s;
}

void str_put(struct str *s) {
    if(s && !--s->refs) free(s);
}

static int continues(const unsigned char *s, unsigned n, unsigned k) {
    return k < n && (s[k] & 0xc0) == 0x80;
}

unsigned char_len(const char *p, unsigned n) {
    const unsigned char *s = (const unsigned char *)p;
    unsigned c = s[0];
    if(c < 0xc2 || c > 0xf4) return 1;
    if(c < 0xe0) return continues(s, n, 1) ? 2 : 1;
    if(c < 0xf0) {
        if(!continues(s, n, 1) || !continues(s, n, 2)) return 1;
        if((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] > 0x9f)) return 1;
        return 3;
    }
    if(!continues(s, n, 1) || !continues(s, n, 2) || !continues(s, n, 3)) return 1;
    if((c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] > 0x8f)) return 1;
    return 4;
}

unsigned chars_in(const char *s, unsigned n) {
    unsigned i = 0, count = 0;
    while(i < n) {
        i += (signed char)s[i] >= 0 ? 1 : char_len(s + i, n - i);
        count++;
    }
    return count;
}

unsigned str_chars(struct str *s) {
    if(s->chars == NOT_COUNTED) s->chars = chars_in(s->s, s->len);
    return s->chars;
}

/**** values ****/

void val_put(struct value *v) {
    switch(v->kind) {
    case V_STR: case V_INPUT: str_put(v->u.s); break;
    case V_ARRAY: array_put(v->u.a); break;
    case V_ITER: iter_free(v->u.it); break;
    }
    v->kind = V_UNSET;
}

void val_copy(struct value *to, const struct value *from) {
    *to = *from;
    if(from->kind == V_STR || from->kind == V_INPUT) from->u.s->refs++;
    else if(from->kind == V_ARRAY) array_ref(from->u.a);
}

static int is_blank(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_dig(int c) {
    return c >= '0' && c <= '9';
}

/* the end of the decimal number at p, or p when there is none */
static const char *number_end(const char *p, const char *end) {
    const char *q = p;
    int digits = 0;
    if(q < end && (*q == '+' || *q == '-')) q++;
    while(q < end && is_dig(*q)) {
        q++;
        digits = 1;
    }
    if(q < end && *q == '.') {
        q++;
        while(q < end && is_dig(*q)) {
            q++;
            digits = 1;
        }
    }
    if(!digits) return p;
    if(q < end && (*q == 'e' || *q == 'E')) {
        const char *r = q + 1;
        if(r < end && (*r == '+' || *r == '-')) r++;
        if(r < end && is_dig(*r)) {
            while(r < end && is_dig(*r)) r++;
            q = r;
        }
    }
    return q;
}

int looks_numeric(const char *s, unsigned n) {
    const char *end = s + n, *q;
    while(s < end && is_blank(*s)) s++;
    if(s == end) return 0;
    q = number_end(s, end);
    if(q == s) return 0;
    while(q < end && is_blank(*q)) q++;
    return q == end;
}

/* The number at the start of the text, for a text that need not be one. The digits go through
   strtod(), and it stops at the same place: the check first keeps hex, inf and nan out. A
   number in the middle of a longer string is copied, as the text need not have a NUL after it. */
double str_num(const char *s, unsigned n) {
    const char *end = s + n, *q;
    char t[64];
    size_t len;
    while(s < end && is_blank(*s)) s++;
    q = number_end(s, end);
    if(q == s) return 0;
    len = (size_t)(q - s);
    if(len < sizeof(t)) {
        double d;
        memcpy(t, s, len);
        t[len] = 0;
        d = strtod(t, NULL);
        return d;
    }
    {
        char *big = xalloc(len + 1);
        double d;
        memcpy(big, s, len);
        big[len] = 0;
        d = strtod(big, NULL);
        free(big);
        return d;
    }
}

double to_num(const struct value *v) {
    switch(v->kind) {
    case V_NUM: return v->u.num;
    case V_STR: case V_INPUT: return str_num(v->u.s->s, v->u.s->len);
    case V_UNSET: case V_REF: return 0;
    }
    fatal("an array where a number should be");
}

static struct str *small_int(long d) {
    char t[16];
    unsigned n = 0;
    unsigned long u = d < 0 ? 0ul - (unsigned long)d : (unsigned long)d;
    do {
        t[sizeof(t) - ++n] = (char)('0' + u % 10);
        u /= 10;
    } while(u);
    if(d < 0) t[sizeof(t) - ++n] = '-';
    return str_new(t + sizeof(t) - n, n);
}

/* POSIX: a whole number as by %d; anything else through CONVFMT or OFMT */
struct str *num_str(double d, int conv) {
    struct value arg;
    struct buf b = { NULL, 0, 0 };
    struct str *fmt, *s;
    if(d > -2147483648.0 && d < 2147483648.0 && d == (double)(long)d) return small_int((long)d);
    arg.kind = V_NUM;
    arg.u.num = d;
    if(isnan(d) || isinf(d)) {
        format_text(&b, "%f", 2, &arg, 1);
    } else if(d == floor(d)) {
        format_text(&b, "%d", 2, &arg, 1);
    } else {
        fmt = to_str(&A.global[conv]);
        format_text(&b, fmt->s, fmt->len, &arg, 1);
        str_put(fmt);
    }
    s = str_new(b.p ? b.p : "", b.len);
    free(b.p);
    return s;
}

struct str *to_str(const struct value *v) {
    switch(v->kind) {
    case V_NUM: return num_str(v->u.num, S_CONVFMT);
    case V_STR: case V_INPUT: return str_ref(v->u.s);
    case V_UNSET: case V_REF: return str_new("", 0);
    }
    fatal("an array where a string should be");
}

struct str *to_output(const struct value *v) {
    return v->kind == V_NUM ? num_str(v->u.num, S_OFMT) : to_str(v);
}

int to_bool(const struct value *v) {
    switch(v->kind) {
    case V_NUM: return v->u.num != 0;
    case V_STR: return v->u.s->len != 0;
    case V_INPUT:
        if(looks_numeric(v->u.s->s, v->u.s->len)) return str_num(v->u.s->s, v->u.s->len) != 0;
        return v->u.s->len != 0;
    case V_UNSET: case V_REF: return 0;
    }
    fatal("an array where a value should be");
}

static int numeric_like(const struct value *v) {
    if(v->kind == V_INPUT) return looks_numeric(v->u.s->s, v->u.s->len);
    return v->kind == V_NUM || v->kind == V_UNSET || v->kind == V_REF;
}

/* POSIX's table: numbers and strings from input that look like numbers go by number, the rest
   by string. An unset value is both, so it goes the way of the other side. A NaN compares with nothing. */
int val_cmp(const struct value *a, const struct value *b) {
    if(numeric_like(a) && numeric_like(b)) {
        double x = to_num(a), y = to_num(b);
        return x < y ? -1 : x > y ? 1 : x == y ? 0 : 2;
    } else {
        struct str *s = to_str(a), *t = to_str(b);
        unsigned n = s->len < t->len ? s->len : t->len;
        int c = n ? memcmp(s->s, t->s, n) : 0;
        if(!c) c = s->len < t->len ? -1 : s->len > t->len;
        str_put(s);
        str_put(t);
        return c < 0 ? -1 : c > 0;
    }
}

/**** arrays ****/

struct elem {
    struct elem *next;
    struct str *key;
    unsigned hash;
    struct value val;
};

struct array {
    unsigned refs;
    unsigned count, nbuckets;
    struct elem **bucket;
};

struct iter {
    unsigned n, i;
    struct str *key[];
};

static unsigned hash_of(const struct str *s) {
    unsigned h = 2166136261u, i;
    for(i = 0; i < s->len; i++) h = (h ^ (unsigned char)s->s[i]) * 16777619u;
    return h;
}

struct array *array_new(void) {
    struct array *a = xalloc(sizeof(*a));
    a->refs = 1;
    a->count = 0;
    a->nbuckets = 16;
    a->bucket = xalloc(16 * sizeof(*a->bucket));
    memset(a->bucket, 0, 16 * sizeof(*a->bucket));
    return a;
}

void array_clear(struct array *a) {
    unsigned i;
    for(i = 0; i < a->nbuckets; i++) {
        struct elem *e = a->bucket[i];
        a->bucket[i] = NULL;
        while(e) {
            struct elem *next = e->next;
            str_put(e->key);
            val_put(&e->val);
            free(e);
            e = next;
        }
    }
    a->count = 0;
}

void array_ref(struct array *a) {
    a->refs++;
}

void array_put(struct array *a) {
    if(!a || --a->refs) return;
    array_clear(a);
    free(a->bucket);
    free(a);
}

unsigned array_count(const struct array *a) {
    return a->count;
}

static void array_grow(struct array *a) {
    unsigned n = a->nbuckets * 2, i;
    struct elem **v;
    if(n > 0x10000000u) return;         /* long chains, rather than a table of a gigabyte */
    v = xalloc(n * sizeof(*v));
    memset(v, 0, n * sizeof(*v));
    for(i = 0; i < a->nbuckets; i++) {
        struct elem *e = a->bucket[i];
        while(e) {
            struct elem *next = e->next;
            unsigned at = e->hash & (n - 1);
            e->next = v[at];
            v[at] = e;
            e = next;
        }
    }
    free(a->bucket);
    a->bucket = v;
    a->nbuckets = n;
}

struct value *array_elem(struct array *a, struct str *key, int create) {
    unsigned h = hash_of(key);
    struct elem *e;
    for(e = a->bucket[h & (a->nbuckets - 1)]; e; e = e->next)
        if(e->hash == h && e->key->len == key->len && !memcmp(e->key->s, key->s, key->len)) return &e->val;
    if(!create) return NULL;
    if(a->count >= a->nbuckets * 2) array_grow(a);
    e = xalloc(sizeof(*e));
    e->key = str_ref(key);
    e->hash = h;
    e->val.kind = V_UNSET;
    e->next = a->bucket[h & (a->nbuckets - 1)];
    a->bucket[h & (a->nbuckets - 1)] = e;
    a->count++;
    return &e->val;
}

void array_del(struct array *a, struct str *key) {
    unsigned h = hash_of(key);
    struct elem **p = &a->bucket[h & (a->nbuckets - 1)];
    for(; *p; p = &(*p)->next) {
        struct elem *e = *p;
        if(e->hash == h && e->key->len == key->len && !memcmp(e->key->s, key->s, key->len)) {
            *p = e->next;
            str_put(e->key);
            val_put(&e->val);
            free(e);
            a->count--;
            return;
        }
    }
}

struct iter *iter_new(const struct array *a) {
    struct iter *it = xalloc(sizeof(*it) + a->count * sizeof(it->key[0]));
    unsigned i, n = 0;
    for(i = 0; i < a->nbuckets; i++) {
        const struct elem *e;
        for(e = a->bucket[i]; e; e = e->next) it->key[n++] = str_ref(e->key);
    }
    it->n = n;
    it->i = 0;
    return it;
}

struct str *iter_next(struct iter *it) {
    return it->i < it->n ? it->key[it->i++] : NULL;
}

void iter_free(struct iter *it) {
    unsigned i;
    for(i = 0; i < it->n; i++) str_put(it->key[i]);
    free(it);
}

/**** the globals ****/

int global_id(const char *name, unsigned len) {
    unsigned i;
    for(i = 0; i < A.nglobal; i++)
        if(!strncmp(A.gname[i], name, len) && !A.gname[i][len]) return (int)i;
    if(A.nglobal == A.capglobal) {
        A.capglobal = A.capglobal ? A.capglobal * 2 : 64;
        A.global = xrealloc(A.global, A.capglobal * sizeof(*A.global));
        A.gname = xrealloc(A.gname, A.capglobal * sizeof(*A.gname));
    }
    A.global[A.nglobal].kind = V_UNSET;
    A.gname[A.nglobal] = xalloc(len + 1);
    memcpy(A.gname[A.nglobal], name, len);
    A.gname[A.nglobal][len] = 0;
    return (int)A.nglobal++;
}

/* FS and OFS take effect for what comes later, and a record may not be split, or put back
   together, yet: that is done first, with the old ones. */
void global_set(int id, const struct value *v) {
    struct value t;
    if(id == S_NF) {
        nf_set(to_num(v));
        return;
    }
    if(id == S_FS) before_fs_change();
    if(id == S_OFS) before_ofs_change();
    val_copy(&t, v);
    val_put(&A.global[id]);
    A.global[id] = t;
}
