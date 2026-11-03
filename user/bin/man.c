/*
SurfOS user program: man
--------------------
File: man.c     Date: 11/01/26
--------------------
man name: the manual page of a program, a kernel command or one of surfsh's own commands, through
less on a terminal and plain when standard output is anything else. man -k word: every page whose
first line has the word, without case in the ASCII letters, one line each, in the order of the
names (the first line is the name and what it does). Pages are /initrd/man/NAME.txt; names that
share a page are in manutil.h's table, and a page of a name's own comes before the table.

Status 0; 1 when there is no such page, or no page has the word; 2 for a mistake or when the
pages cannot be read.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <surf.h>
#include "fileutil.h"
#include "lineutil.h"
#include "manutil.h"

#define LESS "/initrd/bin/less"

/* the page's file for a name that may be one: 0, or -1 when there is none */
static int page_file(const char *page, char *path, size_t size) {
    struct stat st;
    if(!man_name_ok(page) || snprintf(path, size, "%s/%s%s", MAN_DIR, page, MAN_SUFFIX) >= (int)size) return -1;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? 0 : -1;
}

/* through less, which gets the terminal: its status, or -1 when it cannot start and the page goes out plain */
static int through_less(const char *path) {
    struct spawnx o;
    struct waitx w;
    char *av[3];
    int pid;
    av[0] = "less";
    av[1] = (char *)path;
    av[2] = NULL;
    memset(&o, 0, sizeof(o));
    o.fds[0] = 0;
    o.fds[1] = 1;
    o.fds[2] = 2;
    o.fds[3] = SPAWN_CLOSED;
    o.envp = environ;
    pid = spawnx(LESS, av, &o);
    if(pid < 0) return -1;
    if(waitx(pid, &w, 0) < 0) return 2;
    return w.how == WX_EXITED ? w.status : 2;
}

static int plain(const char *path) {
    char buf[4096];
    int fd = open(path, O_RDONLY), rc = 0;
    ssize_t n;
    if(fd < 0) {
        fprintf(stderr, "man: %s: %s\n", path, strerror(errno));
        return 2;
    }
    while((n = read(fd, buf, sizeof(buf))) > 0) {
        if(write_all(1, buf, (size_t)n) < 0) {
            fprintf(stderr, "man: %s\n", strerror(errno));
            rc = 2;
            break;
        }
    }
    if(n < 0) {
        fprintf(stderr, "man: %s: %s\n", path, strerror(errno));
        rc = 2;
    }
    close(fd);
    return rc;
}

static int show(const char *name) {
    char path[MAN_NAME_MAX + sizeof(MAN_DIR) + sizeof(MAN_SUFFIX) + 2];
    const char *page = man_alias_of(name);
    int r;
    if(page_file(name, path, sizeof(path)) < 0 && (!page || page_file(page, path, sizeof(path)) < 0)) {
        fprintf(stderr, "man: %s: no such page\n", name);
        return 1;
    }
    if(!isatty(1)) return plain(path);
    r = through_less(path);
    return r < 0 ? plain(path) : r;
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int search(const char *word) {
    char **names, *line = NULL, path[PATH_MAX];
    size_t cap = 0;
    int n, i, found = 0, nl, bad = 0;
    names = list_names(MAN_DIR, &n);
    if(!names) {
        fprintf(stderr, "man: %s: %s\n", MAN_DIR, strerror(errno));
        return 2;
    }
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    for(i = 0; i < n; i++) {
        FILE *f;
        if(!man_page_file(names[i]) || path_join(path, sizeof(path), MAN_DIR, names[i]) < 0) continue;
        f = lu_open(path);
        if(!f) {
            bad = 1;
            continue;
        }
        if(lu_getline(f, &line, &cap, &nl) >= 0 && man_has_word(line, word)) {
            printf("%s\n", line);
            found = 1;
        }
        lu_close(f);
    }
    free(line);
    free_names(names, n);
    return bad ? 2 : found ? 0 : 1;
}

int main(int argc, char **argv) {
    lu_prog = fu_prog = "man";
    if(argc == 3 && !strcmp(argv[1], "-k") && argv[2][0]) return search(argv[2]);
    if(argc == 2 && (argv[1][0] != '-' || !argv[1][1])) return show(argv[1]);
    fprintf(stderr, "usage: man name | man -k word\n");
    return 2;
}
