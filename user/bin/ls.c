/* ls: list directories through the system calls (10/5/26: -l, -a, sorted) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

static int opt_long, opt_all;

struct entry {
    char *name;
    int type;
    unsigned int size, mtime;
};

static void when(unsigned int mtime, char *buf, size_t size) {
    time_t t = (time_t)mtime;
    struct tm tm;
    if(!mtime) { snprintf(buf, size, "%16s", "-"); return; }
    strftime(buf, size, "%Y-%m-%d %H:%M", localtime_r(&t, &tm));
}

static void show(const char *name, int type, unsigned int size, unsigned int mtime) {
    const char *slash = S_ISDIR(type) && strcmp(name, ".") && strcmp(name, "..") ? "/" : "";
    if(opt_long) {
        char date[32];
        when(mtime, date, sizeof(date));
        printf("%c %10u %s %s%s\n", S_ISDIR(type) ? 'd' : '-', size, date, name, slash);
    } else {
        printf("%s%s\n", name, slash);
    }
}

static int by_name(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static int list_dir(const char *path) {
    DIR *d = opendir(path);
    struct dirent *de;
    struct entry *v = NULL;
    size_t n = 0, cap = 0, i;
    int rc = 0;
    if(!d) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return 1; }
    errno = 0;
    while((de = readdir(d))) {
        if(de->d_name[0] == '.' && !opt_all) continue;  /* hidden, like the files macOS leaves (._x, .Trashes) */
        if(n == cap) {
            struct entry *nv = realloc(v, (cap = cap ? cap * 2 : 32) * sizeof(*v));
            if(!nv) { rc = 1; break; }
            v = nv;
        }
        v[n].name = strdup(de->d_name);
        if(!v[n].name) { rc = 1; break; }
        v[n].type = de->d_type;
        v[n].size = de->size;
        v[n].mtime = de->mtime;
        n++;
    }
    if(!de && errno) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); rc = 1; }
    closedir(d);
    if(opt_all) {                       /* the kernel does not list these two */
        struct stat st;
        char parent[300];
        if(stat(path, &st) == 0) show(".", st.type, 0, st.mtime);
        snprintf(parent, sizeof(parent), "%s/..", path);
        if(stat(parent, &st) == 0) show("..", st.type, 0, st.mtime);
    }
    qsort(v, n, sizeof(*v), by_name);
    for(i = 0; i < n; i++) {
        show(v[i].name, v[i].type, v[i].size, v[i].mtime);
        free(v[i].name);
    }
    free(v);
    if(opt_long) printf("%u entr%s\n", (unsigned)n, n == 1 ? "y" : "ies");
    return rc;
}

int main(int argc, char **argv) {
    int i, rc = 0, paths = 0;
    struct stat st;
    for(i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'l') opt_long = 1;
            else if(*o == 'a') opt_all = 1;
            else { fprintf(stderr, "usage: ls [-la] [path...]\n"); return 2; }
        }
    }
    paths = argc - i;
    if(!paths) return list_dir(".");
    for(; i < argc; i++) {
        if(stat(argv[i], &st) < 0) { fprintf(stderr, "ls: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if(!S_ISDIR(st.type)) { show(argv[i], st.type, st.size, st.mtime); continue; }
        if(paths > 1) printf("%s:\n", argv[i]);
        rc |= list_dir(argv[i]);
        if(paths > 1 && i < argc - 1) putchar('\n');
    }
    return rc;
}
