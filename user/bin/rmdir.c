/*
SurfOS user program: rmdir
--------------------
File: rmdir.c   Date: 10/5/26
--------------------
rmdir <dir>...: delete empty directories.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int i, rc = 0;
    if(argc < 2) { fprintf(stderr, "usage: rmdir <dir>...\n"); return 2; }
    for(i = 1; i < argc; i++) {
        if(rmdir(argv[i]) < 0) {
            fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
