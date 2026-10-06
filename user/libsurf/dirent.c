/*
SurfOS user library: directories
--------------------
File: dirent.c  Date: 10/5/26
--------------------
*/

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <surf.h>

struct _DIR {
    int fd;
    struct dirent de;
};

DIR *opendir(const char *path) {
    DIR *d;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if(fd < 0) return NULL;
    d = malloc(sizeof(DIR));
    if(!d) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d) {
    return getdent(d->fd, &d->de) == 0 ? &d->de : NULL;    /* 1 is the end, -1 an error (errno) */
}

int closedir(DIR *d) {
    int r = close(d->fd);
    free(d);
    return r;
}

void rewinddir(DIR *d) {
    lseek(d->fd, 0, SEEK_SET);          /* a directory's position is the index of its next entry */
}

int dirfd(DIR *d) {
    return d->fd;
}
