/*
SurfOS user program: awk
--------------------
File: parse.c   Date: 11/01/26
--------------------
Reads the program and writes code for vm.c as it goes, in one pass, without a tree. A mistake ends
awk, so a parsing function never has to report one.

The grammar's corners, which this file is mostly about:

A / starts a regular expression where an operand is expected, and divides otherwise, so primary()
asks for the regular expression from where the lexer stands. A regular expression alone means
$0 ~ it. As an argument of ~, match, split, sub or gsub it means itself: the code just written
is one OP_RE, and is changed to OP_RELIT.

An argument of a function that is one bare name may be an array, which only the running program
can tell: the OP_LOADV just written is changed to OP_ARGV.

Assignment is taken up where its name is met, in after_lvalue(), as the grammar has it: 1 + x = 2 is
1 + (x = 2). Everything else is precedence climbing in binary(), so that brackets nest in a
handful of C stack frames and AWK_DEPTH_MAX of them fit well inside the 4 MB a program has.

In print, > is a redirection unless it stands in brackets: p->no_gt. print (a, b) > "x" has its
list in brackets, and so has (i, j) in x: primary() reads the list either way, and only then knows.
A bare | after an expression is print's, and a | before getline is an operator: looking at the
text after it tells them apart.

Concatenation has no token: an operand that stands next to another is one. + and - are not such
a start, so 1 " " -1 is 1 (" " - 1), as POSIX's grammar has it, and every awk.
*/

#include <stdlib.h>
#include <string.h>
#include "awk.h"

enum {
    T_EOF = 256, T_NL, T_NUM, T_STR, T_NAME, T_FUNCNAME, T_BUILTIN,
    T_BEGIN, T_END, T_FUNCTION, T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_BREAK, T_CONTINUE, T_NEXT, T_NEXTFILE, T_EXIT,
    T_RETURN, T_DELETE, T_IN, T_GETLINE, T_PRINT, T_PRINTF,
    T_EQ, T_NE, T_LE, T_GE, T_MATCH, T_NOMATCH, T_AND, T_OR, T_INC, T_DEC, T_APPEND, T_POW,
    T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_MODEQ, T_POWEQ
};

static const struct { const char *word; int token; } keywords[] = {
    { "BEGIN", T_BEGIN }, { "END", T_END }, { "function", T_FUNCTION }, { "func", T_FUNCTION }, { "if", T_IF },
    { "else", T_ELSE }, { "while", T_WHILE }, { "for", T_FOR }, { "do", T_DO }, { "break", T_BREAK },
    { "continue", T_CONTINUE }, { "next", T_NEXT }, { "nextfile", T_NEXTFILE }, { "exit", T_EXIT },
    { "return", T_RETURN }, { "delete", T_DELETE }, { "in", T_IN }, { "getline", T_GETLINE }, { "print", T_PRINT },
    { "printf", T_PRINTF },
};

static const struct { const char *word; int id, least, most; } builtins[] = {
    { "length", B_LENGTH, 0, 1 }, { "substr", B_SUBSTR, 2, 3 }, { "index", B_INDEX, 2, 2 }, { "split", B_SPLIT, 2, 3 },
    { "sub", B_SUB, 2, 3 }, { "gsub", B_GSUB, 2, 3 }, { "match", B_MATCH, 2, 2 }, { "sprintf", B_SPRINTF, 1, 100000 },
    { "sin", B_SIN, 1, 1 }, { "cos", B_COS, 1, 1 }, { "atan2", B_ATAN2, 2, 2 }, { "exp", B_EXP, 1, 1 }, { "log", B_LOG, 1, 1 },
    { "sqrt", B_SQRT, 1, 1 }, { "int", B_INT, 1, 1 }, { "rand", B_RAND, 0, 0 }, { "srand", B_SRAND, 0, 1 },
    { "tolower", B_TOLOWER, 1, 1 }, { "toupper", B_TOUPPER, 1, 1 }, { "system", B_SYSTEM, 1, 1 }, { "close", B_CLOSE, 1, 1 },
    { "fflush", B_FFLUSH, 0, 1 },
};

struct loop {
    struct loop *up;
    int breaks;                         /* the jumps to the end: a chain through their operands */
    int next;                           /* where continue goes, or -1 in a do: */
    int conts;                          /* then the jumps to the condition, a chain as well */
};

/* a section of the program ending in a jump to the next of its kind */
struct chain {
    unsigned first;
    int hole;
};

struct parser {
    const char *src;
    unsigned pos, len;
    unsigned line;
    const unsigned *starts;             /* where each program file starts in src */
    unsigned nsources, source;
    int tok;
    double tnum;
    struct buf text;                    /* of a string, a name or a regular expression */
    int tbuiltin;                       /* which, of T_BUILTIN */
    struct loop *loop;
    unsigned depth;
    int no_gt;                          /* print's list, outside brackets */
    int group_ok;                       /* the ( here may hold print's list */
    int group_n;                        /* and it held so many, when it is more than one */
    int in_function;
    int in_section;                     /* BEGIN or END */
    char **locals;
    unsigned nlocals, caplocals;
    unsigned call_line;
};

static void error_at(struct parser *p, const char *what) __attribute__((noreturn));
static void unexpected(struct parser *p) __attribute__((noreturn));

static void error_at(struct parser *p, const char *what) {
    char *at = NULL;
    if(A.sources && A.sources[p->source]) at = A.sources[p->source];
    fflush(stdout);
    if(at) fprintf(stderr, "awk: %s: line %u: %s\n", at, p->line, what);
    else fprintf(stderr, "awk: line %u: %s\n", p->line, what);
    exit(2);
}

static void unexpected(struct parser *p) {
    static const char *const ops[] = { "==", "!=", "<=", ">=", "~", "!~", "&&", "||", "++", "--", ">>", "**", "+=", "-=", "*=", "/=", "%=", "^=" };
    char what[80];
    int t = p->tok;
    if(t == T_EOF) error_at(p, "unexpected end of the program");
    if(t == T_NL) error_at(p, "unexpected end of line");
    if(t == T_NUM) error_at(p, "unexpected number");
    if(t == T_STR) error_at(p, "unexpected string");
    if(t >= T_EQ && t <= T_POWEQ) snprintf(what, sizeof(what), "syntax error near '%s'", ops[t - T_EQ]);
    else if(t >= 256) snprintf(what, sizeof(what), "syntax error near '%.40s'", p->text.p ? p->text.p : "");
    else snprintf(what, sizeof(what), "syntax error near '%c'", t);
    error_at(p, what);
}

/**** tokens ****/

static int peek(struct parser *p, unsigned k) {
    return p->pos + k < p->len ? (unsigned char)p->src[p->pos + k] : -1;
}

static void new_source_check(struct parser *p) {
    while(p->source + 1 < p->nsources && p->pos >= p->starts[p->source + 1]) {
        p->source++;
        p->line = 1;
    }
}

static int in_name(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int two(struct parser *p, int second, int both, int one) {
    if(peek(p, 0) == second) {
        p->pos++;
        return both;
    }
    return one;
}

/* the byte of the escape after a backslash; *used: its bytes; -1: no escape of awk's */
static int escape_byte(const char *s, unsigned n, unsigned *used) {
    *used = 1;
    switch(s[0]) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '\\': return '\\';
    case '"': return '"';
    case '/': return '/';
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return '\f';
    case 'v': return '\v';
    }
    if(s[0] >= '0' && s[0] <= '7') {
        unsigned k = 0;
        int v = 0;
        while(k < 3 && k < n && s[k] >= '0' && s[k] <= '7') v = v * 8 + (s[k++] - '0');
        *used = k;
        return v & 255;
    }
    return -1;
}

static void lex_string(struct parser *p) {
    p->text.len = 0;
    buf_add(&p->text, "", 0);
    for(;;) {
        int c = peek(p, 0);
        if(c < 0 || c == '\n') error_at(p, "a string without its closing \"");
        p->pos++;
        if(c == '"') break;
        if(c == '\\') {
            unsigned used;
            int v;
            if(peek(p, 0) == '\n') {            /* the string goes on on the next line */
                p->pos++;
                p->line++;
                continue;
            }
            if(p->pos == p->len) error_at(p, "a string without its closing \"");
            v = escape_byte(p->src + p->pos, p->len - p->pos, &used);
            if(v < 0) {                         /* not an escape: the backslash stays, for a regular expression */
                buf_ch(&p->text, '\\');
                buf_ch(&p->text, peek(p, 0));
                p->pos++;
            } else {
                buf_ch(&p->text, v);
                p->pos += used;
            }
        } else {
            buf_ch(&p->text, c);
        }
    }
    p->tok = T_STR;
}

/* from just after the /: only \/ and the slash that ends it are the lexer's; a / in brackets is no end */
static void lex_regex(struct parser *p, const char *first) {
    int bracket = 0, at_start = 0;
    p->text.len = 0;
    buf_add(&p->text, first, (unsigned)strlen(first));
    for(;;) {
        int c = peek(p, 0), d;
        if(c < 0 || c == '\n') error_at(p, "a regular expression without its closing /");
        p->pos++;
        d = peek(p, 0);
        if(c == '\\') {
            if(d < 0 || d == '\n') error_at(p, "a regular expression without its closing /");
            p->pos++;
            if(d != '/') buf_ch(&p->text, '\\');
            buf_ch(&p->text, d);
            at_start = 0;
            continue;
        }
        if(!bracket) {
            if(c == '/') break;
            if(c == '[') {
                bracket = 1;
                at_start = 1;
            }
        } else if(c == ']' && !at_start) {
            bracket = 0;
        } else if(c == '[' && (d == ':' || d == '.' || d == '=')) {
            unsigned k = 1;                     /* [:alpha:] and the like hold a ] of their own */
            while(peek(p, k) >= 0 && peek(p, k) != '\n' && !(peek(p, k) == d && peek(p, k + 1) == ']')) k++;
            at_start = 0;
            if(peek(p, k) == d) {
                buf_add(&p->text, p->src + p->pos - 1, k + 3);
                p->pos += k + 2;
                continue;
            }
        } else {
            at_start = at_start == 1 && c == '^';
        }
        buf_ch(&p->text, c);
    }
}

static void lex_number(struct parser *p) {
    unsigned s = p->pos;
    while(peek(p, 0) >= '0' && peek(p, 0) <= '9') p->pos++;
    if(peek(p, 0) == '.') {
        p->pos++;
        while(peek(p, 0) >= '0' && peek(p, 0) <= '9') p->pos++;
    }
    if(peek(p, 0) == 'e' || peek(p, 0) == 'E') {
        unsigned k = peek(p, 1) == '+' || peek(p, 1) == '-' ? 2 : 1;
        if(peek(p, k) >= '0' && peek(p, k) <= '9') {
            p->pos += k;
            while(peek(p, 0) >= '0' && peek(p, 0) <= '9') p->pos++;
        }
    }
    p->tnum = str_num(p->src + s, p->pos - s);
    p->tok = T_NUM;
}

static void lex_name(struct parser *p) {
    unsigned s = p->pos, i, n;
    while(in_name(peek(p, 0))) p->pos++;
    n = p->pos - s;
    p->text.len = 0;
    buf_add(&p->text, p->src + s, n);
    for(i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if(!strcmp(p->text.p, keywords[i].word)) {
            p->tok = keywords[i].token;
            return;
        }
    }
    for(i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if(!strcmp(p->text.p, builtins[i].word)) {
            p->tok = T_BUILTIN;
            p->tbuiltin = (int)i;
            return;
        }
    }
    p->tok = peek(p, 0) == '(' ? T_FUNCNAME : T_NAME;
}

static void next(struct parser *p) {
    int c;
    for(;;) {
        new_source_check(p);
        c = peek(p, 0);
        if(c == ' ' || c == '\t' || c == '\r') {
            p->pos++;
        } else if(c == '\\' && (peek(p, 1) == '\n' || (peek(p, 1) == '\r' && peek(p, 2) == '\n'))) {
            p->pos += peek(p, 1) == '\n' ? 2 : 3;
            p->line++;
        } else if(c == '#') {
            while(peek(p, 0) >= 0 && peek(p, 0) != '\n') p->pos++;
        } else {
            break;
        }
    }
    if(c < 0) {
        p->tok = T_EOF;
        return;
    }
    if(c == '\n') {
        p->pos++;
        p->line++;
        p->tok = T_NL;
        return;
    }
    if((c >= '0' && c <= '9') || (c == '.' && peek(p, 1) >= '0' && peek(p, 1) <= '9')) {
        lex_number(p);
        return;
    }
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        lex_name(p);
        return;
    }
    p->pos++;
    switch(c) {
    case '"': lex_string(p); return;
    case '+':
        if(peek(p, 0) == '+') {
            p->pos++;
            p->tok = T_INC;
        } else {
            p->tok = two(p, '=', T_ADDEQ, '+');
        }
        return;
    case '-':
        if(peek(p, 0) == '-') {
            p->pos++;
            p->tok = T_DEC;
        } else {
            p->tok = two(p, '=', T_SUBEQ, '-');
        }
        return;
    case '*':
        if(peek(p, 0) == '*') {                 /* as ^, in every awk but POSIX's */
            p->pos++;
            p->tok = two(p, '=', T_POWEQ, '^');
        } else {
            p->tok = two(p, '=', T_MULEQ, '*');
        }
        return;
    case '/': p->tok = two(p, '=', T_DIVEQ, '/'); return;
    case '%': p->tok = two(p, '=', T_MODEQ, '%'); return;
    case '^': p->tok = two(p, '=', T_POWEQ, '^'); return;
    case '=': p->tok = two(p, '=', T_EQ, '='); return;
    case '<': p->tok = two(p, '=', T_LE, '<'); return;
    case '>':
        p->tok = two(p, '=', T_GE, '>');
        if(p->tok == '>' && peek(p, 0) == '>') {
            p->pos++;
            p->tok = T_APPEND;
        }
        return;
    case '!':
        if(peek(p, 0) == '~') {
            p->pos++;
            p->tok = T_NOMATCH;
        } else {
            p->tok = two(p, '=', T_NE, '!');
        }
        return;
    case '~': p->tok = T_MATCH; return;
    case '&':
        if(peek(p, 0) == '&') {
            p->pos++;
            p->tok = T_AND;
            return;
        }
        break;
    case '|': p->tok = two(p, '|', T_OR, '|'); return;
    case '{': case '}': case '(': case ')': case '[': case ']': case ';': case ',': case '?': case ':': case '$':
        p->tok = c;
        return;
    }
    {
        char what[48];
        if(c >= 32 && c < 127) snprintf(what, sizeof(what), "unexpected character '%c'", c);
        else snprintf(what, sizeof(what), "unexpected character 0x%02x", (unsigned)c);
        error_at(p, what);
    }
}

static void skip_newlines(struct parser *p) {
    while(p->tok == T_NL) next(p);
}

static void expect(struct parser *p, int tok) {
    if(p->tok != tok) unexpected(p);
    next(p);
}

/* the text after a | says what it is for: the word getline, and not a name that starts with it */
static int getline_follows(struct parser *p) {
    unsigned k = 0;
    while(peek(p, k) == ' ' || peek(p, k) == '\t') k++;
    return p->pos + k + 7 <= p->len && !memcmp(p->src + p->pos + k, "getline", 7) && !in_name(peek(p, k + 7));
}

/**** writing code ****/

static void emit(int word) {
    if(A.ncode == A.capcode) {
        A.capcode = A.capcode ? A.capcode * 2 : 256;
        if(A.capcode >= 0x10000000u) fatal("the program is too long");
        A.code = xrealloc(A.code, A.capcode * sizeof(int));
    }
    A.code[A.ncode++] = word;
}

static void emit2(int op, int a) {
    emit(op);
    emit(a);
}

static void emit3(int op, int a, int b) {
    emit2(op, a);
    emit(b);
}

static int emit_jump(int op) {
    emit2(op, 0);
    return (int)A.ncode - 1;
}

static void patch(int hole) {
    A.code[hole] = (int)A.ncode;
}

static void emit_break(struct parser *p, int op) {
    int hole = emit_jump(op);
    A.code[hole] = p->loop->breaks;
    p->loop->breaks = hole;
}

static void patch_breaks(struct loop *l) {
    int at = l->breaks;
    while(at >= 0) {
        int up = A.code[at];
        A.code[at] = (int)A.ncode;
        at = up;
    }
}

static void mark_line(struct parser *p) {
    if(A.nmarks && A.marks[A.nmarks - 1].line == p->line && A.marks[A.nmarks - 1].source == p->source) return;
    if(A.nmarks == A.capmarks) {
        A.capmarks = A.capmarks ? A.capmarks * 2 : 64;
        A.marks = xrealloc(A.marks, A.capmarks * sizeof(*A.marks));
    }
    A.marks[A.nmarks].pc = A.ncode;
    A.marks[A.nmarks].line = p->line;
    A.marks[A.nmarks++].source = p->source;
}

static int add_num(double d) {
    if(A.nknum == A.capknum) {
        A.capknum = A.capknum ? A.capknum * 2 : 32;
        A.knum = xrealloc(A.knum, A.capknum * sizeof(*A.knum));
    }
    A.knum[A.nknum] = d;
    return (int)A.nknum++;
}

static int add_str(const char *s, unsigned n) {
    if(A.nkstr == A.capkstr) {
        A.capkstr = A.capkstr ? A.capkstr * 2 : 32;
        A.kstr = xrealloc(A.kstr, A.capkstr * sizeof(*A.kstr));
    }
    A.kstr[A.nkstr] = str_new(s, n);
    return (int)A.nkstr++;
}

static int add_regex(struct parser *p) {
    const struct regex *re = regex_static(p->text.p, p->text.len);
    char what[192];
    if(!re) {
        snprintf(what, sizeof(what), "/%.40s/: %s", p->text.p, regex_error);
        error_at(p, what);
    }
    if(A.nkre == A.capkre) {
        A.capkre = A.capkre ? A.capkre * 2 : 16;
        A.kre = xrealloc(A.kre, A.capkre * sizeof(*A.kre));
    }
    A.kre[A.nkre] = re;
    return (int)A.nkre++;
}

static int func_id(const char *name) {
    unsigned i;
    for(i = 0; i < A.nfn; i++) if(!strcmp(A.fn[i].name, name)) return (int)i;
    if(A.nfn == A.capfn) {
        A.capfn = A.capfn ? A.capfn * 2 : 16;
        A.fn = xrealloc(A.fn, A.capfn * sizeof(*A.fn));
    }
    A.fn[A.nfn].name = xalloc(strlen(name) + 1);
    strcpy(A.fn[A.nfn].name, name);
    A.fn[A.nfn].defined = 0;
    A.fn[A.nfn].nparams = 0;
    A.fn[A.nfn].entry = 0;
    A.fn[A.nfn].most_args = 0;
    A.fn[A.nfn].line = 0;
    return (int)A.nfn++;
}

/**** expressions ****/

struct place {
    int kind, operand;
};

static void expr(struct parser *p);
static void binary(struct parser *p, int min);
static void primary(struct parser *p, int no_post);
static void primary_in(struct parser *p, int no_post);
static void unary(struct parser *p);

enum { PREC_OR = 1, PREC_AND, PREC_IN, PREC_MATCH, PREC_REL, PREC_PIPE, PREC_CONCAT, PREC_ADD, PREC_MUL };

static void deeper(struct parser *p) {
    if(++p->depth > AWK_DEPTH_MAX) error_at(p, "nested too deeply");
}

static int local_slot(struct parser *p, const char *name) {
    unsigned i;
    if(!p->in_function) return -1;
    for(i = 0; i < p->nlocals; i++) if(!strcmp(p->locals[i], name)) return (int)i;
    return -1;
}

/* the regular expression that the code from start is, as itself: for ~ and the functions that take one */
static void regex_itself(unsigned start) {
    if(A.ncode == start + 2 && A.code[start] == OP_RE) A.code[start] = OP_RELIT;
}

/* the array of this name, on the stack */
static void emit_array(struct parser *p) {
    int slot = local_slot(p, p->text.p);
    if(slot >= 0) emit2(OP_ARRL, slot);
    else emit2(OP_ARRV, global_id(p->text.p, p->text.len));
}

/* brackets and arguments start afresh: > is a comparison there */
#define FRESH_BEGIN int saved_gt = p->no_gt; p->no_gt = 0
#define FRESH_END p->no_gt = saved_gt

/* [ list ] after an array's name: one key on the stack */
static void subscripts(struct parser *p) {
    int n = 0;
    FRESH_BEGIN;
    next(p);
    for(;;) {
        expr(p);
        n++;
        if(p->tok != ',') break;
        next(p);
        skip_newlines(p);
    }
    expect(p, ']');
    if(n > 1) emit2(OP_JOIN, n);
    FRESH_END;
}

static void emit_load(const struct place *w) {
    switch(w->kind) {
    case PL_VAR: emit2(OP_LOADV, w->operand); break;
    case PL_LOCAL: emit2(OP_LOADL, w->operand); break;
    case PL_NF: emit(OP_LOADNF); break;
    case PL_ARRAY: emit(OP_AGET); break;
    case PL_FIELD: emit(OP_FIELD); break;
    }
}

static void emit_store(const struct place *w) {
    emit3(OP_STORE, w->kind, w->operand);
}

static int compound_op(int tok) {
    switch(tok) {
    case T_ADDEQ: return OP_ADD;
    case T_SUBEQ: return OP_SUB;
    case T_MULEQ: return OP_MUL;
    case T_DIVEQ: return OP_DIV;
    case T_MODEQ: return OP_MOD;
    case T_POWEQ: return OP_POW;
    }
    return -1;
}

/* An lvalue has been read and its prefix written; what stands after it says what it is for. */
static void after_lvalue(struct parser *p, const struct place *w, int no_post) {
    int op = compound_op(p->tok);
    if(no_post) {
        emit_load(w);
        return;
    }
    if(p->tok == '=') {
        next(p);
        skip_newlines(p);
        expr(p);
        emit_store(w);
    } else if(op >= 0) {                /* the right side first, as every awk has it: x += x += 2 */
        next(p);
        skip_newlines(p);
        expr(p);
        emit(OP_UPDATE);
        emit(w->kind);
        emit(w->operand);
        emit(op);
    } else if(p->tok == T_INC || p->tok == T_DEC) {
        emit(OP_STEP);
        emit(w->kind);
        emit(w->operand);
        emit(STEP_POST | (p->tok == T_DEC ? STEP_DOWN : 0));
        next(p);
    } else {
        emit_load(w);
    }
}

/* a name and its subscripts, as a place */
static void name_place(struct parser *p, struct place *w) {
    char *name;
    int slot;
    if(p->tok != T_NAME) unexpected(p);
    name = xalloc(p->text.len + 1);
    memcpy(name, p->text.p, p->text.len + 1);
    next(p);
    if(p->tok == '[') {
        slot = local_slot(p, name);
        if(slot >= 0) emit2(OP_ARRL, slot);
        else emit2(OP_ARRV, global_id(name, (unsigned)strlen(name)));
        free(name);
        subscripts(p);
        w->kind = PL_ARRAY;
        w->operand = 0;
        return;
    }
    slot = local_slot(p, name);
    if(slot >= 0) {
        w->kind = PL_LOCAL;
        w->operand = slot;
    } else {
        w->operand = global_id(name, (unsigned)strlen(name));
        w->kind = w->operand == S_NF ? PL_NF : PL_VAR;
    }
    free(name);
}

/* $ and what it takes: the highest precedence there is, so $i++ is ($i)++ and $x^2 is ($x)^2 */
static void field_place(struct parser *p, struct place *w) {
    next(p);
    primary(p, 1);
    w->kind = PL_FIELD;
    w->operand = 0;
}

static void lvalue(struct parser *p, struct place *w) {
    if(p->tok == '$') field_place(p, w);
    else name_place(p, w);
}

static void pre_step(struct parser *p) {
    struct place w;
    int down = p->tok == T_DEC;
    next(p);
    if(p->tok != T_NAME && p->tok != '$') unexpected(p);
    lvalue(p, &w);
    emit(OP_STEP);
    emit(w.kind);
    emit(w.operand);
    emit(down ? STEP_DOWN : 0);
}

/* how much a place has on the stack */
static int prefix_size(const struct place *w) {
    return w->kind == PL_ARRAY ? 2 : w->kind == PL_FIELD ? 1 : 0;
}

static void simple_get(struct parser *p, int source) {
    struct place w;
    w.kind = PL_RECORD;
    w.operand = 0;
    next(p);
    if(p->tok == T_NAME || p->tok == '$') lvalue(p, &w);
    if(source == GL_MAIN && p->tok == '<') {
        next(p);
        binary(p, PREC_ADD);
        source = GL_FILE;
    }
    emit3(OP_GETLINE, source, prefix_size(&w));
    emit3(OP_GL, w.kind, w.operand);
}

static void arguments(struct parser *p, const char *name) {
    int id = func_id(name), n = 0;
    FRESH_BEGIN;
    next(p);                            /* the ( */
    next(p);
    skip_newlines(p);
    while(p->tok != ')') {
        unsigned start = A.ncode;
        if(n) {
            expect(p, ',');
            skip_newlines(p);
            start = A.ncode;
        }
        expr(p);
        skip_newlines(p);
        if(A.ncode == start + 2 && A.code[start] == OP_LOADV) A.code[start] = OP_ARGV;
        else if(A.ncode == start + 2 && A.code[start] == OP_LOADL) A.code[start] = OP_ARGL;
        n++;
    }
    next(p);
    emit3(OP_CALL, id, n);
    if((unsigned)n > A.fn[id].most_args) A.fn[id].most_args = (unsigned)n;
    if(!A.fn[id].line) A.fn[id].line = p->line;
    FRESH_END;
}

/* the array of split(): a name */
static void array_argument(struct parser *p) {
    if(p->tok != T_NAME) unexpected(p);
    emit_array(p);
    next(p);
}

static void builtin(struct parser *p) {
    int i = p->tbuiltin, id = builtins[i].id, n = 0;
    FRESH_BEGIN;
    next(p);
    if(id == B_LENGTH && p->tok != '(') {       /* length alone is length($0) */
        emit3(OP_BUILTIN, B_LENGTH, 0);
        FRESH_END;
        return;
    }
    if(p->tok != '(') unexpected(p);
    next(p);
    skip_newlines(p);
    if(id == B_SUB || id == B_GSUB) {
        struct place w;
        unsigned start = A.ncode;
        expr(p);
        regex_itself(start);
        skip_newlines(p);
        expect(p, ',');
        skip_newlines(p);
        expr(p);
        skip_newlines(p);
        w.kind = PL_RECORD;
        w.operand = 0;
        if(p->tok == ',') {
            next(p);
            skip_newlines(p);
            if(p->tok != T_NAME && p->tok != '$') error_at(p, "the third argument of sub and gsub must be a variable");
            lvalue(p, &w);
            skip_newlines(p);
        }
        if(p->tok != ')') error_at(p, "the third argument of sub and gsub must be a variable");
        next(p);
        emit(OP_SUBST);
        emit(w.kind);
        emit(w.operand);
        emit(id == B_GSUB);
        FRESH_END;
        return;
    }
    while(p->tok != ')') {
        unsigned start;
        if(n) {
            expect(p, ',');
            skip_newlines(p);
        }
        start = A.ncode;
        if(id == B_SPLIT && n == 1) {
            array_argument(p);
        } else if((id == B_SPLIT && n == 2) || (id == B_MATCH && n == 1)) {
            expr(p);
            regex_itself(start);
        } else {
            expr(p);
            if(id == B_LENGTH && A.ncode == start + 2) {        /* the length of an array */
                if(A.code[start] == OP_LOADV) A.code[start] = OP_ARGV;
                else if(A.code[start] == OP_LOADL) A.code[start] = OP_ARGL;
            }
        }
        skip_newlines(p);
        n++;
    }
    next(p);
    if(n < builtins[i].least || n > builtins[i].most) {
        char what[64];
        snprintf(what, sizeof(what), "%s takes %d to %d arguments, not %d", builtins[i].word, builtins[i].least, builtins[i].most, n);
        error_at(p, what);
    }
    emit3(OP_BUILTIN, id, n);
    FRESH_END;
}

/* ( expr ), or ( list ) before in, or print's ( list ) */
static void group(struct parser *p) {
    int ok = p->group_ok, n = 1;
    FRESH_BEGIN;
    p->group_ok = 0;
    next(p);
    expr(p);
    while(p->tok == ',') {
        next(p);
        skip_newlines(p);
        expr(p);
        n++;
    }
    expect(p, ')');
    FRESH_END;
    if(n == 1) return;
    if(p->tok == T_IN) {
        next(p);
        emit2(OP_JOIN, n);
        if(p->tok != T_NAME) unexpected(p);
        emit_array(p);
        next(p);
        emit(OP_AIN);
    } else if(ok) {
        p->group_n = n;
    } else {
        error_at(p, "a list where a value should be");
    }
}

/* every way down the grammar comes through here, $$$$ and $---- too, so it is here that depth is counted */
static void primary(struct parser *p, int no_post) {
    deeper(p);
    primary_in(p, no_post);
    p->depth--;
}

static void primary_in(struct parser *p, int no_post) {
    struct place w;
    switch(p->tok) {
    case T_NUM:
        emit2(OP_NUM, add_num(p->tnum));
        next(p);
        return;
    case T_STR:
        emit2(OP_STR, add_str(p->text.p, p->text.len));
        next(p);
        return;
    case '/': case T_DIVEQ:
        lex_regex(p, p->tok == T_DIVEQ ? "=" : "");
        emit2(OP_RE, add_regex(p));
        next(p);
        return;
    case '(':
        group(p);
        return;
    case '$':
        field_place(p, &w);
        after_lvalue(p, &w, no_post);
        return;
    case T_INC: case T_DEC:
        pre_step(p);
        return;
    case '-': case '+': case '!':
        if(no_post) {                   /* $-1, $!x */
            int t = p->tok;
            next(p);
            primary(p, 1);
            emit(t == '-' ? OP_NEG : t == '+' ? OP_PLUS : OP_NOT);
            return;
        }
        break;
    case T_NAME:
        name_place(p, &w);
        after_lvalue(p, &w, no_post);
        return;
    case T_FUNCNAME: {
        char *name = xalloc(p->text.len + 1);
        memcpy(name, p->text.p, p->text.len + 1);
        arguments(p, name);
        free(name);
        return;
    }
    case T_BUILTIN:
        builtin(p);
        return;
    case T_GETLINE:
        simple_get(p, GL_MAIN);
        return;
    }
    unexpected(p);
}

static void power(struct parser *p) {
    primary(p, 0);
    if(p->tok == '^') {
        deeper(p);
        next(p);
        unary(p);                       /* 2^-1, and it goes right to left */
        p->depth--;
        emit(OP_POW);
    }
}

static void unary(struct parser *p) {
    int t = p->tok;
    deeper(p);
    if(t == '!' || t == '-' || t == '+') {
        next(p);
        unary(p);
        emit(t == '-' ? OP_NEG : t == '+' ? OP_PLUS : OP_NOT);
    } else {
        power(p);
    }
    p->depth--;
}

static int starts_operand(int t) {
    return t == T_NUM || t == T_STR || t == T_NAME || t == T_FUNCNAME || t == T_BUILTIN || t == '$' || t == '(' ||
           t == T_INC || t == T_DEC;
}

static int binary_op(struct parser *p, int *prec) {
    switch(p->tok) {
    case '+': *prec = PREC_ADD; return OP_ADD;
    case '-': *prec = PREC_ADD; return OP_SUB;
    case '*': *prec = PREC_MUL; return OP_MUL;
    case '/': *prec = PREC_MUL; return OP_DIV;
    case '%': *prec = PREC_MUL; return OP_MOD;
    case '<': *prec = PREC_REL; return OP_LT;
    case T_LE: *prec = PREC_REL; return OP_LE;
    case T_EQ: *prec = PREC_REL; return OP_EQ;
    case T_NE: *prec = PREC_REL; return OP_NE;
    case T_GE: *prec = PREC_REL; return OP_GE;
    case '>':
        *prec = p->no_gt ? 0 : PREC_REL;
        return OP_GT;
    }
    *prec = 0;
    return -1;
}

/* a && b and a || b, which leave 1 or 0 and only run the right side when it counts */
static void logic(struct parser *p, int op, int prec) {
    int jump = op == T_AND ? OP_JZ : OP_JNZ, first, second, skip;
    first = emit_jump(jump);
    next(p);
    skip_newlines(p);
    binary(p, prec + 1);
    second = emit_jump(jump);
    if(op == T_AND) emit(OP_ONE);
    else emit2(OP_NUM, add_num(0));
    skip = emit_jump(OP_JMP);
    patch(first);
    patch(second);
    if(op == T_AND) emit2(OP_NUM, add_num(0));
    else emit(OP_ONE);
    patch(skip);
}

static void binary(struct parser *p, int min) {
    unary(p);
    for(;;) {
        int prec, op = binary_op(p, &prec), t = p->tok;
        if(p->group_n) return;          /* print's list: nothing goes on from it */
        if(prec && prec >= min) {
            deeper(p);
            next(p);
            if(prec == PREC_REL) skip_newlines(p);
            binary(p, prec + 1);
            p->depth--;
            emit(op);
        } else if(starts_operand(t) && PREC_CONCAT >= min) {
            int n = 1;
            while(starts_operand(p->tok) && !p->group_n) {
                binary(p, PREC_ADD);
                n++;
            }
            emit2(OP_CONCAT, n);
        } else if(t == '|' && PREC_PIPE >= min && getline_follows(p)) {
            next(p);
            simple_get(p, GL_CMD);
        } else if((t == T_MATCH || t == T_NOMATCH) && PREC_MATCH >= min) {
            unsigned start;
            next(p);
            start = A.ncode;
            binary(p, PREC_MATCH + 1);
            regex_itself(start);
            emit(OP_MATCH);
            if(t == T_NOMATCH) emit(OP_NOT);
        } else if(t == T_IN && PREC_IN >= min) {
            next(p);
            if(p->tok != T_NAME) unexpected(p);
            emit_array(p);
            next(p);
            emit(OP_AIN);
        } else if(t == T_AND && PREC_AND >= min) {
            logic(p, T_AND, PREC_AND);
        } else if(t == T_OR && PREC_OR >= min) {
            logic(p, T_OR, PREC_OR);
        } else {
            return;
        }
    }
}

static void expr(struct parser *p) {
    deeper(p);
    binary(p, PREC_OR);
    if(p->tok == '?') {
        int to_else, to_end;
        next(p);
        skip_newlines(p);
        to_else = emit_jump(OP_JZ);
        expr(p);
        skip_newlines(p);
        expect(p, ':');
        skip_newlines(p);
        to_end = emit_jump(OP_JMP);
        patch(to_else);
        expr(p);
        patch(to_end);
    }
    p->depth--;
}

/**** statements ****/

static void statement(struct parser *p);

static void block(struct parser *p) {
    next(p);
    for(;;) {
        while(p->tok == T_NL || p->tok == ';') next(p);
        if(p->tok == '}') break;
        statement(p);
    }
    next(p);
}

/* the end of a simple statement: ; or a newline, or a } or the end that the caller takes */
static void terminator(struct parser *p) {
    if(p->tok == ';' || p->tok == T_NL) {
        next(p);
    } else if(p->tok != '}' && p->tok != T_EOF) {
        unexpected(p);
    }
}

static int ends_statement(int t) {
    return t == ';' || t == T_NL || t == '}' || t == T_EOF;
}

static void print_statement(struct parser *p) {
    int op = p->tok == T_PRINT ? OP_PRINT : OP_PRINTF, n = 0, how = TO_STDOUT;
    next(p);
    p->no_gt = 1;
    if(!ends_statement(p->tok) && p->tok != '>' && p->tok != T_APPEND && p->tok != '|') {
        p->group_n = 0;
        p->group_ok = p->tok == '(';
        expr(p);
        p->group_ok = 0;
        n = p->group_n ? p->group_n : 1;
        p->group_n = 0;
        while(p->tok == ',') {
            next(p);
            skip_newlines(p);
            expr(p);
            n++;
        }
    }
    if(op == OP_PRINTF && !n) error_at(p, "printf without a format");
    if(p->tok == '>' || p->tok == T_APPEND || p->tok == '|') {
        how = p->tok == '>' ? TO_FILE : p->tok == T_APPEND ? TO_APPEND : TO_PIPE;
        next(p);
        binary(p, PREC_CONCAT);
    }
    p->no_gt = 0;
    emit3(op, n, how);
}

static void simple_statement(struct parser *p) {
    switch(p->tok) {
    case T_PRINT: case T_PRINTF:
        print_statement(p);
        break;
    case T_DELETE:
        next(p);
        if(p->tok != T_NAME) unexpected(p);
        emit_array(p);
        next(p);
        if(p->tok == '[') {
            subscripts(p);
            emit(OP_ADEL);
        } else {
            emit(OP_ACLEAR);
        }
        break;
    default:
        expr(p);
        emit(OP_POP);
        break;
    }
}

static void st_if(struct parser *p) {
    int to_else, to_end;
    next(p);
    expect(p, '(');
    expr(p);
    expect(p, ')');
    skip_newlines(p);
    to_else = emit_jump(OP_JZ);
    statement(p);
    /* an else may stand on a later line; these are only separators, so nothing is lost by skipping them */
    while(p->tok == T_NL || p->tok == ';') next(p);
    if(p->tok != T_ELSE) {
        patch(to_else);
        return;
    }
    to_end = emit_jump(OP_JMP);
    patch(to_else);
    next(p);
    skip_newlines(p);
    statement(p);
    patch(to_end);
}

static void st_while(struct parser *p) {
    struct loop l;
    l.up = p->loop;
    l.breaks = -1;
    l.next = (int)A.ncode;
    l.conts = -1;
    p->loop = &l;
    next(p);
    expect(p, '(');
    expr(p);
    expect(p, ')');
    emit_break(p, OP_JZ);
    if(p->tok == ';') {                 /* while(x); is a loop of nothing */
        next(p);
    } else {
        skip_newlines(p);
        statement(p);
    }
    emit2(OP_JMP, l.next);
    patch_breaks(&l);
    p->loop = l.up;
}

static void st_do(struct parser *p) {
    struct loop l;
    int top = (int)A.ncode, cont;
    l.up = p->loop;
    l.breaks = -1;
    l.next = -1;
    l.conts = -1;
    p->loop = &l;
    next(p);
    skip_newlines(p);
    statement(p);
    while(p->tok == T_NL || p->tok == ';') next(p);
    if(p->tok != T_WHILE) unexpected(p);
    cont = l.conts;
    while(cont >= 0) {
        int up = A.code[cont];
        A.code[cont] = (int)A.ncode;
        cont = up;
    }
    next(p);
    expect(p, '(');
    expr(p);
    expect(p, ')');
    emit2(OP_JNZ, top);
    patch_breaks(&l);
    p->loop = l.up;
    terminator(p);
}

/* After the name that follows for(: in, a name and ), which make a for-in, looked for in the text.
   Anything else is an ordinary for, and no token has been used. */
static int for_in_ahead(struct parser *p) {
    unsigned k = 0;
#define SKIP_BLANKS while(peek(p, k) == ' ' || peek(p, k) == '\t') k++
#define NAME_CHAR(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9') || (c) == '_')
    SKIP_BLANKS;
    if(peek(p, k) != 'i' || peek(p, k + 1) != 'n' || NAME_CHAR(peek(p, k + 2))) return 0;
    k += 2;
    SKIP_BLANKS;
    if(!NAME_CHAR(peek(p, k)) || (peek(p, k) >= '0' && peek(p, k) <= '9')) return 0;
    while(NAME_CHAR(peek(p, k))) k++;
    SKIP_BLANKS;
    return peek(p, k) == ')';
}

static char *token_copy(struct parser *p) {
    char *t = xalloc(p->text.len + 1);
    memcpy(t, p->text.p, p->text.len + 1);
    return t;
}

static void st_for_in(struct parser *p, char *name, char *array) {
    struct loop l;
    int slot, kind, operand, exit_hole, top;
    next(p);                            /* the ) */
    slot = local_slot(p, array);
    if(slot >= 0) emit2(OP_ARRL, slot);
    else emit2(OP_ARRV, global_id(array, (unsigned)strlen(array)));
    emit(OP_ITER);
    slot = local_slot(p, name);
    kind = slot >= 0 ? PL_LOCAL : PL_VAR;
    operand = slot >= 0 ? slot : global_id(name, (unsigned)strlen(name));
    if(kind == PL_VAR && operand == S_NF) kind = PL_NF;
    free(name);
    free(array);
    top = (int)A.ncode;
    emit(OP_ITERNEXT);
    emit(kind);
    emit(operand);
    emit(0);
    exit_hole = (int)A.ncode - 1;
    l.up = p->loop;
    l.breaks = -1;
    l.next = top;
    l.conts = -1;
    p->loop = &l;
    if(p->tok == ';') {
        next(p);
    } else {
        skip_newlines(p);
        statement(p);
    }
    emit2(OP_JMP, top);
    patch(exit_hole);
    patch_breaks(&l);
    emit(OP_POP);                       /* the iterator */
    p->loop = l.up;
}

static void st_for(struct parser *p) {
    struct loop l;
    int test, to_body;
    next(p);
    expect(p, '(');
    if(p->tok == T_NAME && for_in_ahead(p)) {
        char *name = token_copy(p), *array;
        next(p);                        /* in */
        next(p);
        array = token_copy(p);
        next(p);
        st_for_in(p, name, array);
        return;
    }
    if(p->tok != ';') simple_statement(p);
    expect(p, ';');
    skip_newlines(p);
    l.up = p->loop;
    l.breaks = -1;
    l.conts = -1;
    p->loop = &l;
    test = (int)A.ncode;
    if(p->tok != ';') {
        expr(p);
        emit_break(p, OP_JZ);
    }
    expect(p, ';');
    skip_newlines(p);
    to_body = emit_jump(OP_JMP);
    l.next = (int)A.ncode;
    if(p->tok != ')') simple_statement(p);
    emit2(OP_JMP, test);
    expect(p, ')');
    patch(to_body);
    if(p->tok == ';') {
        next(p);
    } else {
        skip_newlines(p);
        statement(p);
    }
    emit2(OP_JMP, l.next);
    patch_breaks(&l);
    p->loop = l.up;
}

static void statement(struct parser *p) {
    deeper(p);
    mark_line(p);
    switch(p->tok) {
    case '{':
        block(p);
        break;
    case ';':
        next(p);
        break;
    case T_IF:
        st_if(p);
        break;
    case T_WHILE:
        st_while(p);
        break;
    case T_DO:
        st_do(p);
        break;
    case T_FOR:
        st_for(p);
        break;
    case T_BREAK:
        if(!p->loop) error_at(p, "break outside a loop");
        next(p);
        emit_break(p, OP_JMP);
        terminator(p);
        break;
    case T_CONTINUE:
        if(!p->loop) error_at(p, "continue outside a loop");
        next(p);
        if(p->loop->next >= 0) {
            emit2(OP_JMP, p->loop->next);
        } else {                        /* do: the condition is yet to come */
            int hole = emit_jump(OP_JMP);
            A.code[hole] = p->loop->conts;
            p->loop->conts = hole;
        }
        terminator(p);
        break;
    case T_NEXT:
        if(p->in_section) error_at(p, "next in BEGIN or END");
        next(p);
        emit(OP_NEXT);
        terminator(p);
        break;
    case T_NEXTFILE:
        if(p->in_section) error_at(p, "nextfile in BEGIN or END");
        next(p);
        emit(OP_NEXTFILE);
        terminator(p);
        break;
    case T_EXIT:
        next(p);
        if(ends_statement(p->tok)) {
            emit(OP_EXIT0);
        } else {
            expr(p);
            emit(OP_EXIT);
        }
        terminator(p);
        break;
    case T_RETURN:
        if(!p->in_function) error_at(p, "return outside a function");
        next(p);
        if(ends_statement(p->tok)) {
            emit(OP_RET0);
        } else {
            expr(p);
            emit(OP_RET);
        }
        terminator(p);
        break;
    default:
        simple_statement(p);
        terminator(p);
        break;
    }
    p->depth--;
}

/**** rules and functions ****/

static void section_begin(struct chain *c, int *used) {
    if(*used) patch(c->hole);
    else c->first = A.ncode;
    *used = 1;
}

static void section_end(struct chain *c) {
    c->hole = emit_jump(OP_JMP);
}

static void action(struct parser *p) {
    p->loop = NULL;
    block(p);
}

static void function(struct parser *p) {
    int id;
    unsigned i;
    next(p);
    if(p->tok != T_NAME && p->tok != T_FUNCNAME) unexpected(p);
    id = func_id(p->text.p);
    if(A.fn[id].defined) error_at(p, "a function defined twice");
    next(p);
    expect(p, '(');
    p->nlocals = 0;
    while(p->tok != ')') {
        if(p->nlocals) {
            expect(p, ',');
            skip_newlines(p);
        }
        if(p->tok != T_NAME) unexpected(p);
        for(i = 0; i < p->nlocals; i++) if(!strcmp(p->locals[i], p->text.p)) error_at(p, "a name twice in one function");
        if(!strcmp(p->text.p, A.fn[id].name)) error_at(p, "a parameter with the function's name");
        if(p->nlocals == p->caplocals) {
            p->caplocals = p->caplocals ? p->caplocals * 2 : 8;
            p->locals = xrealloc(p->locals, p->caplocals * sizeof(char *));
            for(i = p->nlocals; i < p->caplocals; i++) p->locals[i] = NULL;
        }
        free(p->locals[p->nlocals]);
        p->locals[p->nlocals] = xalloc(p->text.len + 1);
        memcpy(p->locals[p->nlocals++], p->text.p, p->text.len + 1);
        next(p);
    }
    next(p);
    skip_newlines(p);
    if(p->tok != '{') unexpected(p);
    A.fn[id].defined = 1;
    A.fn[id].nparams = p->nlocals;
    A.fn[id].entry = A.ncode;
    p->in_function = 1;
    action(p);
    emit(OP_RET0);
    p->in_function = 0;
    p->nlocals = 0;
}

static void rule(struct parser *p, struct chain *mains, int *used) {
    int to_next = -1, has_action = 0;
    section_begin(mains, used);
    A.have_main = 1;
    mark_line(p);
    if(p->tok == '{') {
        has_action = 1;
    } else {
        int skip_start = -1;
        unsigned r = 0;
        unsigned at = A.ncode;
        emit3(OP_RANGE, 0, 0);                  /* a place for it, in case this is a range; taken back if not */
        expr(p);
        if(p->tok == ',') {
            int to_body;
            r = A.nranges++;
            A.code[at + 1] = (int)r;
            skip_start = (int)at + 2;
            to_next = emit_jump(OP_JZ);
            emit3(OP_RANGESET, (int)r, 1);
            A.code[skip_start] = (int)A.ncode;
            next(p);
            skip_newlines(p);
            expr(p);
            to_body = emit_jump(OP_JZ);
            emit3(OP_RANGESET, (int)r, 0);
            patch(to_body);
        } else {
            /* OP_RANGE with a range of none never jumps, and is one no-op a record */
            A.code[at + 1] = -1;
            to_next = emit_jump(OP_JZ);
        }
        has_action = p->tok == '{';
    }
    if(has_action) {
        action(p);
    } else {
        emit3(OP_PRINT, 0, TO_STDOUT);
    }
    if(to_next >= 0) patch(to_next);
    section_end(mains);
}

void parse_program(const char *text, unsigned len, const unsigned *starts, unsigned nsources) {
    struct parser pp, *p = &pp;
    struct chain begin = { 0, 0 }, mains = { 0, 0 }, end = { 0, 0 };
    int used_begin = 0, used_main = 0, used_end = 0;
    unsigned i, halt;
    memset(p, 0, sizeof(*p));
    p->src = text;
    p->len = len;
    p->line = 1;
    p->starts = starts;
    p->nsources = nsources;
    next(p);
    for(;;) {
        while(p->tok == T_NL || p->tok == ';') next(p);
        if(p->tok == T_EOF) break;
        if(p->tok == T_FUNCTION) {
            function(p);
        } else if(p->tok == T_BEGIN || p->tok == T_END) {
            int is_begin = p->tok == T_BEGIN;
            next(p);
            if(p->tok != '{') error_at(p, "BEGIN and END need an action");
            section_begin(is_begin ? &begin : &end, is_begin ? &used_begin : &used_end);
            if(!is_begin) A.have_end = 1;
            p->in_section = 1;
            mark_line(p);
            action(p);
            p->in_section = 0;
            section_end(is_begin ? &begin : &end);
        } else {
            rule(p, &mains, &used_main);
        }
    }
    halt = A.ncode;
    emit(OP_HALT);
    A.begin_pc = used_begin ? begin.first : halt;
    A.main_pc = used_main ? mains.first : halt;
    A.end_pc = used_end ? end.first : halt;
    if(used_begin) A.code[begin.hole] = (int)halt;
    if(used_main) A.code[mains.hole] = (int)halt;
    if(used_end) A.code[end.hole] = (int)halt;
    for(i = 0; i < A.nfn; i++) {
        char what[96];
        unsigned g;
        for(g = 0; g < A.nglobal; g++) {
            if(!strcmp(A.gname[g], A.fn[i].name)) {
                p->line = A.fn[i].line ? A.fn[i].line : p->line;
                snprintf(what, sizeof(what), "%.40s is both a function and a variable", A.fn[i].name);
                error_at(p, what);
            }
        }
        if(!A.fn[i].defined) {
            p->line = A.fn[i].line;
            snprintf(what, sizeof(what), "the function %.40s is not defined", A.fn[i].name);
            error_at(p, what);
        }
        if(A.fn[i].most_args > A.fn[i].nparams) {
            p->line = A.fn[i].line;
            snprintf(what, sizeof(what), "the function %.40s takes %u arguments", A.fn[i].name, A.fn[i].nparams);
            error_at(p, what);
        }
    }
    A.range_open = xalloc(A.nranges + 1);
    memset(A.range_open, 0, A.nranges + 1);
    free(p->text.p);
    for(i = 0; i < p->caplocals; i++) free(p->locals[i]);
    free(p->locals);
}

/* no variable can have such a name */
int is_reserved(const char *s, unsigned n) {
    unsigned i;
    for(i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
        if(!strncmp(keywords[i].word, s, n) && !keywords[i].word[n]) return 1;
    for(i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if(!strncmp(builtins[i].word, s, n) && !builtins[i].word[n]) return 1;
    return 0;
}

/* The escapes of a value from the command line, as in a string. */
void parse_assign_value(const char *s, unsigned n, struct buf *to) {
    unsigned i = 0;
    buf_add(to, "", 0);
    while(i < n) {
        if(s[i] == '\\' && i + 1 < n) {
            unsigned used;
            int v = escape_byte(s + i + 1, n - i - 1, &used);
            if(v >= 0) {
                buf_ch(to, v);
                i += 1 + used;
                continue;
            }
        }
        buf_ch(to, s[i++]);
    }
}
