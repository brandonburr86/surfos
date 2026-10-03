/* ls: list a directory through the system calls */
#include <surf.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    struct dirent de;
    struct stat st;
    int fd, r, n = 0;
    r = stat(path, &st);
    if(r < 0) { dprintf(2, "ls: %s: %s\n", path, strerror(r)); return 1; }
    if(st.type != VN_DIR) { printf("%8u  %s\n", st.size, path); return 0; }
    fd = open(path, O_RDONLY | O_DIRECTORY);
    if(fd < 0) { dprintf(2, "ls: %s: %s\n", path, strerror(fd)); return 1; }
    while((r = readdir(fd, &de)) == 0) {
        printf("%c %8u  %s%s\n", de.type == VN_DIR ? 'd' : '-', de.size, de.name, de.type == VN_DIR ? "/" : "");
        n++;
    }
    close(fd);
    if(r < 0) { dprintf(2, "ls: %s\n", strerror(r)); return 1; }
    printf("%d entries\n", n);
    return 0;
}
