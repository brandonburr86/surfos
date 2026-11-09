/*
SurfOS chat client: wchat
--------------------
File: proc.c    Date: 11/05/26
--------------------
window.h's calls that are SurfOS's own: the helper started with its two pipes, killed, collected,
and the clock's minute. Apart, so that tests/host/test_wchatwin.c stands in for them, as Wundertow's
loadproc.c is.

The helper is wchat itself (chat_self: the path the window was started by, else
/initrd/bin/wchat), started with spawnx() as `wchat --helper HOST PORT tls|tcp`: its descriptor 0
the reading end of the window's pipe to it, 1 the writing end of its pipe to the window, and
nothing else, so the only other descriptor it ever has is its connection. No environment, and the
password never on its command line: it comes down the pipe inside the line that carries it.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <surf.h>
#include "window.h"

const char *chat_self = "/initrd/bin/wchat";

int chat_spawn(const char *host, unsigned port, int tls, int *to_fd, int *from_fd) {
    char portword[8], *argv[6];
    struct spawnx o;
    int down[2], up[2], pid, err;
    if(pipe(down) < 0) return -1;
    if(pipe(up) < 0) {
        err = errno;
        close(down[0]);
        close(down[1]);
        errno = err;
        return -1;
    }
    snprintf(portword, sizeof(portword), "%u", port);
    argv[0] = (char *)"wchat";
    argv[1] = (char *)"--helper";
    argv[2] = (char *)host;
    argv[3] = portword;
    argv[4] = (char *)(tls ? "tls" : "tcp");
    argv[5] = NULL;
    memset(&o, 0, sizeof(o));
    o.fds[0] = down[0];
    o.fds[1] = up[1];
    o.fds[2] = SPAWN_CLOSED;
    o.fds[3] = SPAWN_CLOSED;
    o.envp = NULL;
    pid = spawnx(chat_self, argv, &o);
    err = errno;
    close(down[0]);                                 /* the helper's ends are its own now */
    close(up[1]);
    if(pid < 0) {
        close(down[1]);
        close(up[0]);
        errno = err;
        return -1;
    }
    *to_fd = down[1];
    *from_fd = up[0];
    return pid;
}

void chat_kill(int pid) {
    if(pid > 0) kill(pid);
}

int chat_reaped(int pid) {
    int status, r = waitpid(pid, &status, WNOHANG);
    return r != 0;                                  /* collected now, or not a child any more (-1) */
}

int chat_minute(void) {
    time_t now = time(NULL);
    struct tm tm;
    if(now <= 0 || !localtime_r(&now, &tm)) return -1;
    return tm.tm_hour * 60 + tm.tm_min;
}
