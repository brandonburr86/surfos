/*
SurfOS shell: surfsh
--------------------
File: builtins.c  Date: 10/6/26
--------------------
surfsh's own commands. They come before the kernel's commands of the same names
(the kernel's help, echo, cd and pwd are not used). echo is one of them, so
"echo text > file" needs nothing from the kernel. Their output can be
redirected like a program's (exec.c puts the file on descriptor 1), and they can
be in a pipeline (in a new surfsh, unless they are its last stage).

    cd [dir]          no dir: /; "cd -": the previous directory (printed); PWD and OLDPWD follow
    pwd               the current directory
    echo [-n] [-e]    the words; -n: no newline; -e: \n \r \t \\ are the characters they name
    exit [n]          leave with status n (0-255, n & 255 as on Unix); 10/25/26: without n, the
                      status of the command before (cmd || exit), as POSIX has it
    help [command]    this shell's commands, the kernel's, the programs; or one of them
    history           the lines typed before, oldest first (history -s LINE adds one: a new surfsh's)
    set [key [value]] the settings (main.c); also "set key = value" and "set key=value",
                      as surfsh.conf writes them; set alone: the settings, then the variables;
                      set -- [args]: the positional parameters
    export [name[=value]...]    programs get these variables (none, or -p: the list)
    readonly [name[=value]...]  these variables cannot change any more (none, or -p: the list)
    unset name...     the variables are gone (unset -f: the functions)
    shift [n]         $n+1 becomes $1 (1)

10/25/26 (scripts): the builtins of script.c (: true false break continue return local eval
. source read kernel), test.c (test and [) and printf.c are in the table too, and exec, which
exec.c carries out itself. POSIX's special builtins (: . break continue eval exec exit export
readonly return set shift unset) come before a function of the same name, the others after
it. set also takes POSIX's options: set -e -u -x -f, +e ... to end them, -o NAME / +o NAME,
set -o and set +o to list them, and set -e -- args.
*/

#include "surfsh.h"
#include <surfos/version.h>

static int b_cd(int argc, char **argv);
static int b_pwd(int argc, char **argv);
static int b_echo(int argc, char **argv);
static int b_history(int argc, char **argv);
static int b_set(int argc, char **argv);
static int b_help(int argc, char **argv);
static int b_exit(int argc, char **argv);
static int b_export(int argc, char **argv);
static int b_readonly(int argc, char **argv);
static int b_unset(int argc, char **argv);
static int b_shift(int argc, char **argv);

const struct builtin builtins[] = {
    { "cd",      "[dir]",         "change the directory (none: /, -: the one before)", b_cd, 0 },
    { "pwd",     "",              "print the current directory", b_pwd, 0 },
    { "echo",    "[-n] [-e] ...", "print the words (-n: no newline, -e: \\n \\r \\t \\\\)", b_echo, 0 },
    { "history", "",              "the lines typed before (Up and Down recall them)", b_history, 0 },
    { "set",     "[key [value]]", "show or change the settings (set alone: all, and the variables)", b_set, 1 },
    { "help",    "[command]",     "this list, or one command", b_help, 0 },
    { "exit",    "[n]",           "leave surfsh with status n, 0-255 (none: $?)", b_exit, 1 },
    { "export",  "[name[=value]...]", "programs get these variables (none: the list)", b_export, 1 },
    { "readonly", "[name[=value]...]", "these variables can no longer change (none: the list)", b_readonly, 1 },
    { "unset",   "[-f] name...",  "the variables are gone (-f: the functions)", b_unset, 1 },
    { "shift",   "[n]",           "the positional parameters move down by n (1)", b_shift, 1 },
    /* 10/25/26: scripts */
    { "test",    "expression",    "true or false: -e -f -d -s file, -n -z s, s = s, n -lt n, ! -a -o ( )", b_test, 0 },
    { "[",       "expression ]",  "the same as test", b_test, 0 },
    { "true",    "",              "status 0 (: too)", b_colon, 0 },
    { "false",   "",              "status 1", b_false, 0 },
    { ":",       "[args]",        "nothing, status 0", b_colon, 1 },
    { "read",    "[-r] [name...]", "a line of input into the variables (split at IFS; REPLY)", b_read, 0 },
    { "printf",  "format [args]", "the arguments as the format says (%s %d %x %c %b, \\n ...)", b_printf, 0 },
    { "break",   "[n]",           "leave the loop (n of them)", b_break, 1 },
    { "continue", "[n]",          "the loop's next round", b_continue, 1 },
    { "return",  "[n]",           "leave the function or the . file, with status n", b_return, 1 },
    { "local",   "name[=value]...", "the function's own variables", b_local, 0 },
    { "eval",    "[args]",        "the arguments as a command line", b_eval, 1 },
    { ".",       "file [args]",   "the commands of the file, in this shell", b_dot, 1 },
    { "source",  "file [args]",   "the same as .", b_dot, 0 },
    { "exec",    "[cmd [args]]",  "cmd, and then this shell ends; alone: its redirections stay", b_colon, 1 },
    { "kernel",  "name [args]",   "the kernel's command name, where surfsh has its own", b_kernel, 0 },
    /* 10/25/26: job control (jobs.c) */
    { "jobs",    "",              "the jobs: running, stopped (Ctrl+Z), and their commands", b_jobs, 0 },
    { "fg",      "[%n]",          "job n (the last one) in the foreground again", b_fg, 0 },
    { "bg",      "[%n]",          "job n goes on in the background", b_bg, 0 },
    { "wait",    "[%n|pid...]",   "until those jobs end (none: all of them); Ctrl+C ends the wait", b_wait, 0 },
    { "kill",    "%n|pid...",     "job n's processes killed, the stopped ones too; a pid as the kernel's kill", b_kill, 0 },
};
const int nbuiltins = sizeof(builtins) / sizeof(builtins[0]);

const struct builtin *builtin_find(const char *name) {
    int i;
    for(i = 0; i < nbuiltins; i++) if(!strcmp(builtins[i].name, name)) return &builtins[i];
    return NULL;
}

/**** cd, pwd ****/

static char oldpwd[PATH_MAX];

static int b_cd(int argc, char **argv) {
    char cwd[PATH_MAX];
    const char *dir = argc > 1 ? argv[1] : "/";
    int back = 0;
    if(argc > 2) {
        fprintf(stderr, "usage: cd [dir]\n");
        return STATUS_USAGE;
    }
    if(!strcmp(dir, "-")) {
        const char *old = var_get("OLDPWD");    /* 10/24/26: the variable, which a script may set */
        if(old && *old && strlen(old) < sizeof(oldpwd)) strcpy(oldpwd, old);
        if(!oldpwd[0]) {
            fprintf(stderr, "surfsh: cd: no previous directory\n");
            return STATUS_FAIL;
        }
        dir = oldpwd;
        back = 1;
    }
    if(strlen(dir) > WORD_LIMIT) {              /* the kernel would answer EFAULT */
        fprintf(stderr, "surfsh: cd: a path of %u bytes: paths have at most %d\n", (unsigned)strlen(dir), WORD_LIMIT);
        return STATUS_USAGE;
    }
    if(!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
    if(chdir(dir) < 0) {
        fprintf(stderr, "surfsh: cd: %s: %s\n", dir, strerror(errno));
        return STATUS_FAIL;
    }
    if(cwd[0]) {                            /* after chdir: dir may be oldpwd itself */
        strcpy(oldpwd, cwd);
        if(!(var_flags("OLDPWD") & V_READONLY)) var_set("OLDPWD", cwd, V_EXPORT);
    }
    if(getcwd(cwd, sizeof(cwd))) {
        if(!(var_flags("PWD") & V_READONLY)) var_set("PWD", cwd, V_EXPORT);
        if(back) printf("%s\n", cwd);
    }
    return 0;
}

static int b_pwd(int argc, char **argv) {
    char cwd[PATH_MAX];
    (void)argc;
    (void)argv;
    if(!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "surfsh: pwd: %s\n", strerror(errno));
        return STATUS_FAIL;
    }
    printf("%s\n", cwd);
    return 0;
}

/**** echo ****/

static void put_escaped(const char *s) {
    for(; *s; s++) {
        if(*s != '\\' || !s[1]) { putchar(*s); continue; }
        switch(*++s) {
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case '\\': putchar('\\'); break;
        default: putchar('\\'); putchar(*s); break;     /* not one of ours: as it was */
        }
    }
}

static int b_echo(int argc, char **argv) {
    int i = 1, newline = 1, escapes = 0;
    for(; i < argc && argv[i][0] == '-' && argv[i][1] && strspn(argv[i] + 1, "ne") == strlen(argv[i] + 1); i++) {
        if(strchr(argv[i], 'n')) newline = 0;
        if(strchr(argv[i], 'e')) escapes = 1;
    }
    for(; i < argc; i++) {
        if(escapes) put_escaped(argv[i]);
        else fputs(argv[i], stdout);
        if(i < argc - 1) putchar(' ');
    }
    if(newline) putchar('\n');
    return 0;                               /* what a full disk does not take: run.c says so */
}

/**** exit, history, set ****/

static int b_exit(int argc, char **argv) {
    long n = last_status;                   /* 10/25/26: none given: the status of the command before */
    if(argc > 2) {
        fprintf(stderr, "usage: exit [n]\n");
        return STATUS_USAGE;
    }
    if(argc == 2) {
        char *end;
        n = strtol(argv[1], &end, 10);
        if(!argv[1][0] || *end) {
            fprintf(stderr, "surfsh: exit: %s is not a number\n", argv[1]);
            return STATUS_USAGE;
        }
    }
    exit_requested = 1;                     /* the main loop ends after this command */
    exit_code = (int)(n & 0xff);            /* 0-255: init takes -1 for a shell that was killed */
    return 0;
}

static int b_history(int argc, char **argv) {
    if(argc == 3 && !strcmp(argv[1], "-s")) {  /* 10/24/26: a line for the history (a new surfsh's) */
        history_add(argv[2]);
        return 0;
    }
    if(argc > 1) {
        fprintf(stderr, "usage: history\n");
        return STATUS_USAGE;
    }
    return history_print();
}

/* 10/25/26: set -e -u -x -f (+ to end them), -o name, +o name, and then -- and the positional
   parameters, or nothing: 0, or a usage status said */
static int set_options(int argc, char **argv) {
    int i;
    for(i = 1; i < argc; i++) {
        const char *a = argv[i];
        int on = a[0] == '-';
        if(!strcmp(a, "--")) {
            pos_set(argc - i - 1, argv + i + 1);
            return 0;
        }
        if((a[0] != '-' && a[0] != '+') || !a[1]) break;
        if(a[1] == 'o' && !a[2]) {
            if(i + 1 == argc) {
                options_print(!on);
                return 0;
            }
            if(option_set(0, argv[++i], on) < 0) {
                fprintf(stderr, "surfsh: set: no option called %s (set -o lists them)\n", argv[i]);
                return STATUS_USAGE;
            }
            continue;
        }
        for(a++; *a; a++) {
            if(option_set(*a, NULL, on) < 0) {
                fprintf(stderr, "surfsh: set: -%c: not an option (-e -u -x -f, -o name)\n", *a);
                return STATUS_USAGE;
            }
        }
    }
    if(i < argc) {
        fprintf(stderr, "surfsh: set: %s: options come first, then -- and the positional parameters\n", argv[i]);
        return STATUS_USAGE;
    }
    return 0;
}

/* set, set key, set key value; with an = the words are "key = value" as surfsh.conf has them.
   10/24/26: set alone lists the variables too, and set -- the positional parameters */
static int b_set(int argc, char **argv) {
    char why[120], text[LINE_LIMIT + 1] = "", *key = argv[1], *value = argc == 3 ? argv[2] : NULL, *eq, *p;
    int i;
    if(argc == 1) {
        struct sbuf f = { NULL, 0, 0 };
        settings_show(NULL);
        printf("\n");
        vars_print(0);
        funcs_text(&f);                         /* 10/25/26: and the functions */
        if(f.len) printf("%s", sb_str(&f));
        sb_free(&f);
        return 0;
    }
    if(argv[1][0] == '-' || (argv[1][0] == '+' && argv[1][1])) return set_options(argc, argv);
    for(i = 1; i < argc; i++) {
        strlcat(text, argv[i], sizeof(text));
        if(i < argc - 1) strlcat(text, " ", sizeof(text));
    }
    if((eq = strchr(text, '='))) {
        for(p = eq; p > text && (p[-1] == ' ' || p[-1] == '\t'); p--);
        *p = 0;
        for(value = eq + 1; *value == ' ' || *value == '\t'; value++);
        key = text;
    } else if(argc == 2) {
        if(settings_show(argv[1]) == 0) return 0;
        fprintf(stderr, "surfsh: set: no setting called %s (set alone lists them)\n", argv[1]);
        return STATUS_USAGE;
    }
    if(!value || !*key) {
        fprintf(stderr, "usage: set [key [value]]   (or set key = value)\n");
        return STATUS_USAGE;
    }
    if(settings_apply(key, value, why, sizeof(why))) {
        fprintf(stderr, "surfsh: set: %s\n", why);
        return STATUS_USAGE;
    }
    return 0;
}

/**** variables: export, readonly, unset, shift ****/

/* export and readonly: each name (name=value assigns it first) gets the flag; none or -p: the list */
static int mark(int argc, char **argv, int flag, const char *cmd) {
    int i, status = 0;
    if(argc == 1 || (argc == 2 && !strcmp(argv[1], "-p"))) {
        vars_print(flag);
        return 0;
    }
    for(i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        int n = eq ? (int)(eq - argv[i]) : (int)strlen(argv[i]);
        char name[WORD_LIMIT + 1];
        if(!is_name(argv[i], n) || n > WORD_LIMIT) {
            fprintf(stderr, "surfsh: %s: %s: not a name\n", cmd, argv[i]);
            status = STATUS_FAIL;
            continue;
        }
        memcpy(name, argv[i], (size_t)n);
        name[n] = 0;
        if(var_set(name, eq ? eq + 1 : NULL, flag) < 0) status = STATUS_FAIL;
    }
    return status;
}

static int b_export(int argc, char **argv) {
    return mark(argc, argv, V_EXPORT, "export");
}

static int b_readonly(int argc, char **argv) {
    return mark(argc, argv, V_READONLY, "readonly");
}

static int b_unset(int argc, char **argv) {
    int i = 1, status = 0, funcs = 0;
    if(i < argc && !strcmp(argv[i], "-v")) i++;
    else if(i < argc && !strcmp(argv[i], "-f")) {  /* 10/25/26: functions */
        funcs = 1;
        i++;
    }
    for(; i < argc; i++) {
        if(!is_name(argv[i], (int)strlen(argv[i]))) {
            fprintf(stderr, "surfsh: unset: %s: not a name\n", argv[i]);
            status = STATUS_FAIL;
        } else if(funcs) {
            func_unset(argv[i]);
        } else if(var_unset(argv[i]) < 0) {
            status = STATUS_FAIL;
        }
    }
    return status;
}

static int b_shift(int argc, char **argv) {
    char *end;
    long n = 1;
    if(argc > 2) {
        fprintf(stderr, "usage: shift [n]\n");
        return STATUS_USAGE;
    }
    if(argc == 2) {
        n = strtol(argv[1], &end, 10);
        if(!argv[1][0] || *end || n < 0) {
            fprintf(stderr, "surfsh: shift: %s is not a number\n", argv[1]);
            return STATUS_USAGE;
        }
    }
    if(pos_shift((int)n) < 0) {
        fprintf(stderr, "surfsh: shift: %ld: there are only %d\n", n, npos);
        return STATUS_FAIL;
    }
    return 0;
}

/**** help ****/

static void put_stdout(const char *s, int n) {
    fwrite(s, 1, (size_t)n, stdout);
}

static void row(const char *name, const char *args, const char *help) {
    printf("    %-9s %-14s %s\n", name, args, help);
}

static int is_program(const char *name, char *path, int size) {
    struct stat st;
    if(strchr(name, '/') || snprintf(path, (size_t)size, PROGRAM_DIR "/%s", name) >= size) return 0;
    return stat(path, &st) == 0 && S_ISREG(st.type);
}

/* help <name>: what runs under that name, and what else has it */
static int help_one(const char *name) {
    const struct builtin *b = builtin_find(name);
    const struct kcmd_info *k = kcmd_find(name);
    char path[PATH_MAX];
    int program = is_program(name, path, sizeof(path));
    if(b) printf("    %s %s\n    %s\n    (surfsh's own command)\n", b->name, b->args, b->help);
    else if(k) printf("    %s %s\n    %s\n    (a kernel command)\n", k->name, k->args, k->help);
    else if(program) printf("    %s: the program %s\n", name, path);
    else {
        fprintf(stderr, "surfsh: help: no command called %s\n", name);
        return STATUS_FAIL;
    }
    if(b && k) printf("    The kernel has a command of this name too; surfsh's own comes first.\n");
    if((b || k) && program) printf("    The program of this name runs by its path: %s\n", path);
    return 0;
}

static int b_help(int argc, char **argv) {
    static const char rule[] = "    ----------------------------";
    struct cands progs = { NULL, 0, 0 };
    char **names = NULL, shadowed[64] = "";
    int i, n = 0, size, width;
    if(argc > 2) {
        fprintf(stderr, "usage: help [command]\n");
        return STATUS_USAGE;
    }
    if(argc == 2) return help_one(argv[1]);

    printf("\n    surfsh %s, the SurfOS shell\n%s\n", SURFOS_VERSION, rule);
    printf("    surfsh's own commands:\n");
    for(i = 0; i < nbuiltins; i++) row(builtins[i].name, builtins[i].args, builtins[i].help);

    printf("\n    the kernel's commands:\n");
    if(!kcmd_bridge()) printf("    (none: this kernel does not offer its commands to programs)\n");
    else if(!kcmd_count()) printf("    (none)\n");
    for(i = 0; i < kcmd_count(); i++) {
        const struct kcmd_info *k = kcmd_entry(i);
        if(builtin_find(k->name)) {             /* surfsh's own comes first: noted below */
            if(strlen(shadowed) + strlen(k->name) + 3 < sizeof(shadowed))
                snprintf(shadowed + strlen(shadowed), sizeof(shadowed) - strlen(shadowed), "%s%s", shadowed[0] ? ", " : "", k->name);
            continue;
        }
        row(k->name, k->args, k->help);
    }
    if(shadowed[0]) printf("    (the kernel's %s as well: surfsh's own, above, are used)\n", shadowed);

    printf("\n    programs in %s:\n", var_get("PATH") ? var_get("PATH") : PROGRAM_DIR);
    if(program_names(&progs) < 0 || !progs.n) printf("    (none)\n");
    else if((names = malloc((size_t)progs.n * sizeof(char *)))) {
        const char *example = NULL;
        for(i = 0; i < progs.n; i++) {
            const char *p = progs.v[i].name;
            int hidden = builtin_find(p) || kcmd_find(p);
            names[n] = malloc(strlen(p) + 2);
            if(!names[n]) continue;
            strcpy(names[n], p);
            if(hidden) {
                strcat(names[n], "*");
                if(!example) example = p;
            }
            n++;
        }
        size = ttyctl(TTY_GETSIZE, 0);
        width = size > 0 ? (size >> 16) & 0xffff : 80;
        columns(names, n, width, "    ", "\n", put_stdout);
        if(example) printf("    * a command of this name runs first; %s/%s runs the program\n", PROGRAM_DIR, example);
        for(i = 0; i < n; i++) free(names[i]);
        free(names);
    }
    cands_free(&progs);

    printf("\n    A line: words with blanks between them. \"double\" or 'single' quotes keep\n"
           "    blanks and the characters below in a word; a \\ keeps the next character.\n"
           "      cmd < file     cmd reads the file (a kernel command reads the console)\n"
           "      cmd > file     what cmd prints goes into the file, which starts empty\n"
           "      cmd >> file    ... is added at the end of the file\n"
           "      cmd 2> file    its error messages go there; 2>&1: where 1 goes\n"
           "      cmd << END     cmd reads the lines that follow, up to a line END\n"
           "      a | b | c      what a prints, b reads; what b prints, c reads\n"
           "      a && b         b runs when a succeeded; a || b: when a failed\n"
           "      ! a            the status turned round: 0 when a failed\n"
           "      { a; b; }      a group: its redirections are for all of it\n"
           "      ( a; b )       the same in a new surfsh: a cd there changes nothing here\n"
           "      cmd &          cmd runs in the background as a job; the first prompt\n"
           "                     after it has ended says how it ended\n"
           "      Ctrl+Z         the command that runs stops: a job (jobs, fg, bg, kill %%1)\n"
           "      a ; b          one after the other\n"
           "      # comment      the rest of the line is not read\n"
           "      name=value     a variable; $name or ${name} is its value (in \"$name\"\n"
           "                     one word, whatever it holds); x=1 cmd: for cmd alone\n"
           "      export name    programs get the variable too, in their environment\n"
           "      ${x:-y} ${#x}  y when x is empty; x's length (${x%%.txt}: without .txt)\n"
           "      $(cmd)         what cmd prints (`cmd` too); $((n + 1)) arithmetic\n"
           "      *.txt  [ab]?   the names that match: * any characters, ? one, [ab] one\n"
           "                     of those; quoted (\"*\") a star is just a star\n"
           "      $? $$ $!       the status of the command before, this shell's pid, the\n"
           "                     last job's; $0 $1 $# \"$@\": the name, the arguments\n"
           "      if a; then b; elif c; then d; else e; fi   b when a succeeded, ...\n"
           "      while a; do b; done   b again while a succeeds (until a: until it does)\n"
           "      for x in w1 w2; do b; done   b with x each of the words in turn\n"
           "      case $x in a|b) c;; *) d;; esac   the first pattern that x matches\n"
           "      f() { b; }     a function: f runs b, with $1 ... its arguments\n"
           "      surfsh file    the commands of the file, a script (a first line\n"
           "                     #!/initrd/bin/surfsh lets it run by its name)\n"
           "    A line that ends with | or && (or inside quotes, an if, a loop, a group)\n"
           "    goes on in the next one, after the prompt %s.\n"
           "    Which command runs: a function or surfsh's own, then the kernel's, then a\n"
           "    program of PATH's directories (%s); a name with a / is a program's path.\n",
           use_unicode() ? "\xE2\x80\xA6 \xE2\x89\x8B>" : "... ~>", var_get("PATH") ? var_get("PATH") : PROGRAM_DIR);
    printf("\n    Keys: Left, Right, Home, End (^A ^E), Backspace, Delete; ^U clears the\n"
           "    line, ^K deletes to its end, ^W the word before the cursor; Up and Down:\n"
           "    the history; Tab completes a command or a path, a second Tab lists the\n"
           "    choices; ^L clears the screen, ^C drops the line, ^D on an empty line\n"
           "    leaves surfsh.\n");
    printf("\n    The prompt shows the current directory and the wave; a ! after the wave\n"
           "    says that the last command failed, an @ that this is a telnet session\n"
           "    (another computer's terminal; there the @ shows instead of a !). set\n"
           "    changes how it looks; %s is read at the start.\n", SURFSH_CONF);
    printf("%s\n    (C)%s %s.\n\n", rule, SURFOS_YEARS, SURFOS_AUTHOR);
    return 0;
}
