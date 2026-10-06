/* sh: a small user-mode shell. Runs programs from /initrd/bin or by path, waits for them.
   10/5/26: redirection (< file, > file, >> file) through spawnfd(), "quoted words",
   and the built-in commands cd, pwd, echo and exit; a trailing & runs in the background. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <surf.h>

#define MAXARGS 32
#define LINE_MAX_LEN 256

struct cmd {
    char *argv[MAXARGS + 1];
    int argc;
    char *in, *out;             /* redirection, NULL for none */
    int append;                 /* >> rather than > */
    int bg;                     /* & */
    char words[LINE_MAX_LEN * 2];
};

/* split a line into words and redirections; 0 for an empty line, -1 after a message */
static int parse(const char *p, struct cmd *c) {
    char *w = c->words, *end = c->words + sizeof(c->words) - 1;
    char **target = NULL;
    c->argc = 0;
    c->in = c->out = NULL;
    c->append = c->bg = 0;
    for(;;) {
        char *start;
        while(*p == ' ' || *p == '\t') p++;
        if(!*p) break;
        if(*p == '<' || *p == '>') {
            if(target) { fprintf(stderr, "sh: a file name must follow %c\n", *p); return -1; }
            if(*p == '<') target = &c->in;
            else { target = &c->out; c->append = (p[1] == '>'); }
            p += (p[0] == '>' && p[1] == '>') ? 2 : 1;
            continue;
        }
        start = w;
        while(*p && *p != ' ' && *p != '\t' && *p != '<' && *p != '>') {
            if(*p == '"' || *p == '\'') {
                char q = *p++;
                while(*p && *p != q) { if(w < end) *w++ = *p; p++; }
                if(!*p) { fprintf(stderr, "sh: unterminated %c\n", q); return -1; }
                p++;
            } else {
                if(w < end) *w++ = *p;
                p++;
            }
        }
        if(w >= end) { fprintf(stderr, "sh: line too long\n"); return -1; }
        *w++ = 0;
        if(target) { *target = start; target = NULL; }
        else if(c->argc < MAXARGS) c->argv[c->argc++] = start;
        else { fprintf(stderr, "sh: too many arguments\n"); return -1; }
    }
    if(target) { fprintf(stderr, "sh: a file name must follow < or >\n"); return -1; }
    if(c->argc > 1 && !strcmp(c->argv[c->argc - 1], "&")) { c->bg = 1; c->argc--; }
    c->argv[c->argc] = NULL;
    if(!c->argc && (c->in || c->out)) { fprintf(stderr, "sh: a redirection needs a command\n"); return -1; }
    return c->argc;
}

/* the redirection files: fds[0] input, fds[1] output, -1 for the console */
static int open_files(const struct cmd *c, int fds[3]) {
    fds[0] = fds[1] = fds[2] = -1;
    if(c->in && (fds[0] = open(c->in, O_RDONLY)) < 0) {
        fprintf(stderr, "sh: %s: %s\n", c->in, strerror(errno));
        return -1;
    }
    if(c->out && (fds[1] = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC))) < 0) {
        fprintf(stderr, "sh: %s: %s\n", c->out, strerror(errno));
        if(fds[0] >= 0) close(fds[0]);
        return -1;
    }
    return 0;
}

static void close_files(int fds[3]) {
    int i;
    for(i = 0; i < 3; i++) if(fds[i] >= 0) close(fds[i]);
}

/* echo's words to out, with -n (no newline) and -e (\n \r \t \\) as the echo program has them */
static void echo(struct cmd *c, int out) {
    char buf[LINE_MAX_LEN * 2];
    int i = 1, n = 0, newline = 1, escapes = 0;
    const char *s;
    for(; i < c->argc && (!strcmp(c->argv[i], "-n") || !strcmp(c->argv[i], "-e")); i++) {
        if(c->argv[i][1] == 'n') newline = 0;
        else escapes = 1;
    }
    for(; i < c->argc; i++) {
        for(s = c->argv[i]; *s && n < (int)sizeof(buf) - 3; s++) {
            char ch = *s;
            if(escapes && ch == '\\' && s[1]) {
                ch = *++s;
                ch = ch == 'n' ? '\n' : ch == 'r' ? '\r' : ch == 't' ? '\t' : ch;
                if(ch == *s && ch != '\\') buf[n++] = '\\';     /* not one of ours: keep the backslash */
            }
            buf[n++] = ch;
        }
        if(i < c->argc - 1 && n < (int)sizeof(buf) - 2) buf[n++] = ' ';
    }
    if(newline) buf[n++] = '\n';
    write(out, buf, (size_t)n);
}

/* cd, pwd, echo: 1 when the command was one of them */
static int builtin(struct cmd *c) {
    int fds[3], out;
    char cwd[PATH_MAX];
    if(!strcmp(c->argv[0], "cd")) {
        if(chdir(c->argc > 1 ? c->argv[1] : "/") < 0) fprintf(stderr, "cd: %s: %s\n", c->argc > 1 ? c->argv[1] : "/", strerror(errno));
        return 1;
    }
    if(strcmp(c->argv[0], "pwd") && strcmp(c->argv[0], "echo")) return 0;
    if(open_files(c, fds)) return 1;
    out = fds[1] >= 0 ? fds[1] : 1;
    fflush(stdout);
    if(!strcmp(c->argv[0], "pwd")) {
        if(getcwd(cwd, sizeof(cwd))) dprintf(out, "%s\n", cwd);
        else fprintf(stderr, "pwd: %s\n", strerror(errno));
    } else {
        echo(c, out);
    }
    close_files(fds);
    return 1;
}

static void run(struct cmd *c) {
    char path[PATH_MAX];
    struct stat st;
    int pid, status = 0, fds[3];
    if(strchr(c->argv[0], '/')) snprintf(path, sizeof(path), "%s", c->argv[0]);
    else snprintf(path, sizeof(path), "/initrd/bin/%s", c->argv[0]);
    if(stat(path, &st) < 0 || !S_ISREG(st.type)) { fprintf(stderr, "sh: %s: command not found\n", c->argv[0]); return; }
    if(open_files(c, fds)) return;
    fflush(stdout);
    pid = (c->in || c->out) ? spawnfd(path, c->argv, fds) : spawn(path, c->argv);
    if(pid < 0) fprintf(stderr, "sh: %s: %s\n", c->argv[0], strerror(errno));
    close_files(fds);                   /* the child has its own references */
    if(pid < 0) return;
    if(c->bg) { printf("[%d] started\n", pid); return; }
    if(wait(pid, &status) == pid && status) printf("[exit status %d]\n", status);
}

int main(int argc, char **argv) {
    static struct cmd c;
    char line[LINE_MAX_LEN], cwd[PATH_MAX];
    (void)argc;
    (void)argv;
    printf("SurfOS user shell (pid %d). Programs live in /initrd/bin; 'exit' leaves.\n", getpid());
    for(;;) {
        if(!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
        printf("%s $ ", cwd);
        if(readline(line, sizeof(line)) < 0) break;
        if(parse(line, &c) <= 0) continue;
        if(!strcmp(c.argv[0], "exit")) return c.argc > 1 ? atoi(c.argv[1]) : 0;
        if(!builtin(&c)) run(&c);
    }
    return 0;
}
