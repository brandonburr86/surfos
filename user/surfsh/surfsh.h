/*
SurfOS shell: surfsh
--------------------
File: surfsh.h  Date: 10/6/26
--------------------
What the files of surfsh, the SurfOS shell, share (the brief:
docs/superpowers/plans/2026-10-06-surfsh.md). surfsh is a ring-3 program in
/initrd/bin; init starts it with -l on every console.

    main.c      the start (-l: the banner), the settings and /initrd/etc/surfsh.conf, the loop
    editor.c    the line editor in raw mode: keys, long lines that wrap, the history
    complete.c  Tab completion: command names and paths
    parse.c     a line becomes commands: words, quotes, < > >>, &, ;, #
    run.c       running a command: surfsh's own, then the kernel's (kcmd), then programs
    builtins.c  cd, pwd, echo, exit, help, history, set

Statuses: 0 success; 1 a command failed; 2 a line or a command used the wrong
way; 126 a program that could not be started; 127 no such command; 137 a
program the kernel killed (128 + 9, as for SIGKILL on Unix). A kernel command
answers its own 0, 1 or 2. Anything but 0 makes the prompt's wave ≋!.
*/

#ifndef _SURFSH_H
#define _SURFSH_H

#include <surf.h>
#include <surfos/kcmd.h>

#define SURFSH_CONF     "/initrd/etc/surfsh.conf"
#define PROGRAM_DIR     "/initrd/bin"

#define LINE_LIMIT      1023        /* bytes of one line; the editor refuses a longer one */
#define HIST_MAX        128         /* lines the history keeps */
#define MAX_WORDS       32          /* words of one command, its name included (SPAWN_MAX_ARGS) */
#define MAX_CMDS        32          /* commands on one line, between ; and & */

#define STATUS_FAIL     1
#define STATUS_USAGE    2
#define STATUS_CANNOT   126
#define STATUS_NOTFOUND 127
#define STATUS_KILLED   137

/* the colours of SurfOS as ANSI SGR (the subset include/surfos/tty.h lists) */
#define SGR_TYPED  "\033[0;36m"     /* teal: what is typed */
#define SGR_LRED   "\033[1;31m"     /* light red: the banner */
#define SGR_ORANGE "\033[0;33m"     /* orange: the banner's version and years */
#define SGR_RESET  "\033[0m"        /* white: what commands print */

/**** main.c: the settings (set, surfsh.conf) and the state of the shell ****/

struct settings {
    int unicode;                    /* prompt.unicode: ▸ ≋ … where the console draws UTF-8 */
    int color;                      /* prompt.color: the SurfOS colours in the prompt and the line */
    int max_depth;                  /* prompt.max_depth: breadcrumb items, … included, 2-16 */
};

extern struct settings settings;
extern int last_status;             /* the previous command's status: the prompt shows it */
extern int exit_requested, exit_code;   /* the exit builtin, carried out by the main loop */

int use_unicode(void);              /* the setting, where the console draws UTF-8 (TTY_CAPS) */
int use_color(void);                /* the setting, where the console draws colours */
int settings_apply(const char *key, const char *value, char *why, int size);  /* 0; -1 and why */
int settings_show(const char *key); /* key = value for one setting or (NULL) all; -1: no such key */
int display_cols(const char *s, int n);   /* the columns of n bytes of UTF-8: one per character */

/**** editor.c: the line at the prompt ****/

/* A line typed at the prompt, without its line end: its length; -1 at the end of the input
   (Ctrl-D on an empty line); -2 when it was longer than LINE_LIMIT and is refused. */
int edit_line(char *buf, int size);
void history_add(const char *line);
int history_print(void);            /* the history builtin */

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
   indent and ends with eol, and put() writes the pieces */
void columns(char *const *names, int n, int width, const char *indent, const char *eol,
             void (*put)(const char *s, int len));

/**** parse.c: a line becomes commands ****/

struct command {
    char *argv[MAX_WORDS + 1];      /* argv[argc] is NULL */
    int argc;
    char *in;                       /* < file, NULL for none */
    char *out;                      /* > file or >> file, NULL for none */
    int append;                     /* >> */
    int bg;                         /* & after it */
};

struct parsed {
    struct command cmd[MAX_CMDS];
    int ncmd;                       /* 0 for a blank line or a comment */
    char words[2 * (LINE_LIMIT + 1)];
};

/* 0 and the commands, or -1 with a message in err */
int parse_line(const char *line, struct parsed *out, char *err, int errsize);
int needs_quotes(const char *s);    /* a word that parse_line would split or change */

/**** run.c: running commands ****/

void run_init(void);                /* the kernel's command table, the saved descriptor 1 */
int run_line(const char *line);     /* every command of the line: the last one's status
                                       (last_status for a blank line) */
int kcmd_count(void);
const struct kcmd_info *kcmd_entry(int i);
const struct kcmd_info *kcmd_find(const char *name);
int kcmd_bridge(void);              /* the kernel offers its commands to programs */
int program_names(struct cands *c); /* the programs in /initrd/bin, added to c, sorted; -1: none */

/**** builtins.c: surfsh's own commands ****/

struct builtin {
    const char *name, *args, *help;
    int (*fn)(int argc, char **argv);
};

extern const struct builtin builtins[];
extern const int nbuiltins;
const struct builtin *builtin_find(const char *name);

#endif
