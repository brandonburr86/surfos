/* user/bin/sed.c and grep.c as programs of the host (11/01/26, the command-line suite, track text), so that tests/test_sed.py can
   compare them with the real tools process against process: compiled with -Itests/host/toolhost, the host's C library
   under them, this file for <surf.h> and regex.h for <regex.h>. What SurfOS has and the host has not is here. */
#ifndef TOOLHOST_SURF_H
#define TOOLHOST_SURF_H

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* saveutil.h reads SurfOS's struct stat. The name stat is both a struct's and a function's, so one macro does for both. */
struct tool_stat {
    unsigned int type, size, mtime;
};

static int tool_stat(const char *path, struct tool_stat *out) {
    struct stat st;
    int r = stat(path, &st);
    if(!r) {
        out->type = (unsigned int)st.st_mode;
        out->size = (unsigned int)st.st_size;
        out->mtime = (unsigned int)st.st_mtime;
    }
    return r;
}
#define stat tool_stat

/* SurfOS's files have no modes, so its open() is called with none */
static inline int tool_open(const char *path, int flags) {
    return open(path, flags, 0644);
}
#define open tool_open

static inline void broken_pipe_exit(int on) {
    signal(SIGPIPE, on ? SIG_DFL : SIG_IGN);
}

#endif
