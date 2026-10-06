/* cat: copy files (or standard input, also "-") to standard output */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

static int write_all(int fd, const char *p, ssize_t n) {
    while(n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if(w <= 0) return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static int dump(int fd) {
    char buf[4096];
    ssize_t n;
    while((n = read(fd, buf, sizeof(buf))) > 0) if(write_all(1, buf, n)) return -1;
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    int i, rc = 0;
    if(argc < 2) return dump(0) ? 1 : 0;
    for(i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
        if(fd < 0 || dump(fd)) { fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
        if(fd > 0) close(fd);
    }
    return rc;
}
