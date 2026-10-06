/*
SurfOS user program: df
--------------------
File: df.c      Date: 10/5/26
--------------------
df [path...]: size and free space of the mounted file systems (statfs), in KB.
Without paths: / and every mount point under it (the root directory lists them).
A file system that does not count its space shows "-".
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/statfs.h>

static void show(const char *path) {
    struct statfs st;
    if(statfs(path, &st) < 0) {
        fprintf(stderr, "df: %s: %s\n", path, strerror(errno));
        return;
    }
    printf("%-10s %-7s", st.dev[0] ? st.dev : "-", st.fs);
    if(st.units && st.unit) {
        unsigned long long bytes = (unsigned long long)st.units * st.unit;
        unsigned long long avail = (unsigned long long)st.free_units * st.unit;
        unsigned int size_kb = (unsigned int)(bytes >> 10), free_kb = (unsigned int)(avail >> 10);
        unsigned int used_kb = size_kb - free_kb, u = used_kb, s = size_kb;
        while(s > 0xFFFFFF) { u >>= 4; s >>= 4; }      /* the percentage in 32-bit arithmetic */
        printf(" %10u %10u %10u %3u%%", size_kb, used_kb, free_kb, s ? (u * 100 + s - 1) / s : 0);
    } else {
        printf(" %10s %10s %10s %4s", "-", "-", "-", "-");
    }
    printf(" %s%s%s%s\n", st.path, (st.flags & ST_RDONLY) ? " (read-only)" : "", st.label[0] ? "  " : "", st.label);
}

int main(int argc, char **argv) {
    int i;
    printf("%-10s %-7s %10s %10s %10s %4s %s\n", "Device", "Type", "Size KB", "Used KB", "Free KB", "Use%", "Mounted on");
    if(argc > 1) {
        for(i = 1; i < argc; i++) show(argv[i]);
    } else {
        DIR *d = opendir("/");
        struct dirent *de;
        char path[300];
        show("/");
        while(d && (de = readdir(d))) {
            snprintf(path, sizeof(path), "/%s", de->d_name);
            show(path);
        }
        if(d) closedir(d);
    }
    return 0;
}
