/*
SurfOS user program: rm
--------------------
File: rm.c      Date: 10/5/26
--------------------
rm [-r] [-f] <path>...: delete files; -r also directories with everything in
them; -f says nothing about paths that do not exist.
*/

#include "fileutil.h"

int main(int argc, char **argv) {
    struct stat st;
    int i, rc = 0, opt_r = 0, opt_f = 0;
    fu_prog = "rm";
    for(i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'r' || *o == 'R') opt_r = 1;
            else if(*o == 'f') opt_f = 1;
            else { fprintf(stderr, "usage: rm [-rf] <path>...\n"); return 2; }
        }
    }
    if(i == argc && !opt_f) { fprintf(stderr, "usage: rm [-rf] <path>...\n"); return 2; }
    for(; i < argc; i++) {
        strip_slashes(argv[i]);
        if(stat(argv[i], &st) < 0) {
            if(errno != ENOENT || !opt_f) { fu_error(argv[i]); rc = 1; }
            continue;
        }
        if(S_ISDIR(st.type)) {
            if(!opt_r) { fprintf(stderr, "rm: %s: is a directory (use -r)\n", argv[i]); rc = 1; continue; }
            rc |= remove_tree(argv[i]);
        } else if(unlink(argv[i]) < 0) {
            fu_error(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
