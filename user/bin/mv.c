/*
SurfOS user program: mv
--------------------
File: mv.c      Date: 10/5/26
--------------------
mv <source> <target>, mv <source>... <dir>: rename, or where the file system
cannot (another mount, or a file system without rename) copy and then remove.
*/

#include "fileutil.h"

static int move(const char *src, const char *dst) {
    struct stat ss, ds;
    int rc;
    if(stat(src, &ss) < 0) { fu_error(src); return 1; }
    if(S_ISDIR(ss.type) && path_inside(dst, src)) {
        fprintf(stderr, "mv: cannot move %s into itself\n", src);
        return 1;
    }
    if(stat(dst, &ds) == 0 && S_ISDIR(ds.type) && !S_ISDIR(ss.type)) { errno = EISDIR; fu_error(dst); return 1; }
    if(rename(src, dst) == 0) return 0;
    if(errno != EXDEV && errno != ENOSYS) { fu_error(src); return 1; }
    rc = S_ISDIR(ss.type) ? copy_tree(src, dst) : copy_file(src, dst);
    if(rc) return 1;
    if(S_ISDIR(ss.type)) return remove_tree(src);
    if(unlink(src) < 0) { fu_error(src); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    struct stat st;
    char *target, to[PATH_MAX];
    int i, rc = 0, to_dir;
    fu_prog = "mv";
    if(argc < 3) { fprintf(stderr, "usage: mv <source>... <target>\n"); return 2; }
    target = argv[argc - 1];
    strip_slashes(target);
    to_dir = stat(target, &st) == 0 && S_ISDIR(st.type);
    if(argc > 3 && !to_dir) { fprintf(stderr, "mv: %s: not a directory\n", target); return 1; }
    for(i = 1; i < argc - 1; i++) {
        strip_slashes(argv[i]);
        if(!to_dir) { rc |= move(argv[i], target); continue; }
        if(path_join(to, sizeof(to), target, path_base(argv[i]))) { fu_error(argv[i]); rc = 1; continue; }
        rc |= move(argv[i], to);
    }
    return rc;
}
