/*
SurfOS user program: wsheet
--------------------
File: formula.c Date: 11/05/26
--------------------
Formulas: their tokens, their program, and their text rewritten when cells move.

The language is OpenFormula's (OASIS, the OpenDocument format 1.2, part 2) as far as this sheet
goes, written the way spreadsheets show it: a formula is typed after '=', arguments are separated
by ',' (or ';', OpenFormula's own), and the decimal point is '.'. It has
- numbers (12, 1.5, .5, 2E-3), text in double quotes ("say ""hi"""), TRUE and FALSE, the errors
  (#DIV/0!, #N/A, ...);
- references: A1, $A$1, A$1, $A1, and ranges A1:B5, the corners in any order;
- the operators, from the tightest: - and + before an operand, % after one, ^, * and /, + and -,
  & (joins text), and = <> < <= > >=. All of them that take two operands group from the left,
  so 2^3^2 is 64, and the sign binds tighter than ^, so -2^2 is 4 (OpenFormula's and every
  spreadsheet's precedence);
- the functions of eval.c, by name in either case.

A name that is no function, and a call of one that is not there, is #NAME? when it runs; a formula
that does not parse is kept as typed and shows #ERROR!, and fx_check() says why. The parser is
Dijkstra's shunting-yard algorithm (1961) over a stack of its own, so no formula, however deep,
recurses in C; FX_DEPTH limits parentheses and calls inside one another anyway, as spreadsheets do.

The text kept is the one typed, with references, names and errors in capitals and ranges with
their corners in order. Copying a formula moves its relative references by as far as it moved
(FX_MOVE); inserting or deleting rows or columns moves every reference to what moved, relative or
absolute (FX_ROWS, FX_COLS): a range grows or shrinks, and what refers to cells that went, or would
go off the sheet, becomes #REF!. Only the reference tokens change; spaces and the rest stay.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sheetpriv.h"

enum { T_END, T_NUM, T_STR, T_REF, T_RANGE, T_NAME, T_ERR, T_OP, T_LP, T_RP, T_SEP, T_BAD };
enum { O_ADD = 1, O_SUB, O_MUL, O_DIV, O_POW, O_CAT, O_PCT, O_EQ, O_NE, O_LT, O_LE, O_GT, O_GE };

#define NUMBER_MAX 255                  /* a number's characters in a formula */

struct ref {
    int row, col, rabs, cabs;
};

struct tok {
    int kind, op, at, len;              /* T_OP: which; where in the text */
    struct ref a, b;                    /* T_REF: a; T_RANGE: a and b */
    int err;                            /* T_ERR */
    double num;                         /* T_NUM */
    const char *why;                    /* T_BAD */
};

static const struct fn {
    const char *name;
    unsigned char min, max, id;
} fns[] = {
    { "ABS", 1, 1, F_ABS }, { "AND", 1, FX_ARGS, F_AND }, { "AVERAGE", 1, FX_ARGS, F_AVERAGE },
    { "CONCAT", 1, FX_ARGS, F_CONCAT }, { "COUNT", 1, FX_ARGS, F_COUNT }, { "COUNTA", 1, FX_ARGS, F_COUNTA },
    { "IF", 2, 3, F_IF }, { "INT", 1, 1, F_INT }, { "LEFT", 1, 2, F_LEFT }, { "LEN", 1, 1, F_LEN },
    { "LOWER", 1, 1, F_LOWER }, { "MAX", 1, FX_ARGS, F_MAX }, { "MID", 3, 3, F_MID }, { "MIN", 1, FX_ARGS, F_MIN },
    { "MOD", 2, 2, F_MOD }, { "NOT", 1, 1, F_NOT }, { "OR", 1, FX_ARGS, F_OR }, { "POWER", 2, 2, F_POWER },
    { "RIGHT", 1, 2, F_RIGHT }, { "ROUND", 1, 2, F_ROUND }, { "SQRT", 1, 1, F_SQRT }, { "SUM", 1, FX_ARGS, F_SUM },
    { "TRIM", 1, 1, F_TRIM }, { "UPPER", 1, 1, F_UPPER },
};
#define NFNS ((int)(sizeof(fns) / sizeof(fns[0])))

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int is_alpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int fn_find(const char *name, int len, int *min, int *max) {
    int i, k;
    for(i = 0; i < NFNS; i++) {
        for(k = 0; k < len && fns[i].name[k] && upper((unsigned char)name[k]) == fns[i].name[k]; k++);
        if(k == len && !fns[i].name[k]) {
            *min = fns[i].min;
            *max = fns[i].max;
            return fns[i].id;
        }
    }
    *min = 0;
    *max = FX_ARGS;
    return F_UNKNOWN;
}

const char *fn_name(int fn) {
    int i;
    for(i = 0; i < NFNS; i++) if(fns[i].id == fn) return fns[i].name;
    return "?";
}

/* s[0..n) as a reference, $?letters$?digits, inside the sheet: 1 with *r, 0 when it is none (A0, IW1, A01) */
static int read_ref(const char *s, int n, struct ref *r) {
    int i = 0, col = 0, row = 0, letters = 0, digits = 0;
    r->cabs = r->rabs = 0;
    if(i < n && s[i] == '$') {
        r->cabs = 1;
        i++;
    }
    while(i < n && is_alpha((unsigned char)s[i])) {
        if(letters < 3) col = col * 26 + (upper((unsigned char)s[i]) - 'A' + 1);
        letters++;
        i++;
    }
    if(!letters || letters > 3) return 0;
    if(i < n && s[i] == '$') {
        r->rabs = 1;
        i++;
    }
    if(i >= n || s[i] == '0') return 0;
    while(i < n && is_digit((unsigned char)s[i])) {
        if(digits < 7) row = row * 10 + (s[i] - '0');
        digits++;
        i++;
    }
    if(!digits || digits > 7 || i != n || col > SH_COLS || row > SH_ROWS) return 0;
    r->col = col - 1;
    r->row = row - 1;
    return 1;
}

static int name_char(int c) {
    return is_alpha(c) || is_digit(c) || c == '_' || c == '.' || c == '$';
}

int sh_colname(int col, char *out) {
    int n = 0;
    if(col < 0 || col >= SH_COLS) {
        out[0] = '?';
        out[1] = 0;
        return 1;
    }
    if(col >= 26) out[n++] = (char)('A' + col / 26 - 1);
    out[n++] = (char)('A' + col % 26);
    out[n] = 0;
    return n;
}

int sh_cellname(int row, int col, char *out) {
    int n = sh_colname(col, out);
    return n + snprintf(out + n, 9, "%d", row + 1);
}

/* a cell's name with spaces around it allowed: what Go to takes */
int sh_parse_cell(const char *s, int *row, int *col) {
    struct ref r;
    int n;
    while(is_space((unsigned char)*s)) s++;
    n = (int)strlen(s);
    while(n && is_space((unsigned char)s[n - 1])) n--;
    if(!read_ref(s, n, &r)) return -1;
    *row = r.row;
    *col = r.col;
    return 0;
}

int sh_parse_range(const char *s, struct sh_range *rg) {
    struct ref a, b;
    const char *colon;
    int n;
    while(is_space((unsigned char)*s)) s++;
    n = (int)strlen(s);
    while(n && is_space((unsigned char)s[n - 1])) n--;
    colon = memchr(s, ':', (size_t)n);
    if(!colon) {
        if(!read_ref(s, n, &a)) return -1;
        b = a;
    } else if(!read_ref(s, (int)(colon - s), &a) || !read_ref(colon + 1, n - (int)(colon - s) - 1, &b)) return -1;
    rg->r1 = a.row < b.row ? a.row : b.row;
    rg->r2 = a.row < b.row ? b.row : a.row;
    rg->c1 = a.col < b.col ? a.col : b.col;
    rg->c2 = a.col < b.col ? b.col : a.col;
    return 0;
}

static int bad(struct tok *t, const char *why) {
    t->kind = T_BAD;
    t->why = why;
    return 0;
}

/* the token of s[0..len) at *pos, and *pos after it */
static void next_tok(const char *s, int len, int *pos, struct tok *t) {
    int i = *pos, j, k, c;
    while(i < len && is_space((unsigned char)s[i])) i++;
    t->at = i;
    t->op = 0;
    t->why = NULL;
    if(i >= len) {
        t->kind = T_END;
        t->len = 0;
        *pos = i;
        return;
    }
    c = (unsigned char)s[i];
    j = i + 1;
    if(is_digit(c) || (c == '.' && j < len && is_digit((unsigned char)s[j]))) {
        char buf[NUMBER_MAX + 1];
        j = i;
        while(j < len && is_digit((unsigned char)s[j])) j++;
        if(j < len && s[j] == '.') {
            j++;
            while(j < len && is_digit((unsigned char)s[j])) j++;
        }
        if(j < len && (s[j] == 'e' || s[j] == 'E')) {
            k = j + 1;
            if(k < len && (s[k] == '+' || s[k] == '-')) k++;
            if(k < len && is_digit((unsigned char)s[k])) {
                while(k < len && is_digit((unsigned char)s[k])) k++;
                j = k;
            }
        }
        t->kind = T_NUM;
        /* 2A5 is no number and a reference but a mistake: moved, it would be 2E6, one number */
        if(j < len && name_char((unsigned char)s[j])) {
            while(j < len && name_char((unsigned char)s[j])) j++;
            bad(t, "a number runs into a name");
        } else if(j - i > NUMBER_MAX) bad(t, "a number is too long");
        else {
            memcpy(buf, s + i, (size_t)(j - i));
            buf[j - i] = 0;
            t->num = strtod(buf, NULL);
            if(!isfinite(t->num)) bad(t, "a number is too large");
        }
    } else if(c == '"') {
        for(;;) {
            if(j >= len) {
                bad(t, "a text has no closing \"");
                break;
            }
            if(s[j] == '"') {
                if(j + 1 < len && s[j + 1] == '"') {
                    j += 2;
                    continue;
                }
                j++;
                t->kind = T_STR;
                break;
            }
            j++;
        }
    } else if(c == '#') {
        for(k = 1; k <= SH_ERRORS; k++) {
            const char *name = sh_error_name(k);
            int n = (int)strlen(name);
            if(len - i >= n && !strncasecmp(s + i, name, (size_t)n)) break;
        }
        if(k > SH_ERRORS) bad(t, "# begins no error that a sheet knows");
        else {
            t->kind = T_ERR;
            t->err = k;
            j = i + (int)strlen(sh_error_name(k));
        }
    } else if(is_alpha(c) || c == '$' || c == '_') {
        while(j < len && name_char((unsigned char)s[j])) j++;
        if(read_ref(s + i, j - i, &t->a)) {
            t->kind = T_REF;
            if(j < len && s[j] == ':') {
                k = j + 1;
                while(k < len && name_char((unsigned char)s[k])) k++;
                if(read_ref(s + j + 1, k - j - 1, &t->b)) {
                    t->kind = T_RANGE;
                    j = k;
                } else {
                    bad(t, "a range needs a cell after its :");
                    j = k;
                }
            }
        } else if(memchr(s + i, '$', (size_t)(j - i))) bad(t, "$ belongs in a reference");
        else t->kind = T_NAME;
    } else {
        t->kind = T_OP;
        switch(c) {
        case '+': t->op = O_ADD; break;
        case '-': t->op = O_SUB; break;
        case '*': t->op = O_MUL; break;
        case '/': t->op = O_DIV; break;
        case '^': t->op = O_POW; break;
        case '&': t->op = O_CAT; break;
        case '%': t->op = O_PCT; break;
        case '=': t->op = O_EQ; break;
        case '<':
            t->op = O_LT;
            if(j < len && s[j] == '=') { t->op = O_LE; j++; }
            else if(j < len && s[j] == '>') { t->op = O_NE; j++; }
            break;
        case '>':
            t->op = O_GT;
            if(j < len && s[j] == '=') { t->op = O_GE; j++; }
            break;
        case '(': t->kind = T_LP; break;
        case ')': t->kind = T_RP; break;
        case ',': case ';': t->kind = T_SEP; break;
        default: bad(t, "a character a formula does not use"); break;
        }
    }
    t->len = j - t->at;
    *pos = j;
}

/**** the program ****/

static int prec(int op) {
    switch(op) {
    case O_EQ: case O_NE: case O_LT: case O_LE: case O_GT: case O_GE: return 1;
    case O_CAT: return 2;
    case O_ADD: case O_SUB: return 3;
    case O_MUL: case O_DIV: return 4;
    case O_POW: return 5;
    }
    return 0;
}

static int op_ins(int op) {
    static const unsigned char map[] = { 0, I_ADD, I_SUB, I_MUL, I_DIV, I_POW, I_CAT, I_PCT, I_EQ, I_NE, I_LT, I_LE,
                                         I_GT, I_GE };
    return map[op];
}

enum { E_OP, E_UNARY, E_LP, E_FN };

struct pending {                        /* the parser's stack: an operator, a ( or a function's ( */
    unsigned char kind, op;
    unsigned short fn;
    int args, at, min, max, name, namelen;
};

struct parse {
    const char *s;
    int len;
    struct ins *code;
    int n;
    char *pool;
    int npool;
    struct pending *st;
    int sp, depth;
    const char *why;
    int why_at;
    int unknown, unknown_len, unknown_fn;   /* the first name that is nothing, for fx_check() */
    int argmsg;                         /* why says which function: its token */
    char fnwhy[64];
};

static int fail(struct parse *p, const char *why, int at) {
    if(!p->why) {
        p->why = why;
        p->why_at = at;
    }
    return -1;
}

static struct ins *emit(struct parse *p, int op) {
    struct ins *in = &p->code[p->n++];
    memset(in, 0, sizeof(*in));
    in->op = (unsigned char)op;
    return in;
}

static void pop_emit(struct parse *p) {
    struct pending *e = &p->st[--p->sp];
    if(e->kind == E_UNARY) emit(p, e->op == O_SUB ? I_NEG : I_PLUS);
    else emit(p, op_ins(e->op));
}

/* a string's text without its quotes, "" as ", into the pool */
static void add_string(struct parse *p, const struct tok *t, struct ins *in) {
    int i, start = p->npool;
    for(i = t->at + 1; i < t->at + t->len - 1; i++) {
        p->pool[p->npool++] = p->s[i];
        if(p->s[i] == '"') i++;
    }
    in->r1 = start;
    in->r2 = p->npool - start;
}

/* a range's corners in order, top left first; of two equal rows (or columns) the absolute one first, so that a range
   has one text */
static void order(struct ref *a, struct ref *b) {
    int t;
    if(a->row > b->row || (a->row == b->row && a->rabs < b->rabs)) {
        t = a->row; a->row = b->row; b->row = t;
        t = a->rabs; a->rabs = b->rabs; b->rabs = t;
    }
    if(a->col > b->col || (a->col == b->col && a->cabs < b->cabs)) {
        t = a->col; a->col = b->col; b->col = t;
        t = a->cabs; a->cabs = b->cabs; b->cabs = t;
    }
}

/* an operand's instruction */
static void operand(struct parse *p, struct tok *t) {
    struct ins *in;
    switch(t->kind) {
    case T_NUM:
        in = emit(p, I_NUM);
        in->num = t->num;
        break;
    case T_STR:
        in = emit(p, I_STR);
        add_string(p, t, in);
        break;
    case T_ERR:
        in = emit(p, I_ERR);
        in->fn = (unsigned short)t->err;
        break;
    case T_REF:
        in = emit(p, I_REF);
        in->r1 = in->r2 = t->a.row;
        in->c1 = in->c2 = t->a.col;
        break;
    case T_RANGE:
        order(&t->a, &t->b);
        in = emit(p, I_RANGE);
        in->r1 = t->a.row;
        in->c1 = t->a.col;
        in->r2 = t->b.row;
        in->c2 = t->b.col;
        break;
    }
}

static int is_word(const struct parse *p, const struct tok *t, const char *w) {
    return t->len == (int)strlen(w) && !strncasecmp(p->s + t->at, w, (size_t)t->len);
}

/* the tokens of p->s into p->code; 0, or -1 with p->why */
static int parse(struct parse *p) {
    struct tok t, look;
    int pos = 0, expect = 1, any = 0;
    struct pending *e;
    for(;;) {
        next_tok(p->s, p->len, &pos, &t);
        if(t.kind == T_END) break;
        any = 1;
        switch(t.kind) {
        case T_BAD:
            return fail(p, t.why, t.at);
        case T_NUM: case T_STR: case T_ERR: case T_REF: case T_RANGE:
            if(!expect) return fail(p, "an operator is missing", t.at);
            operand(p, &t);
            expect = 0;
            break;
        case T_NAME:
            if(!expect) return fail(p, "an operator is missing", t.at);
            {
                int at = pos;
                next_tok(p->s, p->len, &at, &look);
                if(look.kind == T_LP) {         /* a call */
                    if(++p->depth > FX_DEPTH) return fail(p, "parentheses and calls go too deep", t.at);
                    e = &p->st[p->sp++];
                    e->kind = E_FN;
                    e->fn = (unsigned short)fn_find(p->s + t.at, t.len, &e->min, &e->max);
                    e->args = 0;
                    e->at = look.at;            /* its (, for "a ( has no )" */
                    e->name = t.at;
                    e->namelen = t.len;
                    if(e->fn == F_UNKNOWN && p->unknown < 0) {
                        p->unknown = t.at;
                        p->unknown_len = t.len;
                        p->unknown_fn = 1;
                    }
                    pos = at;
                    break;                  /* expect stays 1 */
                }
            }
            if(is_word(p, &t, "TRUE") || is_word(p, &t, "FALSE"))
                emit(p, I_BOOL)->fn = (unsigned short)is_word(p, &t, "TRUE");
            else {
                emit(p, I_ERR)->fn = SH_NAME;
                if(p->unknown < 0) {
                    p->unknown = t.at;
                    p->unknown_len = t.len;
                    p->unknown_fn = 0;
                }
            }
            expect = 0;
            break;
        case T_LP:
            if(!expect) return fail(p, "an operator is missing", t.at);
            if(++p->depth > FX_DEPTH) return fail(p, "parentheses and calls go too deep", t.at);
            e = &p->st[p->sp++];
            e->kind = E_LP;
            e->at = t.at;
            break;
        case T_RP:
            if(expect && !(p->sp && p->st[p->sp - 1].kind == E_FN && p->st[p->sp - 1].args == 0))
                return fail(p, "something is missing before )", t.at);
            while(p->sp && p->st[p->sp - 1].kind != E_LP && p->st[p->sp - 1].kind != E_FN) pop_emit(p);
            if(!p->sp) return fail(p, "a ) has no (", t.at);
            e = &p->st[--p->sp];
            p->depth--;
            if(e->kind == E_FN) {
                struct ins *in;
                int args = expect ? 0 : e->args + 1;
                if(args < e->min || args > e->max) {
                    if(e->min == e->max)
                        snprintf(p->fnwhy, sizeof(p->fnwhy), "%s takes %d argument%s", fn_name(e->fn), e->min,
                                 e->min == 1 ? "" : "s");
                    else if(args < e->min)
                        snprintf(p->fnwhy, sizeof(p->fnwhy), "%s takes at least %d argument%s", fn_name(e->fn),
                                 e->min, e->min == 1 ? "" : "s");
                    else snprintf(p->fnwhy, sizeof(p->fnwhy), "%s takes at most %d arguments", fn_name(e->fn), e->max);
                    return fail(p, p->fnwhy, e->name);
                }
                in = emit(p, I_CALL);
                in->fn = e->fn;
                in->argc = (unsigned char)args;
            }
            expect = 0;
            break;
        case T_SEP:
            if(expect) return fail(p, "an argument is missing", t.at);
            while(p->sp && p->st[p->sp - 1].kind != E_LP && p->st[p->sp - 1].kind != E_FN) pop_emit(p);
            if(!p->sp || p->st[p->sp - 1].kind != E_FN) return fail(p, "a , outside a function's ( )", t.at);
            if(++p->st[p->sp - 1].args >= FX_ARGS) return fail(p, "a function takes at most 255 arguments", t.at);
            expect = 1;
            break;
        case T_OP:
            if(t.op == O_PCT) {
                if(expect) return fail(p, "% needs a number before it", t.at);
                while(p->sp && p->st[p->sp - 1].kind == E_UNARY) pop_emit(p);
                emit(p, I_PCT);
                break;
            }
            if(expect) {
                if(t.op != O_ADD && t.op != O_SUB) return fail(p, "an operand is missing", t.at);
                e = &p->st[p->sp++];
                e->kind = E_UNARY;
                e->op = (unsigned char)t.op;
                e->at = t.at;
                break;
            }
            while(p->sp && (p->st[p->sp - 1].kind == E_UNARY ||
                            (p->st[p->sp - 1].kind == E_OP && prec(p->st[p->sp - 1].op) >= prec(t.op)))) pop_emit(p);
            e = &p->st[p->sp++];
            e->kind = E_OP;
            e->op = (unsigned char)t.op;
            e->at = t.at;
            expect = 1;
            break;
        }
    }
    if(!any) return fail(p, "the formula is empty", 0);
    if(expect) return fail(p, "the formula ends too soon", p->len);
    while(p->sp) {
        if(p->st[p->sp - 1].kind == E_LP || p->st[p->sp - 1].kind == E_FN)
            return fail(p, "a ( has no )", p->st[p->sp - 1].at);
        pop_emit(p);
    }
    return 0;
}

/* tokens in s, at least: what the arrays for one formula need */
static int count_tokens(const char *s, int len) {
    struct tok t;
    int pos = 0, n = 0;
    do {
        next_tok(s, len, &pos, &t);
        n++;
    } while(t.kind != T_END && t.kind != T_BAD);
    return n;
}

static int stack_depth(const struct ins *code, int n) {
    int i, d = 0, most = 0;
    for(i = 0; i < n; i++) {
        switch(code[i].op) {
        case I_NUM: case I_STR: case I_BOOL: case I_ERR: case I_REF: case I_RANGE: d++; break;
        case I_NEG: case I_PLUS: case I_PCT: break;
        case I_CALL: d += 1 - code[i].argc; break;
        default: d--; break;
        }
        if(d > most) most = d;
    }
    return most;
}

/* parse src into p, with arrays of its own: 0, -1 (p->why), or -2 when there is no memory */
static int parse_setup(struct parse *p, const char *src, int len) {
    int n = count_tokens(src, len) + 1;
    memset(p, 0, sizeof(*p));
    p->s = src;
    p->len = len;
    p->unknown = -1;
    p->code = mem_alloc((size_t)n * sizeof(struct ins));
    p->st = mem_alloc((size_t)n * sizeof(struct pending));
    p->pool = mem_alloc((size_t)len + 1);
    if(!p->code || !p->st || !p->pool) return -2;
    return parse(p);
}

static void parse_done(struct parse *p) {
    mem_free(p->code);
    mem_free(p->st);
    mem_free(p->pool);
}

int fx_compile(struct formula *f) {
    struct parse p;
    int r = parse_setup(&p, f->src, f->len);
    mem_free(f->code);
    f->code = NULL;
    f->ncode = f->depth = 0;
    if(r == 0) {
        f->code = mem_alloc((size_t)p.n * sizeof(struct ins) + (size_t)p.npool + 1);
        if(!f->code) r = -2;
        else {
            memcpy(f->code, p.code, (size_t)p.n * sizeof(struct ins));
            f->ncode = p.n;
            memcpy(FORMULA_POOL(f), p.pool, (size_t)p.npool);
            FORMULA_POOL(f)[p.npool] = 0;
            f->depth = stack_depth(f->code, f->ncode);
        }
    }
    parse_done(&p);
    return r;
}

int fx_check(const char *src, int len, char *why, int size) {
    struct parse p;
    int r = parse_setup(&p, src, len);
    if(r == -2) snprintf(why, (size_t)size, "Not enough memory to read the formula");
    else if(r < 0) snprintf(why, (size_t)size, "%s (at character %d)", p.why, p.why_at + 2);   /* the = counted */
    else if(p.unknown >= 0)
        snprintf(why, (size_t)size,
                 p.unknown_fn ? "%.*s is no function of this sheet" : "%.*s is no name of this sheet",
                 p.unknown_len > 40 ? 40 : p.unknown_len, src + p.unknown);
    parse_done(&p);
    return r != 0 || p.unknown >= 0;
}

/**** rewriting ****/

struct out {
    char *s;
    int size, len;
};

static void put(struct out *o, const char *s, int n) {
    if(o->len + n < o->size) memcpy(o->s + o->len, s, (size_t)n);
    o->len += n;
}

static void put_ref(struct out *o, const struct ref *r) {
    char buf[16];
    int n = 0;
    if(r->cabs) buf[n++] = '$';
    n += sh_colname(r->col, buf + n);
    if(r->rabs) buf[n++] = '$';
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%d", r->row + 1);
    put(o, buf, n);
}

/* v after the rows or columns from at moved by n (n < 0: -n of them went): -1 when it went or left the sheet */
static int moved(int v, int at, int n, int limit) {
    if(n >= 0) v = v >= at ? v + n : v;
    else if(v >= at) v = v < at - n ? -1 : v + n;
    return v >= limit ? -1 : v;
}

/* a range's two ends along the axis that moved; 0 when nothing of it is left */
static int moved_span(int *lo, int *hi, int at, int n, int limit) {
    if(n >= 0) {
        if(*lo >= at) *lo += n;
        if(*hi >= at) *hi += n;
        if(*lo >= limit) return 0;
        if(*hi >= limit) *hi = limit - 1;           /* the range's end stays at the sheet's */
        return 1;
    }
    if(*lo >= at) *lo = *lo < at - n ? at : *lo + n;
    if(*hi >= at) *hi = *hi < at - n ? at - 1 : *hi + n;
    return *lo <= *hi;
}

static struct { int r1, c1, r2, c2; } cut;    /* FX_CUT's rectangle */

void fx_cut_box(int r1, int c1, int r2, int c2) {
    cut.r1 = r1;
    cut.c1 = c1;
    cut.r2 = r2;
    cut.c2 = c2;
}

static int in_cut(const struct ref *r) {
    return r->row >= cut.r1 && r->row <= cut.r2 && r->col >= cut.c1 && r->col <= cut.c2;
}

/* one reference after how: 0 when it is #REF! now */
static int move_ref(struct ref *r, int how, int a, int b) {
    switch(how) {
    case FX_CUT:                                /* what was cut moves, $ or not; the rest stays where it is */
        if(!in_cut(r)) return 1;
        r->row += a;
        r->col += b;
        return r->row >= 0 && r->row < SH_ROWS && r->col >= 0 && r->col < SH_COLS;
    case FX_MOVE:
        if(!r->rabs) r->row += a;
        if(!r->cabs) r->col += b;
        return r->row >= 0 && r->row < SH_ROWS && r->col >= 0 && r->col < SH_COLS;
    case FX_ROWS:
        r->row = moved(r->row, a, b, SH_ROWS);
        return r->row >= 0;
    case FX_COLS:
        r->col = moved(r->col, a, b, SH_COLS);
        return r->col >= 0;
    }
    return 1;
}

static int move_range(struct ref *x, struct ref *y, int how, int a, int b) {
    switch(how) {
    case FX_CUT:                                /* a range moves when all of it was cut */
        if(in_cut(x) && in_cut(y) && (!move_ref(x, how, a, b) || !move_ref(y, how, a, b))) return 0;
        break;
    case FX_MOVE:
        if(!move_ref(x, how, a, b) || !move_ref(y, how, a, b)) return 0;
        break;
    case FX_ROWS:
        if(!moved_span(&x->row, &y->row, a, b, SH_ROWS)) return 0;
        break;
    case FX_COLS:
        if(!moved_span(&x->col, &y->col, a, b, SH_COLS)) return 0;
        break;
    }
    order(x, y);
    return 1;
}

int fx_rewrite(const char *src, int len, int how, int a, int b, char *outbuf, int size) {
    struct out o = { outbuf, size, 0 };
    struct tok t;
    int pos = 0, last = 0, i;
    for(;;) {
        next_tok(src, len, &pos, &t);
        if(t.kind == T_BAD) break;
        put(&o, src + last, t.at - last);               /* the spaces before it, as they were */
        if(t.kind == T_END) break;
        switch(t.kind) {
        case T_REF:
            if(move_ref(&t.a, how, a, b)) put_ref(&o, &t.a);
            else put(&o, "#REF!", 5);
            break;
        case T_RANGE:
            order(&t.a, &t.b);
            if(move_range(&t.a, &t.b, how, a, b)) {
                put_ref(&o, &t.a);
                put(&o, ":", 1);
                put_ref(&o, &t.b);
            } else put(&o, "#REF!", 5);
            break;
        case T_NAME:
            for(i = 0; i < t.len; i++) {
                char c = (char)upper((unsigned char)src[t.at + i]);
                put(&o, &c, 1);
            }
            break;
        case T_ERR:
            put(&o, sh_error_name(t.err), (int)strlen(sh_error_name(t.err)));
            break;
        default:
            put(&o, src + t.at, t.len);
            break;
        }
        last = pos;
    }
    if(t.kind == T_BAD || o.len >= size) {              /* as it was: the caller keeps src */
        if(len < size) {
            memcpy(outbuf, src, (size_t)len);
            outbuf[len] = 0;
        }
        return t.kind == T_BAD ? -1 : -2;
    }
    outbuf[o.len] = 0;
    return o.len;
}
