/*
SurfOS shell: surfsh
--------------------
File: glob.c    Date: 10/24/26
--------------------
Patterns: * is any characters, ? one character, [abc] and [a-z] one of those ([!...] or
[^...] one not of them; a ] first in them is itself; a [ that is not closed is itself), and
anything else is itself; so is a character that was in quotes or after a \ (expand.c says
which: q[i] is set for those). A character is a whole UTF-8 sequence, so ? takes one é.

Pathname expansion: a field with a pattern character outside quotes becomes the names that
match it, sorted (strcmp). Each part between /s is matched against the names in the
directory the parts before it name (the current one for a relative field); a part with no
pattern in it is only joined on, and the whole name must exist. A name that starts with . is
matched only by a part that starts with a . (the directory list has no . and .. of its own).
A field that nothing matches stays as it was: POSIX's rule, so "rm *.tmp" with none says
"no such file" about *.tmp. Names are matched as stored, case and all.
*/

#include "surfsh.h"

#define MATCHES_MAX 4096

/* the bytes of the UTF-8 character at s (1 for a byte that begins none) */
static int char_len(const unsigned char *s, int n) {
    int k = 1;
    if(*s >= 0xC0) while(k < n && k < 4 && (s[k] & 0xC0) == 0x80) k++;
    return k;
}

/* [ ... ] at p[0]: does the character c (clen bytes) match it? *plen gets the bracket's length;
   -1 when it is no bracket expression (not closed: the [ is itself) */
static int bracket(const char *p, const char *q, int pn, const char *c, int clen, int *plen) {
    int i = 1, neg = 0, hit = 0, first = 1;
    if(i < pn && (p[i] == '!' || p[i] == '^') && !q[i]) {
        neg = 1;
        i++;
    }
    for(; i < pn; first = 0) {
        int lo = i, lolen, hi, hilen;
        if(p[i] == ']' && !q[i] && !first) {
            *plen = i + 1;
            return hit != neg;
        }
        lolen = char_len((const unsigned char *)p + i, pn - i);
        i += lolen;
        if(i + 1 < pn && p[i] == '-' && !q[i] && !(p[i + 1] == ']' && !q[i + 1])) {   /* a range */
            hi = i + 1;
            hilen = char_len((const unsigned char *)p + hi, pn - hi);
            i = hi + hilen;
            if(clen == 1 && lolen == 1 && hilen == 1 &&
               (unsigned char)*c >= (unsigned char)p[lo] && (unsigned char)*c <= (unsigned char)p[hi]) hit = 1;
            continue;
        }
        if(clen == lolen && !memcmp(c, p + lo, (size_t)clen)) hit = 1;
    }
    return -1;
}

/* does the pattern p (pn bytes, quote flags q) match all of s (sn bytes)? */
int pmatch(const char *p, const char *q, int pn, const char *s, int sn) {
    int pi = 0, si = 0, star_p = -1, star_s = 0;
    while(si < sn) {
        if(pi < pn && p[pi] == '*' && !q[pi]) {
            star_p = ++pi;                          /* the * takes nothing first, more on the way back */
            star_s = si;
            continue;
        }
        if(pi < pn) {
            int clen = char_len((const unsigned char *)s + si, sn - si), plen = 1, ok;
            if(p[pi] == '?' && !q[pi]) ok = 1;
            else if(p[pi] == '[' && !q[pi] && (ok = bracket(p + pi, q + pi, pn - pi, s + si, clen, &plen)) >= 0)
                ;
            else {
                plen = char_len((const unsigned char *)p + pi, pn - pi);
                ok = plen == clen && !memcmp(p + pi, s + si, (size_t)clen);
            }
            if(ok > 0) {
                pi += plen;
                si += clen;
                continue;
            }
        }
        if(star_p < 0) return 0;                    /* no * to give a character to */
        pi = star_p;
        star_s += char_len((const unsigned char *)s + star_s, sn - star_s);
        si = star_s;
    }
    while(pi < pn && p[pi] == '*' && !q[pi]) pi++;
    return pi == pn;
}

/* is there a pattern character outside quotes in p? */
int has_pattern(const char *p, const char *q, int pn) {
    int i;
    for(i = 0; i < pn; i++) if(!q[i] && (p[i] == '*' || p[i] == '?' || p[i] == '[')) return 1;
    return 0;
}

struct found {
    char **v;
    int n, cap;
};

static void found_add(struct found *f, const char *s) {
    if(f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->v = xrealloc(f->v, (size_t)f->cap * sizeof(char *));
    }
    f->v[f->n++] = xstrdup(s);
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The parts of the pattern from p on (pn bytes, flags q) below the directory dir (its path as it
   is to be written in the names, "" for the current one): the names that match, into f */
static void walk(const char *dir, const char *p, const char *q, int pn, struct found *f) {
    struct sbuf name = { NULL, 0, 0 };
    struct stat st;
    int n = 0, last;
    while(n < pn && p[n] != '/') n++;               /* this part: p[0 .. n) */
    last = n == pn;
    if(!has_pattern(p, q, n)) {                     /* a plain part: joined on */
        sb_puts(&name, dir);
        sb_put(&name, p, n);
        if(last) {
            if(stat(sb_str(&name), &st) == 0) found_add(f, sb_str(&name));
        } else {
            sb_putc(&name, '/');
            walk(sb_str(&name), p + n + 1, q + n + 1, pn - n - 1, f);
        }
    } else {
        DIR *d = opendir(dir[0] ? dir : ".");
        struct dirent *de;
        if(d) {
            while((de = readdir(d)) && f->n < MATCHES_MAX) {
                if(de->d_name[0] == '.' && !(n && p[0] == '.')) continue;   /* a dot name: only by a dot */
                if(!pmatch(p, q, n, de->d_name, (int)strlen(de->d_name))) continue;
                name.len = 0;
                sb_puts(&name, dir);
                sb_puts(&name, de->d_name);
                if(last) found_add(f, sb_str(&name));
                else if(de->d_type == DT_DIR) {
                    sb_putc(&name, '/');
                    walk(sb_str(&name), p + n + 1, q + n + 1, pn - n - 1, f);
                }
            }
            closedir(d);
        }
    }
    sb_free(&name);
}

/* The names that match the field p (pn bytes, quote flags q), sorted, added to out: how many (0:
   none matched, the field is the caller's to keep) */
int glob_field(const char *p, const char *q, int pn, struct fields *out) {
    struct found f = { NULL, 0, 0 };
    int i;
    char *qq = xmalloc((size_t)pn + 1), *pp = xmalloc((size_t)pn + 1);
    memcpy(pp, p, (size_t)pn);
    memcpy(qq, q, (size_t)pn);
    pp[pn] = qq[pn] = 0;
    if(pn && pp[0] == '/') {                        /* from the root: the rest below "/" */
        int k = 1;
        while(k < pn && pp[k] == '/') k++;
        walk("/", pp + k, qq + k, pn - k, &f);
    } else {
        walk("", pp, qq, pn, &f);
    }
    free(pp);
    free(qq);
    qsort(f.v, (size_t)f.n, sizeof(char *), by_name);
    for(i = 0; i < f.n; i++) fields_take(out, f.v[i]);
    free(f.v);
    return f.n;
}
