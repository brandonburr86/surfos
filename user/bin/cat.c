/* cat: copy files (or standard input) to standard output */
#include <surf.h>

static int dump(int fd) {
    char buf[1024];
    int n;
    while((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n);
    return n < 0 ? n : 0;
}

int main(int argc, char **argv) {
    int i, rc = 0;
    if(argc < 2) return dump(0) ? 1 : 0;
    for(i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY), r;
        if(fd < 0) { dprintf(2, "cat: %s: %s\n", argv[i], strerror(fd)); rc = 1; continue; }
        r = dump(fd);
        if(r) { dprintf(2, "cat: %s: %s\n", argv[i], strerror(r)); rc = 1; }
        close(fd);
    }
    return rc;
}
