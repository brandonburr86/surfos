/*
SurfOS user program: env
--------------------
File: env.c     Date: 10/24/26
--------------------
env [-i] [name=value]... [program [arg...]]: with no program, the environment, a
"name=value" line each, the assignments put over it; with a program, that program
runs with the environment so made and env waits for it: its status is env's (137 when
the kernel killed it, 127 when there is no such program). -i starts from an empty
environment. A name without a / is looked for in the directories of the new
environment's PATH (/initrd/bin when it has none).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <surf.h>
#include <surfos/spawn.h>

static char *env[SPAWN_ENV_MAX + 1];
static int n;

static int put(char *s) {
    size_t len = strchr(s, '=') - s;
    int i;
    for(i = 0; i < n; i++) {
        if(!strncmp(env[i], s, len) && env[i][len] == '=') {
            env[i] = s;
            return 0;
        }
    }
    if(n == SPAWN_ENV_MAX) {
        fprintf(stderr, "env: more than %d variables\n", SPAWN_ENV_MAX);
        return -1;
    }
    env[n++] = s;
    env[n] = NULL;
    return 0;
}

static const char *get(const char *name) {
    size_t len = strlen(name);
    int i;
    for(i = 0; i < n; i++) if(!strncmp(env[i], name, len) && env[i][len] == '=') return env[i] + len + 1;
    return NULL;
}

/* the program's path: 0, or the status (said) */
static int find(const char *name, char *path, int size) {
    const char *at = get("PATH");
    struct stat st;
    if(strchr(name, '/')) {
        if(stat(name, &st) < 0) {
            fprintf(stderr, "env: %s: %s\n", name, strerror(errno));
            return 127;
        }
        if(!S_ISREG(st.type)) {
            fprintf(stderr, "env: %s: is a directory\n", name);
            return 126;
        }
        snprintf(path, (size_t)size, "%s", name);
        return 0;
    }
    if(!at) at = "/initrd/bin";
    for(;;) {
        const char *colon = strchr(at, ':');
        int len = colon ? (int)(colon - at) : (int)strlen(at);
        if(snprintf(path, (size_t)size, "%.*s/%s", len ? len : 1, len ? at : ".", name) < size &&
           stat(path, &st) == 0 && S_ISREG(st.type))
            return 0;
        if(!colon) break;
        at = colon + 1;
    }
    fprintf(stderr, "env: %s: no such program\n", name);
    return 127;
}

int main(int argc, char **argv) {
    struct spawnx o;
    struct stat st;
    char path[256];
    int i = 1, k, pid, status = 0;
    if(i < argc && !strcmp(argv[i], "-i")) i++;
    else for(k = 0; environ && environ[k]; k++) if(strchr(environ[k], '=') && put(environ[k]) < 0) return 2;
    for(; i < argc && strchr(argv[i], '=') && argv[i][0] != '='; i++) if(put(argv[i]) < 0) return 2;
    if(i < argc && !strcmp(argv[i], "--")) i++;
    if(i == argc) {
        for(k = 0; k < n; k++) printf("%s\n", env[k]);
        return 0;
    }
    if((status = find(argv[i], path, sizeof(path)))) return status;
    memset(&o, 0, sizeof(o));
    for(k = 0; k < 3; k++) o.fds[k] = fstat(k, &st) == 0 ? k : SPAWN_CLOSED;
    o.fds[3] = -1;
    o.envp = env;
    fflush(stdout);
    if((pid = spawnx(path, argv + i, &o)) < 0) {
        fprintf(stderr, "env: %s: %s\n", argv[i], strerror(errno));
        return 126;
    }
    if(waitpid(pid, &status, 0) != pid) {
        fprintf(stderr, "env: waiting for %s: %s\n", argv[i], strerror(errno));
        return 1;
    }
    return status == -1 ? 137 : status;
}
