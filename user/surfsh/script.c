/*
SurfOS shell: surfsh
--------------------
File: script.c  Date: 10/25/26
--------------------
The builtins of scripts (exec.c runs them like the others; builtins.c lists them):

    :, true           nothing, status 0; false: status 1
    break [n]         leave the loop, or n loops (all of them when there are fewer)
    continue [n]      the loop's next round (the n-th loop's, from the innermost)
    return [n]        leave the function or the . file, with status n ($? when it is not given)
    local name[=value]...   the function's own variables: what they were comes back when it
                      returns (a local keeps the value it had until it is given another)
    eval [args]       the arguments, joined by blanks, run as a command line in this shell
    . file [args], source   the commands of the file in this shell, $1... the args while it runs;
                      a name without a / is looked for in PATH's directories, then here
    read [-r] [name...]     a line of input: split at IFS, a field to each name, the rest of the
                      line to the last one (REPLY without names); \ joins a line to the next and
                      keeps the character after it, unless -r. Status 1 at the end of the input.
    kernel name [args]      the kernel's command name, where surfsh has one of its own (test,
                      echo, cd, pwd, help, history, set) or, 10/25/26, a program of PATH's
                      directories comes first (ls, cat, cp, df, hexdump, mkdir, mv, rm, rmdir)
    time cmd [args]   10/25/26 (track A, A3): cmd runs in this shell as it would alone (its
                      words quoted again, so nothing is expanded twice), then "real n.nn s" on 2;
                      the status is cmd's

read takes one line and no more: from a terminal a line is one read; from a file it reads a
block and puts back what follows the line (lseek), so the next command reads on from there;
from a pipe one byte at a time.
*/

#include "surfsh.h"

int b_colon(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

int b_false(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return STATUS_FAIL;
}

/* the count of break, continue, return: n >= min, or -1 said */
static long count(const char *cmd, const char *arg, long min) {
    char *end;
    long n = strtol(arg, &end, 10);
    if(!*arg || *end || n < min) {
        fprintf(stderr, "surfsh: %s: %s is not a number%s\n", cmd, arg, min ? " of loops" : "");
        return -1;
    }
    return n;
}

static int loop_word(int argc, char **argv, int *what) {
    long n = 1;
    if(argc > 2) {
        fprintf(stderr, "usage: %s [n]\n", argv[0]);
        return STATUS_USAGE;
    }
    if(!loop_depth) {
        fprintf(stderr, "surfsh: %s: not in a loop\n", argv[0]);
        return STATUS_FAIL;
    }
    if(argc == 2 && (n = count(argv[0], argv[1], 1)) < 0) return STATUS_USAGE;
    *what = n > loop_depth ? loop_depth : (int)n;
    return 0;
}

int b_break(int argc, char **argv) {
    return loop_word(argc, argv, &break_count);
}

int b_continue(int argc, char **argv) {
    return loop_word(argc, argv, &continue_count);
}

int b_return(int argc, char **argv) {
    long n = last_status;
    if(argc > 2) {
        fprintf(stderr, "usage: return [n]\n");
        return STATUS_USAGE;
    }
    if(!func_depth && !dot_depth) {
        fprintf(stderr, "surfsh: return: not in a function or a . file\n");
        return STATUS_FAIL;
    }
    if(argc == 2 && (n = count("return", argv[1], 0)) < 0) return STATUS_USAGE;
    returning = 1;
    return_status = (int)(n & 0xff);
    return return_status;
}

int b_local(int argc, char **argv) {
    int i, status = 0;
    if(!func_depth) {
        fprintf(stderr, "surfsh: local: not in a function\n");
        return STATUS_FAIL;
    }
    for(i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        int n = eq ? (int)(eq - argv[i]) : (int)strlen(argv[i]);
        char name[WORD_LIMIT + 1];
        if(!is_name(argv[i], n) || n > WORD_LIMIT) {
            fprintf(stderr, "surfsh: local: %s: not a name\n", argv[i]);
            status = STATUS_FAIL;
            continue;
        }
        memcpy(name, argv[i], (size_t)n);
        name[n] = 0;
        if(local_keep(name, local_base) < 0 || (eq && var_set(name, eq + 1, 0) < 0)) status = STATUS_FAIL;
    }
    return status;
}

int b_eval(int argc, char **argv) {
    struct sbuf b = { NULL, 0, 0 };
    int i, status;
    for(i = 1; i < argc; i++) {
        if(i > 1) sb_putc(&b, ' ');
        sb_puts(&b, argv[i]);
    }
    status = b.len ? run_text(sb_str(&b), NULL) : 0;   /* 10/25/26: $? there is the one before it */
    sb_free(&b);
    return status;
}

/* the file . runs: a path as it is, any other name in PATH's directories, then here */
static int dot_file(const char *name, char *path, int size) {
    struct stat st;
    const char *at = var_get("PATH");
    if(strchr(name, '/')) {
        snprintf(path, (size_t)size, "%s", name);
        return stat(path, &st) == 0 && S_ISREG(st.type) ? 0 : -1;
    }
    for(at = at ? at : PROGRAM_DIR; ; ) {
        const char *colon = strchr(at, ':');
        int len = colon ? (int)(colon - at) : (int)strlen(at);
        if(snprintf(path, (size_t)size, "%.*s/%s", len ? len : 1, len ? at : ".", name) < size &&
           stat(path, &st) == 0 && S_ISREG(st.type))
            return 0;
        if(!colon) break;
        at = colon + 1;
    }
    snprintf(path, (size_t)size, "%s", name);
    return stat(path, &st) == 0 && S_ISREG(st.type) ? 0 : -1;
}

int b_dot(int argc, char **argv) {
    struct sbuf text = { NULL, 0, 0 };
    struct pos_saved ps;
    char path[PATH_MAX], buf[1024];
    ssize_t n;
    int fd, status;
    if(argc < 2) {
        fprintf(stderr, "usage: %s file [args]\n", argv[0]);
        return STATUS_USAGE;
    }
    if(dot_file(argv[1], path, sizeof(path)) < 0 || (fd = open(path, O_RDONLY)) < 0) {
        fprintf(stderr, "surfsh: %s: %s: no such file\n", argv[0], argv[1]);
        return STATUS_FAIL;
    }
    while((n = read(fd, buf, sizeof(buf))) > 0) {
        if(text.len + n > TEXT_LIMIT) {
            fprintf(stderr, "surfsh: %s: more than %d bytes\n", path, TEXT_LIMIT);
            close(fd);
            sb_free(&text);
            return STATUS_FAIL;
        }
        sb_put(&text, buf, (int)n);
    }
    close(fd);
    if(dot_depth + func_depth >= FUNC_DEPTH_LIMIT) {
        fprintf(stderr, "surfsh: %s: . files and functions inside each other: at most %d\n", path, FUNC_DEPTH_LIMIT);
        sb_free(&text);
        return STATUS_FAIL;
    }
    if(argc > 2) {
        pos_save(&ps);
        pos_set(argc - 2, argv + 2);
    }
    dot_depth++;
    status = run_text(sb_str(&text), path);
    if(returning) {
        returning = 0;
        status = return_status;
    }
    dot_depth--;
    if(argc > 2) pos_restore(&ps);
    sb_free(&text);
    return status;
}

/**** read ****/

/* One line of descriptor 0 into line (its \n not kept): 0, 1 at the end of the input (what came
   before it is in line), -1 said. Never more than the line: see the top. */
static int read_line(struct sbuf *line, int raw, int *cut) {
    char buf[512];
    int tty = isatty(0), seekable = !tty && lseek(0, 0, SEEK_CUR) >= 0;
    *cut = 0;
    for(;;) {
        ssize_t n;
        int i;
        n = read(0, buf, tty || seekable ? sizeof(buf) : 1);
        if(n < 0) {
            if(errno == EINTR) *cut = 1;        /* Ctrl+C at the terminal, which said ^C */
            else fprintf(stderr, "surfsh: read: %s\n", strerror(errno));
            return -1;
        }
        if(!n) return 1;
        for(i = 0; i < n; i++) {
            if(buf[i] == '\n') {
                if(seekable && i + 1 < n) lseek(0, -(off_t)(n - i - 1), SEEK_CUR);
                if(!raw && line->len && line->s[line->len - 1] == '\\' &&
                   !(line->len > 1 && line->s[line->len - 2] == '\\')) {
                    line->s[--line->len] = 0;   /* \ and a newline: the line goes on */
                    if(seekable && i + 1 < n) break;
                    continue;
                }
                return 0;
            }
            if(line->len >= TEXT_LIMIT) {
                fprintf(stderr, "surfsh: read: a line of more than %d bytes\n", TEXT_LIMIT);
                return -1;
            }
            sb_putc(line, buf[i]);
        }
    }
}

static int in_set(const char *set, char c) {
    return c && strchr(set, c) != NULL;
}

static int ifs_blank(const char *set, char c) {
    return in_set(set, c) && (c == ' ' || c == '\t' || c == '\n');
}

int b_read(int argc, char **argv) {
    struct sbuf line = { NULL, 0, 0 }, field = { NULL, 0, 0 };
    const char *set = ifs(), *s;
    char *reply[] = { "REPLY" }, **names = argv + 1;
    char *q;
    int raw = 0, nnames, r, i, k, len, cut, status = 0;
    if(argc > 1 && !strcmp(argv[1], "-r")) {
        raw = 1;
        names++;
    }
    nnames = argc - (int)(names - argv);
    for(i = 0; i < nnames; i++) {
        if(!is_name(names[i], (int)strlen(names[i]))) {
            fprintf(stderr, "surfsh: read: %s: not a name\n", names[i]);
            return STATUS_USAGE;
        }
    }
    if(!nnames) {
        names = reply;
        nnames = 1;
    }
    fflush(stdout);
    r = read_line(&line, raw, &cut);
    if(r < 0) {
        if(cut) interrupt_said = 1;
        sb_free(&line);
        return cut ? STATUS_INTERRUPTED : STATUS_FAIL;
    }
    /* q[i]: s[i] was after a \ (without -r): itself, never a separator */
    if(!line.s) {                               /* the end, nothing read: the names become empty */
        sb_putc(&line, 0);
        line.len = 0;
    }
    s = sb_str(&line);
    len = line.len;
    q = xmalloc((size_t)len + 1);
    for(i = k = 0; i < len; i++) {
        if(!raw && s[i] == '\\' && i + 1 < len) {
            line.s[k] = s[++i];
            q[k++] = 1;
        } else {
            line.s[k] = s[i];
            q[k++] = 0;
        }
    }
    line.s[k] = 0;
    len = k;
    s = line.s;
    i = 0;
    while(i < len && !q[i] && ifs_blank(set, s[i])) i++;
    for(k = 0; k < nnames; k++) {
        field.len = 0;
        if(field.s) field.s[0] = 0;
        if(k == nnames - 1) {                   /* the last name: the rest, its blanks at the end off */
            int end = len;
            while(end > i && !q[end - 1] && ifs_blank(set, s[end - 1])) end--;
            sb_put(&field, s + i, end - i);
            i = len;
        } else {
            while(i < len && (q[i] || !in_set(set, s[i]))) sb_putc(&field, s[i++]);
            while(i < len && !q[i] && ifs_blank(set, s[i])) i++;
            if(i < len && !q[i] && in_set(set, s[i])) {    /* one other IFS character, and blanks after it */
                i++;
                while(i < len && !q[i] && ifs_blank(set, s[i])) i++;
            }
        }
        if(var_set(names[k], sb_str(&field), 0) < 0) status = STATUS_FAIL;
    }
    free(q);
    sb_free(&field);
    sb_free(&line);
    return status ? status : r;
}

/**** kernel ****/

int b_kernel(int argc, char **argv) {
    int found, i;
    if(argc < 2) {
        fprintf(stderr, "usage: kernel name [args]\n");
        return STATUS_USAGE;
    }
    if(!kcmd_find(argv[1])) {
        fprintf(stderr, "surfsh: kernel: %s is not one of the kernel's commands\n", argv[1]);
        return STATUS_NOTFOUND;
    }
    for(i = 1; i < argc; i++) {
        if(strlen(argv[i]) > WORD_LIMIT) {
            fprintf(stderr, "surfsh: kernel: an argument of %u bytes: at most %d\n", (unsigned)strlen(argv[i]), WORD_LIMIT);
            return STATUS_USAGE;
        }
    }
    return run_kernel(argv + 1, &found);
}

/**** time (10/25/26, track A: how long a screenful takes) ****/

int b_time(int argc, char **argv) {
    struct sbuf b = { NULL, 0, 0 };
    unsigned int t0, ms;
    int i, status = 0;
    if(argc < 2) {
        fprintf(stderr, "usage: time cmd [args]\n");
        return STATUS_USAGE;
    }
    for(i = 1; i < argc; i++) {
        if(i > 1) sb_putc(&b, ' ');
        if(argv[i][0]) sb_quote(&b, argv[i]);
        else sb_puts(&b, "''");                 /* an empty word stays a word */
    }
    t0 = uptime();
    status = run_text(sb_str(&b), NULL);
    ms = uptime() - t0;
    sb_free(&b);
    fflush(stdout);
    fprintf(stderr, "real %u.%02u s\n", ms / 1000, ms % 1000 / 10);
    return status;
}
