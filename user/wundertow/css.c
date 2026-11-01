/*
SurfOS Undertow's window: style sheets
----------------------
File: css.c  Date: 11/01/26
----------------------
css.h's reader, in one pass over the bytes as CSS Syntax Level 3 says: tokens (next()), then rules and at-rules
(rules(), qualified(), at_rule()), then selectors as their tokens come (struct sel) and declarations (declaration(),
the r_* readers). Every loop moves forward through the sheet, so a sheet costs its length; brackets are followed on
a stack of CSS_DEPTH_MAX, and nothing else recurses but @media, MEDIA_DEPTH deep. url() is one token, and what is in
it is not looked at.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "css.h"

#define WORD_MAX    24                  /* bytes of a name kept to compare (the longest word here is 20) */
#define VALUES_MAX  32                  /* tokens of a declaration's value that are looked at */
#define MEDIA_DEPTH 4                   /* @media in @media; deeper ones are skipped */
#define TYPE_CHARS  15                  /* a tag's name as html_scan.h keeps it */
#define NUM_MAX     65536               /* numbers saturate at this */
#define ONE         256                 /* a number's 1 */
#define FNV_BASIS   2166136261u
#define FNV_PRIME   16777619u

/**** bytes ****/

static int ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int nl(unsigned char c) { return c == '\n' || c == '\r' || c == '\f'; }
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int alpha(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }
static int name_start(unsigned char c) { return alpha(c) || c == '_' || c >= 0x80 || !c; }
static int name_char(unsigned char c) { return name_start(c) || digit(c) || c == '-'; }

static int hexval(unsigned char c) {
    if(digit(c)) return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static unsigned int fix(unsigned int h) { return h ? h : 1; }

unsigned int css_name(const char *s, size_t n) {
    unsigned int h = FNV_BASIS;
    size_t i;
    for(i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * FNV_PRIME;
    return fix(h);
}

unsigned int css_type(const char *s, size_t n) {
    unsigned int h = FNV_BASIS;
    size_t i;
    for(i = 0; i < n && i < TYPE_CHARS; i++) h = (h ^ lower((unsigned char)s[i])) * FNV_PRIME;
    return fix(h);
}

/**** tokens ****/

enum {
    T_EOF, T_WS, T_IDENT, T_FUNC, T_AT, T_HASH, T_STRING, T_BAD, T_URL, T_NUMBER, T_PERCENT, T_DIM, T_CDO, T_CDC,
    T_COLON, T_SEMI, T_COMMA, T_LBRACK, T_RBRACK, T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_DELIM
};

struct tok {
    unsigned char type;                 /* T_* */
    unsigned char delim;                /* T_DELIM: the byte */
    unsigned char ident;                /* T_HASH: its name could be an identifier (an id) */
    int num;                            /* T_NUMBER, T_PERCENT, T_DIM: in 1/256ths, within +-(NUM_MAX * 256) */
    unsigned int hash, lhash;           /* a name as written; in lowercase, its first TYPE_CHARS bytes */
    unsigned int nlen;                  /* its bytes */
    char text[WORD_MAX];                /* in lowercase, cut: a function's without the '(', a dimension's unit */
};

struct lex {
    const unsigned char *s;
    size_t n, i;
    char str[CSS_URL_MAX + 1];          /* the last string, cut at CSS_URL_MAX (str_over) */
    size_t nstr;
    int str_over;
};

static void name_byte(struct tok *t, unsigned char c) {
    unsigned char l = lower(c);
    t->hash = (t->hash ^ c) * FNV_PRIME;
    if(t->nlen < TYPE_CHARS) t->lhash = (t->lhash ^ l) * FNV_PRIME;
    if(t->nlen + 1 < WORD_MAX) {
        t->text[t->nlen] = (char)(l ? l : 0xff);    /* (a NUL is U+FFFD: it ends no word) */
        t->text[t->nlen + 1] = 0;
    }
    if(t->nlen < WORD_MAX) t->nlen++;
    else t->nlen = WORD_MAX;
}

/* s[i] is a backslash's next byte, i may be n. Its character in u (*k bytes): what follows, past the escape. */
static size_t escape(const unsigned char *s, size_t n, size_t i, unsigned char *u, size_t *k) {
    unsigned int v = 0;
    int d = 0, h;
    if(i >= n) {
        *k = ut_utf8_encode(UT_REPLACEMENT, u);
        return i;
    }
    if(hexval(s[i]) < 0) {                          /* the byte itself; of a longer character, its first */
        u[0] = s[i];
        *k = 1;
        return i + 1;
    }
    while(d < 6 && i < n && (h = hexval(s[i])) >= 0) {
        v = v * 16 + (unsigned int)h;
        i++;
        d++;
    }
    if(i + 1 < n && s[i] == '\r' && s[i + 1] == '\n') i += 2;
    else if(i < n && ws(s[i])) i++;
    *k = ut_utf8_encode(v ? v : UT_REPLACEMENT, u);  /* (a surrogate or too large: U+FFFD too) */
    return i;
}

static int valid_escape(const unsigned char *s, size_t n, size_t i) { return s[i] == '\\' && (i + 1 >= n || !nl(s[i + 1])); }

static void name(struct lex *x, struct tok *t) {
    const unsigned char *s = x->s;
    size_t n = x->n, i = x->i;
    while(i < n) {
        if(name_char(s[i])) name_byte(t, s[i++]);
        else if(valid_escape(s, n, i)) {
            unsigned char u[4];
            size_t k, j;
            i = escape(s, n, i + 1, u, &k);
            for(j = 0; j < k; j++) name_byte(t, u[j]);
        } else break;
    }
    x->i = i;
}

static int starts_ident(const unsigned char *s, size_t n, size_t i) {
    if(i >= n) return 0;
    if(s[i] == '-') {
        if(i + 1 >= n) return 0;
        return name_start(s[i + 1]) || s[i + 1] == '-' || valid_escape(s, n, i + 1);
    }
    return name_start(s[i]) || valid_escape(s, n, i);
}

static int starts_number(const unsigned char *s, size_t n, size_t i) {
    if(i < n && (s[i] == '+' || s[i] == '-')) i++;
    if(i < n && digit(s[i])) return 1;
    return i + 1 < n && s[i] == '.' && digit(s[i + 1]);
}

/* a number at x->i, into t: saturated, and its first four decimals are all that counts */
static void number(struct lex *x, struct tok *t) {
    const unsigned char *s = x->s;
    size_t n = x->n, i = x->i, k;
    unsigned int whole = 0, frac = 0, scale = 1;
    int neg = 0, eneg = 0, e = 0, v;
    if(s[i] == '+' || s[i] == '-') neg = s[i++] == '-';
    for(; i < n && digit(s[i]); i++) if(whole <= NUM_MAX) whole = whole * 10 + (s[i] - '0');
    if(i + 1 < n && s[i] == '.' && digit(s[i + 1])) {
        for(i++; i < n && digit(s[i]); i++) {
            if(scale < 10000) {
                frac = frac * 10 + (s[i] - '0');
                scale *= 10;
            }
        }
    }
    if(i < n && (s[i] | 0x20) == 'e') {
        k = i + 1;
        if(k < n && (s[k] == '+' || s[k] == '-')) eneg = s[k++] == '-';
        if(k < n && digit(s[k])) for(i = k; i < n && digit(s[i]); i++) if(e < 100) e = e * 10 + (s[i] - '0');
    }
    if(whole > NUM_MAX) whole = NUM_MAX;
    v = (int)(whole * ONE + frac * ONE / scale);
    if(v > NUM_MAX * ONE) v = NUM_MAX * ONE;
    while(e-- > 0 && v && (eneg || v < NUM_MAX * ONE)) {
        v = eneg ? v / 10 : v * 10;
        if(v > NUM_MAX * ONE) v = NUM_MAX * ONE;
    }
    t->num = neg ? -v : v;
    x->i = i;
}

static void numeric(struct lex *x, struct tok *t) {
    number(x, t);
    if(x->i < x->n && x->s[x->i] == '%') {
        x->i++;
        t->type = T_PERCENT;
    } else if(starts_ident(x->s, x->n, x->i)) {
        name(x, t);
        t->type = T_DIM;
    } else t->type = T_NUMBER;
}

static void str_add(struct lex *x, unsigned char c) {
    if(!c) {                                        /* a NUL is U+FFFD */
        str_add(x, 0xef);
        str_add(x, 0xbf);
        c = 0xbd;
    }
    if(x->nstr < CSS_URL_MAX) {
        x->str[x->nstr++] = (char)c;
        x->str[x->nstr] = 0;
    } else x->str_over = 1;
}

static void string(struct lex *x, struct tok *t, unsigned char quote) {
    const unsigned char *s = x->s;
    size_t n = x->n, i = x->i + 1;
    x->nstr = 0;
    x->str[0] = 0;
    x->str_over = 0;
    t->type = T_STRING;
    while(i < n) {
        unsigned char c = s[i];
        if(c == quote) {
            x->i = i + 1;
            return;
        }
        if(nl(c)) {                                 /* a string ends where its line does: the rest is not it */
            x->i = i;
            t->type = T_BAD;
            return;
        }
        if(c == '\\') {
            if(i + 1 < n && nl(s[i + 1])) i += s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n' ? 3 : 2;
            else if(i + 1 >= n) i++;
            else {
                unsigned char u[4];
                size_t k, j;
                i = escape(s, n, i + 1, u, &k);
                for(j = 0; j < k; j++) str_add(x, u[j]);
            }
            continue;
        }
        str_add(x, c);
        i++;
    }
    x->i = n;
}

/* A name, and what it is by what follows. url(address) without quotes is one token, whose address is not kept; a
   malformed one (a space inside it, a quote, a bracket, a control) is that one token and a bad one. */
static void ident_like(struct lex *x, struct tok *t) {
    const unsigned char *s = x->s;
    size_t n = x->n, i;
    int bad = 0;
    name(x, t);
    t->type = T_IDENT;
    if(x->i >= n || s[x->i] != '(') return;
    x->i++;
    t->type = T_FUNC;
    if(strcmp(t->text, "url")) return;
    for(i = x->i; i < n && ws(s[i]); i++) ;
    if(i < n && (s[i] == '"' || s[i] == '\'')) return;
    while(i < n && s[i] != ')') {
        unsigned char c = s[i];
        if(c == '\\') {
            if(i + 1 < n && nl(s[i + 1])) bad = 1;
            i += 2;
            continue;
        }
        if(ws(c)) {
            while(i < n && ws(s[i])) i++;
            if(i < n && s[i] != ')') bad = 1;
            continue;
        }
        if(c == '"' || c == '\'' || c == '(' || (c && c < 0x20) || c == 0x7f) bad = 1;
        i++;
    }
    x->i = i < n ? i + 1 : n;
    t->type = bad ? T_BAD : T_URL;
}

static void next(struct lex *x, struct tok *t) {
    const unsigned char *s = x->s;
    size_t n = x->n, i;
    unsigned char c;
    t->delim = t->ident = 0;
    t->num = 0;
    t->hash = t->lhash = FNV_BASIS;
    t->nlen = 0;
    t->text[0] = 0;
    for(;;) {                                       /* comments are nothing */
        i = x->i;
        if(i + 1 >= n || s[i] != '/' || s[i + 1] != '*') break;
        for(i += 2; i + 1 < n && (s[i] != '*' || s[i + 1] != '/'); i++) ;
        x->i = i + 1 < n ? i + 2 : n;
    }
    if(i >= n) {
        t->type = T_EOF;
        return;
    }
    c = s[i];
    if(ws(c)) {
        while(i < n && ws(s[i])) i++;
        x->i = i;
        t->type = T_WS;
        return;
    }
    if(digit(c) || ((c == '+' || c == '-' || c == '.') && starts_number(s, n, i))) {
        numeric(x, t);
        return;
    }
    if(c == '"' || c == '\'') {
        string(x, t, c);
        return;
    }
    if(c == '-' && i + 2 < n && s[i + 1] == '-' && s[i + 2] == '>') {
        x->i = i + 3;
        t->type = T_CDC;
        return;
    }
    if(starts_ident(s, n, i)) {
        ident_like(x, t);
        return;
    }
    if(c == '<' && n - i >= 4 && !memcmp(s + i, "<!--", 4)) {
        x->i = i + 4;
        t->type = T_CDO;
        return;
    }
    if((c == '#' && i + 1 < n && (name_char(s[i + 1]) || valid_escape(s, n, i + 1))) ||
       (c == '@' && starts_ident(s, n, i + 1))) {
        t->ident = (unsigned char)starts_ident(s, n, i + 1);
        x->i = i + 1;
        name(x, t);
        t->type = c == '#' ? T_HASH : T_AT;
        return;
    }
    x->i = i + 1;
    switch(c) {
    case '(': t->type = T_LPAREN; return;
    case ')': t->type = T_RPAREN; return;
    case '[': t->type = T_LBRACK; return;
    case ']': t->type = T_RBRACK; return;
    case '{': t->type = T_LBRACE; return;
    case '}': t->type = T_RBRACE; return;
    case ',': t->type = T_COMMA; return;
    case ':': t->type = T_COLON; return;
    case ';': t->type = T_SEMI; return;
    }
    t->type = T_DELIM;
    t->delim = c;
}

/**** the parser's state ****/

struct parser {
    struct lex x;
    struct tok t;                       /* the token now */
    int again;                          /* fetch() gives it once more */
    struct css *c;                      /* a sheet's rules go here; NULL: declarations go to out */
    struct css_decl *out;
    size_t nout, cap;
    int stop;                           /* a limit: no more of this sheet */
    int media;                          /* @media blocks open */
    struct tok vals[VALUES_MAX];        /* the value being read, its whitespace left out */
    int nvals;
    int bad;                            /* it has what no value here may have: a stray bracket, a broken string */
    int custom;                         /* it is a custom property's (--name): a block is part of it */
    char url[CSS_URL_MAX + 1];          /* css_imports()'s: an @import's address */
};

static struct parser P_work;            /* 6 KB, off the stack; nothing here runs twice at once */

static void start(struct parser *P, const char *s, size_t n, struct css *c, struct css_decl *out, size_t cap) {
    P->x.s = (const unsigned char *)s;
    P->x.n = n;
    P->x.i = 0;
    P->x.nstr = 0;
    P->x.str_over = 0;
    P->again = 0;
    P->c = c;
    P->out = out;
    P->nout = 0;
    P->cap = cap;
    P->stop = 0;
    P->media = 0;
    P->custom = 0;
    if(!c && out) return;
    if(n >= 3 && !memcmp(s, "\xef\xbb\xbf", 3)) P->x.i = 3;       /* a sheet's byte order mark is no name */
}

static void fetch(struct parser *P) {
    if(P->again) P->again = 0;
    else next(&P->x, &P->t);
}

/* A block's opener was read: past its closer (closer: what closes it), the brackets inside followed. A closer of
   another kind inside it is only a token, as the standard says. */
static void skip_block(struct parser *P, int closer) {
    unsigned char open[CSS_DEPTH_MAX];
    int depth = 1;
    open[0] = (unsigned char)closer;
    while(depth) {
        fetch(P);
        switch(P->t.type) {
        case T_EOF:
            return;
        case T_LBRACE: case T_LBRACK: case T_LPAREN: case T_FUNC:
            if(depth == CSS_DEPTH_MAX) {
                if(P->c) P->c->truncated = 1;
                P->stop = 1;
                return;
            }
            open[depth++] = P->t.type == T_LBRACE ? T_RBRACE : P->t.type == T_LBRACK ? T_RBRACK : T_RPAREN;
            break;
        case T_RBRACE: case T_RBRACK: case T_RPAREN:
            if(open[depth - 1] == P->t.type) depth--;
            break;
        }
    }
}

static int grow(struct css *c, void **arr, size_t count, size_t *cap, size_t size) {
    size_t n;
    void *p;
    if(count < *cap) return 0;
    n = *cap ? *cap * 2 : 64;
    p = realloc(*arr, n * size);
    if(!p) {
        c->failed = 1;
        return -1;
    }
    *arr = p;
    *cap = n;
    return 0;
}

static void limit(struct parser *P) {
    P->c->truncated = 1;
    P->stop = 1;
}

/**** values ****/

static void emit(struct parser *P, int prop, int unit, int value, int important) {
    struct css_decl *d;
    if(P->c) {
        struct css *c = P->c;
        if(P->stop) return;
        if(c->ndecls >= CSS_DECLS_MAX) {
            limit(P);
            return;
        }
        if(grow(c, (void **)&c->decls, c->ndecls, &c->cdecls, sizeof(*c->decls))) {
            P->stop = 1;
            return;
        }
        d = &c->decls[c->ndecls++];
    } else {
        if(P->nout >= P->cap) return;
        d = &P->out[P->nout++];
    }
    d->prop = (unsigned char)prop;
    d->unit = (unsigned char)unit;
    d->important = (unsigned char)important;
    d->value = value;
}

static int word(const struct tok *t, const char *w) { return t->type == T_IDENT && !strcmp(t->text, w); }

static int one_of(const char *s, const char *const *list) {     /* its place in a list that ends with NULL, or -1 */
    int i;
    for(i = 0; list[i]; i++) if(!strcmp(s, list[i])) return i;
    return -1;
}

/**** colours ****/

static const struct { const char *name; unsigned int rgb; } colors[] = {    /* CSS Color's, sorted */
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff }, { "aquamarine", 0x7fffd4 },
    { "azure", 0xf0ffff }, { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 }, { "black", 0x000000 },
    { "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff }, { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a },
    { "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 }, { "chocolate", 0xd2691e },
    { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c },
    { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b },
    { "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b },
    { "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f }, { "darkorange", 0xff8c00 },
    { "darkorchid", 0x9932cc }, { "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f },
    { "darkslateblue", 0x483d8b }, { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f },
    { "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 }, { "deepskyblue", 0x00bfff },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 },
    { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 }, { "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc },
    { "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 }, { "gray", 0x808080 },
    { "green", 0x008000 }, { "greenyellow", 0xadff2f }, { "grey", 0x808080 }, { "honeydew", 0xf0fff0 },
    { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c }, { "indigo", 0x4b0082 }, { "ivory", 0xfffff0 },
    { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa }, { "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 },
    { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 }, { "lightcoral", 0xf08080 }, { "lightcyan", 0xe0ffff },
    { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 }, { "lightgreen", 0x90ee90 },
    { "lightgrey", 0xd3d3d3 }, { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a }, { "lightseagreen", 0x20b2aa },
    { "lightskyblue", 0x87cefa }, { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 },
    { "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 },
    { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 }, { "mediumaquamarine", 0x66cdaa },
    { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 }, { "mediumpurple", 0x9370db },
    { "mediumseagreen", 0x3cb371 }, { "mediumslateblue", 0x7b68ee }, { "mediumspringgreen", 0x00fa9a },
    { "mediumturquoise", 0x48d1cc }, { "mediumvioletred", 0xc71585 }, { "midnightblue", 0x191970 },
    { "mintcream", 0xf5fffa }, { "mistyrose", 0xffe4e1 }, { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead },
    { "navy", 0x000080 }, { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 }, { "olivedrab", 0x6b8e23 },
    { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 }, { "palegoldenrod", 0xeee8aa },
    { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee }, { "palevioletred", 0xdb7093 },
    { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 }, { "peru", 0xcd853f }, { "pink", 0xffc0cb },
    { "plum", 0xdda0dd }, { "powderblue", 0xb0e0e6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 },
    { "red", 0xff0000 }, { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 }, { "saddlebrown", 0x8b4513 },
    { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 }, { "seashell", 0xfff5ee },
    { "sienna", 0xa0522d }, { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xfffafa }, { "springgreen", 0x00ff7f },
    { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c }, { "teal", 0x008080 }, { "thistle", 0xd8bfd8 },
    { "tomato", 0xff6347 }, { "turquoise", 0x40e0d0 }, { "violet", 0xee82ee }, { "wheat", 0xf5deb3 },
    { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 }, { "yellowgreen", 0x9acd32 }
};

static int named_color(const char *s, unsigned int *rgb) {
    size_t lo = 0, hi = sizeof(colors) / sizeof(colors[0]);
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(colors[mid].name, s);
        if(!c) {
            *rgb = colors[mid].rgb;
            return 1;
        }
        if(c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Partly see-through colours over white: a page is usually light, and nothing here blends */
static unsigned int mixed(int r, int g, int b, int a) {
    r = (r * a + 255 * (255 - a) + 127) / 255;
    g = (g * a + 255 * (255 - a) + 127) / 255;
    b = (b * a + 255 * (255 - a) + 127) / 255;
    return (unsigned int)r << 16 | (unsigned int)g << 8 | (unsigned int)b;
}

static int channel(const struct tok *t) {           /* of rgb(): 0-255, or 0-100% */
    if(t->type == T_PERCENT) return (clamp(t->num, 0, 100 * ONE) * 255 + 50 * ONE) / (100 * ONE);
    return clamp((t->num + ONE / 2) / ONE, 0, 255);
}

static int alpha_of(const struct tok *t) {          /* 0-1, or 0-100% */
    if(t->type == T_PERCENT) return (clamp(t->num, 0, 100 * ONE) * 255 + 50 * ONE) / (100 * ONE);
    return (clamp(t->num, 0, ONE) * 255 + ONE / 2) / ONE;
}

static unsigned int from_hsl(int h, int s, int l) {  /* h in degrees, s and l in percent: in thousandths below */
    int c, x, m, r = 0, g = 0, b = 0, hh;
    h = ((h % 360) + 360) % 360;
    s = clamp(s, 0, 100);
    l = clamp(l, 0, 100);
    c = (1000 - (l * 20 - 1000 < 0 ? 1000 - l * 20 : l * 20 - 1000)) * s / 100;
    hh = h * 1000 / 60;
    x = hh % 2000 - 1000;
    x = c * (1000 - (x < 0 ? -x : x)) / 1000;
    m = l * 10 - c / 2;
    switch(hh / 1000) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    return (unsigned int)(((r + m) * 255 + 500) / 1000) << 16 | (unsigned int)(((g + m) * 255 + 500) / 1000) << 8 |
           (unsigned int)(((b + m) * 255 + 500) / 1000);
}

/* A colour function's numbers (commas and '/' between them count for nothing): how many, or -1; *i past its ')' */
static int color_args(const struct tok *v, int n, int *i, const struct tok **arg, int cap) {
    int j = *i + 1, k = 0;
    while(j < n && v[j].type != T_RPAREN) {
        if(v[j].type == T_COMMA || (v[j].type == T_DELIM && v[j].delim == '/')) {
            j++;
            continue;
        }
        if((v[j].type != T_NUMBER && v[j].type != T_PERCENT && v[j].type != T_DIM) || k == cap) return -1;
        arg[k++] = &v[j++];
    }
    *i = j < n ? j + 1 : n;                         /* (the sheet's end closes it) */
    return k;
}

static int fn_color(const struct tok *v, int n, int *i, int *unit, int *rgb) {
    const struct tok *a[4];
    int fn = one_of(v[*i].text, (const char *const[]){ "rgb", "rgba", "hsl", "hsla", NULL });
    int k, op = 255;
    if(fn < 0) return 0;
    if((k = color_args(v, n, i, a, 4)) < 3) return 0;
    if(k == 4) op = alpha_of(a[3]);
    if(!op) {
        *unit = CU_TRANSPARENT;
        *rgb = 0;
        return 1;
    }
    *unit = CU_RGB;
    if(fn < 2) {
        if(a[0]->type == T_DIM || a[1]->type == T_DIM || a[2]->type == T_DIM) return 0;
        *rgb = (int)mixed(channel(a[0]), channel(a[1]), channel(a[2]), op);
    } else {
        unsigned int c;
        if(a[0]->type == T_PERCENT || (a[0]->type == T_DIM && strcmp(a[0]->text, "deg"))) return 0;
        c = from_hsl(a[0]->num / ONE, a[1]->num / ONE, a[2]->num / ONE);
        *rgb = (int)mixed((int)(c >> 16), (int)(c >> 8 & 255), (int)(c & 255), op);
    }
    return 1;
}

/* A colour at v[*i]: 1, *unit (CU_RGB, CU_TRANSPARENT, CU_INHERIT for currentcolor), *rgb and *i past it, or 0 */
static int color(const struct tok *v, int n, int *i, int *unit, int *rgb) {
    const struct tok *t = &v[*i];
    if(t->type == T_HASH) {
        int d[8], k = (int)strlen(t->text), j, c[4];
        if(k != 3 && k != 4 && k != 6 && k != 8) return 0;
        for(j = 0; j < k; j++) if((d[j] = hexval((unsigned char)t->text[j])) < 0) return 0;
        for(j = 0; j < 4; j++) {
            if(k <= 4) c[j] = j < k ? d[j] * 17 : 255;
            else c[j] = j * 2 < k ? d[j * 2] * 16 + d[j * 2 + 1] : 255;
        }
        (*i)++;
        if(!c[3]) {
            *unit = CU_TRANSPARENT;
            *rgb = 0;
        } else {
            *unit = CU_RGB;
            *rgb = (int)mixed(c[0], c[1], c[2], c[3]);
        }
        return 1;
    }
    if(t->type == T_FUNC) return fn_color(v, n, i, unit, rgb);
    if(t->type != T_IDENT) return 0;
    if(!strcmp(t->text, "transparent") || !strcmp(t->text, "currentcolor")) {
        *unit = t->text[0] == 't' ? CU_TRANSPARENT : CU_INHERIT;
        *rgb = 0;
    } else {
        unsigned int c;
        if(!named_color(t->text, &c)) return 0;
        *unit = CU_RGB;
        *rgb = (int)c;
    }
    (*i)++;
    return 1;
}

/**** lengths ****/

/* A length: *unit (CU_PX, CU_EM, CU_REM, CU_PCT) and *value, or 0 */
static int length(const struct tok *t, int *unit, int *value) {
    static const struct { const char *name; int unit, mul, div; } units[] = {
        { "px", CU_PX, 1, 1 }, { "pt", CU_PX, 4, 3 }, { "pc", CU_PX, 16, 1 }, { "in", CU_PX, 96, 1 },
        { "cm", CU_PX, 189, 5 }, { "mm", CU_PX, 189, 50 }, { "q", CU_PX, 189, 200 }, { "em", CU_EM, 1, 1 },
        { "rem", CU_REM, 1, 1 }, { "ex", CU_EM, 1, 2 }, { "ch", CU_EM, 1, 2 }
    };
    size_t i;
    if(t->type == T_NUMBER) {
        if(t->num) return 0;                        /* only 0 goes without a unit */
        *unit = CU_PX;
        *value = 0;
        return 1;
    }
    if(t->type == T_PERCENT) {
        *unit = CU_PCT;
        *value = t->num;
        return 1;
    }
    if(t->type != T_DIM) return 0;
    for(i = 0; i < sizeof(units) / sizeof(units[0]); i++)
        if(!strcmp(t->text, units[i].name)) {
            *unit = units[i].unit;
            *value = clamp(t->num, -(1 << 20), 1 << 20) * units[i].mul / units[i].div;
            return 1;
        }
    return 0;
}

/* A font's size, a keyword too: 1, or 0 for none */
static int font_size(const struct tok *t, int *unit, int *value) {
    static const struct { const char *name; int unit, value; } sizes[] = {
        { "xx-small", CU_BASE, ONE * 3 / 5 }, { "x-small", CU_BASE, ONE * 3 / 4 }, { "small", CU_BASE, ONE * 8 / 9 },
        { "medium", CU_BASE, ONE }, { "large", CU_BASE, ONE * 6 / 5 }, { "x-large", CU_BASE, ONE * 3 / 2 },
        { "xx-large", CU_BASE, ONE * 2 }, { "xxx-large", CU_BASE, ONE * 3 }, { "smaller", CU_EM, ONE * 5 / 6 },
        { "larger", CU_EM, ONE * 6 / 5 }
    };
    size_t i;
    if(t->type == T_IDENT) {
        for(i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if(!strcmp(t->text, sizes[i].name)) {
                *unit = sizes[i].unit;
                *value = sizes[i].value;
                return 1;
            }
        return 0;
    }
    return length(t, unit, value) && *value >= 0;
}

/**** the declarations it knows ****/

static void r_color(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    int i = 0, unit, rgb;
    if(!color(v, n, &i, &unit, &rgb) || i != n) return;
    if(prop == CP_COLOR ? unit == CU_TRANSPARENT : unit == CU_INHERIT) return;
    emit(P, prop, unit, rgb, imp);
}

/* background: its colour is what counts, images and the rest are read past (an image's url() is never followed).
   As a shorthand it says "no colour" when it gives none. */
static void r_background(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    static const char *const words[] = { "none", "repeat", "repeat-x", "repeat-y", "no-repeat", "round", "space", "top",
                                         "bottom", "left", "right", "center", "cover", "contain", "fixed", "scroll",
                                         "local", "border-box", "padding-box", "content-box", "text", "auto", NULL };
    static const char *const placed[] = { "url", "calc", "min", "max", "clamp", NULL };
    int i = 0, depth = 0, unit = CU_TRANSPARENT, rgb = 0, found = 0, gradient = 0;
    (void)prop;
    while(i < n) {
        int j = i, u, c;
        if(depth) {
            if(v[i].type == T_FUNC || v[i].type == T_LPAREN) depth++;
            else if(v[i].type == T_RPAREN) depth--;
            i++;
        } else if(!found && color(v, n, &j, &u, &c) && u != CU_INHERIT) {
            unit = u;
            rgb = c;
            found = 1;
            i = j;
        } else {
            if(v[i].type == T_FUNC) {
                if(one_of(v[i].text, placed) < 0) { /* (a name is cut: "gradi" finds repeating-linear-gradient's) */
                    if(!strstr(v[i].text, "gradi") && !strstr(v[i].text, "repeating") && !strstr(v[i].text, "image-set"))
                        return;                     /* var() and the like: an earlier colour stands */
                    gradient = 1;
                }
                depth++;
            } else if(v[i].type == T_IDENT ? one_of(v[i].text, words) < 0
                                           : v[i].type != T_URL && v[i].type != T_COMMA && !length(&v[i], &u, &c) &&
                                             (v[i].type != T_DELIM || v[i].delim != '/')) return;
            i++;
        }
    }
    if(gradient && !found) return;                  /* a gradient is not shown: the colour before it is its stand-in */
    emit(P, CP_BACKGROUND, unit, rgb, imp);
}

static void r_display(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    static const char *const none[] = { "none", NULL };
    static const char *const block[] = { "block", "flex", "grid", "table", "flow-root", "list-item", NULL };
    static const char *const inl[] = { "inline", "inline-block", "inline-flex", "inline-grid", "inline-table", NULL };
    static const char *const keep[] = { "contents", "ruby", "ruby-text", "table-caption", "table-cell", "table-column",
                                        "table-column-group", "table-footer-group", "table-header-group", "table-row",
                                        "table-row-group", "-webkit-box", "-webkit-flex", NULL };
    if(n != 1 || v[0].type != T_IDENT) return;
    if(one_of(v[0].text, none) >= 0) emit(P, prop, CU_NONE, CSS_D_NONE, imp);
    else if(one_of(v[0].text, block) >= 0) emit(P, prop, CU_NONE, CSS_D_BLOCK, imp);
    else if(one_of(v[0].text, inl) >= 0) emit(P, prop, CU_NONE, CSS_D_INLINE, imp);
    else if(one_of(v[0].text, keep) >= 0) emit(P, prop, CU_NONE, CSS_D_ASIS, imp);   /* shown, as HTML has it */
}

static int weight_of(const struct tok *t) {          /* 0 normal, 1 bold, -1 none */
    int w = one_of(t->text, (const char *const[]){ "normal", "lighter", "bold", "bolder", NULL });
    if(t->type == T_IDENT) return w < 0 ? -1 : w >= 2;
    if(t->type != T_NUMBER || t->num < ONE || t->num > 1000 * ONE) return -1;
    return t->num >= 600 * ONE;
}

static void r_weight(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    int w;
    if(n == 1 && (w = weight_of(&v[0])) >= 0) emit(P, prop, CU_NONE, w, imp);
}

static int fstyle_of(const struct tok *t) {          /* 0 normal, 1 italic, -1 none */
    int s = t->type == T_IDENT ? one_of(t->text, (const char *const[]){ "normal", "italic", "oblique", NULL }) : -1;
    return s < 0 ? -1 : s > 0;
}

static void r_fstyle(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    int s = n ? fstyle_of(&v[0]) : -1;
    if(s >= 0 && (n == 1 || (n == 2 && s && v[1].type == T_DIM))) emit(P, prop, CU_NONE, s, imp);
}

/* The lines of text-decoration, and of the rest of the shorthand only that it is one */
static void r_decoration(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    static const char *const others[] = { "overline", "blink", "solid", "double", "dotted", "dashed", "wavy", "auto",
                                          "from-font", NULL };
    int i = 0, lines = 0, none = 0, any = 0;
    while(i < n) {
        int j = i, u, c;
        if(word(&v[i], "none")) none = 1;
        else if(word(&v[i], "underline")) lines |= CSS_LINE_UNDER;
        else if(word(&v[i], "line-through")) lines |= CSS_LINE_THROUGH;
        else if(v[i].type == T_IDENT && one_of(v[i].text, others) >= 0) ;
        else if(color(v, n, &j, &u, &c)) {
            i = j;
            continue;
        } else if(!length(&v[i], &u, &c)) return;
        any = 1;
        i++;
    }
    if(any && (!none || !lines)) emit(P, prop, CU_NONE, lines, imp);
}

static void r_align(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    static const char *const left[] = { "left", "start", "justify", NULL };
    static const char *const right[] = { "right", "end", NULL };
    if(n != 1 || v[0].type != T_IDENT) return;
    if(one_of(v[0].text, left) >= 0) emit(P, prop, CU_NONE, CSS_A_LEFT, imp);
    else if(!strcmp(v[0].text, "center")) emit(P, prop, CU_NONE, CSS_A_CENTER, imp);
    else if(one_of(v[0].text, right) >= 0) emit(P, prop, CU_NONE, CSS_A_RIGHT, imp);
}

static void r_size(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    int unit, value;
    if(n == 1 && font_size(&v[0], &unit, &value)) emit(P, prop, unit, value, imp);
}

/* font: [style or weight or the like]* size [/ line height] family. A shorthand sets what it does not name back. */
static void r_font(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    static const char *const others[] = { "normal", "small-caps", "condensed", "semi-condensed", "extra-condensed",
                                          "ultra-condensed", "expanded", "semi-expanded", "extra-expanded",
                                          "ultra-expanded", NULL };
    int i, unit = 0, value = 0, style = 0, weight = 0, w;
    (void)prop;
    for(i = 0; i < n; i++) {
        if(font_size(&v[i], &unit, &value)) break;
        if(fstyle_of(&v[i]) > 0) style = 1;
        else if(v[i].type == T_IDENT && one_of(v[i].text, others) >= 0) ;
        else if((w = weight_of(&v[i])) >= 0) weight = w;
        else return;
    }
    if(i >= n) return;
    i++;
    if(i + 1 < n && v[i].type == T_DELIM && v[i].delim == '/') i += 2;
    if(i >= n || (v[i].type != T_IDENT && v[i].type != T_STRING)) return;
    emit(P, CP_STYLE, CU_NONE, style, imp);
    emit(P, CP_WEIGHT, CU_NONE, weight, imp);
    emit(P, CP_SIZE, unit, value, imp);
}

/* one side's margin or padding: a length, and for a margin auto; a padding is never negative */
static int side(int prop, const struct tok *t, int *unit, int *value) {
    int margin = prop >= CP_MARGIN_TOP && prop <= CP_MARGIN_LEFT;
    if(margin && word(t, "auto")) {
        *unit = CU_AUTO;
        *value = 0;
        return 1;
    }
    return length(t, unit, value) && (margin || *value >= 0);
}

static void r_side(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    int unit, value;
    if(n == 1 && side(prop, &v[0], &unit, &value)) emit(P, prop, unit, value, imp);
}

/* margin and padding: top, right, bottom, left, as one to four values give them; prop is the top's */
static void r_sides(struct parser *P, int prop, const struct tok *v, int n, int imp) {
    int unit[4], value[4], i, r, b, l;
    if(n < 1 || n > 4) return;
    for(i = 0; i < n; i++) if(!side(prop, &v[i], &unit[i], &value[i])) return;
    r = n > 1 ? 1 : 0;
    b = n > 2 ? 2 : 0;
    l = n > 3 ? 3 : r;
    emit(P, prop, unit[0], value[0], imp);
    emit(P, prop + 1, unit[r], value[r], imp);
    emit(P, prop + 2, unit[b], value[b], imp);
    emit(P, prop + 3, unit[l], value[l], imp);
}

struct property {
    const char *name;
    int prop;
    void (*read)(struct parser *, int, const struct tok *, int, int);
};

static const struct property properties[] = {
    { "background", CP_BACKGROUND, r_background }, { "background-color", CP_BACKGROUND, r_color },
    { "color", CP_COLOR, r_color }, { "display", CP_DISPLAY, r_display }, { "font", CP_SIZE, r_font },
    { "font-size", CP_SIZE, r_size }, { "font-style", CP_STYLE, r_fstyle }, { "font-weight", CP_WEIGHT, r_weight },
    { "margin", CP_MARGIN_TOP, r_sides }, { "margin-bottom", CP_MARGIN_BOTTOM, r_side },
    { "margin-left", CP_MARGIN_LEFT, r_side }, { "margin-right", CP_MARGIN_RIGHT, r_side },
    { "margin-top", CP_MARGIN_TOP, r_side }, { "padding", CP_PADDING_TOP, r_sides },
    { "padding-bottom", CP_PADDING_BOTTOM, r_side }, { "padding-left", CP_PADDING_LEFT, r_side },
    { "padding-right", CP_PADDING_RIGHT, r_side }, { "padding-top", CP_PADDING_TOP, r_side },
    { "text-align", CP_ALIGN, r_align }, { "text-decoration", CP_DECORATION, r_decoration },
    { "text-decoration-line", CP_DECORATION, r_decoration }
};

/* inherit, initial, unset and revert, alone in a value, are every property's. inherit where this file has no parent's
   value to give (a box's margin) is read as revert: the page's rules say nothing. */
static void keyword(struct parser *P, int prop, const char *w, int imp) {
    int inherited = prop == CP_COLOR || prop == CP_WEIGHT || prop == CP_STYLE || prop == CP_SIZE || prop == CP_ALIGN;
    if(!strcmp(w, "unset")) w = inherited ? "inherit" : "initial";
    if(!strcmp(w, "inherit") && prop == CP_DECORATION) emit(P, prop, CU_INHERIT, 0, imp);
    else if(!strcmp(w, "revert") || (!strcmp(w, "inherit") && !inherited)) emit(P, prop, CU_REVERT, 0, imp);
    else if(!strcmp(w, "inherit")) emit(P, prop, CU_INHERIT, 0, imp);
    else switch(prop) {                             /* initial */
    case CP_COLOR: emit(P, prop, CU_INITIAL, 0, imp); break;
    case CP_BACKGROUND: emit(P, prop, CU_TRANSPARENT, 0, imp); break;
    case CP_DISPLAY: emit(P, prop, CU_NONE, CSS_D_INLINE, imp); break;
    case CP_ALIGN: emit(P, prop, CU_NONE, CSS_A_LEFT, imp); break;
    case CP_SIZE: emit(P, prop, CU_BASE, ONE, imp); break;
    case CP_WEIGHT: case CP_STYLE: case CP_DECORATION: emit(P, prop, CU_NONE, 0, imp); break;
    default: emit(P, prop, CU_PX, 0, imp); break;   /* margins and paddings */
    }
}

static void wide(struct parser *P, const struct property *p, const char *w, int imp) {
    int i, n = p->read == r_sides ? 4 : 1;
    if(p->read == r_font) {
        keyword(P, CP_STYLE, w, imp);
        keyword(P, CP_WEIGHT, w, imp);
        keyword(P, CP_SIZE, w, imp);
    } else for(i = 0; i < n; i++) keyword(P, p->prop + i, w, imp);
}

static const struct property *property_named(const char *s) {
    size_t i;
    for(i = 0; i < sizeof(properties) / sizeof(properties[0]); i++)
        if(!strcmp(s, properties[i].name)) return &properties[i];
    return NULL;
}

/**** declarations ****/

/* A declaration's value, or what is left of a bad one: tokens to its ';' or the block's end, brackets followed.
   have: the token now is the first. 1 when the block ended with it. */
static int value(struct parser *P, int braces, int have) {
    unsigned char open[CSS_DEPTH_MAX];
    int depth = 0;
    struct tok *t = &P->t;
    P->nvals = 0;
    P->bad = 0;
    if(have) P->again = 1;
    for(;;) {
        fetch(P);
        switch(t->type) {
        case T_EOF:
            return 1;
        case T_WS:
            continue;
        case T_SEMI:
            if(!depth) return 0;
            break;
        case T_BAD: case T_CDO: case T_CDC:
            P->bad = 1;
            break;
        case T_LBRACE: case T_LBRACK: case T_LPAREN: case T_FUNC:
            if(t->type == T_LBRACE) P->bad = 1;
            if(depth == CSS_DEPTH_MAX) {
                if(P->c) P->c->truncated = 1;
                P->stop = 1;
                return 1;
            }
            open[depth++] = t->type == T_LBRACE ? T_RBRACE : t->type == T_LBRACK ? T_RBRACK : T_RPAREN;
            break;
        case T_RBRACE:
            if(!depth && braces) return 1;
            if(braces && depth == 1 && open[0] == T_RBRACE && !P->custom) { /* a nested rule ends with its block */
                P->bad = 1;
                return 0;
            }
            /* fall through */
        case T_RBRACK: case T_RPAREN:
            if(depth && open[depth - 1] == t->type) depth--;
            else P->bad = 1;
            break;
        }
        if(P->nvals == VALUES_MAX) {                /* its start, and its end: that is where !important is */
            P->vals[VALUES_MAX - 2] = P->vals[VALUES_MAX - 1];
            P->nvals--;
        }
        P->vals[P->nvals++] = *t;
    }
}

/* The token now is a name, of a declaration or of something else. 1 when the block ended with it. */
static int declaration(struct parser *P, int braces) {
    const struct property *p = property_named(P->t.text);
    struct tok *t = &P->t;
    int custom = P->t.text[0] == '-' && P->t.text[1] == '-', n, important = 0, end;
    static const char *const words[] = { "inherit", "initial", "unset", "revert", NULL };
    fetch(P);
    while(t->type == T_WS) fetch(P);
    if(t->type != T_COLON) return value(P, braces, 1);
    P->custom = custom;
    end = value(P, braces, 0);
    P->custom = 0;
    n = P->nvals;
    if(n >= 2 && word(&P->vals[n - 1], "important") && P->vals[n - 2].type == T_DELIM && P->vals[n - 2].delim == '!') {
        important = 1;
        n -= 2;
    }
    if(p && n && !P->bad && !P->stop) {
        if(n == 1 && P->vals[0].type == T_IDENT && one_of(P->vals[0].text, words) >= 0)
            wide(P, p, P->vals[0].text, important);
        else p->read(P, p->prop, P->vals, n, important);
    }
    return end;
}

/* A block's declarations (braces: up to its '}'), or a style attribute's (to the text's end) */
static void declarations(struct parser *P, int braces) {
    struct tok *t = &P->t;
    while(!P->stop) {
        fetch(P);
        if(t->type == T_EOF) return;
        if(t->type == T_WS || t->type == T_SEMI) continue;
        if(t->type == T_RBRACE && braces) return;
        if(t->type == T_IDENT ? declaration(P, braces) : value(P, braces, 1)) return;
    }
}

/**** selectors ****/

struct sel {
    struct css_part part[CSS_SEL_PARTS];    /* left to right, as they come */
    int n, over;
    int ids, classes, types;
    int compound;                       /* simple selectors of the compound now */
    int comb;                           /* a combinator waits for its compound, -1: none */
    int ws;                             /* whitespace came after the compound */
    int never;                          /* well-formed, but of what is not here: it matches nothing */
    int element;                        /* a pseudo-element came: only pseudo-classes may follow it */
    int bad;                            /* malformed: the rule goes */
    unsigned int count;                 /* selectors of the list so far */
};

static void sel_reset(struct sel *S) {
    S->n = S->over = S->ids = S->classes = S->types = S->compound = S->ws = S->never = S->element = 0;
    S->comb = -1;
}

static void add_part(struct sel *S, int kind, unsigned int hash) {
    if(S->n == CSS_SEL_PARTS) {
        S->over = 1;
        return;
    }
    S->part[S->n].kind = (unsigned int)kind;
    S->part[S->n++].hash = hash;
}

/* A simple selector comes: after a combinator or whitespace it starts a new compound. A type or '*' must be first. */
static void simple(struct sel *S, int first) {
    if(S->element) S->bad = 1;
    if(S->comb >= 0 || S->ws) {
        add_part(S, S->comb >= 0 ? S->comb : CSS_DESCENDANT, 0);
        S->compound = 0;
        S->comb = -1;
        S->ws = 0;
    }
    if(first && S->compound) S->bad = 1;
    S->compound++;
}

static void combinator(struct sel *S, int kind) {
    if(!S->compound || S->comb >= 0 || S->element) S->bad = 1;
    S->comb = kind;
    S->ws = 0;
}

static int known(const char *s, const char *const *sorted, size_t n) {
    size_t lo = 0, hi = n;
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(sorted[mid], s);
        if(!c) return 1;
        if(c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* The standard's pseudo-classes and pseudo-elements: what is not here, not -webkit-'s, makes a selector invalid, and
   so does one written as a function that is none, or the other way round */
static int known_pseudo(const char *s, int element, int func) {
    static const char *const classes[] = {
        "active", "any-link", "autofill", "blank", "checked", "current", "default", "defined", "dir", "disabled",
        "empty", "enabled", "first-child", "first-of-type", "focus", "focus-visible", "focus-within", "fullscreen",
        "future", "has", "host", "hover", "in-range", "indeterminate", "invalid", "is", "lang", "last-child",
        "last-of-type", "link", "local-link", "modal", "muted", "not", "nth-child", "nth-last-child",
        "nth-last-of-type", "nth-of-type", "only-child", "only-of-type", "open", "optional", "out-of-range", "past",
        "paused", "picture-in-picture", "placeholder-shown", "playing", "popover-open", "read-only", "read-write",
        "required", "root", "scope", "stalled", "state", "target", "target-within", "user-invalid", "user-valid",
        "valid", "visited", "volume-locked", "where"
    };
    static const char *const elements[] = {
        "after", "backdrop", "before", "cue", "details-content", "file-selector-button", "first-letter", "first-line",
        "grammar-error", "highlight", "marker", "part", "placeholder", "selection", "slotted", "spelling-error",
        "target-text", "view-transition"
    };
    static const char *const functions[] = {            /* those that are only functions */
        "dir", "has", "highlight", "is", "lang", "not", "nth-child", "nth-last-child", "nth-last-of-type",
        "nth-of-type", "part", "slotted", "state", "where"
    };
    int fn_only = known(s, functions, sizeof(functions) / sizeof(functions[0]));
    if(element && !strncmp(s, "-webkit-", 8)) return 1;
    if(!fn_only && func && strcmp(s, "cue") && strcmp(s, "host")) return 0;
    if(fn_only && !func) return 0;
    if(known(s, elements, sizeof(elements) / sizeof(elements[0])))
        return element || !strcmp(s, "after") || !strcmp(s, "before") || !strcmp(s, "first-letter") || !strcmp(s, "first-line");
    return !element && known(s, classes, sizeof(classes) / sizeof(classes[0]));
}

/* The ':' was read */
static void pseudo(struct parser *P, struct sel *S) {
    struct tok *t = &P->t;
    int element = 0;
    fetch(P);
    if(t->type == T_COLON) {
        element = 1;
        fetch(P);
    }
    if(t->type != T_IDENT && t->type != T_FUNC) {
        S->bad = 1;
        P->again = 1;
        return;
    }
    if(!S->element) simple(S, 0);                   /* (pseudo-classes may follow a pseudo-element) */
    if(t->type == T_IDENT && !element && !S->element && !strcmp(t->text, "link")) {
        add_part(S, CSS_LINK, 0);
        S->classes++;
        return;
    }
    S->never = 1;                                   /* :visited with the rest: no link here is one */
    if(!known_pseudo(t->text, element, t->type == T_FUNC) || (element && S->element)) S->bad = 1;
    if(element || !strcmp(t->text, "after") || !strcmp(t->text, "before") || !strcmp(t->text, "first-letter") ||
       !strcmp(t->text, "first-line")) S->element = 1;
    if(t->type == T_FUNC) skip_block(P, T_RPAREN);
}

static void fetch_past_ws(struct parser *P) {
    do fetch(P); while(P->t.type == T_WS);
}

/* The '[' was read: an attribute's selector matches nothing here, but it must be one: [a], [a=b], [a~="b" i] */
static void attribute(struct parser *P, struct sel *S) {
    struct tok *t = &P->t;
    simple(S, 0);
    S->never = 1;
    fetch_past_ws(P);
    if(t->type == T_IDENT) {
        fetch_past_ws(P);
        if(t->type == T_RBRACK) return;
        if(t->type == T_DELIM && strchr("~|^$*", t->delim) && t->delim) fetch(P);
        if(t->type == T_DELIM && t->delim == '=') {
            fetch_past_ws(P);
            if(t->type == T_IDENT || t->type == T_STRING) {
                fetch_past_ws(P);
                if(word(t, "i") || word(t, "s")) fetch_past_ws(P);
                if(t->type == T_RBRACK) return;
            }
        }
    }
    S->bad = 1;
    P->again = 1;
    skip_block(P, T_RBRACK);
}

/* A selector of the list ends: kept in c, its subject's compound first */
static void sel_end(struct parser *P, struct sel *S) {
    struct css *c = P->c;
    struct css_selector *o;
    int end = S->n, i, k;
    unsigned int key = 0;
    if(S->bad) return;
    if(!S->compound || S->comb >= 0 || ++S->count > CSS_RULE_SELECTORS) {
        S->bad = 1;
        return;
    }
    if(S->never || S->over) return;
    if(c->nsels >= CSS_SELECTORS_MAX || c->nparts + (size_t)S->n > CSS_PARTS_MAX) {
        limit(P);
        return;
    }
    while(c->nparts + (size_t)S->n > c->cparts)
        if(grow(c, (void **)&c->parts, c->cparts, &c->cparts, sizeof(*c->parts))) {
            P->stop = 1;
            return;
        }
    if(grow(c, (void **)&c->sels, c->nsels, &c->csels, sizeof(*c->sels))) {
        P->stop = 1;
        return;
    }
    o = &c->sels[c->nsels];
    o->first = (unsigned int)c->nparts;
    o->nparts = (unsigned int)S->n;
    o->spec = (unsigned int)S->ids << 20 | (unsigned int)S->classes << 10 | (unsigned int)S->types;
    for(;;) {                                       /* the compounds from the right */
        for(i = end; i > 0 && S->part[i - 1].kind < CSS_DESCENDANT; i--) ;
        for(k = i; k < end; k++) c->parts[c->nparts++] = S->part[k];
        if(!i) break;
        c->parts[c->nparts++] = S->part[i - 1];
        end = i - 1;
    }
    for(k = (int)o->first; k < (int)c->nparts && c->parts[k].kind < CSS_DESCENDANT; k++) {
        unsigned int kind = c->parts[k].kind, h = c->parts[k].hash;
        if(kind == CSS_ID) {                        /* the rarest of what it asks for */
            key = h;
            break;
        }
        if(kind == CSS_CLASS || (kind == CSS_TYPE && !key)) key = h;
    }
    o->key = key;
    o->decl = o->ndecl = 0;
    o->next = -1;
    c->nsels++;
}

/* One token of a selector list */
static void sel_token(struct parser *P, struct sel *S) {
    struct tok *t = &P->t;
    switch(t->type) {
    case T_WS:
        if(S->compound) S->ws = 1;
        return;
    case T_IDENT:
        simple(S, 1);
        add_part(S, CSS_TYPE, fix(t->lhash));
        S->types++;
        return;
    case T_HASH:
        if(!t->ident) {
            S->bad = 1;
            return;
        }
        simple(S, 0);
        add_part(S, CSS_ID, fix(t->hash));
        S->ids++;
        return;
    case T_COMMA:
        sel_end(P, S);
        sel_reset(S);
        return;
    case T_COLON:
        pseudo(P, S);
        return;
    case T_LBRACK:
        attribute(P, S);
        return;
    case T_LPAREN: case T_FUNC:
        S->bad = 1;
        skip_block(P, T_RPAREN);
        return;
    case T_DELIM:
        switch(t->delim) {
        case '*':
            simple(S, 1);
            return;
        case '.':
            fetch(P);
            if(t->type != T_IDENT) {
                S->bad = 1;
                P->again = 1;
                return;
            }
            simple(S, 0);
            add_part(S, CSS_CLASS, fix(t->hash));
            S->classes++;
            return;
        case '>':
            combinator(S, CSS_CHILD);
            return;
        case '+': case '~':
            combinator(S, CSS_DESCENDANT);
            S->never = 1;
            return;
        }
        break;
    }
    S->bad = 1;
}

static void index_selector(struct css *c, int i) {
    struct css_selector *s = &c->sels[i];
    int *head = s->key ? &c->head[s->key % CSS_BUCKETS] : &c->any;
    s->next = *head;
    *head = i;
}

/* A rule; the token now is the first of its prelude. 1 when the block around it ended instead (nested: an @media) */
static int qualified(struct parser *P, int nested) {
    struct css *c = P->c;
    struct tok *t = &P->t;
    struct sel sel, *S = &sel;
    size_t nsels = c->nsels, nparts = c->nparts, ndecls = c->ndecls, i;
    int ended = 0;
    if(c->rules >= CSS_RULES_MAX) {
        limit(P);
        return 1;
    }
    S->bad = 0;
    S->count = 0;
    sel_reset(S);
    for(;;) {
        if(t->type == T_EOF || P->stop) goto rewind;
        if(t->type == T_LBRACE) break;
        if(t->type == T_RBRACE && nested) {
            ended = 1;
            goto rewind;
        }
        if(!S->bad) sel_token(P, S);
        else if(t->type == T_LPAREN || t->type == T_FUNC) skip_block(P, T_RPAREN);    /* its block is still to come */
        else if(t->type == T_LBRACK) skip_block(P, T_RBRACK);
        if(P->stop) goto rewind;
        fetch(P);
    }
    sel_end(P, S);
    if(S->bad || P->stop || c->nsels == nsels) {
        skip_block(P, T_RBRACE);
        goto rewind;
    }
    declarations(P, 1);
    if(c->ndecls == ndecls) goto rewind;
    for(i = nsels; i < c->nsels; i++) {
        c->sels[i].decl = (unsigned int)ndecls;
        c->sels[i].ndecl = (unsigned int)(c->ndecls - ndecls);
        index_selector(c, (int)i);
    }
    c->rules++;
    return 0;
rewind:
    c->nsels = nsels;
    c->nparts = nparts;
    c->ndecls = ndecls;
    return ended;
}

/**** media queries ****/

struct mq {
    int any;                            /* a query of the list held */
    int queries;                        /* it has any */
    int state;                          /* the query now: 0 nothing yet, 1 not or only, 2 a type or a feature, 3 and */
    int neg, type, feature, bad;
};

static void mq_end(struct mq *q) {
    if(q->state || q->bad) {
        if(!q->bad && q->state == 2 && !q->feature && (q->neg ? !q->type : q->type)) q->any = 1;
        q->queries = 1;
    }
    q->state = q->neg = q->type = q->feature = q->bad = 0;
}

static void mq_token(struct parser *P, struct mq *q) {
    struct tok *t = &P->t;
    switch(t->type) {
    case T_WS:
        return;
    case T_COMMA:
        if(!q->state) q->bad = 1;
        mq_end(q);
        return;
    case T_LPAREN:
        if(q->state == 2) q->bad = 1;
        q->feature = 1;
        q->state = 2;
        skip_block(P, T_RPAREN);
        return;
    case T_FUNC:
        q->bad = 1;
        skip_block(P, T_RPAREN);
        return;
    case T_LBRACK:
        q->bad = 1;
        skip_block(P, T_RBRACK);
        return;
    case T_LBRACE:
        q->bad = 1;
        skip_block(P, T_RBRACE);
        return;
    case T_IDENT:
        if(!strcmp(t->text, "not") || !strcmp(t->text, "only")) {
            if(q->state) q->bad = 1;
            q->neg = t->text[0] == 'n';
            q->state = 1;
        } else if(!strcmp(t->text, "and")) {
            if(q->state != 2) q->bad = 1;
            q->state = 3;
        } else {
            if(q->state > 1 || !strcmp(t->text, "or") || !strcmp(t->text, "layer")) q->bad = 1;
            q->type = !strcmp(t->text, "screen") || !strcmp(t->text, "all");
            q->state = 2;
        }
        return;
    }
    q->bad = 1;
}

int css_media(const char *s, size_t n) {
    struct parser *P = &P_work;
    struct mq q;
    memset(&q, 0, sizeof(q));
    start(P, s, n, NULL, NULL, 0);
    for(;;) {
        fetch(P);
        if(P->t.type == T_EOF || P->stop) break;
        mq_token(P, &q);
    }
    mq_end(&q);
    return q.any || !q.queries;
}

/**** rules ****/

static void rules(struct parser *P, int nested);

/* The token now is an at-keyword: its rule is skipped, but for @media's block. 1 when the block around it ended. */
static int at_rule(struct parser *P, int nested) {
    struct tok *t = &P->t;
    int media = !strcmp(t->text, "media") && P->media < MEDIA_DEPTH;
    struct mq q;
    memset(&q, 0, sizeof(q));
    for(;;) {
        fetch(P);
        switch(t->type) {
        case T_EOF: case T_SEMI:
            return 0;
        case T_RBRACE:
            if(nested) return 1;
            break;
        case T_LBRACE:
            mq_end(&q);
            if(media && (q.any || !q.queries)) {
                P->media++;
                rules(P, 1);
                P->media--;
            } else skip_block(P, T_RBRACE);
            return 0;
        }
        if(media) mq_token(P, &q);
        else if(t->type == T_LPAREN || t->type == T_FUNC) skip_block(P, T_RPAREN);
        else if(t->type == T_LBRACK) skip_block(P, T_RBRACK);
        if(P->stop) return 1;
    }
}

static void rules(struct parser *P, int nested) {
    struct tok *t = &P->t;
    while(!P->stop) {
        fetch(P);
        switch(t->type) {
        case T_EOF:
            return;
        case T_WS:
            continue;
        case T_CDO: case T_CDC:
            if(!nested) continue;
            break;
        case T_RBRACE:
            if(nested) return;
            break;
        case T_AT:
            if(at_rule(P, nested)) return;
            continue;
        }
        if(qualified(P, nested)) return;
    }
}

void css_init(struct css *c) {
    size_t i;
    memset(c, 0, sizeof(*c));
    c->any = -1;
    for(i = 0; i < CSS_BUCKETS; i++) c->head[i] = -1;
}

void css_free(struct css *c) {
    if(!c) return;
    free(c->parts);
    free(c->sels);
    free(c->decls);
    css_init(c);
}

int css_add(struct css *c, const char *s, size_t n) {
    struct parser *P = &P_work;
    if(n > CSS_BYTES_MAX) {
        c->truncated = 1;
        return 0;
    }
    start(P, s, n, c, NULL, 0);
    rules(P, 0);
    return c->failed ? -1 : 0;
}

size_t css_style(const char *s, size_t n, struct css_decl *out, size_t cap) {
    struct parser *P = &P_work;
    start(P, s, n, NULL, out, cap);
    declarations(P, 0);
    return P->nout;
}

/* The rest of a statement at-rule, up to its ';' or the sheet's end: what follows its first thing is its media, in q;
   that first thing, when it is a string, goes to url (CSS_URL_MAX + 1 bytes, "" when it is anything else or too
   long). 0 when a block comes: it was no statement. */
static int statement(struct parser *P, struct mq *q, char *url) {
    struct tok *t = &P->t;
    int first = 1;
    for(;;) {
        fetch(P);
        if(t->type == T_EOF || t->type == T_SEMI || P->stop) return 1;
        if(t->type == T_LBRACE) return 0;
        if(t->type == T_WS) continue;
        if(!first) mq_token(P, q);
        else {
            first = 0;
            if(t->type == T_STRING && !P->x.str_over) memcpy(url, P->x.str, P->x.nstr + 1);
            else if(t->type == T_LPAREN || t->type == T_FUNC) skip_block(P, T_RPAREN);
            else if(t->type == T_LBRACK) skip_block(P, T_RBRACK);
        }
    }
}

size_t css_imports(const char *s, size_t n, char (*urls)[CSS_URL_MAX + 1], size_t cap) {
    struct parser *P = &P_work;
    struct tok *t = &P->t;
    size_t count = 0;
    if(n > CSS_BYTES_MAX) return 0;
    start(P, s, n, NULL, NULL, 0);
    while(count < cap) {                            /* they come before every other rule */
        struct mq q;
        int import;
        fetch(P);
        if(t->type == T_WS || t->type == T_CDO || t->type == T_CDC) continue;
        if(t->type != T_AT || P->stop) break;
        import = !strcmp(t->text, "import");
        if(!import && strcmp(t->text, "charset") && strcmp(t->text, "layer")) break;
        memset(&q, 0, sizeof(q));
        P->url[0] = 0;
        if(!statement(P, &q, P->url)) break;        /* (@charset "x"; and @layer a; may come first; @layer a {} not) */
        mq_end(&q);
        if(import && P->url[0] && (q.any || !q.queries)) memcpy(urls[count++], P->url, strlen(P->url) + 1);
    }
    return count;
}
