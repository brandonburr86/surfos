/*
SurfOS user program: touch
--------------------
File: touch.c   Date: 10/5/26
--------------------
touch <file>...: create an empty file, or give an existing one the current time.
There is no call to set a time, but FAT stamps a file whenever it is written,
so touch writes the first byte back as it was (an empty file gets a byte that
is truncated away again).
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int touch(const char *path) {
    struct stat st;
    char c = 0;
    int fd = open(path, O_RDWR);
    if(fd < 0) {
        if(errno != ENOENT) return -1;
        fd = open(path, O_WRONLY | O_CREAT);
        return fd < 0 ? -1 : close(fd);
    }
    if(fstat(fd, &st) < 0) { close(fd); return -1; }
    if(st.size == 0) {
        if(write(fd, "", 1) != 1 || ftruncate(fd, 0) < 0) { close(fd); return -1; }
    } else if(read(fd, &c, 1) != 1 || lseek(fd, 0, SEEK_SET) < 0 || write(fd, &c, 1) != 1) {
        close(fd);
        return -1;
    }
    return close(fd);
}

int main(int argc, char **argv) {
    int i, rc = 0;
    if(argc < 2) { fprintf(stderr, "usage: touch <file>...\n"); return 2; }
    for(i = 1; i < argc; i++) {
        if(touch(argv[i]) < 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
