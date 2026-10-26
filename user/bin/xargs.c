/*
SurfOS user program: xargs
--------------------
File: xargs.c   Date: 10/25/26
--------------------
xargs [-n N] [-t] [-0] [command [argument...]]: runs the command (echo without one) with the
words of standard input after its own arguments, as many as one run takes (a program takes
SPAWN_MAX_ARGS arguments in SPAWN_ARG_BYTES, each at most 255 bytes: surfos/spawn.h), and
again until the words run out (track A, A9). -n N: N words at most in a run.

Words are separated by blanks and newlines; '...' and "..." keep blanks in a word and a
backslash the next character; -0 takes words that end with a NUL instead, as they are (find
-print0). No words: nothing runs.

The command is a program (a name with a / its path, any other looked for in PATH, /initrd/bin
when it is not set) or else a kernel command, as surfsh finds them; a program's standard
input is /dev/null and its output xargs's. -t says each command on the standard error before
it runs. The status: 0; 123 when a run failed; 124 when one was killed (xargs stops there);
126 when the command could not start; 127 when it is nowhere.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <surf.h>

#define WORD_MAX 255

static char *args[SPAWN_MAX_ARGS + 1];
static int nfixed, nargs;           /* the command's own arguments; all of them so far */
static size_t bytes;                /* theirs, terminators included */
static int per_run;                 /* -n; 0: as many as fit */
static int trace, nul_ends;
static char program[512];           /* the command's path; "" for a kernel command */
static int devnull = -1;
static int status;

/* the command's program, on PATH unless it has a /: 0, or -1 when there is none (a kernel command, maybe) */
static int find_program(const char *name) {
    const char *path = getenv("PATH"), *p, *colon;
    struct stat st;
    if(strchr(name, '/')) {
        snprintf(program, sizeof(program), "%s", name);
        return 0;
    }
    if(!path) path = "/initrd/bin";
    for(p = path; ; p = colon + 1) {
        size_t len;
        colon = strchr(p, ':');
        len = colon ? (size_t)(colon - p) : strlen(p);
        if(len && len + strlen(name) + 2 <= sizeof(program)) {
            memcpy(program, p, len);
            snprintf(program + len, sizeof(program) - len, "/%s", name);
            if(stat(program, &st) == 0 && S_ISREG(st.st_mode)) return 0;
        }
        if(!colon) break;
    }
    program[0] = 0;
    return -1;
}

/* the command with the words collected; 1 when xargs should stop */
static int run(void) {
    struct spawnx o;
    struct waitx w;
    int pid, i, r;
    args[nargs] = NULL;
    if(trace) {
        for(i = 0; i < nargs; i++) fprintf(stderr, i ? " %s" : "%s", args[i]);
        fputc('\n', stderr);
    }
    fflush(stdout);
    if(!program[0]) {                                   /* a kernel command, in this process */
        r = kcmd(args);
        if(r < 0) {
            fprintf(stderr, "xargs: %s: %s\n", args[0], errno == ENOENT ? "not found" : strerror(errno));
            status = errno == ENOENT ? 127 : 126;
            return 1;
        }
        if(r) status = 123;
    } else {
        memset(&o, 0, sizeof(o));
        o.fds[0] = devnull >= 0 ? devnull : SPAWN_CLOSED;
        o.fds[1] = 1;
        o.fds[2] = 2;
        o.fds[3] = SPAWN_CLOSED;
        o.envp = environ;
        pid = spawnx(program, args, &o);
        if(pid < 0) {
            fprintf(stderr, "xargs: %s: %s\n", program, strerror(errno));
            status = errno == ENOENT ? 127 : 126;
            return 1;
        }
        if(waitx(pid, &w, 0) < 0) {
            fprintf(stderr, "xargs: wait: %s\n", strerror(errno));
            status = 1;
            return 1;
        }
        if(w.how != WX_EXITED) {
            fprintf(stderr, "xargs: %s: killed\n", args[0]);
            status = 124;
            return 1;
        }
        if(w.status) status = 123;
    }
    for(i = nfixed; i < nargs; i++) free(args[i]);
    nargs = nfixed;
    bytes = 0;
    for(i = 0; i < nfixed; i++) bytes += strlen(args[i]) + 1;
    return 0;
}

/* one more word; 1 when xargs should stop */
static int add(const char *word, size_t len) {
    if(len > WORD_MAX) {
        fprintf(stderr, "xargs: a word longer than %d bytes, which a program cannot take\n", WORD_MAX);
        status = 1;
        return 1;
    }
    /* 10/25/26 (the programs' review, U1): >=, as main() leaves room for a word at least (args[] holds the NULL too) */
    if(nargs > nfixed && (nargs >= SPAWN_MAX_ARGS || bytes + len + 1 > SPAWN_ARG_BYTES) && run()) return 1;
    args[nargs] = malloc(len + 1);
    if(!args[nargs]) {
        fprintf(stderr, "xargs: out of memory\n");
        status = 1;
        return 1;
    }
    memcpy(args[nargs], word, len);
    args[nargs++][len] = 0;
    bytes += len + 1;
    return per_run && nargs - nfixed == per_run ? run() : 0;
}

/* the words of standard input, each added as it ends */
static void read_words(void) {
    char word[WORD_MAX + 2];
    size_t len = 0;
    int c, quote = 0, in_word = 0;
    while((c = getchar()) != EOF) {
        if(nul_ends) {
            if(c) {
                if(len < sizeof(word)) word[len++] = (char)c;
                continue;
            }
        } else if(quote) {
            if(c != quote) {
                if(len < sizeof(word)) word[len++] = (char)c;
                continue;
            }
            quote = 0;
            continue;
        } else if(c == '\'' || c == '"') {
            quote = c;
            in_word = 1;
            continue;
        } else if(c == '\\') {
            c = getchar();
            if(c == EOF) break;
            if(len < sizeof(word)) word[len++] = (char)c;
            in_word = 1;
            continue;
        } else if(c != ' ' && c != '\t' && c != '\n') {
            if(len < sizeof(word)) word[len++] = (char)c;
            in_word = 1;
            continue;
        } else if(!in_word) continue;
        if(add(word, len)) return;
        len = 0;
        in_word = 0;
    }
    if(quote) {
        fprintf(stderr, "xargs: a quote (%c) is not closed\n", quote);
        status = 1;
        return;
    }
    if((len || in_word) && add(word, len)) return;
    if(nargs > nfixed) run();
}

int main(int argc, char **argv) {
    int i = 1;
    char *end;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if(!strcmp(argv[i], "-t")) trace = 1;
        else if(!strcmp(argv[i], "-0")) nul_ends = 1;
        else if(!strncmp(argv[i], "-n", 2) && (argv[i][2] || i + 1 < argc)) {
            const char *n = argv[i][2] ? argv[i] + 2 : argv[++i];
            per_run = (int)strtol(n, &end, 10);
            if(*end || per_run < 1) {
                fprintf(stderr, "xargs: %s: not a number of words\n", n);
                return 2;
            }
        } else {
            fprintf(stderr, "usage: xargs [-n N] [-t] [-0] [command [argument...]]\n");
            return 2;
        }
    }
    if(i == argc) args[nargs++] = "echo";
    for(; i < argc && nargs < SPAWN_MAX_ARGS; i++) args[nargs++] = argv[i];
    if(i < argc || nargs >= SPAWN_MAX_ARGS) {           /* 10/25/26 (the review, U1): no room left for a word (the
                                                           kernel gives a program 128 arguments, its name the first:
                                                           a guard, should that change) */
        fprintf(stderr, "xargs: the command takes all %d arguments a run has\n", SPAWN_MAX_ARGS);
        return 2;
    }
    nfixed = nargs;
    for(i = 0; i < nfixed; i++) bytes += strlen(args[i]) + 1;
    if(bytes > SPAWN_ARG_BYTES / 2) {
        fprintf(stderr, "xargs: the command takes half of the %d bytes a run has\n", SPAWN_ARG_BYTES);
        return 2;
    }
    find_program(args[0]);                              /* none: a kernel command, or nothing (127 at the first run) */
    devnull = open("/dev/null", O_RDONLY);
    read_words();
    return status;
}
