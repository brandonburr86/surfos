/*
SurfOS user program: wsheet
--------------------
File: sheetpriv.h  Date: 11/05/26
--------------------
What the model's files (cells.c, number.c, formula.c, eval.c, recalc.c, edit.c, files.c) share
and main.c never sees.

The sheet is a sorted array of rows, each a sorted array of its cells: a cell is found by two
binary searches, a range is walked row by row, left to right, and a whole row or column moves by
renumbering. A cell's content is what was typed into it and its format; a formula's content
holds its text, its program and the value the last recalculation gave it. A change of the
history swaps contents: the step keeps the one that is not in the cell, so that Undo and Redo
are the same swap again (edit.c).
*/
#ifndef _WSHEET_SHEETPRIV_H
#define _WSHEET_SHEETPRIV_H

#include <stddef.h>
#include "sheet.h"

/* what a cell holds as typed; K_NONE with SH_GENERAL is no cell at all */
enum { K_NONE, K_NUMBER, K_TEXT, K_BOOL, K_ERROR, K_FORMULA };

struct formula;

struct content {
    unsigned char kind, fmt;
    unsigned char b;                    /* K_BOOL: 0 or 1; K_ERROR: the error */
    int len;                            /* K_TEXT: the text's bytes */
    double num;                         /* K_NUMBER */
    char *text;                         /* K_TEXT: len bytes and a NUL */
    struct formula *f;                  /* K_FORMULA */
};

struct cell {
    int col;
    struct content c;
};

struct row {
    int r;
    int n, cap;                         /* cells, by column */
    int formulas;                       /* how many of them are formulas: recalc.c skips a row without */
    struct cell *cells;
};

/* a formula's program: postfix, run on a stack by eval.c */
enum { I_NUM, I_STR, I_BOOL, I_ERR, I_REF, I_RANGE, I_NEG, I_PLUS, I_PCT, I_ADD, I_SUB, I_MUL, I_DIV, I_POW, I_CAT,
       I_EQ, I_NE, I_LT, I_LE, I_GT, I_GE, I_CALL };

struct ins {
    unsigned char op, argc;             /* I_CALL: its arguments */
    unsigned short fn;                  /* I_CALL: the function (F_*); I_ERR, I_BOOL: the error or the truth */
    int r1, c1, r2, c2;                 /* I_REF (r1, c1), I_RANGE; I_STR: offset and length in the pool */
    double num;                         /* I_NUM */
};

/* the functions; F_UNKNOWN is a name that is none of them, whose value is #NAME? */
enum { F_UNKNOWN, F_ABS, F_AND, F_AVERAGE, F_CONCAT, F_COUNT, F_COUNTA, F_IF, F_INT, F_LEFT, F_LEN, F_LOWER, F_MAX,
       F_MID, F_MIN, F_MOD, F_NOT, F_OR, F_POWER, F_RIGHT, F_ROUND, F_SQRT, F_SUM, F_TRIM, F_UPPER };

struct formula {
    char *src;                          /* the formula after its '=', as kept: refs and names in capitals */
    int len;
    struct ins *code;                   /* NULL when it does not parse: its value is #ERROR! */
    int ncode, depth;                   /* instructions, and the stack they need; the strings follow them */
    /* the value of the last recalculation */
    unsigned char vtype, vb;
    int vlen;
    double vnum;
    char *vtext;
    /* the recalculation's bookkeeping (recalc.c) */
    int index, low;
    unsigned char mark;
};
#define FORMULA_POOL(f) ((char *)((f)->code + (f)->ncode))

/* a step of the history (edit.c) */
struct change {
    int row, col;
    struct content other;               /* what the cell does not hold now */
};
struct wchange {
    int col, other;                     /* a column's width, likewise */
};
struct step;

struct sheet {
    struct row *rows;
    int nrows, caprows;
    int ncells, nformulas;
    unsigned short width[SH_COLS];
    struct step **hist;                 /* the history: hist[0..pos) are done, hist[pos..nhist) undone */
    int nhist, caphist, pos;
    long hist_bytes;
    unsigned state, saved, serials;     /* the state's serial, the one the file holds, the last given */
    int limited;                        /* the last recalculation stopped at a limit */
    char why[SH_WHY];
};

/* cells.c: memory, counted against sh_mem_max */
void *mem_alloc(size_t n);
void *mem_zalloc(size_t n);
void *mem_realloc(void *p, size_t n);
void mem_free(void *p);
char *mem_dup(const char *s, int len);  /* len bytes and a NUL */

/* cells.c: finding, walking, contents */
int row_index(const struct sheet *s, int r, int *found);       /* the row's index, or where it would go */
int cell_index(const struct row *rw, int c, int *found);
struct cell *cell_at(const struct sheet *s, int r, int c);
struct walk {
    int r1, c1, r2, c2;
    int ri, ci;                         /* the row's index, the cell's in it */
    int formulas;                       /* only formulas */
};
void walk_start(const struct sheet *s, struct walk *w, int r1, int c1, int r2, int c2, int formulas);
struct cell *walk_next(const struct sheet *s, struct walk *w, int *row, long *steps);
int content_absent(const struct content *c);
void content_free(struct content *c);
int content_copy(struct content *dst, const struct content *src);      /* 0, or -1: no memory */
int content_equal(const struct content *a, const struct content *b);
/* what text typed with a format makes; 0, or -1 when there is no memory */
int content_parse(struct content *c, const char *text, int len, int fmt);
int content_formula(struct content *c, const char *src, int len, int fmt);     /* a formula's text, no '=' */
int content_input(const struct content *c, char *out, int size);
int text_needs_quote(const char *t, int len);
void cell_value(const struct sheet *s, const struct cell *c, struct sh_value *v);
/* the cell at r, c takes other's content and other the cell's: 0, or -1 (no memory to make the cell) */
int cell_swap(struct sheet *s, int r, int c, struct content *other);
void content_forget_value(struct content *c);
void drop_empty_rows(struct sheet *s);
void sheet_set_why(struct sheet *s, const char *fmt, ...);
/* a sheet read from a file (files.c): cells in any order, sorted at the end */
struct build;
enum { BUILD_NOMEM = -1, BUILD_FULL = -2, BUILD_TWICE = -3 };
struct build *build_new(void);
int build_add(struct build *b, int r, int c, struct content *ct);       /* takes ct: 0 or BUILD_* */
int build_finish(struct build *b, struct sheet *s, char *why, int size);       /* into the empty s: 0, or -1 */
void build_free(struct build *b);
void shift_rows(struct sheet *s, int at, int n);        /* rows from at move by n; with n < 0, -n of them go */
void shift_cols(struct sheet *s, int at, int n);

/* number.c */
int num_read(const char *s, int len, double *v, int *pct);     /* a number as typed; *pct: its decimals + 1 when % */
int num_read_json(const char *s, int len, double *v);          /* a number as RFC 8259 writes one (CSV) */
int num_shortest(double x, char *out, int size);               /* the fewest digits that read back as x */
int num_shortest_percent(double x, char *out, int size);       /* x as a percent, likewise, with its % */
int num_general(double x, int chars, char *out, int size);     /* General in chars characters (0: no limit) */
int num_fixed(double x, int decimals, int percent, char *out, int size);
double num_round(double x, int digits);                        /* ROUND(): a half away from 0, at 15 digits */
int num_equal(double a, double b);                             /* the same to 15 significant digits */
int num_digits15(double x, char *dig, int *e);                 /* x's 15 digits; x = d.ddd * 10^e */

/* formula.c */
#define FX_DEPTH 64                     /* parentheses and calls inside one another */
#define FX_ARGS 255                     /* a function's arguments */
int fx_compile(struct formula *f);      /* f->src into f->code: 0, -1 when it does not parse, -2 no memory */
int fx_check(const char *src, int len, char *why, int size);   /* 0, or 1 with why */
enum { FX_NORMAL, FX_MOVE, FX_ROWS, FX_COLS };
/* the formula's text again: FX_NORMAL with refs and names in capitals; FX_MOVE with its relative references
   moved by a rows and b columns; FX_ROWS or FX_COLS after rows or columns from a moved by b (b < 0: -b of them
   went). Its length; -1 when it does not even split into tokens, and stays as it is; -2 when it would not fit in
   size (out is src then, when that fits) */
int fx_rewrite(const char *src, int len, int how, int a, int b, char *out, int size);
int fn_find(const char *name, int len, int *min, int *max);   /* F_*, or F_UNKNOWN */
const char *fn_name(int fn);

/* eval.c */
/* the formula's value into f; *steps counts the work, and past sh_steps_max it stops: 0, or -1 for that */
int ev_formula(const struct sheet *s, struct formula *f, long *steps);
void ev_set_error(struct formula *f, int e);
int u8_chars(const char *s, int len);   /* characters of UTF-8 text, a byte that is no UTF-8 one each */
int text_fold_compare(const char *a, int alen, const char *b, int blen);  /* either case */

/* recalc.c */
void recalc(struct sheet *s);

/* edit.c */
void hist_free(struct sheet *s);

/* files.c: delimited text (RFC 4180's CSV, and tab-separated text pasted), fed a piece at a time: each field goes to
   cell(), empty ones too, which returns 0 or -1 to stop */
struct dsv {
    int sep, state, failed;
    char *field;                        /* SH_INPUT_MAX bytes and a NUL */
    int len;
    int row, col;                       /* where the next field goes */
    int rows, cols;                     /* records ended, and the most fields of one */
    const char *why;
    int (*cell)(void *ctx, int row, int col, const char *text, int len);
    void *ctx;
};
int dsv_init(struct dsv *d, int sep, int (*cell)(void *ctx, int row, int col, const char *text, int len), void *ctx);
int dsv_feed(struct dsv *d, const char *bytes, int n);         /* 0, or -1 with d->why */
int dsv_end(struct dsv *d);
void dsv_free(struct dsv *d);

#endif
