/*
SurfOS user program: bc
--------------------
File: bc.h      Date: 11/01/26
--------------------
What the files of bc share. parse.c turns the text into code for a small stack machine as it
reads, without a tree; vm.c runs it. Neither makes a system call: main.c hands them a struct
bc_host and a way to read a character, and tests/host/test_bc.c hands them its own.

Input is compiled and run one item at a time (the statements up to a newline, or a function),
as POSIX asks of a terminal, so a script of any size takes the memory of its longest item. Names
are the same three things in every scope: a variable, an array and a function of one name do not
meet. Nothing in the machine recurses on the C stack: calls keep their frames on the heap, so a
bc function can recurse until memory runs out and is told so. The parser recurses, up to
BC_DEPTH_MAX, and every list it keeps grows with a checked realloc().
*/

#ifndef _BC_BC_H
#define _BC_BC_H

#include "num.h"

#define BC_DIM_MAX   16777216u          /* indexes below this */
#define BC_DEPTH_MAX 1000               /* brackets and blocks inside each other */
#define BC_LINE_CHARS 68                /* of a number on a line, ahead of the backslash and the newline */

/* what bc_run() ends with */
enum { BC_END, BC_QUIT, BC_TROUBLE };

struct bc_host {
    void (*put)(void *ctx, int c);                      /* standard output */
    void (*flush)(void *ctx);                           /* what was put should show now */
    void (*error)(void *ctx, const char *text);         /* one message, without "bc: " or a newline */
    void *ctx;
};

struct bc;

struct bc *bc_new(const struct bc_host *host);          /* NULL when memory runs out */
void bc_free(struct bc *b);
int bc_library(struct bc *b);                           /* -l: BC_END, or BC_TROUBLE (said) */

/* Reads get() to its end (EOF, a negative number) or to quit, running each item as it is
   complete. name is for the messages. interactive: a mistake costs its line, and the reading
   goes on; otherwise it ends the run with BC_TROUBLE. */
int bc_run(struct bc *b, int (*get)(void *), void *in, const char *name, int interactive);

/* for the tests: this many jumps and calls, then a run stops with an error; 0, no limit */
void bc_set_budget(struct bc *b, unsigned long steps);

/**** inside bc: parse.c and vm.c ****/

enum {
    OP_END,                             /* the end of an item's code */
    OP_CONST,                           /* k: the constant, in ibase */
    OP_ZERO, OP_ONE,
    OP_LOADV, OP_STOREV,                /* s: the variable; a store leaves its value */
    OP_LOADA, OP_STOREA,                /* s: the array: [index] to [value]; [index value] to [value] */
    OP_LOADR, OP_STORER,                /* r: scale, ibase or obase */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_NEG, OP_NOT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_LENGTH, OP_SCALEOF, OP_SQRT,
    OP_JMP, OP_JZ, OP_JNZ,              /* a: to; the last two take a value off the stack */
    OP_POP, OP_DUP,
    OP_TUCK,                            /* [a b] to [b a b] */
    OP_PRINT,                           /* a value and its newline */
    OP_STR,                             /* t: the text, as it is */
    OP_CALL,                            /* s n a1..an: an is 0 for a value on the stack, or 1 + the array's symbol */
    OP_RET,                             /* the value on the stack is the result */
    OP_RET0,                            /* the result is 0 */
    OP_FAIL                             /* t: the library's own error */
};

enum { REG_SCALE, REG_IBASE, REG_OBASE };

/* A constant keeps its text: its value depends on ibase, when it is run. The value last made
   is kept, so that a loop does not convert it each time round. */
struct konst {
    char *text;
    unsigned len;
    unsigned base;                      /* of value; 0, none yet */
    struct num *value;
};

struct local {
    unsigned sym;
    int array;
};

struct func {
    int *code;
    unsigned ncode, capcode;
    struct konst *k;
    unsigned nk, capk;
    char **text;
    unsigned ntext, captext;
    struct local *loc;                  /* the parameters, then the autos */
    unsigned nparams, nloc, caploc;
    unsigned sym;                       /* its name */
    int lib;                            /* -l's: constants in base 10, scale put back on return */
};

struct array {                          /* two levels, so that a[1000000] = 1 costs a page or two */
    unsigned ntop;
    struct num ***top;
};

struct sym {
    char *name;
    unsigned next;                      /* the hash chain: 1 + the next symbol, 0 at the end */
    struct num *var;                    /* NULL is zero */
    struct array *arr;                  /* NULL is empty */
    struct func *fn;
};

union saved {                           /* a name's value from before a call */
    struct num *n;
    struct array *a;
};

struct frame {                          /* a call: where it came from */
    struct func *caller;
    unsigned pc;
    unsigned scale;
};

#define BC_HASH 251

struct bc {
    struct bc_host host;
    struct sym *sym;
    unsigned nsym, capsym;
    unsigned hash[BC_HASH];             /* 1 + the first symbol of each chain */
    unsigned scale, ibase, obase;
    struct num **stack;
    unsigned sp, capstack;
    struct frame *frames;
    unsigned nframes, capframes;
    union saved *saved;
    unsigned nsaved, capsaved;
    struct func main;                   /* the item being run */
    struct num *zero, *one;
    unsigned col;                       /* characters on the line so far, for numbers' line breaks */
    int held;                           /* a number's character that waits for the next: put_number_char() */
    unsigned long budget;
    const char *name;                   /* the input, for messages */
    unsigned line;
};

int bc_intern(struct bc *b, const char *name, unsigned len);        /* the symbol, or -1 */
void bc_func_clear(struct func *f);
void bc_func_free(struct func *f);
int bc_exec(struct bc *b, struct func *f);                          /* 0, or -1 (said) */
void bc_say(struct bc *b, const char *what);                        /* with the input's name and line */

extern const char bc_library_text[];

#endif
