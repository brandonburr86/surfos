/*
SurfOS user program: tree
--------------------
File: tree.c    Date: 11/01/26
--------------------
tree [-a] [-d] [-L level] [-s] [directory...]: what is in a directory (. without one) and in the
directories under it, drawn as a tree, then how many of each there were. -a shows names that
start with a dot too, -d only directories, -L goes that many levels down at most, and -s puts
each file's size in bytes before its name. Names come in ls's order, case not counting.

On a console that does not draw UTF-8 the lines are ASCII. A directory is read whole and closed
before going into it (fileutil.h), so one descriptor is enough however deep it goes; the depth
is bounded by PATH_MAX. What cannot be read is said, and the status is 2 at the end.
*/

#include "fileutil.h"
#include "../diff/cli.h"
#include <surf.h>

struct entry {
    char *name;
    unsigned size;
    int dir;
};

static int show_hidden, dirs_only, sizes, rc;
static long max_level = LONG_MAX;
static unsigned long dirs_seen, files_seen;
static const char *branch = "├── ", *corner = "└── ", *bar = "│   ", *space = "    ";
static char indent[PATH_MAX * 7];       /* a bar is 6 bytes with its spaces, and a path has PATH_MAX / 2 levels at most */

static int by_name(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

/* what will be shown of a directory, sorted; *count entries; NULL said */
static struct entry *read_entries(const char *dir, int *count) {
    char path[PATH_MAX];
    struct entry *v;
    char **names;
    int n, i, kept = 0;
    names = list_names(dir, &n);
    if(!names) {
        fu_error(dir);
        return NULL;
    }
    v = lu_alloc(NULL, ((size_t)n + 1) * sizeof(*v));
    for(i = 0; i < n; i++) {
        struct stat st;
        if(!strcmp(names[i], ".") || !strcmp(names[i], "..")) continue;         /* the host's readdir lists them */
        if(names[i][0] == '.' && !show_hidden) continue;
        if(path_join(path, sizeof(path), dir, names[i]) < 0 || stat(path, &st) < 0) {
            fu_error(names[i]);
            rc = 2;
            continue;
        }
        if(dirs_only && !S_ISDIR(st.st_mode)) continue;
        v[kept].dir = S_ISDIR(st.st_mode);
        v[kept].size = (unsigned)st.st_size;
        if(!(v[kept].name = strdup(names[i]))) lu_alloc(NULL, (size_t)-1);
        kept++;
    }
    free_names(names, n);
    qsort(v, (size_t)kept, sizeof(*v), by_name);
    *count = kept;
    return v;
}

static void walk(const char *dir, long level, size_t depth) {
    char path[PATH_MAX];
    struct entry *v;
    int n, i;
    if(!(v = read_entries(dir, &n))) {
        rc = 2;
        return;
    }
    for(i = 0; i < n; i++) {
        int last = i == n - 1;
        printf("%s%s", indent, last ? corner : branch);
        if(sizes) printf("[%11u]  ", v[i].size);
        printf("%s\n", v[i].name);
        if(v[i].dir) dirs_seen++;
        else files_seen++;
        if(v[i].dir && level < max_level) {
            const char *more = last ? space : bar;
            size_t k = strlen(more);
            if(path_join(path, sizeof(path), dir, v[i].name) < 0 || depth + k >= sizeof(indent)) {
                fu_error(v[i].name);
                rc = 2;
            } else {
                memcpy(indent + depth, more, k + 1);
                walk(path, level + 1, depth + k);
                indent[depth] = 0;
            }
        }
    }
    for(i = 0; i < n; i++) free(v[i].name);
    free(v);
}

static int usage(void) {
    fprintf(stderr, "usage: tree [-a] [-d] [-L level] [-s] [directory...]\n");
    return 2;
}

int main(int argc, char **argv) {
    int i = 1, k;
    lu_prog = fu_prog = "tree";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'a') show_hidden = 1;
            else if(*o == 'd') dirs_only = 1;
            else if(*o == 's') sizes = 1;
            else if(*o == 'L') {
                const char *arg = o[1] ? o + 1 : ++i < argc ? argv[i] : NULL;
                int v;
                if(!arg) return usage();
                if(cli_int(arg, &v) < 0 || !v) {
                    fprintf(stderr, "tree: %s: not a level (from 1)\n", arg);
                    return 2;
                }
                max_level = v;
                break;
            } else return usage();
        }
    }
    if(isatty(1) && !(ttyctl(TTY_CAPS, 0) & TTY_CAP_UTF8)) {
        branch = "|-- ";
        corner = "`-- ";
        bar = "|   ";
    }
    for(k = i; k < argc || k == i; k++) {
        const char *dir = k < argc ? argv[k] : ".";
        struct stat st;
        if(stat(dir, &st) < 0) {
            fu_error(dir);
            rc = 2;
            continue;
        }
        if(!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            fu_error(dir);
            rc = 2;
            continue;
        }
        printf("%s\n", dir);
        walk(dir, 1, 0);
    }
    printf("\n%lu director%s", dirs_seen, dirs_seen == 1 ? "y" : "ies");
    if(!dirs_only) printf(", %lu file%s", files_seen, files_seen == 1 ? "" : "s");
    putchar('\n');
    return cli_flush("tree") ? 2 : rc;
}
