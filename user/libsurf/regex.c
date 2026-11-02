/*
SurfOS user library: regular expressions
--------------------
File: regex.c   Date: 11/01/26
--------------------
POSIX regcomp() and regexec(): the longest of the leftmost matches, each subexpression from
the left taking the longest it can, in time bounded by the pattern's size times the text's.

The pattern becomes a tree and the tree a program: an automaton as a list of states, in
which each subtree's states are one run [lo, hi), left at state hi. x{2,5} is the code for x
five times, so what the pattern costs at run time is what it costs in states: a count of them
(MAX_STATES) and of parentheses (MAX_NEST) bounds a pattern. Text is read a character at a
time, and every state is in step with the same character, so that no match can start or
end inside a UTF-8 sequence. A byte that is in no sequence is a character of its own, with a
code above Unicode's; a pattern's such byte matches just that byte.

Finding the match, scan(): every state the automaton can be in, with for each the
start of the match it would make, the earliest when several paths reach it. Each state is
in the list once per character. That is all most callers need.

Finding the subexpressions, walk(): given the span of the whole match, the tree is walked from the
top, each node's span dividing among its children by POSIX's rule: the first child the longest
that leaves a match for the rest, then the second, and in a repetition the first iteration,
then the second (only the last iteration's subexpressions are reported, so only its span
goes further down). The rule cannot be followed state by state, since which branch of (a|ab)
gives the longest is not known at the branch. So first make_table() runs the node's states
backward from the end of its span, which says for each position which states can
still reach the end; then fwd() runs one child forward among those states alone. Every thread
there is one that finishes, so the child's run reads no further than the child's
longest match, and the children of a node, or the iterations of a repetition, read the
span once between them. That makes each level of the tree cost states times span, and
levels are bounded by the nesting limit. The table takes a bit per state and position of
that node, for as long as the node is being divided. A node with no subexpression in it is
never looked at.

Back-references: no automaton can do them. bt_try() follows the program depth first, every way
through, with its own stack, for the longest at the leftmost place that has one,
and the first path to that length is the one reported: a star takes as much as it can first,
an alternation its first branch first. Time can be exponential, so it counts steps
and gives up with REG_ESTEPS. scan() runs first with each back-reference taken as any text: most
text that does not match is thrown out there without a single step, and the leftmost
start is known not to be before scan()'s.

Beyond POSIX, as GNU has them: \+ \? \| in basic syntax, \1 in extended, \w \W \s \S \< \> \b
\B \` \'. Classes are ASCII: no character above 127 is in [:alpha:] or \w, and REG_ICASE folds only
A-Z. Ranges go by code point, so there is no locale.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <regex.h>

typedef unsigned int u32;

#define MAX_STATES  65000               /* a program */
#define MAX_NODES   65000               /* a tree */
#define MAX_NEST    64                  /* parentheses inside each other: the recursion's limit */
#define MAX_WRAPS   16                  /* repetition operators on one atom: a***, which also nest */
#define MAX_WORK    (16 * MAX_STATES)   /* tree nodes emitted, with their copies */
#define BT_STEPS    20000000            /* steps of one regexec() with back-references, and BT_PER_BYTE per byte */
#define BT_PER_BYTE 16
#define BT_FRAMES   (1 << 22)           /* its stack */
#define INF         (-1)                /* no upper limit in a repetition */
#define BAD_BYTE    0x110000            /* the character of a byte in no sequence: this plus the byte */

/* the tree */
enum { N_EMPTY, N_CHAR, N_ANY, N_SET, N_ASSERT, N_BACKREF, N_GROUP, N_CAT, N_ALT, N_REPEAT };

struct rnode {
    int type;
    int arg;                            /* N_CHAR the code, N_SET the set, N_ASSERT the kind, N_GROUP the number */
    int min, max;                       /* N_REPEAT */
    int first, last, next;              /* children, siblings: indices */
    int lo, hi;                         /* its states, in the first copy of it */
    int groups;                         /* subexpressions in it, itself too */
    int so, eo;                         /* walk(): its span */
};

/* the program: states before R_SPLIT take a character and go on to the next state */
enum { R_CHAR, R_ANY, R_ANYNONL, R_SET, R_BACKREF,
       R_SPLIT, R_JMP, R_ASSERT, R_SAVE, R_CLEAR, R_MARK, R_CHECK, R_ACCEPT };

/* assertions */
enum { A_BOL, A_EOL, A_BUFBEG, A_BUFEND, A_WORDBEG, A_WORDEND, A_WORDB, A_NWORDB };

struct rstate {
    int op;
    int x, y;
};

struct rrange {
    int lo, hi;
};

/* a bracket: ASCII as bits, the rest as ranges, which neg turns over */
struct rset {
    u32 bits[4];
    int first, n;
    int neg;
};

struct thread {
    int pc;
    int start;                          /* scan(): where its match started */
};

struct tlist {
    int n;
    struct thread *t;
};

/* the compiled pattern, with room for regexec(), which so needs no memory for a plain match */
struct rx {
    int icase, newline, nosub;
    int backref;                        /* it has one: bt_try() */
    int nsub;
    int nstate, capstate;
    struct rstate *st;
    int nnode, capnode, root;
    struct rnode *node;
    int nset, capset;
    struct rset *set;
    int nrange, caprange;
    struct rrange *range;
    int nslot;                          /* bt_try(): two per group, then one per loop */
    int *slot, *best;
    int use_first;                      /* a match can start only with a byte in first[] */
    int anchored;                       /* only at the start of the string */
    unsigned char first[256];
    struct tlist l1, l2;
    unsigned int *mark;                 /* a state's last generation */
    unsigned int gen;
    int *stack;                         /* of the search through a state's empty moves */
    int *predi, *pred;                  /* walk(): which states move to a state without a character */
    regoff_t *caps;
};

/**** characters ****/

/* the character at s, of n bytes at most: its length, its code in *c */
static int char_at(const unsigned char *s, size_t n, int *c) {
    unsigned b = s[0];
    if(b < 0x80) {
        *c = (int)b;
        return 1;
    }
    if(b >= 0xC2 && b <= 0xDF) {
        if(n >= 2 && (s[1] & 0xC0) == 0x80) {
            *c = (int)((b & 0x1F) << 6 | (s[1] & 0x3Fu));
            return 2;
        }
    } else if(b >= 0xE0 && b <= 0xEF) {
        if(n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            int v = (int)((b & 0x0F) << 12 | (s[1] & 0x3Fu) << 6 | (s[2] & 0x3Fu));
            if(v >= 0x800 && (v < 0xD800 || v > 0xDFFF)) {  /* not overlong, not a surrogate */
                *c = v;
                return 3;
            }
        }
    } else if(b >= 0xF0 && b <= 0xF4) {
        if(n >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            int v = (int)((b & 0x07) << 18 | (s[1] & 0x3Fu) << 12 | (s[2] & 0x3Fu) << 6 | (s[3] & 0x3Fu));
            if(v >= 0x10000 && v <= 0x10FFFF) {
                *c = v;
                return 4;
            }
        }
    }
    *c = BAD_BYTE + (int)b;
    return 1;
}

/* where the character that ends at p (above lo) starts */
static int char_before(const unsigned char *s, int lo, int p) {
    int q, c;
    if(s[p - 1] < 0x80) return p - 1;
    for(q = p - 2; q >= lo && q >= p - 4; q--) {
        if((s[q] & 0xC0) == 0x80) continue;
        if(char_at(s + q, (size_t)(p - q), &c) == p - q) return q;
        break;
    }
    return p - 1;
}

static int is_word(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int set_has(const struct rx *r, const struct rset *s, int c) {
    int i;
    if(c < 128) return (int)(s->bits[c >> 5] >> (c & 31)) & 1;
    for(i = 0; i < s->n; i++) {
        const struct rrange *g = &r->range[s->first + i];
        if(c >= g->lo && c <= g->hi) return !s->neg;
    }
    return s->neg;
}

/* c as read, cf with A-Z folded when the pattern's were */
static int takes(const struct rx *r, const struct rstate *st, int c, int cf) {
    switch(st->op) {
    case R_CHAR: return st->x == cf;
    case R_ANY: return 1;
    case R_ANYNONL: return c != '\n';
    case R_SET: return set_has(r, &r->set[st->x], c);
    }
    return 1;                                   /* R_BACKREF in scan(): any text */
}

/**** the parser ****/

struct parse {
    const unsigned char *p, *end;
    struct rx *r;
    int ext;
    int err;
    int depth;                                  /* parentheses open */
    int ngroup;
    u32 closed;                                 /* bit n: group n has ended, so \n may name it */
};

static int node_new(struct parse *ps, int type, int arg) {
    struct rx *r = ps->r;
    struct rnode *nd;
    if(ps->err) return -1;
    if(r->nnode >= MAX_NODES) {
        ps->err = REG_ESIZE;
        return -1;
    }
    if(r->nnode == r->capnode) {
        int cap = r->capnode ? r->capnode * 2 : 32;
        nd = realloc(r->node, (size_t)cap * sizeof(*nd));
        if(!nd) {
            ps->err = REG_ESPACE;
            return -1;
        }
        r->node = nd;
        r->capnode = cap;
    }
    nd = &r->node[r->nnode];
    nd->type = type;
    nd->arg = arg;
    nd->min = nd->max = 0;
    nd->first = nd->last = nd->next = -1;
    nd->lo = nd->hi = -1;
    nd->groups = 0;
    nd->so = nd->eo = -1;
    return r->nnode++;
}

static void node_add(struct parse *ps, int parent, int child) {
    struct rnode *nd;
    if(ps->err) return;
    nd = &ps->r->node[parent];
    if(nd->last >= 0) ps->r->node[nd->last].next = child;
    else nd->first = child;
    nd->last = child;
}

static int set_new(struct parse *ps) {
    struct rx *r = ps->r;
    struct rset *s;
    if(r->nset == r->capset) {
        int cap = r->capset ? r->capset * 2 : 8;
        s = realloc(r->set, (size_t)cap * sizeof(*s));
        if(!s) {
            ps->err = REG_ESPACE;
            return -1;
        }
        r->set = s;
        r->capset = cap;
    }
    s = &r->set[r->nset];
    memset(s, 0, sizeof(*s));
    s->first = r->nrange;
    return r->nset++;
}

static void set_range(struct parse *ps, struct rset *s, int lo, int hi) {
    struct rx *r = ps->r;
    for(; lo <= hi && lo < 128; lo++) s->bits[lo >> 5] |= 1u << (lo & 31);
    if(lo > hi) return;
    if(r->nrange == r->caprange) {
        int cap = r->caprange ? r->caprange * 2 : 8;
        struct rrange *g = realloc(r->range, (size_t)cap * sizeof(*g));
        if(!g) {
            ps->err = REG_ESPACE;
            return;
        }
        r->range = g;
        r->caprange = cap;
    }
    r->range[r->nrange].lo = lo;
    r->range[r->nrange++].hi = hi;
    s->n++;
}

static int class_has(int kind, int c) {
    switch(kind) {
    case 0: return isalpha(c);
    case 1: return isdigit(c);
    case 2: return isalnum(c);
    case 3: return isupper(c);
    case 4: return islower(c);
    case 5: return isspace(c);
    case 6: return c == ' ' || c == '\t';
    case 7: return ispunct(c);
    case 8: return isprint(c);
    case 9: return isgraph(c);
    case 10: return iscntrl(c);
    case 11: return isxdigit(c);
    }
    return is_word(c);                          /* \w */
}

#define CLASS_WORD 12
#define CLASS_SPACE 5
static const char *const class_names[12] = { "alpha", "digit", "alnum", "upper", "lower", "space", "blank", "punct",
                                              "print", "graph", "cntrl", "xdigit" };

static void set_class(struct rset *s, int kind) {
    int c;
    for(c = 0; c < 128; c++)
        if(class_has(kind, c)) s->bits[c >> 5] |= 1u << (c & 31);
}

/* what ends a set, whichever way it was made */
static void set_finish(struct parse *ps, struct rset *s, int neg) {
    int c;
    if(ps->r->icase) {
        for(c = 'a'; c <= 'z'; c++) {
            int u = c - 32;
            if((s->bits[c >> 5] >> (c & 31) | s->bits[u >> 5] >> (u & 31)) & 1) {
                s->bits[c >> 5] |= 1u << (c & 31);
                s->bits[u >> 5] |= 1u << (u & 31);
            }
        }
    }
    if(neg) {
        for(c = 0; c < 4; c++) s->bits[c] = ~s->bits[c];
        s->neg = 1;
        if(ps->r->newline) s->bits['\n' >> 5] &= ~(1u << ('\n' & 31));
    }
}

/* [.x.] [=x=] [:name:] at ps->p, after the bracket and d: a class: -1 with it added; a character: it */
static int bracket_named(struct parse *ps, struct rset *s, int d) {
    const unsigned char *p = ps->p, *q;
    size_t n;
    int c, k;
    for(q = p; q + 1 < ps->end; q++) if(q[0] == d && q[1] == ']') break;
    if(q + 1 >= ps->end) {
        ps->err = REG_EBRACK;
        return 0;
    }
    n = (size_t)(q - p);
    ps->p = q + 2;
    if(d != ':') {                              /* a collating element is one character here: there is no locale */
        if(!n || char_at(p, n, &c) != (int)n) {
            ps->err = REG_ECOLLATE;
            return 0;
        }
        return c;
    }
    for(k = 0; k < 12; k++) {
        if(strlen(class_names[k]) == n && !memcmp(class_names[k], p, n)) {
            set_class(s, k);
            return -1;
        }
    }
    ps->err = REG_ECTYPE;
    return 0;
}

/* one end of a range, or all of a class (-1) */
static int bracket_elem(struct parse *ps, struct rset *s) {
    int c;
    if(ps->p + 1 < ps->end && ps->p[0] == '[' && (ps->p[1] == '.' || ps->p[1] == '=' || ps->p[1] == ':')) {
        int d = ps->p[1];
        ps->p += 2;
        return bracket_named(ps, s, d);
    }
    ps->p += char_at(ps->p, (size_t)(ps->end - ps->p), &c);
    return c;
}

/* [ at ps->p - 1 */
static int parse_bracket(struct parse *ps) {
    int si = set_new(ps), neg = 0, first = 1, n;
    struct rset *s;
    if(ps->err) return -1;
    s = &ps->r->set[si];
    if(ps->p < ps->end && *ps->p == '^') {
        neg = 1;
        ps->p++;
    }
    for(;;) {
        int lo, hi;
        if(ps->p >= ps->end) {
            ps->err = REG_EBRACK;
            return -1;
        }
        if(*ps->p == ']' && !first) {
            ps->p++;
            break;
        }
        first = 0;
        lo = bracket_elem(ps, s);
        if(ps->err) return -1;
        hi = lo;
        if(ps->p + 1 < ps->end && ps->p[0] == '-' && ps->p[1] != ']') {
            ps->p++;
            if(lo < 0) {                        /* [[:alpha:]-z] */
                ps->err = REG_ERANGE;
                return -1;
            }
            hi = bracket_elem(ps, s);
            if(ps->err) return -1;
            if(hi < lo) {                       /* a class too, which is -1 */
                ps->err = REG_ERANGE;
                return -1;
            }
        }
        if(lo >= 0) set_range(ps, s, lo, hi);
        if(ps->err) return -1;
    }
    set_finish(ps, s, neg);
    n = node_new(ps, N_SET, si);
    return n;
}

static int class_node(struct parse *ps, int kind, int neg) {
    int si = set_new(ps);
    if(ps->err) return -1;
    set_class(&ps->r->set[si], kind);
    set_finish(ps, &ps->r->set[si], neg);
    return node_new(ps, N_SET, si);
}

static int at_seq(const struct parse *ps, const char *seq) {
    size_t n = strlen(seq);
    return (size_t)(ps->end - ps->p) >= n && !memcmp(ps->p, seq, n);
}

static int at_bar(const struct parse *ps) {
    return at_seq(ps, ps->ext ? "|" : "\\|");
}

static int at_close(const struct parse *ps) {
    return at_seq(ps, ps->ext ? ")" : "\\)");
}

static int parse_alt(struct parse *ps);

static int parse_group(struct parse *ps) {
    int g, n, c;
    if(++ps->depth > MAX_NEST) {
        ps->err = REG_ESIZE;
        return -1;
    }
    g = ++ps->ngroup;
    n = node_new(ps, N_GROUP, g);
    c = parse_alt(ps);
    if(ps->err) return -1;
    if(!at_close(ps)) {
        ps->err = REG_EPAREN;
        return -1;
    }
    ps->p += ps->ext ? 1 : 2;
    ps->depth--;
    node_add(ps, n, c);
    if(g <= 9) ps->closed |= 1u << g;
    return n;
}

/* a character of the pattern, as itself */
static int parse_literal(struct parse *ps) {
    int c;
    ps->p += char_at(ps->p, (size_t)(ps->end - ps->p), &c);
    if(ps->r->icase && c >= 'A' && c <= 'Z') c += 32;
    return node_new(ps, N_CHAR, c);
}

/* \ at ps->p, with a character after it */
static int parse_escape(struct parse *ps, int *assertion) {
    int d = ps->p[1], kind = -1;
    switch(d) {
    case 'w': ps->p += 2; return class_node(ps, CLASS_WORD, 0);
    case 'W': ps->p += 2; return class_node(ps, CLASS_WORD, 1);
    case 's': ps->p += 2; return class_node(ps, CLASS_SPACE, 0);
    case 'S': ps->p += 2; return class_node(ps, CLASS_SPACE, 1);
    case '<': kind = A_WORDBEG; break;
    case '>': kind = A_WORDEND; break;
    case 'b': kind = A_WORDB; break;
    case 'B': kind = A_NWORDB; break;
    case '`': kind = A_BUFBEG; break;
    case '\'': kind = A_BUFEND; break;
    }
    if(kind >= 0) {
        ps->p += 2;
        *assertion = 1;
        return node_new(ps, N_ASSERT, kind);
    }
    if(d >= '1' && d <= '9') {
        if(!(ps->closed & (1u << (d - '0')))) {
            ps->err = REG_ESUBREG;
            return -1;
        }
        ps->p += 2;
        ps->r->backref = 1;
        return node_new(ps, N_BACKREF, d - '0');
    }
    ps->p++;
    return parse_literal(ps);
}

static int parse_atom(struct parse *ps, int atstart, int *assertion) {
    int c = *ps->p;
    *assertion = 0;
    if(c == '\\') {
        if(ps->p + 1 >= ps->end) {
            ps->err = REG_EESCAPE;
            return -1;
        }
        if(!ps->ext) {
            if(ps->p[1] == '(') {
                ps->p += 2;
                return parse_group(ps);
            }
            if(ps->p[1] == '{') {               /* an interval with nothing before it */
                ps->err = REG_BADRPT;
                return -1;
            }
        }
        return parse_escape(ps, assertion);
    }
    if(c == '.') {
        ps->p++;
        return node_new(ps, N_ANY, 0);
    }
    if(c == '[') {
        ps->p++;
        return parse_bracket(ps);
    }
    if(ps->ext) {
        switch(c) {
        case '(':
            ps->p++;
            return parse_group(ps);
        case '*': case '+': case '?':           /* with nothing to repeat; { is itself then, as in awk's /{/ */
            ps->err = REG_BADRPT;
            return -1;
        case '^':
            ps->p++;
            *assertion = 1;
            return node_new(ps, N_ASSERT, A_BOL);
        case '$':
            ps->p++;
            *assertion = 1;
            return node_new(ps, N_ASSERT, A_EOL);
        }
    } else if(c == '^' && atstart) {
        ps->p++;
        *assertion = 1;
        return node_new(ps, N_ASSERT, A_BOL);
    } else if(c == '$') {                       /* only at the end of the pattern or of a part of it */
        struct parse after = *ps;
        after.p++;
        if(after.p == after.end || at_close(&after) || at_bar(&after)) {
            ps->p++;
            *assertion = 1;
            return node_new(ps, N_ASSERT, A_EOL);
        }
    }
    return parse_literal(ps);
}

/* After { or \{: an interval's numbers, with ps->p past its end: 1. 0 when this is no interval, with ps->p where it was. */
static int parse_interval(struct parse *ps, int *min, int *max) {
    const unsigned char *p = ps->p;
    long lo = -1, hi = -1;
    int comma = 0;
    for(; p < ps->end && *p >= '0' && *p <= '9'; p++) {
        if(lo < 0) lo = 0;
        if(lo <= RE_DUP_MAX) lo = lo * 10 + (*p - '0');
    }
    if(p < ps->end && *p == ',') {
        comma = 1;
        for(p++; p < ps->end && *p >= '0' && *p <= '9'; p++) {
            if(hi < 0) hi = 0;
            if(hi <= RE_DUP_MAX) hi = hi * 10 + (*p - '0');
        }
    }
    if(ps->ext ? !(p < ps->end && *p == '}') : !(p + 1 < ps->end && p[0] == '\\' && p[1] == '}')) return 0;
    if(lo < 0 && !comma) return 0;              /* {} */
    if(lo < 0) lo = 0;
    if(!comma) hi = lo;
    ps->p = p + (ps->ext ? 1 : 2);
    if(lo > RE_DUP_MAX || hi > RE_DUP_MAX || (hi >= 0 && hi < lo)) {
        ps->err = REG_BADBR;
        return 0;
    }
    *min = (int)lo;
    *max = (int)hi;                             /* -1, INF, when it has no end */
    return 1;
}

/* a repetition operator at ps->p: 1, with its counts, and past it */
static int parse_repeat(struct parse *ps, int *min, int *max) {
    const unsigned char *p = ps->p;
    if(p >= ps->end) return 0;
    *min = 0;
    *max = INF;
    if(*p == '*') {
        ps->p++;
        return 1;
    }
    if(ps->ext) {
        if(*p == '+' || *p == '?') {
            *min = *p == '+';
            *max = *p == '+' ? INF : 1;
            ps->p++;
            return 1;
        }
        if(*p == '{') {
            ps->p++;
            if(parse_interval(ps, min, max)) return 1;
            if(!ps->err) ps->p = p;             /* not an interval: { is itself */
            return 0;
        }
        return 0;
    }
    if(*p != '\\' || p + 1 >= ps->end) return 0;
    if(p[1] == '+' || p[1] == '?') {
        *min = p[1] == '+';
        *max = p[1] == '+' ? INF : 1;
        ps->p += 2;
        return 1;
    }
    if(p[1] == '{') {
        ps->p += 2;
        if(parse_interval(ps, min, max)) return 1;
        if(!ps->err) {
            const unsigned char *q;
            for(q = ps->p; q + 1 < ps->end; q++) if(q[0] == '\\' && q[1] == '}') break;
            ps->err = q + 1 < ps->end ? REG_BADBR : REG_EBRACE;
        }
    }
    return 0;
}

static int parse_piece(struct parse *ps, int atstart, int *assertion) {
    int n = parse_atom(ps, atstart, assertion), wraps = 0, min, max;
    if(ps->err) return -1;
    for(;;) {
        const unsigned char *at = ps->p;
        if(!parse_repeat(ps, &min, &max)) break;
        if(*assertion) {                        /* basic syntax: a * here is itself */
            if(ps->ext) ps->err = REG_BADRPT;
            else ps->p = at;
            break;
        }
        if(++wraps > MAX_WRAPS) ps->err = REG_ESIZE;
        else if(min != 1 || max != 1) {
            int w = node_new(ps, N_REPEAT, 0);
            if(!ps->err) {
                ps->r->node[w].min = min;
                ps->r->node[w].max = max;
                node_add(ps, w, n);
                n = w;
            }
        }
        if(ps->err) return -1;
    }
    return ps->err ? -1 : n;
}

static int parse_cat(struct parse *ps) {
    int first = -1, cat = -1, n, atstart = 1, assertion;
    while(ps->p < ps->end && !at_bar(ps)) {
        if(at_close(ps)) {
            if(!ps->depth) ps->err = REG_EPAREN;
            break;
        }
        n = parse_piece(ps, atstart, &assertion);
        if(ps->err) return -1;
        atstart = assertion;                    /* basic syntax: ^* is ^ and a star */
        if(first < 0) first = n;
        else {
            if(cat < 0) {
                cat = node_new(ps, N_CAT, 0);
                node_add(ps, cat, first);
            }
            node_add(ps, cat, n);
        }
        if(ps->err) return -1;
    }
    if(ps->err) return -1;
    if(cat >= 0) return cat;
    return first >= 0 ? first : node_new(ps, N_EMPTY, 0);
}

static int parse_alt(struct parse *ps) {
    int n = parse_cat(ps), alt;
    if(ps->err || !at_bar(ps)) return n;
    alt = node_new(ps, N_ALT, 0);
    node_add(ps, alt, n);
    while(!ps->err && at_bar(ps)) {
        ps->p += ps->ext ? 1 : 2;
        n = parse_cat(ps);
        node_add(ps, alt, n);
    }
    return alt;
}

/**** from the tree to the program ****/

struct build {
    struct rx *r;
    int err;
    int work;
    int loops;
};

static int inst(struct build *b, int op, int x, int y) {
    struct rx *r = b->r;
    if(b->err) return 0;
    if(r->nstate >= MAX_STATES) {
        b->err = REG_ESIZE;
        return 0;
    }
    if(r->nstate == r->capstate) {
        int cap = r->capstate ? r->capstate * 2 : 64;
        struct rstate *st;
        st = realloc(r->st, (size_t)cap * sizeof(*st));
        if(!st) {
            b->err = REG_ESPACE;
            return 0;
        }
        r->st = st;
        r->capstate = cap;
    }
    r->st[r->nstate].op = op;
    r->st[r->nstate].x = x;
    r->st[r->nstate].y = y;
    return r->nstate++;
}

static void emit(struct build *b, int n);

/* A repetition, in the three shapes walk() knows (body B, the same code each time):
     x{0,}   L: split L+1, END; B; jmp L; END:
     x{m,}   B ... B (m - 1 times); L: B; split L, END; END:
     x{m,n}  B ... B (m times); split +1, END; B; split +1, END; B ...; END:
   With back-references, bt_try() needs each loop's check that an iteration moved on */
static void emit_repeat(struct build *b, int n) {
    struct rx *r = b->r;
    struct rnode *nd = &r->node[n];
    int c = nd->first, min = nd->min, max = nd->max, i, l = 0, k = 0, s, chain = -1;
    if(max == 0) return;
    if(max == INF) {
        if(r->backref) k = 2 * (r->nsub + 1) + b->loops++;
        if(min == 0) {
            l = inst(b, R_SPLIT, r->nstate + 1, 0);
            if(r->backref) inst(b, R_MARK, k, 0);
            emit(b, c);
            if(r->backref) chain = inst(b, R_CHECK, k, 0);
            inst(b, R_JMP, l, 0);
            if(b->err) return;
            r->st[l].y = r->nstate;
            if(r->backref) r->st[chain].y = r->nstate;
            return;
        }
        for(i = 0; i < min - 1; i++) {
            emit(b, c);
            if(b->err) return;
            if(r->node[c].hi == r->node[c].lo) {        /* the body has no code: once does as well as many times */
                nd->min = nd->max = 1;
                return;
            }
        }
        l = r->nstate;
        if(r->backref) inst(b, R_MARK, k, 0);
        emit(b, c);
        if(b->err) return;
        if(r->node[c].hi == r->node[c].lo) {
            nd->min = nd->max = 1;
            return;
        }
        if(r->backref) chain = inst(b, R_CHECK, k, 0);
        inst(b, R_SPLIT, l, r->nstate + 1);
        if(b->err) return;
        if(r->backref) r->st[chain].y = r->nstate;
        return;
    }
    for(i = 0; i < min; i++) {
        emit(b, c);
        if(b->err) return;
        if(r->node[c].hi == r->node[c].lo) {
            nd->min = nd->max = 1;
            return;
        }
    }
    for(i = 0; i < max - min; i++) {
        s = inst(b, R_SPLIT, r->nstate + 1, chain);
        chain = s;
        emit(b, c);
        if(b->err) return;
        if(r->node[c].hi == r->node[c].lo) {
            nd->max = min + 1;
            break;
        }
    }
    while(chain >= 0) {
        s = r->st[chain].y;
        r->st[chain].y = r->nstate;
        chain = s;
    }
}

static void emit(struct build *b, int n) {
    struct rx *r = b->r;
    struct rnode *nd = &r->node[n];
    int lo = r->nstate, c;
    if(b->err) return;
    if(++b->work > MAX_WORK) {                  /* (()()()a){999}: nodes that make no code still take time */
        b->err = REG_ESIZE;
        return;
    }
    switch(nd->type) {
    case N_EMPTY:
        break;
    case N_CHAR:
        inst(b, R_CHAR, nd->arg, 0);
        break;
    case N_ANY:
        inst(b, r->newline ? R_ANYNONL : R_ANY, 0, 0);
        break;
    case N_SET:
        inst(b, R_SET, nd->arg, 0);
        break;
    case N_ASSERT:
        inst(b, R_ASSERT, nd->arg, 0);
        break;
    case N_BACKREF:
        inst(b, R_BACKREF, nd->arg, 0);
        break;
    case N_GROUP:
        if(r->backref) {
            inst(b, R_SAVE, 2 * nd->arg, 0);
            if(nd->groups > 1) inst(b, R_CLEAR, nd->arg + 1, nd->groups - 1);
        }
        emit(b, nd->first);
        if(r->backref) inst(b, R_SAVE, 2 * nd->arg + 1, 0);
        break;
    case N_CAT:
        for(c = nd->first; c >= 0; c = r->node[c].next) emit(b, c);
        break;
    case N_ALT: {
        int chain = -1, s = -1, next;
        for(c = nd->first; c >= 0; c = r->node[c].next) {
            int last = r->node[c].next < 0;
            if(!last) s = inst(b, R_SPLIT, r->nstate + 1, 0);
            emit(b, c);
            if(!last) {
                int j = inst(b, R_JMP, chain, 0);
                if(b->err) return;
                chain = j;
                r->st[s].y = r->nstate;
            }
        }
        if(b->err) return;
        for(; chain >= 0; chain = next) {
            next = r->st[chain].x;
            r->st[chain].x = r->nstate;
        }
        break;
    }
    case N_REPEAT:
        emit_repeat(b, n);
        break;
    }
    if(nd->hi < 0) {                            /* the first copy is the one the tree remembers */
        nd->lo = lo;
        nd->hi = r->nstate;
    }
}

/* the counts walk() needs */
static int count_groups(struct rx *r, int n) {
    struct rnode *nd = &r->node[n];
    int c, g = nd->type == N_GROUP;
    for(c = nd->first; c >= 0; c = r->node[c].next) g += count_groups(r, c);
    nd->groups = g;
    return g;
}

/* the states the search from state 0 can stand on before reading anything, to know the bytes a match can start with, and
   whether all ways begin with ^ */
static void find_first(struct rx *r) {
    int sp = 0, b, pc;
    int *stack = r->stack;
    r->use_first = !r->backref;
    r->anchored = !r->newline;
    memset(r->first, 0, sizeof(r->first));
    memset(r->mark, 0, (size_t)(r->nstate + 1) * sizeof(*r->mark));
    r->gen = 1;
    stack[sp++] = 0;
    while(sp) {
        const struct rstate *st;
        pc = stack[--sp];
        if(r->mark[pc]) continue;
        r->mark[pc] = 1;
        st = &r->st[pc];
        switch(st->op) {
        case R_SPLIT:
            stack[sp++] = st->y;
            stack[sp++] = st->x;
            break;
        case R_JMP:
            stack[sp++] = st->x;
            break;
        case R_SAVE: case R_CLEAR: case R_MARK:
            stack[sp++] = pc + 1;
            break;
        case R_CHECK:
            stack[sp++] = pc + 1;
            stack[sp++] = st->y;
            break;
        case R_ASSERT:
            if(st->x != A_BOL && st->x != A_BUFBEG) r->anchored = 0;
            stack[sp++] = pc + 1;
            break;
        case R_CHAR:
            r->anchored = 0;
            if(st->x < 0x80) {
                r->first[st->x] = 1;
                if(r->icase && st->x >= 'a' && st->x <= 'z') r->first[st->x - 32] = 1;
            } else if(st->x >= BAD_BYTE) {
                r->first[st->x - BAD_BYTE] = 1;
                if(st->x < BAD_BYTE + 0xC0) r->use_first = 0;  /* such a byte can be inside a sequence */
            } else {
                r->first[st->x < 0x800 ? 0xC0 | st->x >> 6 : st->x < 0x10000 ? 0xE0 | st->x >> 12 : 0xF0 | st->x >> 18] = 1;
            }
            break;
        case R_SET: {
            const struct rset *s = &r->set[st->x];
            r->anchored = 0;
            if(s->neg || s->n) r->use_first = 0;
            for(b = 0; b < 128; b++) if(s->bits[b >> 5] >> (b & 31) & 1) r->first[b] = 1;
            break;
        }
        default:                                /* . or a match of nothing */
            r->anchored = 0;
            r->use_first = 0;
        }
    }
    /* ^ can be passed above: the thing to know is that no way avoids it, so go again from the top without passing it */
    if(r->anchored) {
        memset(r->mark, 0, (size_t)(r->nstate + 1) * sizeof(*r->mark));
        sp = 0;
        stack[sp++] = 0;
        while(sp) {
            const struct rstate *st;
            pc = stack[--sp];
            if(r->mark[pc]) continue;
            r->mark[pc] = 1;
            st = &r->st[pc];
            if(st->op == R_SPLIT) {
                stack[sp++] = st->y;
                stack[sp++] = st->x;
            } else if(st->op == R_JMP) {
                stack[sp++] = st->x;
            } else if(st->op != R_ASSERT) {
                r->anchored = 0;
            }
        }
    }
}

/* for make_table(): the moves without a character, backward */
static int find_preds(struct rx *r) {
    int n = r->nstate + 2, pc, total = 0;
    int *count = calloc((size_t)n, sizeof(int));
    if(!count) return REG_ESPACE;
    r->predi = count;
    for(pc = 0; pc < r->nstate; pc++) {
        const struct rstate *st = &r->st[pc];
        if(st->op == R_SPLIT) {
            count[st->x + 1]++;
            count[st->y + 1]++;
            total += 2;
        } else if(st->op == R_JMP) {
            count[st->x + 1]++;
            total++;
        } else if(st->op == R_ASSERT) {
            count[pc + 2]++;
            total++;
        }
    }
    for(pc = 1; pc < n; pc++) count[pc] += count[pc - 1];
    r->pred = malloc((size_t)(total + 1) * sizeof(int));
    if(!r->pred) return REG_ESPACE;
    for(pc = r->nstate - 1; pc >= 0; pc--) {    /* count[q] walks forward through q's part, then is put back below */
        const struct rstate *st = &r->st[pc];
        if(st->op == R_SPLIT) {
            r->pred[count[st->x]++] = pc;
            r->pred[count[st->y]++] = pc;
        } else if(st->op == R_JMP) {
            r->pred[count[st->x]++] = pc;
        } else if(st->op == R_ASSERT) {
            r->pred[count[pc + 1]++] = pc;
        }
    }
    for(pc = n - 1; pc > 0; pc--) count[pc] = count[pc - 1];
    count[0] = 0;
    return 0;
}

static void rx_free(struct rx *r) {
    free(r->st);
    free(r->node);
    free(r->set);
    free(r->range);
    free(r->slot);
    free(r->best);
    free(r->l1.t);
    free(r->l2.t);
    free(r->mark);
    free(r->stack);
    free(r->predi);
    free(r->pred);
    free(r->caps);
    free(r);
}

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    struct rx *r = calloc(1, sizeof(*r));
    struct parse ps;
    struct build b;
    size_t states;
    int err;
    if(!r) return REG_ESPACE;
    memset(&ps, 0, sizeof(ps));
    memset(&b, 0, sizeof(b));
    r->icase = (cflags & REG_ICASE) != 0;
    r->newline = (cflags & REG_NEWLINE) != 0;
    r->nosub = (cflags & REG_NOSUB) != 0;
    ps.p = (const unsigned char *)pattern;
    ps.end = ps.p + strlen(pattern);
    ps.r = r;
    ps.ext = (cflags & REG_EXTENDED) != 0;
    r->root = parse_alt(&ps);
    if(!ps.err && ps.p < ps.end) ps.err = REG_EPAREN;   /* only a ) can stop the parser early */
    err = ps.err;
    if(err) goto fail;
    r->nsub = ps.ngroup;
    count_groups(r, r->root);
    b.r = r;
    emit(&b, r->root);
    inst(&b, R_ACCEPT, 0, 0);
    err = b.err;
    if(err) goto fail;
    states = (size_t)r->nstate + 1;
    r->nslot = 2 * (r->nsub + 1) + b.loops;
    r->l1.t = malloc(states * sizeof(struct thread));
    r->l2.t = malloc(states * sizeof(struct thread));
    r->mark = calloc(states, sizeof(*r->mark));
    r->stack = malloc(2 * states * sizeof(int));
    r->caps = malloc((size_t)(r->nsub + 1) * 2 * sizeof(regoff_t));
    r->slot = malloc((size_t)r->nslot * sizeof(int));
    r->best = malloc((size_t)r->nslot * sizeof(int));
    if(!r->l1.t || !r->l2.t || !r->mark || !r->stack || !r->caps || !r->slot || !r->best) {
        err = REG_ESPACE;
        goto fail;
    }
    find_first(r);
    if(r->nsub && !r->nosub && !r->backref && (err = find_preds(r))) goto fail;
    preg->re_nsub = (size_t)r->nsub;
    preg->re_g = r;
    return 0;
fail:
    rx_free(r);
    preg->re_g = NULL;
    return err;
}

void regfree(regex_t *preg) {
    if(preg->re_g) rx_free(preg->re_g);
    preg->re_g = NULL;
}

size_t regerror(int err, const regex_t *preg, char *buf, size_t size) {
    static const char *const text[] = {
        "success", "no match", "invalid pattern", "invalid collating element", "unknown character class",
        "trailing backslash", "back-reference to a group that has not ended", "[ without its ]",
        "( without its ) or ) without its (", "\\{ without its \\}", "invalid number in an interval",
        "invalid range end", "out of memory", "nothing to repeat", "pattern too big or nested too deeply",
        "too many steps on this text"
    };
    const char *s = err >= 0 && err < (int)(sizeof(text) / sizeof(text[0])) ? text[err] : "unknown error";
    size_t n = strlen(s) + 1;
    (void)preg;
    if(size) {
        size_t k = n < size ? n : size;
        memcpy(buf, s, k - 1);
        buf[k - 1] = 0;
    }
    return n;
}

/**** matching: what every way of doing it needs ****/

struct exec {
    struct rx *r;
    const unsigned char *s;
    int end;                                    /* the text ends here */
    int eflags;
    int ms, me;                                 /* scan()'s match */
    int boolean;                                /* whether it matches is all that is wanted */
    int last;                                   /* fwd(): the furthest end */
    /* bt_try() */
    long long steps, limit;
    struct frame *fr;
    int nfr, capfr;
};

static int holds(const struct exec *x, int kind, int p) {
    const unsigned char *s = x->s;
    int before, after;
    switch(kind) {
    case A_BOL:
        if(!p) return !(x->eflags & REG_NOTBOL);
        return x->r->newline && s[p - 1] == '\n';
    case A_EOL:
        if(p == x->end) return !(x->eflags & REG_NOTEOL);
        return x->r->newline && s[p] == '\n';
    case A_BUFBEG:
        return !p;
    case A_BUFEND:
        return p == x->end;
    }
    before = p > 0 && is_word(s[p - 1]);
    after = p < x->end && is_word(s[p]);
    switch(kind) {
    case A_WORDBEG: return !before && after;
    case A_WORDEND: return before && !after;
    case A_WORDB: return before != after;
    }
    return before == after;
}

/* the next generation of marks: each state once per position */
static void next_gen(struct rx *r) {
    if(!++r->gen) {
        memset(r->mark, 0, (size_t)(r->nstate + 1) * sizeof(*r->mark));
        r->gen = 1;
    }
}

static int fold(const struct rx *r, int c) {
    return r->icase && c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/**** scan(): the leftmost longest match, or its start where there are back-references ****/

/* a new thread at state pc, with all the states it reaches without reading: first come in the list, first in time */
static void add_thread(struct exec *x, struct tlist *l, int pc, int start, int pos) {
    struct rx *r = x->r;
    int *stack = r->stack, sp = 0;
    stack[sp++] = pc;
    while(sp) {
        const struct rstate *st;
        pc = stack[--sp];
        if(r->mark[pc] == r->gen) continue;
        r->mark[pc] = r->gen;
        st = &r->st[pc];
        switch(st->op) {
        case R_SPLIT:
            stack[sp++] = st->y;
            stack[sp++] = st->x;
            break;
        case R_JMP:
            stack[sp++] = st->x;
            break;
        case R_ASSERT:
            if(holds(x, st->x, pos)) stack[sp++] = pc + 1;
            break;
        case R_SAVE: case R_CLEAR: case R_MARK:
            stack[sp++] = pc + 1;
            break;
        case R_CHECK:
            stack[sp++] = st->y;
            stack[sp++] = pc + 1;
            break;
        case R_ACCEPT:
            if(x->ms < 0 || start < x->ms || (start == x->ms && pos > x->me)) {
                x->ms = start;
                x->me = pos;
            }
            break;
        case R_BACKREF:                         /* any text: that of no length here, the rest below */
            stack[sp++] = pc + 1;
            /* fall through */
        default:
            l->t[l->n].pc = pc;
            l->t[l->n++].start = start;
        }
    }
}

static int scan(struct exec *x, int from) {
    struct rx *r = x->r;
    const unsigned char *s = x->s;
    struct tlist *cur = &r->l1, *nxt = &r->l2, *t;
    int p = from, i;
    x->ms = x->me = -1;
    cur->n = 0;
    next_gen(r);
    for(;;) {
        int len, c, cf;
        if(x->ms < 0) {
            if(!cur->n && r->use_first) {       /* nothing under way: on to the next byte that could start */
                while(p < x->end && !r->first[s[p]]) p++;
                if(p >= x->end) break;
                next_gen(r);
            }
            add_thread(x, cur, 0, p, p);        /* last in the list: the others started earlier */
            if(x->boolean && x->ms >= 0) return 1;
        }
        if(p >= x->end) break;
        if(!cur->n) {
            if(x->ms >= 0 || r->anchored) break;
            len = char_at(s + p, (size_t)(x->end - p), &c);
            p += len;
            next_gen(r);
            continue;
        }
        len = char_at(s + p, (size_t)(x->end - p), &c);
        cf = fold(r, c);
        next_gen(r);
        nxt->n = 0;
        for(i = 0; i < cur->n; i++) {
            const struct thread *th = &cur->t[i];
            const struct rstate *st = &r->st[th->pc];
            if(x->ms >= 0 && th->start > x->ms) break;
            if(!takes(r, st, c, cf)) continue;
            add_thread(x, nxt, st->op == R_BACKREF ? th->pc : th->pc + 1, th->start, p + len);
            if(x->boolean && x->ms >= 0) return 1;
        }
        t = cur;
        cur = nxt;
        nxt = t;
        p += len;
    }
    return x->ms >= 0;
}

/**** walk(): the subexpressions ****/

/* per position of a span, one bit per state of [lo, hi]: can the state reach hi at the span's end? */
struct rtab {
    u32 *bits;
    int lo, words, base;
};

static int tab_has(const struct rtab *t, int pos, int pc) {
    int k = pc - t->lo;
    return (int)(t->bits[(size_t)(pos - t->base) * (size_t)t->words + (size_t)(k >> 5)] >> (k & 31)) & 1;
}

static void tab_set(struct rtab *t, int pos, int pc) {
    int k = pc - t->lo;
    t->bits[(size_t)(pos - t->base) * (size_t)t->words + (size_t)(k >> 5)] |= 1u << (k & 31);
}

/* pc can reach the end from position pos, and so can what moves to it without reading */
static void add_back(struct exec *x, struct rtab *t, struct tlist *l, int pc, int pos, int lo, int hi) {
    struct rx *r = x->r;
    int *stack = r->stack, sp = 0, i;
    stack[sp++] = pc;
    while(sp) {
        pc = stack[--sp];
        if(r->mark[pc] == r->gen) continue;
        r->mark[pc] = r->gen;
        tab_set(t, pos, pc);
        l->t[l->n++].pc = pc;
        for(i = r->predi[pc]; i < r->predi[pc + 1]; i++) {
            int q = r->pred[i];
            if(q < lo || q >= hi) continue;
            if(r->st[q].op == R_ASSERT && !holds(x, r->st[q].x, pos)) continue;
            stack[sp++] = q;
        }
    }
}

static int make_table(struct exec *x, struct rtab *t, int lo, int hi, int i, int e) {
    struct rx *r = x->r;
    struct tlist *cur = &r->l1, *nxt = &r->l2, *sw;
    int words = (hi - lo + 32) >> 5, p = e, k;
    unsigned long long size = (unsigned long long)words * (unsigned long long)(e - i + 1);
    if(size > 0x10000000ULL) return REG_ESPACE; /* a gigabyte */
    t->lo = lo;
    t->words = words;
    t->base = i;
    t->bits = calloc((size_t)size, sizeof(u32));
    if(!t->bits) return REG_ESPACE;
    cur->n = 0;
    next_gen(r);
    add_back(x, t, cur, hi, e, lo, hi);
    while(p > i) {
        int q = char_before(x->s, i, p), c, cf;
        char_at(x->s + q, (size_t)(p - q), &c);
        cf = fold(r, c);
        next_gen(r);
        nxt->n = 0;
        for(k = 0; k < cur->n; k++) {
            int pc = cur->t[k].pc - 1;          /* a state that reads goes on to the next one */
            if(pc >= lo && r->st[pc].op < R_SPLIT && takes(r, &r->st[pc], c, cf)) add_back(x, t, nxt, pc, q, lo, hi);
        }
        sw = cur;
        cur = nxt;
        nxt = sw;
        p = q;
    }
    return 0;
}

/* the states pc reaches without reading, among [lo, hi]; hi is the way out, and passing it says so */
static void add_fwd(struct exec *x, struct tlist *l, int pc, int pos, int hi, const struct rtab *t) {
    struct rx *r = x->r;
    int *stack = r->stack, sp = 0;
    stack[sp++] = pc;
    while(sp) {
        const struct rstate *st;
        pc = stack[--sp];
        if(r->mark[pc] == r->gen) continue;
        r->mark[pc] = r->gen;
        if(t && !tab_has(t, pos, pc)) continue;
        if(pc == hi) {
            x->last = pos;
            continue;
        }
        st = &r->st[pc];
        switch(st->op) {
        case R_SPLIT:
            stack[sp++] = st->y;
            stack[sp++] = st->x;
            break;
        case R_JMP:
            stack[sp++] = st->x;
            break;
        case R_ASSERT:
            if(holds(x, st->x, pos)) stack[sp++] = pc + 1;
            break;
        default:
            l->t[l->n++].pc = pc;
        }
    }
}

/* The furthest position up to e at which the code [lo, hi) started at i ends, -1 when it does not. With a table, only
   among the states the table allows. */
static int fwd(struct exec *x, int lo, int hi, int i, int e, const struct rtab *t) {
    struct rx *r = x->r;
    struct tlist *cur = &r->l1, *nxt = &r->l2, *sw;
    int p = i, k;
    x->last = -1;
    cur->n = 0;
    next_gen(r);
    add_fwd(x, cur, lo, p, hi, t);
    while(cur->n && p < e) {
        int c, cf, len = char_at(x->s + p, (size_t)(x->end - p), &c);
        cf = fold(r, c);
        next_gen(r);
        nxt->n = 0;
        for(k = 0; k < cur->n; k++) {
            int pc = cur->t[k].pc;
            if(takes(r, &r->st[pc], c, cf)) add_fwd(x, nxt, pc + 1, p + len, hi, t);
        }
        sw = cur;
        cur = nxt;
        nxt = sw;
        p += len;
    }
    return x->last;
}

static int walk(struct exec *x, int n, int delta, int i, int e);

/* the iterations of a repetition, each the longest that leaves a match for the rest (the shapes are emit_repeat()'s).
   Only the last one's subexpressions are reported. A body that can match nothing makes one iteration of nothing
   where there is nothing to read, as POSIX has it (a null string is better than no match at all), and only there. */
static int walk_repeat(struct exec *x, struct rnode *nd, int delta, int i, int e) {
    struct rx *r = x->r;
    struct rnode *k = &r->node[nd->first];
    struct rtab t;
    int lo = nd->lo + delta, b = k->hi - k->lo, min = nd->min, max = nd->max, p = i, q, j, base, err;
    int ls = -1, le = -1, ldelta = 0;
    if(!max) return 0;
    if((err = make_table(x, &t, lo, nd->hi + delta, i, e))) return err;
    for(j = 0; max == INF || j < max; j++) {
        if(max == INF) base = !min ? lo + 1 : j < min ? lo + j * b : lo + (min - 1) * b;
        else base = j < min ? lo + j * b : lo + min * b + (j - min) * (b + 1) + 1;
        if(j >= min && p >= e && j) break;
        q = fwd(x, base, base + b, p, e, &t);
        if(q < 0 || (j >= min && j && q == p)) break;
        ls = p;
        le = q;
        ldelta = base - k->lo;
        p = q;
    }
    free(t.bits);
    return ls >= 0 ? walk(x, nd->first, ldelta, ls, le) : 0;
}

/* node n's copy at delta, whose span is [i, e) */
static int walk(struct exec *x, int n, int delta, int i, int e) {
    struct rx *r = x->r;
    struct rnode *nd = &r->node[n];
    int c, err;
    if(!nd->groups) return 0;
    switch(nd->type) {
    case N_GROUP:
        r->caps[2 * nd->arg] = i;
        r->caps[2 * nd->arg + 1] = e;
        for(c = 1; c < nd->groups; c++) r->caps[2 * (nd->arg + c)] = r->caps[2 * (nd->arg + c) + 1] = -1;
        return walk(x, nd->first, delta, i, e);
    case N_CAT: {
        struct rtab t;
        int p = i, left = nd->groups, last = -1;
        if((err = make_table(x, &t, nd->lo + delta, nd->hi + delta, i, e))) return err;
        for(c = nd->first; c >= 0 && left; c = r->node[c].next) {   /* up to the last child with a group in it */
            struct rnode *k = &r->node[c];
            k->so = p;
            k->eo = p = fwd(x, k->lo + delta, k->hi + delta, p, e, &t);
            left -= k->groups;
            last = c;
        }
        free(t.bits);                           /* the children's tables come after, not on top of, this one */
        for(c = nd->first; last >= 0 && c >= 0; c = r->node[c].next) {
            struct rnode *k = &r->node[c];
            if(k->groups && (err = walk(x, c, delta, k->so, k->eo))) return err;
            if(c == last) break;
        }
        return 0;
    }
    case N_ALT:
        for(c = nd->first; c >= 0; c = r->node[c].next) {
            struct rnode *k = &r->node[c];
            if(fwd(x, k->lo + delta, k->hi + delta, i, e, NULL) == e) return walk(x, c, delta, i, e);
        }
        return 0;
    case N_REPEAT:
        return walk_repeat(x, nd, delta, i, e);
    }
    return 0;
}

/**** back-references: every way through ****/

struct frame {
    int pc;                                     /* a state to try; below 0, ~pc: a slot to put back */
    int value;                                  /* a position; or the slot's value */
};

static int push(struct exec *x, int pc, int value) {
    if(x->nfr == x->capfr) {
        int cap = x->capfr ? x->capfr * 2 : 256;
        struct frame *f;
        if(cap > BT_FRAMES) return REG_ESTEPS;
        f = realloc(x->fr, (size_t)cap * sizeof(*f));
        if(!f) return REG_ESPACE;
        x->fr = f;
        x->capfr = cap;
    }
    x->fr[x->nfr].pc = pc;
    x->fr[x->nfr++].value = value;
    return 0;
}

static int same_text(const struct exec *x, int a, int b, int n) {
    int i;
    for(i = 0; i < n; i++) {
        int p = x->s[a + i], q = x->s[b + i];
        if(fold(x->r, p) != fold(x->r, q)) return 0;
    }
    return 1;
}

/* from start: 1 when there is a match (r->best has it, x->me its end), 0 when not, or an error (negative) */
static int bt_try(struct exec *x, int start) {
    struct rx *r = x->r;
    int *slot = r->slot, pc = 0, pos = start, found = 0, i, err;
    for(i = 0; i < r->nslot; i++) slot[i] = -1;
    x->nfr = 0;
    for(;;) {
        const struct rstate *st = &r->st[pc];
        if(++x->steps > x->limit) return -REG_ESTEPS;
        switch(st->op) {
        case R_CHAR: case R_ANY: case R_ANYNONL: case R_SET: {
            int c, len;
            if(pos >= x->end) goto fail;
            len = char_at(x->s + pos, (size_t)(x->end - pos), &c);
            if(!takes(r, st, c, fold(r, c))) goto fail;
            pos += len;
            pc++;
            continue;
        }
        case R_BACKREF: {
            int so = slot[2 * st->x], eo = slot[2 * st->x + 1];
            if(so < 0 || eo < 0 || eo - so > x->end - pos || !same_text(x, so, pos, eo - so)) goto fail;
            x->steps += (eo - so) / 16;
            pos += eo - so;
            pc++;
            continue;
        }
        case R_SPLIT:
            if((err = push(x, st->y, pos))) return -err;
            pc = st->x;
            continue;
        case R_JMP:
            pc = st->x;
            continue;
        case R_ASSERT:
            if(!holds(x, st->x, pos)) goto fail;
            pc++;
            continue;
        case R_SAVE: case R_MARK:
            if((err = push(x, ~st->x, slot[st->x]))) return -err;
            slot[st->x] = pos;
            pc++;
            continue;
        case R_CLEAR:
            for(i = 0; i < st->y; i++) {
                int g = 2 * (st->x + i);
                if((err = push(x, ~g, slot[g])) || (err = push(x, ~(g + 1), slot[g + 1]))) return -err;
                slot[g] = slot[g + 1] = -1;
            }
            pc++;
            continue;
        case R_CHECK:                           /* an iteration that read nothing does not go round again */
            pc = slot[st->x] == pos ? st->y : pc + 1;
            continue;
        case R_ACCEPT:
            if(!found || pos > x->me) {
                found = 1;
                x->me = pos;
                memcpy(r->best, slot, (size_t)r->nslot * sizeof(int));
            }
            if(x->boolean || pos == x->end) return 1;
            goto fail;
        }
    fail:
        for(;;) {
            struct frame *f;
            if(!x->nfr) return found;
            f = &x->fr[--x->nfr];
            if(f->pc >= 0) {
                pc = f->pc;
                pos = f->value;
                break;
            }
            slot[~f->pc] = f->value;
        }
    }
}

static int bt_exec(struct exec *x, int from) {
    int start, rc;
    x->steps = 0;
    x->limit = BT_STEPS + (long long)x->end * BT_PER_BYTE;
    x->fr = NULL;
    x->nfr = x->capfr = 0;
    rc = REG_NOMATCH;
    for(start = from; start <= x->end; ) {
        int c, got = bt_try(x, start);
        if(got < 0) {
            rc = -got;
            break;
        }
        if(got) {
            x->ms = start;
            rc = 0;
            break;
        }
        if(start == x->end) break;
        start += char_at(x->s + start, (size_t)(x->end - start), &c);
    }
    free(x->fr);
    return rc;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t *pmatch, int eflags) {
    struct rx *r = preg->re_g;
    struct exec x;
    size_t i, len;
    int from = 0, rc, g;
    if(!r) return REG_BADPAT;
    if(eflags & REG_STARTEND) {
        if(pmatch[0].rm_so < 0 || pmatch[0].rm_eo < pmatch[0].rm_so) return REG_BADPAT;
        from = pmatch[0].rm_so;
        len = (size_t)pmatch[0].rm_eo;
    } else {
        len = strlen(string);
        if(len > (size_t)INT_MAX) return REG_ESPACE;
    }
    if(r->nosub) nmatch = 0;
    x.r = r;
    x.s = (const unsigned char *)string;
    x.end = (int)len;
    x.eflags = eflags;
    x.boolean = !nmatch;
    if(r->backref) {
        int boolean = x.boolean;
        x.boolean = 0;                          /* its leftmost start, not just any */
        if(!scan(&x, from)) return REG_NOMATCH;
        x.boolean = boolean;
        if((rc = bt_exec(&x, x.ms))) return rc;
    } else if(!scan(&x, from)) {
        return REG_NOMATCH;
    }
    if(!nmatch) return 0;
    pmatch[0].rm_so = x.ms;
    pmatch[0].rm_eo = x.me;
    if(nmatch > 1 && r->nsub) {
        if(r->backref) {
            memcpy(r->caps, r->best, (size_t)(r->nsub + 1) * 2 * sizeof(regoff_t));
        } else {
            for(g = 2; g < 2 * (r->nsub + 1); g++) r->caps[g] = -1;
            if((rc = walk(&x, r->root, 0, x.ms, x.me))) return rc;
        }
    }
    for(i = 1; i < nmatch; i++) {
        int have = i <= (size_t)r->nsub;
        pmatch[i].rm_so = have ? r->caps[2 * i] : -1;
        pmatch[i].rm_eo = have ? r->caps[2 * i + 1] : -1;
    }
    return 0;
}
