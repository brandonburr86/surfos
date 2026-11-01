/*
SurfOS Undertow's window: helpers as processes
----------------------
File: loadproc.c  Date: 11/1/26
----------------------
loader.h's calls that are SurfOS's own: a helper started, killed, collected. Apart, so that the host tests can stand in
for them.
*/

#include <stdio.h>
#include <string.h>
#include <surf.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "browser.h"
#include "loader.h"

const char *ld_self = LD_SELF;

int ld_spawn(const char *kind, const char *url, int out_fd) {
    static char env[sizeof(LD_URL_ENV) + WB_ADDR_MAX];
    char *argv[4], *envp[2];
    struct spawnx o;
    argv[0] = (char *)"wundertow";
    argv[1] = (char *)"--helper";
    argv[2] = (char *)kind;
    argv[3] = NULL;
    snprintf(env, sizeof(env), "%s=%s", LD_URL_ENV, url);
    envp[0] = env;
    envp[1] = NULL;
    memset(&o, 0, sizeof(o));
    o.fds[0] = SPAWN_CLOSED;
    o.fds[1] = out_fd;
    o.fds[2] = SPAWN_CLOSED;
    o.fds[3] = -1;
    o.envp = envp;
    return spawnx(ld_self, argv, &o);
}

void ld_kill(int pid) {
    kill(pid);
}

int ld_collect(void) {
    int status, r = waitpid(-1, &status, WNOHANG);
    return r < 0 ? -1 : r > 0;
}
