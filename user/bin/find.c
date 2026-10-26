/*
SurfOS user program: find
--------------------
File: find.c    Date: 10/25/26
--------------------
find [path...] [test...] [-print0]: the names under each path (. without one), the path
itself first and each directory's entries in the order its file system keeps them; with
tests, only the names that pass every one (track A, A9).

  -name PATTERN   the last part of the name matches a wildcard pattern: * ? [abc] [a-z]
                  [!abc], a backslash taking the next character as it is
  -iname PATTERN  the same, case not counting
  -type f | d     files | directories
  -maxdepth N     no deeper than N (the path itself is depth 0)
  -mindepth N     no shallower than N
  -print0         each name ends with a NUL, not a newline (for xargs -0)

A directory is read whole before find goes into it, so it holds one open at a time. A
name that cannot be read is said on the standard error, and the status is 1.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_TESTS 16

static struct {
    const char *pattern;
    int nocase;
} names[MAX_TESTS];
static int nnames;
static int type;                    /* 0, or 'f' or 'd' */
static int maxdepth = -1, mindepth;
static char ending = '\n';
static int status;

static int fold(int c, int nocase) {
    return nocase && c < 0x80 ? tolower(c) : c;
}

/* s matches the wildcard pattern p */
static int match(const char *p, const char *s, int nocase) {
    for(; *p; p++, s++) {
        if(*p == '*') {
            while(p[1] == '*') p++;
            if(!p[1]) return 1;
            for(; *s; s++)
                if(match(p + 1, s, nocase)) return 1;
            return 0;
        }
        if(!*s) return 0;
        if(*p == '?') continue;
        if(*p == '[') {
            const char *q = p + 1;
            int neg = 0, hit = 0, c = fold((unsigned char)*s, nocase);
            if(*q == '!' || *q == '^') {
                neg = 1;
                q++;
            }
            do {                                        /* a ']' first is one of the set */
                if(q[1] == '-' && q[2] && q[2] != ']') {
                    if(c >= fold((unsigned char)q[0], nocase) && c <= fold((unsigned char)q[2], nocase)) hit = 1;
                    q += 3;
                } else {
                    if(c == fold((unsigned char)*q, nocase)) hit = 1;
                    q++;
                }
            } while(*q && *q != ']');
            if(!*q) {                                   /* no ']': a plain '[' */
                if(*s != '[') return 0;
                continue;
            }
            if(hit == neg) return 0;
            p = q;
            continue;
        }
        if(*p == '\\' && p[1]) p++;
        if(fold((unsigned char)*p, nocase) != fold((unsigned char)*s, nocase)) return 0;
    }
    return !*s;
}

/* the last part of a path: "/initrd/bin" bin, "/initrd/" initrd, "/" / */
static void last_part(const char *path, char *out, size_t cap) {
    size_t end = strlen(path), start;
    while(end > 1 && path[end - 1] == '/') end--;
    start = end;
    while(start > 0 && path[start - 1] != '/') start--;
    if(start == end && end) start = end - 1;            /* "/" */
    if(end - start >= cap) end = start + cap - 1;
    memcpy(out, path + start, end - start);
    out[end - start] = 0;
}

static int passes(const char *path, int is_dir) {
    char base[256];
    int i;
    if(type == 'f' && is_dir) return 0;
    if(type == 'd' && !is_dir) return 0;
    if(!nnames) return 1;
    last_part(path, base, sizeof(base));
    for(i = 0; i < nnames; i++)
        if(!match(names[i].pattern, base, names[i].nocase)) return 0;
    return 1;
}

static void visit(const char *path, int depth, int is_dir) {
    DIR *d;
    struct dirent *e;
    char **list = NULL, *child;
    int *dirs = NULL, n = 0, cap = 0, i;
    size_t len;
    if(depth >= mindepth && passes(path, is_dir)) printf("%s%c", path, ending);
    if(!is_dir || (maxdepth >= 0 && depth >= maxdepth)) return;
    d = opendir(path);
    if(!d) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    while((e = readdir(d))) {
        if(n == cap) {
            int more = cap ? cap * 2 : 32;
            char **l = realloc(list, (size_t)more * sizeof(*list));
            int *t = NULL;
            if(l) {
                list = l;
                t = realloc(dirs, (size_t)more * sizeof(*dirs));
            }
            if(!t) break;
            dirs = t;
            cap = more;
        }
        if(!(list[n] = strdup(e->d_name))) break;
        dirs[n++] = e->d_type == DT_DIR;
    }
    if(e) {                                             /* left early */
        fprintf(stderr, "find: %s: out of memory\n", path);
        status = 1;
    }
    closedir(d);
    len = strlen(path);
    for(i = 0; i < n; i++) {
        child = malloc(len + strlen(list[i]) + 2);
        if(child) {
            sprintf(child, len && path[len - 1] == '/' ? "%s%s" : "%s/%s", path, list[i]);
            visit(child, depth + 1, dirs[i]);
            free(child);
        }
        free(list[i]);
    }
    free(list);
    free(dirs);
}

static int usage(void) {
    fprintf(stderr, "usage: find [path...] [-name PATTERN] [-iname PATTERN] [-type f|d] [-maxdepth N] [-mindepth N] "
                    "[-print0]\n");
    return 2;
}

static int number(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if(!*s || *end || v < 0 || v > 1000000) return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, paths;
    struct stat st;
    while(i < argc && argv[i][0] != '-') i++;          /* the paths come first */
    paths = i - 1;
    for(; i < argc; i++) {
        const char *a = argv[i];
        if(!strcmp(a, "-print0")) ending = 0;
        else if(!strcmp(a, "-print")) ending = '\n';
        else if(i + 1 >= argc) return usage();
        else if(!strcmp(a, "-name") || !strcmp(a, "-iname")) {
            if(nnames == MAX_TESTS) return usage();
            names[nnames].pattern = argv[++i];
            names[nnames++].nocase = a[1] == 'i';
        } else if(!strcmp(a, "-type")) {
            i++;
            if(strcmp(argv[i], "f") && strcmp(argv[i], "d")) return usage();
            type = argv[i][0];
        } else if(!strcmp(a, "-maxdepth")) {
            if(number(argv[++i], &maxdepth) < 0) return usage();
        } else if(!strcmp(a, "-mindepth")) {
            if(number(argv[++i], &mindepth) < 0) return usage();
        } else return usage();
    }
    if(!paths) {
        if(stat(".", &st) < 0) {
            fprintf(stderr, "find: .: %s\n", strerror(errno));
            return 1;
        }
        visit(".", 0, S_ISDIR(st.st_mode));
    }
    for(i = 1; i <= paths; i++) {
        if(stat(argv[i], &st) < 0) {
            fprintf(stderr, "find: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        visit(argv[i], 0, S_ISDIR(st.st_mode));
    }
    fflush(stdout);
    return status;
}
