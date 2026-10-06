/*
SurfOS user program: mkdir
--------------------
File: mkdir.c   Date: 10/5/26
--------------------
mkdir [-p] <dir>...: -p makes the missing parents too and is quiet about a
directory that already exists.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.type);
}

static int make_parents(const char *path) {
    char buf[PATH_MAX];
    char *p;
    if(strlen(path) >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    strcpy(buf, path);
    for(p = buf + 1; *p; p++) {
        if(*p != '/') continue;
        *p = 0;
        if(mkdir(buf, 0777) < 0 && !(errno == EEXIST && is_dir(buf))) {
            if(errno == EEXIST) errno = ENOTDIR;
            fprintf(stderr, "mkdir: %s: %s\n", buf, strerror(errno));
            return -1;
        }
        *p = '/';
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0, opt_p = 0;
    if(argc > 1 && !strcmp(argv[1], "-p")) { opt_p = 1; i++; }
    if(i == argc) { fprintf(stderr, "usage: mkdir [-p] <dir>...\n"); return 2; }
    for(; i < argc; i++) {
        size_t n = strlen(argv[i]);
        while(n > 1 && argv[i][n - 1] == '/') argv[i][--n] = 0;
        if(opt_p && make_parents(argv[i]) < 0) { rc = 1; continue; }
        if(mkdir(argv[i], 0777) == 0) continue;
        if(opt_p && errno == EEXIST && is_dir(argv[i])) continue;
        fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
        rc = 1;
    }
    return rc;
}
