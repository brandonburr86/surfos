/* ls: list a directory through the system calls */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <surf.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    struct dirent de;
    struct stat st;
    int fd, r, n = 0;
    if(stat(path, &st) < 0) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return 1; }
    if(!S_ISDIR(st.type)) { printf("%8u  %s\n", st.size, path); return 0; }
    fd = open(path, O_RDONLY | O_DIRECTORY);
    if(fd < 0) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return 1; }
    while((r = getdent(fd, &de)) == 0) {
        printf("%c %8u  %s%s\n", S_ISDIR(de.type) ? 'd' : '-', de.size, de.name, S_ISDIR(de.type) ? "/" : "");
        n++;
    }
    close(fd);
    if(r < 0) { fprintf(stderr, "ls: %s\n", strerror(errno)); return 1; }
    printf("%d entries\n", n);
    return 0;
}
