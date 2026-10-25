/*
SurfOS shell: surfsh
--------------------
File: surfsh.h  Date: 10/6/26, 10/24/26
--------------------
What the files of surfsh, the SurfOS shell, share (the briefs:
docs/superpowers/plans/2026-10-06-surfsh.md, 2026-10-24-surfsh-scripting.md). surfsh is a
ring-3 program in /initrd/bin; init starts it with -l on every console.

    main.c      the start (-l, -c, --sub), the settings and /initrd/etc/surfsh.conf, the input
    editor.c    the line editor in raw mode: keys, long lines that wrap, the history
    complete.c  Tab completion: command names and paths
    util.c      strings that grow, arenas, memory
    parse.c     text becomes a tree: lists, && and ||, pipelines, commands, redirections
    unparse.c   a tree becomes text again (what a new surfsh gets to run)
    expand.c    words become fields: parameters, $( ), $(( )), splitting at IFS, quotes
    arith.c     $(( )): C's integer arithmetic
    glob.c      patterns (* ? [...]) and the names they match
    vars.c      variables, the environment, the positional parameters
    exec.c      the tree runs: lists, pipelines, groups, subshells, redirections
    run.c       one command: surfsh's own, then programs, then the kernel's (kcmd; 10/25/26)
    jobs.c      the jobs in the background: started, collected, reported
    builtins.c  cd, pwd, echo, exit, help, history, set, export, readonly, unset, shift, type
    script.c    10/25/26: : true false break continue return local eval . source read kernel time
    test.c      test and [;  printf.c  printf

10/24/26: a line is parsed into a tree (parse.c) and run (exec.c). A stage of a pipeline
that is surfsh's own (a builtin, a kernel command, a group), except the last, a job in the
background that is, and ( list ) run in a new surfsh, started as "surfsh --sub" with the
text to run on its descriptor 3 (exec.c): SurfOS has no fork().

Statuses: 0 success; 1 a command failed; 2 a line or a command used the wrong way; 126 a
program that could not be started; 127 no such command; 137 a program the kernel killed
(128 + 9, as for SIGKILL on Unix); 141 a program that wrote to a pipe nobody reads; 130 a
command Ctrl+C stopped (10/25/26: a program it killed, a loop, read). A kernel command
answers its own 0, 1 or 2 (130 when Ctrl+C cut it short), and run the program's.
10/25/26: if, while, until, for, case and functions; scripts (surfsh FILE, #!, a file that
is no program); set -e -u -x -f; Ctrl+C leaves a loop or a script (exec.c).
Anything but 0 makes the prompt's wave ≋!, and $? on the next command's line is the number.
*/

#ifndef _SURFSH_H
#define _SURFSH_H

#include <surf.h>
#include <surfos/kcmd.h>

#define SURFSH_CONF     "/initrd/etc/surfsh.conf"
#define SURFSH_PATH     "/initrd/bin/surfsh"    /* a new surfsh: subshells, jobs of surfsh's own */
#define PROGRAM_DIR     "/initrd/bin"

#define LINE_LIMIT      1023        /* bytes of one typed line; the editor refuses a longer one */
#define HIST_MAX        128         /* lines the history keeps */
#define MAX_WORDS       128         /* words of one command, its name included (SPAWN_MAX_ARGS) */
#define WORD_LIMIT      255         /* bytes of a path or an argument the kernel takes (VFS_PATH_MAX - 1) */
#define TEXT_LIMIT      (1024 * 1024)   /* bytes of a script, or of what a new surfsh is given */
#define DEPTH_LIMIT     64          /* compound commands inside each other, at most */
#define FUNC_DEPTH_LIMIT 100        /* 10/25/26: function calls (and . files) inside each other */
#define STAGES_LIMIT    16          /* commands of one pipeline */

#define STATUS_FAIL     1
#define STATUS_USAGE    2
#define STATUS_CANNOT   126
#define STATUS_NOTFOUND 127
#define STATUS_INTERRUPTED 130     /* 10/25/26: Ctrl+C stopped it (128 + 2, SIGINT's number) */
#define STATUS_STOPPED  148         /* 10/25/26: Ctrl+Z stopped it (128 + 20, SIGTSTP's), job control */
#define STATUS_KILLED   137

/* the colours of SurfOS as ANSI SGR (the subset include/surfos/tty.h lists) */
#define SGR_TYPED  "\033[0;36m"     /* teal: what is typed */
#define SGR_LRED   "\033[1;31m"     /* light red: the banner */
#define SGR_ORANGE "\033[0;33m"     /* orange: the banner's version and years */
#define SGR_RESET  "\033[0m"        /* white: what commands print */

/**** util.c: strings that grow, arenas ****/

struct sbuf {
    char *s;                        /* NUL-terminated (NULL until something is put) */
    int len, cap;
};

void *xmalloc(size_t n);            /* never NULL: no memory ends surfsh (status 2) */
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void sb_put(struct sbuf *b, const char *s, int n);
void sb_puts(struct sbuf *b, const char *s);
void sb_putc(struct sbuf *b, char c);
void sb_printf(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_quote(struct sbuf *b, const char *s);   /* s as one word: in single quotes when it needs any */
const char *sb_str(struct sbuf *b);  /* the text so far, "" when empty */
void sb_free(struct sbuf *b);

struct ablock;
struct arena {
    struct ablock *blocks;
};

void *arena_alloc(struct arena *a, size_t n);   /* zeroed; it goes with arena_free() */
char *arena_strndup(struct arena *a, const char *s, size_t n);
void arena_free(struct arena *a);

/**** main.c: the settings (set, surfsh.conf) and the state of the shell ****/

struct settings {
    int unicode;                    /* prompt.unicode: ▸ ≋ … where the console draws UTF-8 */
    int color;                      /* prompt.color: the SurfOS colours in the prompt and the line */
    int max_depth;                  /* prompt.max_depth: breadcrumb items, … included, 2-16 */
};

extern struct settings settings;
extern int last_status;             /* the previous command's status: the prompt shows it */
extern int exit_requested, exit_code;   /* the exit builtin, carried out by whoever runs the commands */
extern int interactive;             /* commands typed at the prompt: reports, paging, ^C back to the prompt */
extern int term_fd;                 /* 10/25/26: where the prompt and the reports go: an interactive shell's own
                                       copy of its terminal (10-15, exec.c), whatever 1 is meanwhile; else 1 */
extern int shell_pid;               /* $$: this shell's, or the one a new surfsh was started from */

int use_unicode(void);              /* the setting, where the console draws UTF-8 (TTY_CAPS) */
int use_color(void);                /* the setting, where the console draws colours */
int settings_apply(const char *key, const char *value, char *why, int size);  /* 0; -1 and why */
int settings_show(const char *key); /* key = value for one setting or (NULL) all; -1: no such key */
int display_cols(const char *s, int n);   /* the columns of n bytes of UTF-8: one per character */

/* A source of lines: the line editor, a descriptor read line by line, or text in memory. get()
   appends one line, its '\n' included (a last line without one gets one), to out: 0, or -1 at
   the end; -2 when the line was refused (too long, or dropped with Ctrl-C), which drops what
   was read of the command. more: the line continues a command (the continuation prompt). */
struct source {
    int (*get)(struct source *s, struct sbuf *out, int more);
    const char *name;               /* for messages: a file's name, or NULL */
    int lines;                      /* lines read so far */
    int fd;                         /* a descriptor's lines: the descriptor */
    const char *text;               /* text's lines: what is left of the text */
};

int run_source(struct source *s);   /* every command in it, each when it is complete: the last status */
int run_text(const char *text, const char *name);   /* the same for text in memory */

/**** editor.c: the line at the prompt ****/

/* A line typed at the prompt, without its line end: its length; -1 at the end of the input
   (Ctrl-D on an empty line); -2 when it was longer than LINE_LIMIT and is refused; -3 when
   Ctrl-C dropped a line that continued a command. more: the continuation prompt. */
int edit_line(char *buf, int size, int more);
void edit_fresh_row(void);          /* the cursor at the start of a fresh row, as before a prompt */
void history_add(const char *line);
int history_print(void);            /* the history builtin */
void history_state(struct sbuf *out);   /* the history as text a new surfsh runs (history -s lines) */

/**** complete.c: Tab ****/

struct cand {
    char *name;                     /* a command name, or a directory entry's name */
    int dir;                        /* a directory: completed with "/", listed with "/" */
};

struct cands {
    struct cand *v;
    int n, cap;
};

/* Tab with the cursor at *pos: the word before the cursor is completed in place.
   0: nothing to complete; 1: the line changed; 2: several candidates with nothing more in
   common, which are in *list, sorted (list them on a second Tab, then cands_free). */
int complete(char *line, int *len, int *pos, int limit, struct cands *list);
void cands_free(struct cands *c);

/* names in columns that fit width, sorted down the columns as ls does; each row starts with
   indent and ends with eol, and put() writes the pieces. columns_rows(): the rows it takes. */
void columns(char *const *names, int n, int width, const char *indent, const char *eol,
             void (*put)(const char *s, int len));
int columns_rows(char *const *names, int n, int width, int indent);

/**** parse.c: text becomes a tree ****/

enum node_type {
    N_SIMPLE,                       /* words, assignments and redirections */
    N_PIPE,                         /* kids[0] | kids[1] | ... */
    N_AND, N_OR,                    /* a && b, a || b */
    N_NOT,                          /* ! a */
    N_SEQ,                          /* a ; b, or a & b when a is N_BG */
    N_BG,                           /* a & */
    N_GROUP,                        /* { a ; } */
    N_SUB,                          /* ( a ) */
    /* 10/25/26: compound commands and functions */
    N_IF,                           /* if a; then b; else c; fi (c: NULL, or the else part: an N_IF for elif) */
    N_WHILE, N_UNTIL,               /* while a; do b; done */
    N_FOR,                          /* for name in words; do b; done (no in: "$@") */
    N_CASE,                         /* case name in items esac */
    N_FUNC                          /* name() a: a function's definition (a: a compound command) */
};

enum redir_op {
    R_IN,                           /* < */
    R_OUT,                          /* > */
    R_APPEND,                       /* >> */
    R_CLOBBER,                      /* >| (the same as >: there is no noclobber) */
    R_RDWR,                         /* <> */
    R_DUPIN,                        /* <& n, <&- */
    R_DUPOUT,                       /* >& n, >&- */
    R_HERE,                         /* << word */
    R_HERESTRIP                     /* <<- word: the leading tabs of its lines go */
};

struct redir {
    struct redir *next;
    int fd;                         /* the descriptor it is for */
    int op;
    char *word;                     /* the file, the descriptor (<& >&), or the end line (<<), as typed */
    char *body;                     /* << : the document, its lines with their '\n' */
    char *end;                      /* << : the end line (the word without its quotes) */
    int quoted;                     /* << : the end line had quotes: the document is not expanded */
};

struct case_item {                  /* 10/25/26: pattern|pattern) body ;; */
    struct case_item *next;
    char **pats;                    /* as typed */
    int npats;
    struct node *body;              /* NULL: nothing to run */
};

struct node {
    int type;
    int line;                       /* the line it began on, for messages */
    struct node *a, *b;             /* N_AND, N_OR, N_SEQ: a and b; N_NOT, N_BG, N_GROUP, N_SUB, N_FUNC: a;
                                       N_IF, N_WHILE, N_UNTIL: a the condition, b what it runs; N_FOR: b */
    struct node *c;                 /* N_IF: the else part */
    struct node **kids;             /* N_PIPE: the stages */
    int nkids;
    char **words;                   /* N_SIMPLE: the words as typed; N_FOR: the words after in */
    int nwords;
    char **assigns;                 /* N_SIMPLE: name=value words before the command */
    int nassigns;
    struct redir *redirs;           /* N_SIMPLE and the compound commands */
    char *name;                     /* N_FOR: the variable; N_CASE: the word, as typed; N_FUNC: the function */
    struct case_item *items;        /* N_CASE */
    int in;                         /* N_FOR: in was there (with words or none) */
};

enum { PARSE_OK, PARSE_EOF, PARSE_MORE, PARSE_ERROR };

struct token {
    int type;                       /* parse.c's T_* */
    int start, end;                 /* the text it is: src[start .. end) */
    int op, fd;                     /* a redirection: R_*, and the descriptor before it (-1: none) */
};

struct heredoc;
struct parser {
    const char *src;                /* the text, NUL-terminated */
    int pos;                        /* where the next token begins */
    struct arena *mem;              /* where the tree goes */
    struct token look;              /* the token looked at, when have */
    int have;
    int status;                     /* 0, or PARSE_MORE or PARSE_ERROR once it is known */
    int depth;                      /* commands open inside each other */
    struct heredoc *pending;        /* here-documents whose lines follow the next newline */
    int line, lpos;                 /* the line that byte lpos is on (line numbers as the scanner goes) */
    char err[160];                  /* PARSE_ERROR and PARSE_MORE: why */
    int err_line;                   /* and on which line of the text */
};

void parser_init(struct parser *p, const char *text, struct arena *mem);
/* The next complete command of the text (up to a newline at the outside, or its end): PARSE_OK
   and *out; PARSE_EOF when only blanks, comments and newlines are left; PARSE_MORE when the
   text ends inside a command (more lines would complete it); PARSE_ERROR. p->pos is past what
   was read; the reason is in p->err. */
int parse_command(struct parser *p, struct node **out);
int is_name(const char *s, int n); /* a variable's name: a letter or _, then letters, digits, _ */
int needs_quotes(const char *s);    /* a word that the parser would split or change */
int word_skip(const char *s, int i);   /* past the quote, $( ), ${ }, $(( )) or ` ` at s[i] (expand.c) */
int line_of(const char *text, int pos);    /* the line (1, 2, ...) that byte pos is on */

/**** unparse.c: a tree becomes text again ****/

void unparse(struct node *n, struct sbuf *out);    /* the text, here-documents and all, ending in '\n' */

/**** expand.c: words become fields ****/

struct fields {
    char **v;                       /* v[n] is NULL */
    int n, cap;
};

/* the words as fields: parameters, $( ), $(( )), splitting, wildcards, quotes; -1 said */
int expand_words(char **words, int n, struct fields *out);
char *expand_one(const char *word);     /* a word as one string, never split nor matched (a file's name): NULL said */
char *expand_doc(const char *body);     /* a here-document's text (its quotes are themselves): NULL said */
int expand_pattern(const char *word, char **pat, char **quoted, int *len);  /* a pattern with its quoted characters */
void fields_free(struct fields *f);
void fields_take(struct fields *f, char *s);    /* s (malloc'ed) is the next field */
extern int subst_status, subst_count;   /* the last command substitution's status, how many ran */

/**** arith.c, glob.c ****/

int arith(const char *text, long *result, char *err, int errsize);   /* 0, or -1 and why */
int pmatch(const char *p, const char *q, int pn, const char *s, int sn);   /* q[i]: p[i] was quoted */
int has_pattern(const char *p, const char *q, int pn);
int glob_field(const char *p, const char *q, int pn, struct fields *out);  /* the names, sorted: how many */

/**** vars.c: variables ****/

#define V_EXPORT   1                /* programs get it in their environment */
#define V_READONLY 2
#define V_SET      4                /* var_flags(): it has a value */

extern char *arg0;                  /* $0 */
extern char **posargs;                  /* $1 ... */
extern int npos;                    /* $# */

const char *var_get(const char *name);          /* NULL: not set */
int var_flags(const char *name);
int var_set(const char *name, const char *value, int flags);    /* value NULL: flags only; -1 said (read-only) */
int var_unset(const char *name);                /* -1 said (read-only) */
int var_assign(const char *word, int flags);    /* "name=value": -1 said */
int var_push(const char *word);                 /* name=value until var_pop() */
void var_pop(int to);
int var_pushed(void);
char **var_environ(char **extra, int nextra);   /* the environment for a program; NULL said: too big */
void var_env_free(char **env);
void vars_init(char **env);
void vars_print(int only);                      /* 0: all; V_EXPORT, V_READONLY: those, as export/readonly lines */
void var_state(struct sbuf *out, int no_errexit);  /* what a new surfsh needs to be this one, as text it runs;
                                                   10/25/26: no_errexit: without set -e (in a condition) */
void pos_set(int n, char **v);
int pos_shift(int n);                           /* -1: not that many */
const char *ifs(void);                          /* IFS, or blank, tab and newline when it is not set */
/* 10/25/26 (scripts) */
struct pos_saved {
    char **v;
    int n;
};
void pos_save(struct pos_saved *s);             /* the positional parameters kept, none in their place */
void pos_restore(struct pos_saved *s);
int local_mark(void);                           /* a function's call begins: its locals from here */
int local_keep(const char *name, int mark);     /* local name: put back at local_restore(mark); -1 said */
void local_restore(int mark);
extern int opt_e, opt_u, opt_x, opt_f;          /* set -e (errexit), -u (nounset), -x (xtrace), -f (noglob) */
int option_set(char letter, const char *name, int on);  /* a letter, or a name for -o (name NULL: the letter) */
const char *options_string(void);               /* $- */
void options_print(int as_commands);            /* set -o, set +o */
struct func;
int func_define(struct node *n);                /* N_FUNC: defined (replacing one of the name); -1 said */
struct func *func_find(const char *name);
struct node *func_body(struct func *f);
void func_hold(struct func *f);                 /* a call runs it: it stays until func_release() */
void func_release(struct func *f);
int func_unset(const char *name);               /* 0; 1 when there is no such function */
void funcs_text(struct sbuf *out);              /* the definitions, each a line (and its here-documents) */

/**** exec.c: the tree runs ****/

#define X_TOP     1                 /* a command of its own at the prompt: its program's end is reported */
#define X_COND    2                 /* the condition of && || ! */
#define X_BACK    4                 /* in a job in the background: nothing runs in this shell */

int exec_tree(struct node *n, int flags);  /* in this shell: the status */
/* 10/25/26: break, continue, return and Ctrl+C on their way out: while stopping() the commands between
   are left; loops (loop_depth of them), functions and . files take what is theirs */
extern int loop_depth, break_count, continue_count, returning, return_status, func_depth, dot_depth, local_base;
extern int interrupted, interrupt_said;         /* Ctrl+C stopped a command; something said so already */
extern int suspended;                           /* 10/25/26: Ctrl+Z stopped a job in the foreground */
int stopping(void);
void note_interrupt(int said);                  /* a script ends (130); an interactive shell's line does */
int check_interrupt(void);                      /* after a command: TTY_INTR's count; interrupted */
/* a new surfsh that runs text: descriptors 0, 1 (-1: this shell's own), the text on its 3; its pid */
int start_surfsh(const char *text, int in, int out, int background);
int exec_capture(const char *text, struct sbuf *out);  /* $( text ): its output, the newlines at its end off; its status */
int fd_copy_high(int fd);           /* 10/25/26: a copy of fd among the shell's own descriptors (10-15); -1: none free */
extern int cond_flag;               /* 10/25/26: X_COND while a command whose status is looked at runs (eval, ., a new surfsh) */

/**** run.c: commands ****/

void run_init(void);                /* the kernel's command table */
int kcmd_count(void);
const struct kcmd_info *kcmd_entry(int i);
const struct kcmd_info *kcmd_find(const char *name);
int kcmd_bridge(void);              /* the kernel offers its commands to programs */
int program_names(struct cands *c); /* the programs in /initrd/bin, added to c, sorted; -1: none */
int find_program(const char *name, char *path, int size);   /* 0 and its path (PATH's), else a status, said */
int program_path(const char *name, char *path, int size);   /* 10/25/26: the same, saying nothing: 0, or -1 */
int run_kernel(char **argv, int *found);   /* a kernel command in this shell: its status; *found 0: none */
/* a program with this shell's 0, 1, 2 (in, out: those instead; -1 not) and the environment env: its pid; -1 said.
   10/25/26: fd3, its descriptor 3 (-1: none) */
int start_program(const char *path, char **argv, int in, int out, int background, char **env, int fd3);
int wait_program(int pid, int report);       /* its status: 137 killed; report: [exit status n] */
int wait_program_ex(int pid, int report, int untraced, int *why);   /* 10/25/26: a stop too: 148, why WX_STOP_* */
void page(int on);                  /* the pager for this shell's commands (TTY_PAGE) */

/**** jobs.c: the jobs in the background ****/

void job_add(const char *text, const int *pids, int n, int kid);   /* "[n] started (pid ...)"; kid: its kernel job */
/* 10/25/26, job control: what is started between job_open() and job_close() is one job of the kernel's
   (an interactive shell's), whose id job_close() answers; spawnx's job is job_spawn_value(), and
   job_spawned() with the pid once a process started. job_hold() keeps a $( ) out of it. */
struct jobwin {
    int next, kid;
};
void job_open(void);
int job_close(void);
int job_spawn_value(void);
void job_spawned(int pid);
void job_hold(struct jobwin *w);
void job_unhold(struct jobwin *w);
void job_stopped(const char *text, const int *pids, int n, int kid, int why);   /* "[n] stopped  ..." */
int b_jobs(int argc, char **argv);
int b_fg(int argc, char **argv);
int b_bg(int argc, char **argv);
int b_wait(int argc, char **argv);
int b_kill(int argc, char **argv);
void jobs_collect(int fresh);       /* the jobs that ended: reported (fresh: from a fresh row) */
void jobs_state(struct sbuf *out);  /* 10/25/26: the table as jobs -s lines, for a new surfsh to show */
int jobs_last_pid(void);            /* the last job's last pid ($!), 0 for none */
void jobs_set_last(int pid);        /* a new surfsh's $!: the one of the shell that started it */

/**** builtins.c: surfsh's own commands ****/

struct builtin {
    const char *name, *args, *help;
    int (*fn)(int argc, char **argv);
    int special;                    /* 10/25/26: POSIX's special builtins come before functions */
};

extern const struct builtin builtins[];
extern const int nbuiltins;
/* 10/25/26: script.c, test.c, printf.c */
int b_colon(int argc, char **argv);
int b_false(int argc, char **argv);
int b_break(int argc, char **argv);
int b_continue(int argc, char **argv);
int b_return(int argc, char **argv);
int b_local(int argc, char **argv);
int b_eval(int argc, char **argv);
int b_dot(int argc, char **argv);
int b_read(int argc, char **argv);
int b_kernel(int argc, char **argv);
int b_time(int argc, char **argv);                 /* 10/25/26 */
int b_test(int argc, char **argv);
int test_eval(int n, char **v);                 /* test's arguments: 0 true, 1 false, 2 said */
int b_printf(int argc, char **argv);
int printf_text(struct sbuf *out, int argc, char **argv);   /* format and args: 0, 1, 2 (said) */
const struct builtin *builtin_find(const char *name);

#endif
