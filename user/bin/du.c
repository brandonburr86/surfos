/*
SurfOS user program: du
--------------------
File: du.c      Date: 10/25/26
--------------------
du [-s] [-a] [-h] [path...]: the space the files under each path take on the disk, for every
directory (after what is in it) and in all, in KB of 1024 bytes (track A, A9). A file takes
its size rounded up to its file system's allocation unit (a FAT cluster, from statfs), as the
disk spends it; directories themselves count nothing. -s only the total of each path; -a
each file too; -h sizes as 512, 4.0K, 12K, 1.2M or 3.4G. Without a path: the current
directory. A directory is read whole before du goes into it, so it holds one open at a time.
The sums are kept in sectors of 512 bytes (2 TB at most), in 32-bit numbers.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>

static int summary, all, human;
static int status;

/* a size in sectors as du shows it: KB rounded up, or with -h in a unit */
static void show(unsigned long sectors, const char *path) {
    static const char units[] = "KMGT";
    char size[24];
    unsigned long whole = sectors / 2, tenths = sectors % 2 * 5;
    int u = 0;
    if(!human) snprintf(size, sizeof(size), "%lu", (sectors + 1) / 2);
    else if(sectors < 2) snprintf(size, sizeof(size), "%lu", sectors * 512);
    else {
        while(whole >= 1024 && u < 3) {
            tenths = whole % 1024 * 10 / 1024;
            whole /= 1024;
            u++;
        }
        if(whole >= 10) snprintf(size, sizeof(size), "%lu%c", whole + (tenths >= 5), units[u]);
        else snprintf(size, sizeof(size), "%lu.%lu%c", whole, tenths, units[u]);
    }
    printf("%s\t%s\n", size, path);
}

/* the sectors a file of size bytes takes on a file system that allocates unit bytes at a time */
static unsigned long on_disk(unsigned int size, unsigned int unit) {
    if(unit < 512 || unit % 512) unit = 512;
    return (unsigned long)(size / unit + (size % unit != 0)) * (unit / 512);
}

/* the sectors under path, a directory; its line printed when it is the path given (top) or without -s */
static unsigned long walk(const char *path, int top) {
    DIR *d;
    struct dirent *e;
    struct statfs fs;
    struct entry { char *name; int dir; unsigned int size; } *list = NULL;
    unsigned long total = 0;
    unsigned int unit;
    int n = 0, cap = 0, i;
    size_t len = strlen(path);
    unit = statfs(path, &fs) == 0 ? fs.unit : 0;
    d = opendir(path);
    if(!d) {
        fprintf(stderr, "du: %s: %s\n", path, strerror(errno));
        status = 1;
        return 0;
    }
    while((e = readdir(d))) {
        if(n == cap) {
            int more = cap ? cap * 2 : 32;
            struct entry *l = realloc(list, (size_t)more * sizeof(*list));
            if(!l) break;
            list = l;
            cap = more;
        }
        if(!(list[n].name = strdup(e->d_name))) break;
        list[n].dir = e->d_type == DT_DIR;
        list[n++].size = e->size;
    }
    if(e) {                                             /* left early */
        fprintf(stderr, "du: %s: out of memory\n", path);
        status = 1;
    }
    closedir(d);
    for(i = 0; i < n; i++) {
        char *child = malloc(len + strlen(list[i].name) + 2);
        if(child) {
            sprintf(child, len && path[len - 1] == '/' ? "%s%s" : "%s/%s", path, list[i].name);
            if(list[i].dir) total += walk(child, 0);
            else {
                unsigned long sectors = on_disk(list[i].size, unit);
                total += sectors;
                if(all && !summary) show(sectors, child);
            }
            free(child);
        }
        free(list[i].name);
    }
    free(list);
    if(top || !summary) show(total, path);
    return total;
}

static void du(const char *path) {
    struct stat st;
    struct statfs fs;
    if(stat(path, &st) < 0) {
        fprintf(stderr, "du: %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    if(S_ISDIR(st.st_mode)) walk(path, 1);
    else show(on_disk(st.st_size, statfs(path, &fs) == 0 ? fs.unit : 0), path);
}

int main(int argc, char **argv) {
    int i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i] + 1;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(; *o; o++) {
            if(*o == 's') summary = 1;
            else if(*o == 'a') all = 1;
            else if(*o == 'h') human = 1;
            else {
                fprintf(stderr, "usage: du [-s] [-a] [-h] [path...]\n");
                return 2;
            }
        }
    }
    if(i == argc) du(".");
    for(; i < argc; i++) du(argv[i]);
    fflush(stdout);
    return status;
}
