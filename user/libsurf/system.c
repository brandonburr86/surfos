/*
SurfOS user library: commands through the shell
--------------------
File: system.c  Date: 11/01/26
--------------------
system() and popen() run a command line with /initrd/bin/surfsh -c, so that kernel commands,
pipelines and the rest work as they do at the prompt, with spawnx(), as nothing here can
fork. The child gets only the descriptors named below, so one popen()'s pipe never leaks into
the next command, which POSIX has to take care to close.

An argument is at most 255 bytes (surfos/spawn.h). Longer text, up to what a pipe holds, goes
as a surfsh gives it to a new surfsh, on descriptor 3 (surfsh --sub, user/surfsh/main.c): the whole
text fits in the pipe before the child starts, so nothing waits and nothing can be broken. Longer
than that is E2BIG.

system() and pclose() answer the shell's exit status, 0 to 255, not a wait status: SurfOS has no
WEXITSTATUS. A command that was killed gives 137, one that Ctrl+C ended 130, as surfsh's $? says.
Both flush stdio first, so that what this program wrote comes before what the command does.
*/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <surf.h>

#define SHELL "/initrd/bin/surfsh"
#define ARG_MAX_BYTES 255               /* surfos/spawn.h: each argument */

/* the streams popen() made, and their commands; a process has FOPEN_MAX descriptors in all */
static struct {
    FILE *f;
    int pid;
} commands[FOPEN_MAX];

static int fd_or_closed(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? fd : SPAWN_CLOSED;
}

/* pipe(), with the ends clear of 0, 1 and 2: a program that closed one of those has the end there, and would hand it to its
   command as that descriptor: a command reading the far end of its own output waits for ever */
static int high_pipe(int p[2]) {
    int i, k;
    if(pipe(p) < 0) return -1;
    for(i = 0; i < 2; i++) {
        struct stat st;
        if(p[i] > 2) continue;
        for(k = 3; k < FOPEN_MAX; k++) if(fstat(k, &st) < 0) break;
        if(k == FOPEN_MAX || dup2(p[i], k) < 0) {
            close(p[0]);
            close(p[1]);
            errno = EMFILE;
            return -1;
        }
        close(p[i]);
        p[i] = k;
    }
    return 0;
}

/* surfsh on cmd, with in and out as its standard input and output (-1: this program's own): its pid, or -1 */
static int start(const char *cmd, int in, int out) {
    char pid[16];
    char *argv[7];
    struct spawnx o;
    size_t n = strlen(cmd);
    int p[2] = { -1, -1 }, child, e;
    memset(&o, 0, sizeof(o));
    o.fds[0] = in >= 0 ? in : fd_or_closed(0);
    o.fds[1] = out >= 0 ? out : fd_or_closed(1);
    o.fds[2] = fd_or_closed(2);
    o.fds[3] = SPAWN_CLOSED;
    o.envp = environ;
    argv[0] = "surfsh";
    if(n <= ARG_MAX_BYTES) {
        argv[1] = "-c";
        argv[2] = (char *)cmd;
        argv[3] = NULL;
    } else {
        if(n > PIPE_SIZE) {
            errno = E2BIG;
            return -1;
        }
        if(high_pipe(p) < 0) return -1;
        if(write(p[1], cmd, n) != (ssize_t)n) {
            e = errno;
            close(p[0]);
            close(p[1]);
            errno = e;
            return -1;
        }
        close(p[1]);
        snprintf(pid, sizeof(pid), "%d", (int)getpid());
        o.fds[3] = p[0];
        argv[1] = "--sub";
        argv[2] = "0";                  /* its $? */
        argv[3] = pid;                  /* its $$ */
        argv[4] = "0";                  /* its $! */
        argv[5] = "surfsh";             /* its $0 */
        argv[6] = NULL;
    }
    child = spawnx(SHELL, argv, &o);
    e = errno;
    if(p[0] >= 0) close(p[0]);
    errno = e;
    return child;
}

/* the end of child: its exit status */
static int finish(int child) {
    struct waitx w;
    int r;
    while((r = waitx(child, &w, 0)) < 0 && errno == EINTR);
    if(r != child) return -1;
    if(w.how == WX_EXITED) return w.status & 0xff;
    return w.how == WX_INTERRUPTED ? 130 : 137;
}

int system(const char *cmd) {
    int child;
    if(!cmd) {                          /* is there a shell? */
        struct stat st;
        return stat(SHELL, &st) == 0;
    }
    fflush(NULL);
    child = start(cmd, -1, -1);
    return child < 0 ? -1 : finish(child);
}

FILE *popen(const char *cmd, const char *mode) {
    int p[2], child, i, e, reading = mode[0] == 'r';
    FILE *f;
    if((mode[0] != 'r' && mode[0] != 'w') || (mode[1] && mode[1] != 'e')) {
        errno = EINVAL;
        return NULL;
    }
    for(i = 0; i < FOPEN_MAX && commands[i].f; i++);
    if(i == FOPEN_MAX) {
        errno = EMFILE;
        return NULL;
    }
    if(high_pipe(p) < 0) return NULL;
    fflush(NULL);
    child = reading ? start(cmd, -1, p[1]) : start(cmd, p[0], -1);
    e = errno;
    close(reading ? p[1] : p[0]);
    if(child < 0) {
        close(reading ? p[0] : p[1]);
        errno = e;
        return NULL;
    }
    f = fdopen(reading ? p[0] : p[1], reading ? "r" : "w");
    if(!f) {                            /* no memory: the command finds its pipe closed, and its end is waited for */
        e = errno;
        close(reading ? p[0] : p[1]);
        finish(child);
        errno = e;
        return NULL;
    }
    commands[i].f = f;
    commands[i].pid = child;
    return f;
}

int pclose(FILE *f) {
    int i, child;
    for(i = 0; f && i < FOPEN_MAX && commands[i].f != f; i++);
    if(!f || i == FOPEN_MAX) {
        errno = EINVAL;
        return -1;
    }
    child = commands[i].pid;
    commands[i].f = NULL;
    fclose(f);
    return finish(child);
}
