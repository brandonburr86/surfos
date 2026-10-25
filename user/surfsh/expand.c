/*
SurfOS shell: surfsh
--------------------
File: expand.c  Date: 10/24/26
--------------------
Words become fields when their command runs (parse.c keeps them as typed), in POSIX's order:
parameters ($name ${name} $1 $# ...), command substitution ($( ) and ` `) and arithmetic
($(( ))), left to right as they come; then field splitting of what unquoted expansions gave,
at the characters of IFS (a blank, a tab and a newline unless it is set otherwise); then
pathname expansion (glob.c) of each field that has a pattern character outside quotes; then
the quotes go.

Quotes: what 'single' quotes hold is itself; in "double" quotes $ ` and \ still work (a \
keeps only a $ ` " \ or a newline after it, and a \ and a newline both go); outside them a \
keeps the next character. What quotes hold is never split nor a pattern, so "$x" is one field
whatever x holds, and "" an empty one; an unquoted expansion that gives nothing gives no
field at all.

Parameters: $name and ${name}; ${name:-word} the word when name is unset or empty
(${name-word}: when unset), ${name:=word} the same, and the word assigned to it, ${name:?word}
the word as an error (the command does not run), ${name:+word} the word when name is set and
not empty, ${#name} its length in characters, ${name#pattern} ${name##pattern} the value
without its shortest or longest beginning that the pattern matches, ${name%pattern}
${name%%pattern} without its end. $1 ... $9 and ${10} ...; $# how many; $@ and $* all of them:
"$@" one field each (none when there are none), "$*" one field, joined by IFS's first
character, unquoted both split; $? the last status; $$ this shell's pid; $! the last job's;
$0 the shell's name; $- the options (10/25/26: set -e -f -u -x, and i in an interactive shell).
A $ before anything else is itself. 10/25/26: with set -u a parameter that is not set is an
error ($@ and $* never are, nor the ${x-...} forms), and with set -f nothing is a pattern.

Command substitution: the command's output, its newlines at the end taken off (exec.c runs
it: a plain program by itself, anything else in a new surfsh). $(( ... )): the text is
expanded first, then worked out as arith.c says. A here-document's text has the same
expansions, with its quotes taken as they are and a \ before $ ` \ or a newline only.

10/25/26: a quote that gives nothing ("", '', "$empty") begins a field where it is, which
splitting keeps even when it stays empty (""$x of x=" a" is "" and a). ${ } inside ${ } goes
DEPTH_LIMIT deep at most; ${#name} of a name longer than any is an error; the value of
${name#pattern} is taken before the pattern expands, which may change it ($((x=1)), ${x:=y}).
*/

#include "surfsh.h"

#define XF_QUOTED 1                 /* in quotes or after a \: itself, never split nor a pattern */
#define XF_SPLIT  2                 /* from an unquoted expansion: split at IFS, may be a pattern */

/* a word as it expands: its characters with a flag for each */
struct xw {
    struct sbuf s;
    char *f;
    int fcap;
    int quoted;                     /* a quote was in it: when nothing else is, it is an empty field */
    int *marks, nmarks, markcap;    /* 10/25/26: where a quote that gave nothing ("", '', "$empty") was:
                                       a field begins there, which splitting keeps even when empty */
};

struct ex {
    struct xw *raw;                 /* finished pieces ("$@" breaks a word into several) */
    int nraw, rawcap;
    struct xw cur;                  /* the piece being made */
    int failed;
    int lit;                        /* the flag of unquoted characters as they are: XF_SPLIT in the word
                                       of an unquoted ${x:-word} (it is split as a value is), else 0 */
};

int subst_status;                   /* the status of the last command substitution */
int subst_count;                    /* how many ran: a line of assignments alone gives the last one's status */

void fields_free(struct fields *f) {
    int i;
    for(i = 0; i < f->n; i++) free(f->v[i]);
    free(f->v);
    f->v = NULL;
    f->n = f->cap = 0;
}

void fields_take(struct fields *f, char *s) {
    if(f->n + 1 >= f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->v = xrealloc(f->v, (size_t)f->cap * sizeof(char *));
    }
    f->v[f->n++] = s;
    f->v[f->n] = NULL;
}

static void xw_put(struct xw *w, const char *s, int n, int flag) {
    if(n <= 0) return;
    if(w->s.len + n + 1 > w->fcap) {
        while(w->s.len + n + 1 > w->fcap) w->fcap = w->fcap ? w->fcap * 2 : 64;
        w->f = xrealloc(w->f, (size_t)w->fcap);
    }
    memset(w->f + w->s.len, flag, (size_t)n);
    sb_put(&w->s, s, n);
}

static void xw_free(struct xw *w) {
    sb_free(&w->s);
    free(w->f);
    free(w->marks);
    memset(w, 0, sizeof(*w));
}

/* 10/25/26: a quote that gave nothing ends here, at the piece's end so far */
static void xw_mark(struct xw *w) {
    if(w->nmarks == w->markcap) {
        w->markcap = w->markcap ? w->markcap * 2 : 4;
        w->marks = xrealloc(w->marks, (size_t)w->markcap * sizeof(int));
    }
    w->marks[w->nmarks++] = w->s.len;
}

/* the piece made so far is a field of its own ("$@"): a new one begins */
static void field_break(struct ex *e) {
    if(e->nraw == e->rawcap) {
        e->rawcap = e->rawcap ? e->rawcap * 2 : 8;
        e->raw = xrealloc(e->raw, (size_t)e->rawcap * sizeof(struct xw));
    }
    e->raw[e->nraw++] = e->cur;
    memset(&e->cur, 0, sizeof(e->cur));
}

static int fail(struct ex *e, const char *fmt, const char *a, const char *b) {
    fprintf(stderr, "surfsh: ");
    fprintf(stderr, fmt, a, b);
    fputc('\n', stderr);
    e->failed = 1;
    return -1;
}

static void put_value(struct ex *e, const char *v, int dq) {
    if(v) xw_put(&e->cur, v, (int)strlen(v), dq ? XF_QUOTED : XF_SPLIT);
}

static int expand_part(struct ex *e, const char *w, int len, int dq, int doc);

/* where one string is made ("$@" in a here-document, a := word, a pattern): the pieces joined into
   e->cur by blanks */
static void join_pieces(struct ex *e) {
    struct xw w;
    int i;
    if(!e->nraw) return;
    memset(&w, 0, sizeof(w));
    for(i = 0; i < e->nraw; i++) {
        w.quoted |= e->raw[i].quoted;
        xw_put(&w, sb_str(&e->raw[i].s), e->raw[i].s.len, 0);
        if(e->raw[i].s.len) memcpy(w.f + w.s.len - e->raw[i].s.len, e->raw[i].f, (size_t)e->raw[i].s.len);
        xw_put(&w, " ", 1, XF_QUOTED);
        xw_free(&e->raw[i]);
    }
    w.quoted |= e->cur.quoted;
    xw_put(&w, sb_str(&e->cur.s), e->cur.s.len, 0);
    if(e->cur.s.len) memcpy(w.f + w.s.len - e->cur.s.len, e->cur.f, (size_t)e->cur.s.len);
    xw_free(&e->cur);
    free(e->raw);
    e->raw = NULL;
    e->nraw = e->rawcap = 0;
    e->cur = w;
}

/* text expanded by itself as one string (quotes gone, nothing split): the := and :? words, $(( )) */
static char *expand_string(const char *w, int len, int dq, int *failed) {
    struct ex x;
    char *r;
    memset(&x, 0, sizeof(x));
    expand_part(&x, w, len, dq, 0);
    *failed = x.failed;
    join_pieces(&x);
    r = xstrdup(sb_str(&x.cur.s));
    xw_free(&x.cur);
    return r;
}

/**** parameters ****/

static char numbuf[16];

/* a special parameter's value (? $ ! # - 0, a positional one), NULL when unset */
static const char *special(const char *name) {
    if(!strcmp(name, "?")) return snprintf(numbuf, sizeof(numbuf), "%d", last_status), numbuf;
    if(!strcmp(name, "$")) return snprintf(numbuf, sizeof(numbuf), "%d", shell_pid), numbuf;
    if(!strcmp(name, "!")) return jobs_last_pid() ? (snprintf(numbuf, sizeof(numbuf), "%d", jobs_last_pid()), numbuf) : NULL;
    if(!strcmp(name, "#")) return snprintf(numbuf, sizeof(numbuf), "%d", npos), numbuf;
    if(!strcmp(name, "-")) return options_string();
    if(!strcmp(name, "0")) return arg0;
    if(isdigit((unsigned char)name[0])) {
        long k = strtol(name, NULL, 10);
        return k >= 1 && k <= npos ? posargs[k - 1] : NULL;
    }
    return var_get(name);
}

/* 10/25/26: $name or ${name}'s value: NULL when it is not set, which set -u makes an error */
static const char *value_of(struct ex *e, const char *name) {
    const char *v = special(name);
    if(!v && opt_u) fail(e, "%s: not set", name, NULL);
    return v;
}

/* $@ and $* (at is '@' or '*'), in or out of double quotes */
static void put_all(struct ex *e, int at, int dq) {
    int k;
    if(dq && at == '*') {
        const char *sep = ifs();
        for(k = 0; k < npos; k++) {
            if(k && *sep) xw_put(&e->cur, sep, 1, XF_QUOTED);
            put_value(e, posargs[k], 1);
        }
        return;
    }
    for(k = 0; k < npos; k++) {
        if(k) field_break(e);
        if(dq) e->cur.quoted = 1;                   /* "$@": an empty parameter is an empty field */
        put_value(e, posargs[k], dq);
    }
}

/* the name a ${ } begins with: its length (a name, a number, or one special character) */
static int param_name(const char *s, int n) {
    int i = 0;
    if(n <= 0) return 0;
    if(isalpha((unsigned char)s[0]) || s[0] == '_') {
        while(i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
        return i;
    }
    if(isdigit((unsigned char)s[0])) {
        while(i < n && isdigit((unsigned char)s[i])) i++;
        return i;
    }
    return strchr("?$!#@*-", s[0]) ? 1 : 0;
}

/* ${name#pattern} and the others: the value without what the pattern matches */
static char *trim(const char *v, const char *op, const char *pat, const char *pq, int pn) {
    int len = (int)strlen(v), k;
    if(op[0] == '#') {                              /* a beginning: shortest (#) or longest (##) */
        for(k = op[1] ? len : 0; op[1] ? k >= 0 : k <= len; k += op[1] ? -1 : 1) {
            if(k < len && ((unsigned char)v[k] & 0xC0) == 0x80) continue;
            if(pmatch(pat, pq, pn, v, k)) return xstrdup(v + k);
        }
    } else {                                        /* an end: shortest (%) or longest (%%) */
        for(k = op[1] ? 0 : len; op[1] ? k <= len : k >= 0; k += op[1] ? 1 : -1) {
            if(k < len && ((unsigned char)v[k] & 0xC0) == 0x80) continue;
            if(pmatch(pat, pq, pn, v + k, len - k)) return xstrndup(v, (size_t)k);
        }
    }
    return xstrdup(v);
}

static int utf8_chars(const char *s) {
    int n = 0;
    for(; *s; s++) if(((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* the word of ${x:-word} or ${x:+word}: unquoted, its characters are split as a value's are */
static int word_of(struct ex *e, const char *word, int wlen, int dq) {
    int was = e->lit, r;
    e->lit = XF_SPLIT;
    r = expand_part(e, word, wlen, dq, 0);
    e->lit = was;
    return r;
}

/* ${ ... }: w[0 .. len) is what is between the braces */
static int braced_in(struct ex *e, const char *w, int len, int dq);

/* 10/25/26: ${ } inside ${ } goes through braced() again: at most DEPTH_LIMIT of them */
static int braced(struct ex *e, const char *w, int len, int dq) {
    static int depth;
    int r;
    if(depth >= DEPTH_LIMIT) {
        char n[16];
        snprintf(n, sizeof(n), "%d", DEPTH_LIMIT);
        return fail(e, "${ } inside each other: at most %s", n, NULL);
    }
    depth++;
    r = braced_in(e, w, len, dq);
    depth--;
    return r;
}

static int braced_in(struct ex *e, const char *w, int len, int dq) {
    char name[WORD_LIMIT + 1], op[3] = "";
    const char *v, *word;
    int n, wlen, is_set, colon = 0;
    if(len > 1 && w[0] == '#' && param_name(w + 1, len - 1) == len - 1) {     /* ${#name} */
        if(len - 1 > WORD_LIMIT) {                  /* 10/25/26: no name is that long */
            char what[WORD_LIMIT + 8];
            snprintf(what, sizeof(what), "%.*s", WORD_LIMIT, w);
            return fail(e, "${%s}: not a parameter", what, NULL);
        }
        memcpy(name, w + 1, (size_t)(len - 1));
        name[len - 1] = 0;
        if(!strcmp(name, "@") || !strcmp(name, "*")) snprintf(numbuf, sizeof(numbuf), "%d", npos);
        else if((v = value_of(e, name)) || !e->failed) snprintf(numbuf, sizeof(numbuf), "%d", v ? utf8_chars(v) : 0);
        else return -1;
        put_value(e, numbuf, dq);
        return 0;
    }
    n = param_name(w, len);
    if(!n || n > WORD_LIMIT) {
        char what[WORD_LIMIT + 8];
        snprintf(what, sizeof(what), "%.*s", len > WORD_LIMIT ? WORD_LIMIT : len, w);
        return fail(e, "${%s}: not a parameter", what, NULL);
    }
    memcpy(name, w, (size_t)n);
    name[n] = 0;
    if(n == len) {                                  /* ${name} */
        if(!strcmp(name, "@") || !strcmp(name, "*")) put_all(e, name[0], dq);
        else if(!(v = value_of(e, name)) && e->failed) return -1;
        else put_value(e, v, dq);
        return 0;
    }
    if(w[n] == ':' && n + 1 < len && strchr("-=?+", w[n + 1])) {
        colon = 1;
        op[0] = w[n + 1];
    } else if(strchr("-=?+", w[n])) {
        op[0] = w[n];
    } else if(w[n] == '#' || w[n] == '%') {
        op[0] = w[n];
        if(n + 1 < len && w[n + 1] == w[n]) op[1] = w[n];
    } else {
        char what[WORD_LIMIT + 8];
        snprintf(what, sizeof(what), "%.*s", len > WORD_LIMIT ? WORD_LIMIT : len, w);
        return fail(e, "${%s}: not understood", what, NULL);
    }
    word = w + n + colon + (int)strlen(op);
    wlen = len - (int)(word - w);
    if(!strcmp(name, "@") || !strcmp(name, "*")) {
        is_set = npos > 0;
        v = npos ? posargs[0] : NULL;                   /* empty or not, as far as :- is concerned */
    } else {
        v = special(name);
        is_set = v != NULL;
    }
    if(op[0] == '#' || op[0] == '%') {             /* the value without what the pattern matches */
        struct ex x;
        char *r, *val = xstrdup(v ? v : "");        /* 10/25/26: the pattern may change it ($((x=1)), ${x:=y}) */
        memset(&x, 0, sizeof(x));
        expand_part(&x, word, wlen, 0, 0);
        join_pieces(&x);
        if(x.failed) {
            xw_free(&x.cur);
            free(val);
            e->failed = 1;
            return -1;
        }
        {
            char *q = xmalloc((size_t)x.cur.s.len + 1);
            int i;
            for(i = 0; i < x.cur.s.len; i++) q[i] = x.cur.f[i] == XF_QUOTED;
            r = trim(val, op, sb_str(&x.cur.s), q, x.cur.s.len);
            free(q);
        }
        free(val);
        put_value(e, r, dq);
        free(r);
        xw_free(&x.cur);
        return 0;
    }
    if(op[0] == '+') {                              /* the word when set (and not empty, with :) */
        if(is_set && !(colon && !*v)) return word_of(e, word, wlen, dq);
        return 0;
    }
    if(is_set && !(colon && !*v)) {                 /* set: the value, for - = ? */
        if(!strcmp(name, "@") || !strcmp(name, "*")) put_all(e, name[0], dq);
        else put_value(e, v, dq);
        return 0;
    }
    if(op[0] == '-') return word_of(e, word, wlen, dq);
    {
        int failed;
        char *s = expand_string(word, wlen, dq, &failed);
        if(failed) {
            free(s);
            e->failed = 1;
            return -1;
        }
        if(op[0] == '?') {
            fail(e, "%s: %s", name, *s ? s : (is_set ? "is empty" : "not set"));
            free(s);
            return -1;
        }
        if(!is_name(name, (int)strlen(name))) {     /* := to $1, $?, ... */
            free(s);
            return fail(e, "${%s=...}: only a variable can be given a value this way", name, NULL);
        }
        if(var_set(name, s, 0) < 0) {
            free(s);
            e->failed = 1;
            return -1;
        }
        put_value(e, s, dq);
        free(s);
    }
    return 0;
}

/* $ at w[i]: what it expands to goes in; the index past it, or -1 */
static int dollar(struct ex *e, const char *w, int i, int len, int dq) {
    char name[WORD_LIMIT + 1];
    int p = i + 1, n;
    if(p >= len) {
        xw_put(&e->cur, "$", 1, dq ? XF_QUOTED : e->lit);
        return p;
    }
    if(w[p] == '{') {
        int end = word_skip(w, i);
        if(end > len) end = len;
        if(braced(e, w + p + 1, end - p - 2, dq) < 0) return -1;
        return end;
    }
    if(w[p] == '(') {
        int end = word_skip(w, i);
        if(end > len) end = len;
        if(w[p + 1] == '(') {                       /* $(( ... )) */
            char err[120];
            long v;
            int failed;
            char *text = expand_string(w + p + 2, end - p - 4, 1, &failed);
            if(failed) {
                free(text);
                e->failed = 1;
                return -1;
            }
            if(arith(text, &v, err, sizeof(err)) < 0) {
                fail(e, "$((%s)): %s", text, err);
                free(text);
                return -1;
            }
            free(text);
            snprintf(numbuf, sizeof(numbuf), "%ld", v);
            put_value(e, numbuf, dq);
        } else {                                    /* $( ... ) */
            struct sbuf out = { NULL, 0, 0 };
            char *text = xstrndup(w + p + 1, (size_t)(end - p - 2));
            subst_status = exec_capture(text, &out);
            subst_count++;
            free(text);
            put_value(e, sb_str(&out), dq);
            sb_free(&out);
        }
        return end;
    }
    if((n = param_name(w + p, len - p)) > 0) {
        if(isdigit((unsigned char)w[p])) n = 1;     /* $10 is $1 and a 0 */
        if(n > WORD_LIMIT) n = WORD_LIMIT;
        memcpy(name, w + p, (size_t)n);
        name[n] = 0;
        if(!strcmp(name, "@") || !strcmp(name, "*")) put_all(e, name[0], dq);
        else {
            const char *v = value_of(e, name);
            if(e->failed) return -1;
            put_value(e, v, dq);
        }
        return p + n;
    }
    xw_put(&e->cur, "$", 1, dq ? XF_QUOTED : e->lit);  /* a $ before anything else is itself */
    return p;
}

/* ` ... ` at w[i]: the command between, its \` \$ \\ (and \" in double quotes) as those characters */
static int backquote(struct ex *e, const char *w, int i, int len, int dq) {
    struct sbuf text = { NULL, 0, 0 }, out = { NULL, 0, 0 };
    int end = word_skip(w, i), k;
    if(end > len) end = len;
    for(k = i + 1; k < end - 1; k++) {
        if(w[k] == '\\' && k + 1 < end - 1 && (w[k + 1] == '`' || w[k + 1] == '$' || w[k + 1] == '\\' || (dq && w[k + 1] == '"')))
            k++;
        sb_putc(&text, w[k]);
    }
    subst_status = exec_capture(sb_str(&text), &out);
    subst_count++;
    put_value(e, sb_str(&out), dq);
    sb_free(&text);
    sb_free(&out);
    return end;
}

/* w[0 .. len) into e; dq: inside double quotes; doc: a here-document's text (quotes are themselves) */
static int expand_part(struct ex *e, const char *w, int len, int dq, int doc) {
    int i = 0;
    while(i < len && !e->failed) {
        char c = w[i];
        if(c == '\'' && !dq) {
            int end = i + 1;
            while(end < len && w[end] != '\'') end++;
            xw_put(&e->cur, w + i + 1, end - i - 1, XF_QUOTED);
            e->cur.quoted = 1;
            if(end == i + 1) xw_mark(&e->cur);
            i = end < len ? end + 1 : end;
        } else if(c == '"' && !dq) {
            int end = word_skip(w, i), raw = e->nraw, at = e->cur.s.len, field = 1;
            if(end > len) end = len;
            if(!(!npos && ((end - i - 2 == 2 && !memcmp(w + i + 1, "$@", 2)) ||
                           (end - i - 2 == 4 && !memcmp(w + i + 1, "${@}", 4)))))
                e->cur.quoted = 1;                  /* "$@" of no parameters is no field */
            else
                field = 0;
            expand_part(e, w + i + 1, end - i - 2 > 0 ? end - i - 2 : 0, 1, 0);
            if(field && e->nraw == raw && e->cur.s.len == at) xw_mark(&e->cur);
            i = end;
        } else if(c == '\\') {
            if(i + 1 >= len) {
                xw_put(&e->cur, "\\", 1, dq ? XF_QUOTED : e->lit);
                i++;
            } else if(w[i + 1] == '\n') {
                i += 2;
            } else if(dq && !strchr(doc ? "$`\\" : "$`\"\\", w[i + 1])) {
                xw_put(&e->cur, "\\", 1, XF_QUOTED);
                i++;
            } else {
                xw_put(&e->cur, w + i + 1, 1, XF_QUOTED);
                i += 2;
            }
        } else if(c == '$') {
            if((i = dollar(e, w, i, len, dq)) < 0) return -1;
        } else if(c == '`') {
            i = backquote(e, w, i, len, dq);
        } else {
            xw_put(&e->cur, &c, 1, dq ? XF_QUOTED : e->lit);
            i++;
        }
    }
    return e->failed ? -1 : 0;
}

/**** fields ****/

static int in(const char *set, char c) {
    return c && strchr(set, c) != NULL;
}

/* one field: the names its pattern matches, or itself (its quotes gone) */
static void emit(struct xw *field, struct fields *out) {
    char *q = xmalloc((size_t)field->s.len + 1);
    int j;
    for(j = 0; j < field->s.len; j++) q[j] = field->f[j] == XF_QUOTED;
    if(opt_f || !has_pattern(sb_str(&field->s), q, field->s.len) || !glob_field(sb_str(&field->s), q, field->s.len, out))
        fields_take(out, xstrdup(sb_str(&field->s)));
    free(q);
    field->s.len = 0;
    if(field->s.s) field->s.s[0] = 0;
}

/* A piece split at IFS (only where an unquoted expansion put its characters), each field then
   matched as a pattern, into out. POSIX's rule: IFS blanks at the start and at the end are none;
   blanks, or one other IFS character with the blanks around it, end a field; a field before an
   other character is there even when it is empty, a last one only when it is not. */
static void split_glob(struct xw *w, struct fields *out) {
    const char *set = ifs(), *s = sb_str(&w->s);
    char *f = w->f;
    int n = w->s.len, i = 0, made = 0, m = 0, begun = 0;
    struct xw field;
    memset(&field, 0, sizeof(field));
#define SPLITS(k) ((f[k] & XF_SPLIT) && in(set, s[k]))
#define BLANK_AT(k) (SPLITS(k) && in(" \t\n", s[k]))
#define MARK_AT(k) (m < w->nmarks && w->marks[m] == (k))  /* 10/25/26: an empty quote: a field begins */
    if(!*set) {                                     /* IFS empty: nothing is split */
        if(n || w->quoted) {
            xw_put(&field, s, n, 0);
            memcpy(field.f, f, (size_t)n);
            emit(&field, out);
        }
        xw_free(&field);
        return;
    }
    while(i < n && BLANK_AT(i) && !MARK_AT(i)) i++;
    while(i < n) {
        int other = 0;
        for(;;) {
            while(MARK_AT(i)) {
                m++;
                begun = 1;
            }
            if(i == n || SPLITS(i)) break;
            xw_put(&field, s + i, 1, f[i]);
            i++;
        }
        if(i == n) break;
        while(i < n && BLANK_AT(i) && !MARK_AT(i)) i++;
        if(i < n && SPLITS(i) && !BLANK_AT(i) && !MARK_AT(i)) {
            other = 1;
            i++;
            while(i < n && BLANK_AT(i) && !MARK_AT(i)) i++;
        }
        if(field.s.len || other || begun) {
            emit(&field, out);
            made++;
        }
        begun = 0;
    }
    while(MARK_AT(n)) {
        m++;
        begun = 1;
    }
    if(field.s.len || begun) {
        emit(&field, out);
        made++;
    }
#undef SPLITS
#undef BLANK_AT
#undef MARK_AT
    if(!made && w->quoted) fields_take(out, xstrdup(""));      /* "" and "$x" of an empty x */
    xw_free(&field);
}

/* the fields of one word into out: 0, or -1 said */
static int expand_word(const char *word, struct fields *out) {
    struct ex e;
    int i, r;
    memset(&e, 0, sizeof(e));
    r = expand_part(&e, word, (int)strlen(word), 0, 0);
    field_break(&e);
    for(i = 0; i < e.nraw; i++) {
        if(r == 0) split_glob(&e.raw[i], out);
        xw_free(&e.raw[i]);
    }
    free(e.raw);
    return r;
}

int expand_words(char **words, int n, struct fields *out) {
    int i;
    for(i = 0; i < n; i++) {
        if(expand_word(words[i], out) < 0) {
            fields_free(out);
            return -1;
        }
    }
    return 0;
}

/* a word as one string: quotes gone, nothing split nor matched (a file's name, an assignment's
   value); NULL said when an expansion failed */
char *expand_one(const char *word) {
    int failed;
    char *s = expand_string(word, (int)strlen(word), 0, &failed);
    if(!failed) return s;
    free(s);
    return NULL;
}

char *expand_doc(const char *body) {
    struct ex e;
    char *r = NULL;
    memset(&e, 0, sizeof(e));
    expand_part(&e, body, (int)strlen(body), 1, 1);
    join_pieces(&e);
    if(!e.failed) r = xstrdup(sb_str(&e.cur.s));
    xw_free(&e.cur);
    return r;
}

/* a pattern (case, later): its characters and which of them were quoted, malloc'ed; -1 said */
int expand_pattern(const char *word, char **pat, char **quoted, int *len) {
    struct ex e;
    int i;
    memset(&e, 0, sizeof(e));
    expand_part(&e, word, (int)strlen(word), 0, 0);
    join_pieces(&e);
    if(e.failed) {
        xw_free(&e.cur);
        return -1;
    }
    *len = e.cur.s.len;
    *pat = xstrdup(sb_str(&e.cur.s));
    *quoted = xmalloc((size_t)*len + 1);
    for(i = 0; i < *len; i++) (*quoted)[i] = e.cur.f[i] == XF_QUOTED;
    xw_free(&e.cur);
    return 0;
}
