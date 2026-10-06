/*
SurfOS user programs: what cp, mv and rm share
----------------------
File: fileutil.h  Date: 10/5/26
----------------------
Paths made absolute, copying files and directory trees, removing trees. A
directory is read completely before anything in it changes: the kernel's
readdir counts entries by position, so deleting while reading would skip some.
*/

#ifndef _FILEUTIL_H
#define _FILEUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#define FU_UNUSED __attribute__((unused))
#define COPY_CHUNK (32 * 1024)

static FU_UNUSED const char *fu_prog = "?";     /* for the messages */

static FU_UNUSED void fu_error(const char *path) {
    fprintf(stderr, "%s: %s: %s\n", fu_prog, path, strerror(errno));
}

/* dir/name into out; 0, or -1 (ENAMETOOLONG) */
static FU_UNUSED int path_join(char *out, size_t size, const char *dir, const char *name) {
    size_t n = strlen(dir);
    int r = (n && dir[n - 1] == '/') ? snprintf(out, size, "%s%s", dir, name) : snprintf(out, size, "%s/%s", dir, name);
    if(r < 0 || (size_t)r >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/* the last component, ignoring trailing slashes ("a/b/" gives "b/", so callers strip those first) */
static FU_UNUSED const char *path_base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static FU_UNUSED void strip_slashes(char *path) {
    size_t n = strlen(path);
    while(n > 1 && path[n - 1] == '/') path[--n] = 0;
}

/* absolute, without "." and ".." and repeated slashes */
static FU_UNUSED int abs_path(const char *path, char *out, size_t size) {
    char tmp[PATH_MAX * 2];
    char *parts[PATH_MAX / 2], *p, *save;
    int n = 0, i;
    size_t len = 0;
    if(path[0] == '/') snprintf(tmp, sizeof(tmp), "%s", path);
    else {
        char cwd[PATH_MAX];
        if(!getcwd(cwd, sizeof(cwd))) return -1;
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }
    for(p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if(!strcmp(p, ".")) continue;
        if(!strcmp(p, "..")) { if(n) n--; continue; }
        if(n < (int)(sizeof(parts) / sizeof(parts[0]))) parts[n++] = p;
    }
    if(size < 2) { errno = ENAMETOOLONG; return -1; }
    out[0] = '/';
    out[1] = 0;
    for(i = 0; i < n; i++) {
        size_t k = strlen(parts[i]);
        if(len + 1 + k + 1 > size) { errno = ENAMETOOLONG; return -1; }
        out[len] = '/';
        memcpy(out + len + 1, parts[i], k + 1);
        len += 1 + k;
    }
    return 0;
}

/* is path dir itself or inside it? Compared without case: FAT names do not have one */
static FU_UNUSED int path_inside(const char *path, const char *dir) {
    char a[PATH_MAX], b[PATH_MAX];
    size_t n;
    if(abs_path(path, a, sizeof(a)) || abs_path(dir, b, sizeof(b))) return 0;
    n = strlen(b);
    if(n == 1) return 1;
    return !strncasecmp(a, b, n) && (a[n] == '/' || a[n] == 0);
}

static FU_UNUSED void free_names(char **v, int n) {
    while(n > 0) free(v[--n]);
    free(v);
}

/* every name in a directory, read before anything changes it */
static FU_UNUSED char **list_names(const char *dir, int *count) {
    DIR *d = opendir(dir);
    struct dirent *de;
    char **v = NULL;
    int n = 0, cap = 0;
    if(!d) return NULL;
    while((de = readdir(d))) {
        if(n + 1 >= cap) {
            char **nv = realloc(v, (size_t)(cap = cap ? cap * 2 : 16) * sizeof(char *));
            if(!nv) goto nomem;
            v = nv;
        }
        if(!(v[n] = strdup(de->d_name))) goto nomem;
        n++;
    }
    closedir(d);
    if(!v && !(v = calloc(1, sizeof(char *)))) { errno = ENOMEM; return NULL; }
    v[n] = NULL;
    *count = n;
    return v;
nomem:
    closedir(d);
    free_names(v, n);
    errno = ENOMEM;
    return NULL;
}

static FU_UNUSED int write_all(int fd, const char *p, size_t n) {
    while(n) {
        ssize_t w = write(fd, p, n);
        if(w <= 0) { if(!w) errno = ENOSPC; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static FU_UNUSED int copy_file(const char *src, const char *dst) {
    static char *buf;
    int in, out, rc = 0;
    ssize_t n;
    if(!buf && !(buf = malloc(COPY_CHUNK))) { fu_error(src); return 1; }
    in = open(src, O_RDONLY);
    if(in < 0) { fu_error(src); return 1; }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if(out < 0) { fu_error(dst); close(in); return 1; }
    while((n = read(in, buf, COPY_CHUNK)) > 0) {
        if(write_all(out, buf, (size_t)n)) { fu_error(dst); rc = 1; break; }
    }
    if(n < 0) { fu_error(src); rc = 1; }
    close(in);
    if(close(out) < 0 && !rc) { fu_error(dst); rc = 1; }
    return rc;
}

static FU_UNUSED int copy_tree(const char *src, const char *dst) {
    char **names, from[PATH_MAX], to[PATH_MAX];
    struct stat st;
    int n, i, rc = 0;
    if(mkdir(dst, 0777) < 0 && (errno != EEXIST || stat(dst, &st) < 0 || !S_ISDIR(st.type))) {
        if(errno == EEXIST) errno = ENOTDIR;
        fu_error(dst);
        return 1;
    }
    names = list_names(src, &n);
    if(!names) { fu_error(src); return 1; }
    for(i = 0; i < n; i++) {
        if(path_join(from, sizeof(from), src, names[i]) || path_join(to, sizeof(to), dst, names[i])) {
            fu_error(names[i]);
            rc = 1;
            continue;
        }
        if(stat(from, &st) < 0) { fu_error(from); rc = 1; continue; }
        rc |= S_ISDIR(st.type) ? copy_tree(from, to) : copy_file(from, to);
    }
    free_names(names, n);
    return rc;
}

static FU_UNUSED int remove_tree(const char *path) {
    char **names, child[PATH_MAX];
    struct stat st;
    int n, i, rc = 0;
    names = list_names(path, &n);
    if(!names) { fu_error(path); return 1; }
    for(i = 0; i < n; i++) {
        if(path_join(child, sizeof(child), path, names[i])) { fu_error(names[i]); rc = 1; continue; }
        if(stat(child, &st) < 0) { fu_error(child); rc = 1; continue; }
        if(S_ISDIR(st.type)) rc |= remove_tree(child);
        else if(unlink(child) < 0) { fu_error(child); rc = 1; }
    }
    free_names(names, n);
    if(!rc && rmdir(path) < 0) { fu_error(path); rc = 1; }
    return rc;
}

#endif
