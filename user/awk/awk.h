/*
SurfOS user program: awk
--------------------
File: awk.h     Date: 11/01/26
--------------------
What the files of awk share. parse.c turns the program into code for a small stack machine, in
one pass, without a tree; vm.c runs it. value.c has the strings, numbers and arrays, field.c the
record and its fields, io.c the files and commands, fmt.c printf, func.c the regular expressions
and the built-in functions, main.c the command line.

There is one program in a process, so the state is one global struct. A mistake at run time ends
awk, as in every awk, so fatal() prints and exits, and nothing below it has to unwind: memory
that runs out is such a mistake. Nothing recurses on the C stack but the parser, up to
AWK_DEPTH_MAX: a function's calls live on heap stacks, as deep as memory goes.
*/

#ifndef _AWK_H
#define _AWK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <regex.h>

#define AWK_DEPTH_MAX 1000              /* brackets and blocks inside each other */
#define NOT_COUNTED 0xffffffffu

struct str {                            /* never changes once made, and is shared by counting */
    unsigned refs;
    unsigned len;                       /* bytes; it may hold NULs */
    unsigned chars;                     /* characters, once counted */
    unsigned *marks;                    /* once it is looked far into, where characters start: char_at() */
    char s[];                           /* with a NUL after the last, for the C library */
};

struct array;
struct iter;
struct regex;

/* A value from input is V_INPUT, a string that counts as a number where it looks like one. */
enum { V_UNSET, V_NUM, V_STR, V_INPUT, V_ARRAY, V_REF, V_REGEX, V_ITER };

struct value {
    int kind;
    union {
        double num;
        struct str *s;
        struct array *a;
        struct iter *it;
        const struct regex *re;
        int ref;                        /* V_REF: an unset name passed down, 0 up a stack slot, below 0 ~global */
    } u;
};

/* the variables awk itself uses have these numbers among the globals */
enum { S_NF, S_NR, S_FNR, S_FS, S_OFS, S_ORS, S_RS, S_FILENAME, S_SUBSEP, S_RSTART, S_RLENGTH, S_CONVFMT, S_OFMT,
       S_ENVIRON, S_ARGC, S_ARGV, S_SPECIALS };

/* where an lvalue is; PL_ARRAY has [array key] on the stack, PL_FIELD [index] */
enum { PL_VAR, PL_LOCAL, PL_ARRAY, PL_FIELD, PL_NF, PL_RECORD };

enum {
    OP_HALT,                            /* the end of a BEGIN, the rules or END */
    OP_NUM, OP_STR,                     /* k: a constant */
    OP_RE,                              /* k: $0 ~ the regular expression */
    OP_RELIT,                           /* k: the regular expression itself, for ~ and the functions that take one */
    OP_LOADV, OP_LOADL,                 /* n: a global, a local */
    OP_LOADNF,
    OP_ARGV, OP_ARGL,                   /* n: a call's argument that is a bare name: an array goes by reference */
    OP_ARRV, OP_ARRL,                   /* n: the array */
    OP_AGET,                            /* [array key] to [value] */
    OP_FIELD,                           /* [index] to [value] */
    OP_AIN,                             /* [key array] to [0 or 1] */
    OP_ADEL,                            /* [array key] */
    OP_ACLEAR,                          /* [array] */
    OP_STORE,                           /* kind n: [prefix value] to [value] */
    OP_UPDATE,                          /* kind n op: [prefix value] to [the result of place op value], stored */
    OP_STEP,                            /* kind n flags: ++ and -- */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_NEG, OP_PLUS, OP_NOT,
    OP_LT, OP_LE, OP_EQ, OP_NE, OP_GT, OP_GE,
    OP_CONCAT,                          /* n: the top n values as one string */
    OP_JOIN,                            /* n: the same with SUBSEP between */
    OP_MATCH,                           /* [text re] to [0 or 1] */
    OP_JMP, OP_JZ, OP_JNZ,              /* a: to; the last two take a value off the stack */
    OP_POP,
    OP_PRINT, OP_PRINTF,                /* n redirect: [values... destination] */
    OP_CALL,                            /* f n: the function, the arguments on the stack */
    OP_RET,                             /* the value on the stack is the result */
    OP_RET0,
    OP_BUILTIN,                         /* b n: the function, the arguments on the stack */
    OP_SUBST,                           /* kind n global: [re replacement prefix] to [count]: sub and gsub */
    OP_GETLINE,                         /* source prefix: [prefix...] to [prefix status line] */
    OP_GL,                              /* kind n: [prefix status line] to [status], stored if status is above 0 */
    OP_NEXT, OP_NEXTFILE,
    OP_EXIT,                            /* [status] */
    OP_EXIT0,
    OP_ITER,                            /* [array] to [iterator] */
    OP_ITERNEXT,                        /* kind n a: the next key into the name, or to a when there is none */
    OP_RANGE,                           /* r a: if range r is open, to a */
    OP_RANGESET,                        /* r on */
    OP_ONE
};

/* OP_STEP's flags */
#define STEP_POST 1                     /* the value from before */
#define STEP_DOWN 2

/* the redirections of print */
enum { TO_STDOUT, TO_FILE, TO_APPEND, TO_PIPE };

/* getline's sources */
enum { GL_MAIN, GL_FILE, GL_CMD };

/* the built-in functions; the numbers of OP_BUILTIN */
enum { B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_MATCH, B_SPRINTF, B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT, B_INT,
       B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH, B_SUB, B_GSUB };

/* what a run of code ends with */
enum { R_DONE, R_NEXT, R_NEXTFILE, R_EXIT };

struct func {
    char *name;
    int defined;
    unsigned nparams;
    unsigned entry;                     /* where its code starts */
    unsigned most_args;                 /* of its calls, and the line of one: checked when all is read */
    unsigned line;
};

struct linemark {
    unsigned pc, line, source;
};

struct frame {
    unsigned ret;                       /* the code to go back to */
    unsigned fp;                        /* the caller's first local */
};

struct awk {
    /* the program */
    int *code;
    unsigned ncode, capcode;
    double *knum;
    unsigned nknum, capknum;
    struct str **kstr;
    unsigned nkstr, capkstr;
    const struct regex **kre;
    unsigned nkre, capkre;
    struct func *fn;
    unsigned nfn, capfn;
    struct linemark *marks;
    unsigned nmarks, capmarks;
    char **sources;                     /* the program files' names, for messages */
    unsigned nsources;
    unsigned begin_pc, main_pc, end_pc;
    int have_main, have_end;
    unsigned char *range_open;
    unsigned nranges;
    /* the variables */
    struct value *global;
    char **gname;
    unsigned nglobal, capglobal;
    /* the machine */
    struct value *stack;
    unsigned sp, capstack, fp;
    struct frame *frames;
    unsigned nframes, capframes;
    unsigned pc;                        /* where it is, for messages */
    int running;                        /* so that a message can say the line */
    int exit_status;
    int in_begin_end;                   /* next and nextfile do not belong there */
    unsigned long long rand_state;
    double rand_seed;
};

extern struct awk A;

void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void say_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));     /* the message, and awk goes on */
void stop(void) __attribute__((noreturn));              /* files and commands ended, status 2 */
void *xalloc(size_t n);
void *xrealloc(void *p, size_t n);

/* value.c */
struct str *str_alloc(unsigned len);
struct str *str_new(const char *s, unsigned len);
void str_put(struct str *s);
static inline struct str *str_ref(struct str *s) {
    s->refs++;
    return s;
}
unsigned str_chars(struct str *s);                      /* how many characters */
unsigned char_len(const char *s, unsigned n);           /* the bytes of the character at s */
unsigned chars_in(const char *s, unsigned n);
unsigned char_at(struct str *s, unsigned k);            /* the byte where character k, from 0, starts */
void val_put(struct value *v);                          /* v's reference goes */
void val_copy(struct value *to, const struct value *from);      /* a new reference */
int looks_numeric(const char *s, unsigned n);
double str_num(const char *s, unsigned n);              /* the number at its start, or 0 */
double to_num(const struct value *v);
struct str *to_str(const struct value *v);              /* a new reference; a number through CONVFMT */
struct str *to_output(const struct value *v);           /* the same through OFMT */
struct str *num_str(double d, int conv);                /* conv: S_CONVFMT or S_OFMT */
int to_bool(const struct value *v);
int val_cmp(const struct value *a, const struct value *b);      /* -1, 0, 1, or 2 for a NaN */
struct array *array_new(void);
void array_ref(struct array *a);
void array_put(struct array *a);
struct value *array_elem(struct array *a, struct str *key, int create);
void array_del(struct array *a, struct str *key);
void array_clear(struct array *a);
unsigned array_count(const struct array *a);
struct iter *iter_new(const struct array *a);
struct str *iter_next(struct iter *it);                 /* the next key, not a new reference; NULL at the end */
void iter_free(struct iter *it);
int global_id(const char *name, unsigned len);          /* the name's number, a new one if it has none */
void global_set(int id, const struct value *v);         /* with what FS, OFS and NF ask */

/* field.c */
void record_set(struct str *s);                         /* takes the reference */
struct str *record_get(void);                           /* $0, not a new reference */
void field_load(double index, struct value *to);
void field_store(double index, const struct value *v);
double nf_get(void);
void nf_set(double n);
void before_fs_change(void);
void before_ofs_change(void);
void split_text(struct str *text, struct str *sep, const struct regex *re, int paragraph,
                void (*add)(void *ctx, unsigned start, unsigned len), void *ctx);

/* io.c */
struct stream;
void assign_operand(const char *s);                     /* var=value, from the command line */
int main_record(struct str **rec);                      /* 1, 0 at the end of the input */
void skip_file(void);
int getline_from(int cmd, struct str *name, struct str **rec);  /* 1, 0, or -1 for a file it cannot read */
FILE *output_to(int how, struct str *name);
int stream_close(struct str *name);
int stream_flush(struct str *name);                     /* NULL: all of them */
void streams_end(void);

/* fmt.c */
struct buf {
    char *p;
    unsigned len, cap;
};
void buf_add(struct buf *b, const char *s, unsigned n);
void buf_ch(struct buf *b, int c);
void format_text(struct buf *b, const char *fmt, unsigned flen, const struct value *args, int nargs);

/* func.c */
const struct regex *regex_static(const char *text, unsigned len);       /* NULL with a message left in regex_error */
extern char regex_error[128];
const struct regex *regex_of(const struct value *v);
int regex_find(const struct regex *re, const char *s, unsigned from, unsigned to, regoff_t *so, regoff_t *eo);
void builtin_call(int b, int nargs);
int substitute(const struct value *re, struct str *repl, struct str *text, int global, struct str **result);

/* parse.c */
void parse_program(const char *text, unsigned len, const unsigned *source_starts, unsigned nsources);
void parse_assign_value(const char *s, unsigned n, struct buf *to);     /* a command line value's escapes */
int is_reserved(const char *s, unsigned n);             /* a word of the language */

/* vm.c */
int run_code(unsigned pc);
void run_program(void);
unsigned line_now(unsigned *source);

#endif
