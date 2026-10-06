/*
SurfOS shell: surfsh
--------------------
File: builtins.c  Date: 10/6/26
--------------------
surfsh's own commands. They come before the kernel's commands of the same names
(the kernel's help, echo, cd and pwd are not used). echo is one of them, so
"echo text > file" needs nothing from the kernel. Their output can be
redirected like a program's (run.c puts the file on descriptor 1).

    cd [dir]          no dir: /; "cd -": the previous directory (printed)
    pwd               the current directory
    echo [-n] [-e]    the words; -n: no newline; -e: \n \r \t \\ are the characters they name
    exit [n]          leave with status n, 0 when it is not given
    help [command]    this shell's commands, the kernel's, the programs; or one of them
    history           the lines typed before, oldest first
    set [key [value]] the settings (main.c); "set key = value" as surfsh.conf writes it
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

const struct builtin builtins[] = {
    { "cd",      "[dir]",         "change the directory (none: /, -: the one before)", b_cd },
    { "pwd",     "",              "print the current directory", b_pwd },
    { "echo",    "[-n] [-e] ...", "print the words (-n: no newline, -e: \\n \\r \\t \\\\)", b_echo },
    { "history", "",              "the lines typed before (Up and Down recall them)", b_history },
    { "set",     "[key [value]]", "show or change the settings (set alone: all)", b_set },
    { "help",    "[command]",     "this list, or one command", b_help },
    { "exit",    "[n]",           "leave surfsh with status n (0 when not given)", b_exit },
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
        if(!oldpwd[0]) {
            fprintf(stderr, "surfsh: cd: no previous directory\n");
            return STATUS_FAIL;
        }
        dir = oldpwd;
        back = 1;
    }
    if(!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
    if(chdir(dir) < 0) {
        fprintf(stderr, "surfsh: cd: %s: %s\n", dir, strerror(errno));
        return STATUS_FAIL;
    }
    if(cwd[0]) strcpy(oldpwd, cwd);         /* after chdir: dir may be oldpwd itself */
    if(back && getcwd(cwd, sizeof(cwd))) printf("%s\n", cwd);
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
    if(fflush(stdout)) {                    /* a redirected echo on a full disk */
        clearerr(stdout);
        fprintf(stderr, "surfsh: echo: %s\n", strerror(errno));
        return STATUS_FAIL;
    }
    return 0;
}

/**** exit, history, set ****/

static int b_exit(int argc, char **argv) {
    long n = 0;
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
    exit_code = (int)n;
    return 0;
}

static int b_history(int argc, char **argv) {
    (void)argv;
    if(argc > 1) {
        fprintf(stderr, "usage: history\n");
        return STATUS_USAGE;
    }
    return history_print();
}

static int b_set(int argc, char **argv) {
    char why[120];
    const char *value = argc == 3 ? argv[2] : argc == 4 && !strcmp(argv[2], "=") ? argv[3] : NULL;
    if(argc == 1) return settings_show(NULL), 0;
    if(argc == 2) {
        if(settings_show(argv[1]) == 0) return 0;
        fprintf(stderr, "surfsh: set: no setting called %s (set alone lists them)\n", argv[1]);
        return STATUS_USAGE;
    }
    if(!value) {
        fprintf(stderr, "usage: set [key [value]]\n");
        return STATUS_USAGE;
    }
    if(settings_apply(argv[1], value, why, sizeof(why))) {
        fprintf(stderr, "surfsh: set: %s\n", why);
        return STATUS_USAGE;
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

    printf("\n    programs in %s:\n", PROGRAM_DIR);
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
        if(example) printf("    * a command of the same name runs first; %s/%s runs the program\n", PROGRAM_DIR, example);
        for(i = 0; i < n; i++) free(names[i]);
        free(names);
    }
    cands_free(&progs);

    printf("\n    A line: words with blanks between them. \"double\" or 'single' quotes keep blanks\n"
           "    and the characters below in a word, and a \\ keeps the character after it.\n"
           "      cmd < file     the program reads the file\n"
           "      cmd > file     what cmd prints goes into the file, which starts empty\n"
           "      cmd >> file    ... is added at the end of the file\n"
           "      cmd &          the program runs in the background\n"
           "      cmd1 ; cmd2    one after the other\n"
           "      # comment      the rest of the line is not read\n"
           "    There is no $ and no *: no variables and no wildcards, every word is as typed.\n"
           "    Which command runs: surfsh's own, then the kernel's, then a program in %s;\n"
           "    a name with a / is the path of a program.\n", PROGRAM_DIR);
    printf("\n    Keys: Left, Right, Home, End (^A ^E), Backspace, Delete; ^U clears the line,\n"
           "    ^K deletes to its end, ^W the word before the cursor; Up and Down: the history;\n"
           "    Tab completes a command or a path, a second Tab lists the choices; ^L clears\n"
           "    the screen, ^C drops the line, ^D on an empty line leaves surfsh.\n");
    printf("\n    The prompt shows the current directory and the wave; a ! after the wave says\n"
           "    that the last command failed. set changes how it looks; %s\n"
           "    is read at the start.\n", SURFSH_CONF);
    printf("%s\n    (C)%s %s.\n\n", rule, SURFOS_YEARS, SURFOS_AUTHOR);
    return 0;
}
