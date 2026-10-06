/*
SurfOS user program: cp
--------------------
File: cp.c      Date: 10/5/26
--------------------
cp [-r] <source> <target>      a file, or with -r a directory and everything in it
cp [-r] <source>... <dir>      into an existing directory, under their own names
*/

#include "fileutil.h"

static int opt_r;

static int copy(const char *src, const char *dst) {
    struct stat ss, ds;
    if(stat(src, &ss) < 0) { fu_error(src); return 1; }
    if(S_ISDIR(ss.type)) {
        if(!opt_r) { fprintf(stderr, "cp: %s: is a directory (use -r)\n", src); return 1; }
        if(path_inside(dst, src)) { fprintf(stderr, "cp: cannot copy %s into itself\n", src); return 1; }
        return copy_tree(src, dst);
    }
    if(stat(dst, &ds) == 0) {
        if(S_ISDIR(ds.type)) { errno = EISDIR; fu_error(dst); return 1; }
        if(ds.ino == ss.ino && !strcmp(ds.dev, ss.dev) && !strcmp(ds.fs, ss.fs)) {
            fprintf(stderr, "cp: %s and %s are the same file\n", src, dst);
            return 1;
        }
    }
    return copy_file(src, dst);
}

int main(int argc, char **argv) {
    struct stat st;
    char *target, to[PATH_MAX];
    int i = 1, rc = 0, to_dir;
    fu_prog = "cp";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if(!strcmp(argv[i], "-r") || !strcmp(argv[i], "-R")) opt_r = 1;
        else { fprintf(stderr, "usage: cp [-r] <source>... <target>\n"); return 2; }
    }
    if(argc - i < 2) { fprintf(stderr, "usage: cp [-r] <source>... <target>\n"); return 2; }
    target = argv[argc - 1];
    strip_slashes(target);
    to_dir = stat(target, &st) == 0 && S_ISDIR(st.type);
    if(argc - i > 2 && !to_dir) { fprintf(stderr, "cp: %s: not a directory\n", target); return 1; }
    for(; i < argc - 1; i++) {
        strip_slashes(argv[i]);
        if(!to_dir) { rc |= copy(argv[i], target); continue; }
        if(path_join(to, sizeof(to), target, path_base(argv[i]))) { fu_error(argv[i]); rc = 1; continue; }
        rc |= copy(argv[i], to);
    }
    return rc;
}
