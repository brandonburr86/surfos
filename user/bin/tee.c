/*
SurfOS user program: tee
--------------------
File: tee.c     Date: 10/25/26
--------------------
tee [-a] [file...]: standard input to standard output and into each file as it comes (-a: at
the files' ends). A file that cannot be opened or written is said, the others go on; status 1
then.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#define TEE_MAX 16

int main(int argc, char **argv) {
    char buf[4096];
    int fds[TEE_MAX], nfds = 0, i = 1, rc = 0, flags = O_WRONLY | O_CREAT | O_TRUNC, k;
    ssize_t n;
    if(i < argc && !strcmp(argv[i], "-a")) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        i++;
    }
    for(; i < argc; i++) {
        int fd;
        if(nfds == TEE_MAX) {
            fprintf(stderr, "tee: at most %d files\n", TEE_MAX);
            return 2;
        }
        fd = open(argv[i], flags, 0644);
        if(fd < 0) {
            fprintf(stderr, "tee: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        fds[nfds++] = fd;
    }
    while((n = read(0, buf, sizeof(buf))) > 0) {
        if(write(1, buf, (size_t)n) != n) rc = 1;
        for(k = 0; k < nfds; k++) {
            if(fds[k] >= 0 && write(fds[k], buf, (size_t)n) != n) {
                fprintf(stderr, "tee: %s\n", strerror(errno));
                close(fds[k]);
                fds[k] = -1;
                rc = 1;
            }
        }
    }
    for(k = 0; k < nfds; k++) if(fds[k] >= 0) close(fds[k]);
    return rc;
}
