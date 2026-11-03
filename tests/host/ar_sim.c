/* An in-memory FAT under SurfOS's system calls, for gzip, tar and unzip as they are (11/01/26): ar_sim.h says what it is.
   Built with the host's headers, so everything crossing to the tools is spelled here in SurfOS's terms: the numbers
   below, struct surf_stat, struct surf_dirent. Memory for the tools comes from the host's malloc, not libsurf's, so that
   a sanitizer on the host sees each of their allocations. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>
#include "ar_sim.h"

/* SurfOS's errno values and types */
#define S_EPERM        1
#define S_ENOENT       2
#define S_EIO          5
#define S_EBADF        9
#define S_ENOMEM      12
#define S_EFAULT      14
#define S_EBUSY       16
#define S_EEXIST      17
#define S_ENOTDIR     20
#define S_EISDIR      21
#define S_EINVAL      22
#define S_EMFILE      24
#define S_ENOSPC      28
#define S_ESPIPE      29
#define S_ENAMETOOLONG 36
#define S_ENOTEMPTY   39
#define S_ENOTTY      25
#define S_O_ACCMODE   3
#define S_O_RDONLY    0
#define S_O_CREAT     0x40
#define S_O_EXCL      0x80
#define S_O_TRUNC     0x200
#define S_O_APPEND    0x400
#define S_O_DIRECTORY 0x10000
#define S_IFREG       1
#define S_IFDIR       2
#define PATH_LIMIT    256
#define NAME_LIMIT    255
#define NR_OPEN       16
#define SEEK_LIMIT    0x7fffffffL

struct surf_stat { int type; unsigned size, mtime, ino; char fs[8]; char dev[16]; };
struct surf_dirent { char name[256]; int type; unsigned size, mtime; };

int surf_errno;
void *surf_stderr = (void *)"stderr", *surf_stdout = (void *)"stdout";

/**** the disk ****/

struct node {
    char *path;                         /* absolute, as made */
    unsigned char *bytes;
    size_t len, cap;
    size_t hole_at;                     /* a run of zeros begins after this many bytes of bytes */
    unsigned long long hole;
    int dir, device, gone, unreadable;
    unsigned lie;                       /* bytes stat claims that are not there */
    int twin;                           /* half a rename: the other node holding the same data, or -1 */
    unsigned mtime;
};

struct desc {
    int used;
    int node;                           /* -1 for 0, 1 and 2 */
    unsigned long long pos;
    int writing;
    int entry;                          /* a folder being read */
};

static struct node *nodes;
static int nnodes, cap_nodes;
static struct desc fds[NR_OPEN];
static char cwd[PATH_LIMIT * 2];
static unsigned clock_now = 1000;

static const unsigned char *in_bytes;
static size_t in_len, in_pos, in_chunk;
static int in_pipe, tty_in, tty_out;
static unsigned char *out_bytes;
static size_t out_len, out_cap;
static char *err_text;
static size_t err_len, err_cap;
static size_t out_limit;
static int out_limited;

static int change_count, fail_nth, fail_err, pull_nth, half_rename, most_open, long_paths, left_open;
static unsigned long long bytes_read;
static long long read_left = -1;        /* files read this many bytes more, then EIO; -1: no limit */
static size_t quota, used_bytes;
static int quota_on, mark_from;
static jmp_buf finish;
static char list_buf[1 << 18];

static void *need(void *p) {            /* the test's own memory: never short */
    if(!p) abort();
    return p;
}

static int fail_with(int e) {
    surf_errno = e;
    return -1;
}

/* always terminated, though it is bytes: a test may look for text in it */
static void grow_out(const void *p, size_t n) {
    if(out_len + n + 1 > out_cap) {
        out_cap = (out_len + n + 1) * 2;
        out_bytes = need(realloc(out_bytes, out_cap));
    }
    if(n) memcpy(out_bytes + out_len, p, n);
    out_len += n;
    out_bytes[out_len] = 0;
}

static void grow_err(const void *p, size_t n) {
    if(err_len + n + 1 > err_cap) {
        err_cap = (err_len + n + 1) * 2;
        err_text = need(realloc(err_text, err_cap));
    }
    if(n) memcpy(err_text + err_len, p, n);
    err_len += n;
    err_text[err_len] = 0;
}

/* cwd and path as one absolute path: 0, or the error */
static int normalize(const char *path, char *out) {
    char tmp[PATH_LIMIT * 3];
    char *comp[PATH_LIMIT];
    int n = 0, i;
    char *p, *save;
    size_t len = 0;
    if(strlen(path) >= PATH_LIMIT) {
        long_paths++;
        return S_EFAULT;
    }
    if(!*path) return S_ENOENT;
    snprintf(tmp, sizeof(tmp), "%s/%s", path[0] == '/' ? "" : cwd, path);
    for(p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if(!strcmp(p, ".")) continue;
        if(!strcmp(p, "..")) {
            if(n) n--;
            continue;
        }
        comp[n++] = p;
    }
    out[0] = '/';
    out[1] = 0;
    for(i = 0; i < n; i++) {
        size_t k = strlen(comp[i]);
        if(len + 1 + k + 1 > PATH_LIMIT) return S_ENAMETOOLONG;
        out[len] = '/';
        memcpy(out + len + 1, comp[i], k + 1);
        len += 1 + k;
    }
    return 0;
}

static int lookup(const char *abs) {
    int i;
    for(i = 0; i < nnodes; i++) if(!nodes[i].gone && !strcasecmp(nodes[i].path, abs)) return i;
    return -1;
}

static int make_node(const char *abs, int dir) {
    struct node *x;
    if(nnodes == cap_nodes) {
        cap_nodes = cap_nodes ? cap_nodes * 2 : 64;
        nodes = need(realloc(nodes, (size_t)cap_nodes * sizeof(*nodes)));
    }
    x = &nodes[nnodes];
    memset(x, 0, sizeof(*x));
    x->path = need(strdup(abs));
    x->dir = dir;
    x->twin = -1;
    x->mtime = ++clock_now;
    return nnodes++;
}

/* the folder abs is directly in: its node, or -1 and *err */
static int parent_of(const char *abs, int *err) {
    char up[PATH_LIMIT * 2];
    char *slash;
    int n;
    strcpy(up, abs);
    slash = strrchr(up, '/');
    if(slash == up) slash[1] = 0;
    else *slash = 0;
    n = lookup(up);
    if(n < 0) *err = S_ENOENT;
    else if(!nodes[n].dir) {
        *err = S_ENOTDIR;
        n = -1;
    }
    return n;
}

/* fs/fat.c's validate_name(), on the last component */
static int bad_name(const char *abs) {
    const unsigned char *s = (const unsigned char *)strrchr(abs, '/') + 1, *p;
    size_t n = strlen((const char *)s);
    if(!n) return S_EINVAL;
    if(n > NAME_LIMIT) return S_ENAMETOOLONG;
    if(!strcmp((const char *)s, ".") || !strcmp((const char *)s, "..")) return S_EINVAL;
    if(s[n - 1] == ' ' || s[n - 1] == '.') return S_EINVAL;
    for(p = s; *p;) {
        unsigned c = *p, need_bytes, cp, i, least;
        if(c < 0x20 || c == 0x7f || strchr("\\/:*?\"<>|", (int)c)) return S_EINVAL;
        if(c < 0x80) {
            p++;
            continue;
        }
        if(c >= 0xC2 && c <= 0xDF) { need_bytes = 1; cp = c & 0x1F; least = 0x80; }
        else if(c >= 0xE0 && c <= 0xEF) { need_bytes = 2; cp = c & 0x0F; least = 0x800; }
        else if(c >= 0xF0 && c <= 0xF4) { need_bytes = 3; cp = c & 0x07; least = 0x10000; }
        else return S_EINVAL;
        for(i = 1; i <= need_bytes; i++) {
            if((p[i] & 0xC0) != 0x80) return S_EINVAL;
            cp = (cp << 6) | (p[i] & 0x3Fu);
        }
        if(cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return S_EINVAL;
        p += need_bytes + 1;
    }
    return 0;
}

/* is n open on a descriptor? fs/fat.c's live_node(): such a file is neither removed nor renamed over */
static int in_use(int n) {
    int i;
    for(i = 3; i < NR_OPEN; i++) if(fds[i].used && fds[i].node == n) return 1;
    return 0;
}

/* The disk has gone: from then on the kernel answers EIO to everything, a lookup too (fs/fat.c's fat_lookup()) */
static int pulled(void) {
    return pull_nth && change_count >= pull_nth;
}

/* one change to the disk: 0, or the error it fails with */
static int change(void) {
    change_count++;
    if(pull_nth && change_count >= pull_nth) return S_EIO;
    if(fail_nth && change_count == fail_nth) {
        fail_nth = 0;
        return fail_err;
    }
    return 0;
}

static unsigned long long node_size(const struct node *x) {
    return x->len + x->hole;
}

static void free_data(struct node *x) {
    used_bytes -= x->len;
    free(x->bytes);
    x->bytes = NULL;
    x->len = x->cap = 0;
}

static void store(struct node *x, unsigned long long pos, const void *p, size_t n) {
    size_t end = (size_t)pos + n;
    if(end > x->cap) {
        x->cap = end * 2;
        x->bytes = need(realloc(x->bytes, x->cap));
    }
    if((size_t)pos > x->len) memset(x->bytes + x->len, 0, (size_t)pos - x->len);
    memcpy(x->bytes + pos, p, n);
    if(end > x->len) {
        used_bytes += end - x->len;
        x->len = end;
    }
}

/* the data of twin t goes with x's: a rename half done leaves both names on one chain */
static void untwin(int n) {
    if(nodes[n].twin >= 0) {
        nodes[nodes[n].twin].twin = -1;
        nodes[n].twin = -1;
    }
}

/**** descriptors ****/

static int new_fd(int node, int writing) {
    int fd, i, open_now = 0;
    for(fd = 3; fd < NR_OPEN && fds[fd].used; fd++) {}
    if(fd == NR_OPEN) return fail_with(S_EMFILE);
    fds[fd].used = 1;
    fds[fd].node = node;
    fds[fd].pos = 0;
    fds[fd].writing = writing;
    fds[fd].entry = 0;
    for(i = 3; i < NR_OPEN; i++) open_now += fds[i].used;
    if(open_now > most_open) most_open = open_now;
    return fd;
}

int surf_open(const char *path, int flags, ...) {
    char abs[PATH_LIMIT * 2];
    int e = normalize(path, abs), n, err = 0, writing = (flags & S_O_ACCMODE) != S_O_RDONLY;
    if(e) return fail_with(e);
    if(pulled()) return fail_with(S_EIO);
    n = lookup(abs);
    if(n >= 0 && (flags & S_O_CREAT) && (flags & S_O_EXCL)) return fail_with(S_EEXIST);
    if(n >= 0 && nodes[n].dir && writing) return fail_with(S_EISDIR);
    if(n >= 0 && !nodes[n].dir && (flags & S_O_DIRECTORY)) return fail_with(S_ENOTDIR);
    if(n >= 0 && nodes[n].unreadable) return fail_with(S_EIO);
    if(n < 0) {
        if(!(flags & S_O_CREAT)) {
            if(parent_of(abs, &err) < 0) return fail_with(err);
            return fail_with(S_ENOENT);
        }
        if(parent_of(abs, &err) < 0) return fail_with(err);
        if((e = bad_name(abs))) return fail_with(e);
        if((e = change())) return fail_with(e);
        n = make_node(abs, 0);
    } else if(flags & S_O_TRUNC) {
        if((e = change())) return fail_with(e);
        untwin(n);
        free_data(&nodes[n]);
    }
    return new_fd(n, writing);
}

int surf_close(int fd) {
    int e = 0;
    if(fd < 0 || fd >= NR_OPEN || !fds[fd].used) return fail_with(S_EBADF);
    if(fd >= 3 && fds[fd].writing) e = change();
    if(fd >= 3) fds[fd].used = 0;
    return e ? fail_with(e) : 0;
}

int surf_read(int fd, void *buf, size_t len) {
    struct node *x;
    unsigned long long size, pos;
    size_t done = 0;
    if(fd == 0) {
        size_t n = in_len - in_pos;
        if(n > len) n = len;
        if(in_chunk && n > in_chunk) n = in_chunk;
        if(n) memcpy(buf, in_bytes + in_pos, n);
        in_pos += n;
        return (int)n;
    }
    if(fd < 0 || fd >= NR_OPEN || !fds[fd].used || fd < 3) return fail_with(S_EBADF);
    x = &nodes[fds[fd].node];
    if(x->dir) return fail_with(S_EISDIR);
    if(pulled() || !read_left) return fail_with(S_EIO);
    size = node_size(x);
    pos = fds[fd].pos;
    if(pos >= size) return 0;
    if(len > size - pos) len = (size_t)(size - pos);
    if(read_left > 0 && (long long)len > read_left) len = (size_t)read_left;
    while(done < len) {                                 /* over the hole, if the file has one */
        unsigned long long at = pos + done;
        size_t left = len - done, k;
        if(at < x->hole_at) {
            k = left < x->hole_at - at ? left : (size_t)(x->hole_at - at);
            memcpy((char *)buf + done, x->bytes + at, k);
        } else if(at < x->hole_at + x->hole) {
            unsigned long long z = x->hole_at + x->hole - at;
            k = left < z ? left : (size_t)z;
            memset((char *)buf + done, 0, k);
        } else {
            k = left;
            memcpy((char *)buf + done, x->bytes + (at - x->hole), k);
        }
        done += k;
    }
    fds[fd].pos += done;
    bytes_read += done;
    if(read_left > 0) read_left -= (long long)done;
    return (int)done;
}

int surf_write(int fd, const void *buf, size_t len) {
    struct node *x;
    int e;
    if(fd == 1) {
        if(out_limited && out_len + len > out_limit) {
            surf_errno = 32;
            longjmp(finish, 1 + 141);
        }
        grow_out(buf, len);
        return (int)len;
    }
    if(fd == 2) {
        grow_err(buf, len);
        return (int)len;
    }
    if(fd < 3 || fd >= NR_OPEN || !fds[fd].used || !fds[fd].writing) return fail_with(S_EBADF);
    if((e = change())) return fail_with(e);
    x = &nodes[fds[fd].node];
    if(quota_on && used_bytes + len > quota) {
        size_t room = quota > used_bytes ? quota - used_bytes : 0;
        if(!room) return fail_with(S_ENOSPC);
        len = room;                                     /* a short write: what fitted */
    }
    store(x, fds[fd].pos, buf, len);
    fds[fd].pos += len;
    x->mtime = ++clock_now;
    return (int)len;
}

long surf_lseek(int fd, long off, int whence) {
    long long base, to;
    if(fd == 0) {
        if(in_pipe) return fail_with(S_ESPIPE);
        base = whence == 0 ? 0 : whence == 1 ? (long long)in_pos : (long long)in_len;
        to = base + off;
        if(to < 0 || to > SEEK_LIMIT) return fail_with(S_EINVAL);
        in_pos = (size_t)to > in_len ? in_len : (size_t)to;
        return (long)to;
    }
    if(fd < 3 || fd >= NR_OPEN || !fds[fd].used) return fail_with(S_EBADF);
    /* fs/vfs.c's vfs_lseek(): 32 signed bits everywhere, so nothing reaches past 2 GB and a base past it is refused */
    base = whence == 0 ? 0 : whence == 1 ? (long long)fds[fd].pos : whence == 2 ? (long long)node_size(&nodes[fds[fd].node]) : -1;
    if(base < 0 || base > SEEK_LIMIT) return fail_with(S_EINVAL);
    if(off > SEEK_LIMIT || off < -SEEK_LIMIT - 1) return fail_with(S_EINVAL);
    to = base + off;
    if(to < 0 || to > SEEK_LIMIT) return fail_with(S_EINVAL);
    fds[fd].pos = (unsigned long long)to;
    return (long)to;
}

int surf_fsync(int fd) {
    int e;
    if(fd < 3 || fd >= NR_OPEN || !fds[fd].used) return fail_with(S_EBADF);
    if((e = change())) return fail_with(e);
    return 0;
}

void surf_sync(void) {
    change();
}

static void fill_stat(int n, struct surf_stat *st) {
    memset(st, 0, sizeof(*st));
    st->type = nodes[n].dir ? S_IFDIR : S_IFREG;
    st->size = (unsigned)node_size(&nodes[n]) + nodes[n].lie;
    st->mtime = nodes[n].mtime;
    st->ino = (unsigned)n + 1;
    strcpy(st->fs, nodes[n].device ? "devfs" : "fat");
}

int surf_fstat(int fd, struct surf_stat *st) {
    if(fd >= 0 && fd < 3) {
        memset(st, 0, sizeof(*st));
        st->type = S_IFREG;
        if(fd == 0 && !in_pipe) st->size = (unsigned)in_len;
        strcpy(st->fs, in_pipe ? "pipefs" : "fat");
        return 0;
    }
    if(fd < 0 || fd >= NR_OPEN || !fds[fd].used) return fail_with(S_EBADF);
    fill_stat(fds[fd].node, st);
    return 0;
}

int surf_stat(const char *path, struct surf_stat *st) {
    char abs[PATH_LIMIT * 2];
    int e = normalize(path, abs), n, err = 0;
    if(e) return fail_with(e);
    if(pulled()) return fail_with(S_EIO);
    n = lookup(abs);
    if(n < 0) {
        if(strcmp(abs, "/") && parent_of(abs, &err) < 0) return fail_with(err);
        return fail_with(S_ENOENT);
    }
    fill_stat(n, st);
    return 0;
}

int surf_mkdir(const char *path, unsigned mode) {
    char abs[PATH_LIMIT * 2];
    int e = normalize(path, abs), err = 0;
    (void)mode;
    if(e) return fail_with(e);
    if(lookup(abs) >= 0) return fail_with(S_EEXIST);
    if(parent_of(abs, &err) < 0) return fail_with(err);
    if((e = bad_name(abs))) return fail_with(e);
    if((e = change())) return fail_with(e);
    make_node(abs, 1);
    return 0;
}

int surf_rmdir(const char *path) {
    char abs[PATH_LIMIT * 2];
    size_t len;
    int e = normalize(path, abs), n, i;
    if(e) return fail_with(e);
    n = lookup(abs);
    if(n < 0) return fail_with(S_ENOENT);
    if(!nodes[n].dir) return fail_with(S_ENOTDIR);
    len = strlen(abs);
    for(i = 0; i < nnodes; i++) {
        if(!nodes[i].gone && !strncasecmp(nodes[i].path, abs, len) && nodes[i].path[len] == '/') return fail_with(S_ENOTEMPTY);
    }
    if((e = change())) return fail_with(e);
    nodes[n].gone = 1;
    return 0;
}

int surf_unlink(const char *path) {
    char abs[PATH_LIMIT * 2];
    int e = normalize(path, abs), n;
    if(e) return fail_with(e);
    n = lookup(abs);
    if(n < 0) return fail_with(S_ENOENT);
    if(nodes[n].dir) return fail_with(S_EISDIR);
    if(in_use(n)) return fail_with(S_EBUSY);
    if((e = change())) return fail_with(e);
    untwin(n);
    free_data(&nodes[n]);
    nodes[n].gone = 1;
    return 0;
}

int surf_rename(const char *from, const char *to) {
    char a[PATH_LIMIT * 2], b[PATH_LIMIT * 2];
    int e = normalize(from, a), x, y, err = 0;
    if(!e) e = normalize(to, b);
    if(e) return fail_with(e);
    x = lookup(a);
    y = lookup(b);
    if(x < 0) return fail_with(S_ENOENT);
    if(parent_of(b, &err) < 0) return fail_with(err);
    if(y >= 0 && nodes[y].dir) return fail_with(S_EISDIR);
    if(y >= 0 && y != x && in_use(y)) return fail_with(S_EBUSY);
    if((e = bad_name(b))) return fail_with(e);
    if(half_rename) {                                   /* the new name written, the old one not yet gone */
        half_rename = 0;
        if(y < 0) y = make_node(b, 0);
        free_data(&nodes[y]);
        if(nodes[x].len) store(&nodes[y], 0, nodes[x].bytes, nodes[x].len);
        nodes[y].mtime = nodes[x].mtime;
        nodes[x].twin = y;
        nodes[y].twin = x;
        return fail_with(S_EIO);
    }
    if((e = change())) return fail_with(e);
    if(y >= 0 && y != x) {
        untwin(y);
        free_data(&nodes[y]);
        nodes[y].gone = 1;
    }
    free(nodes[x].path);
    nodes[x].path = need(strdup(b));
    return 0;
}

int surf_chdir(const char *path) {
    char abs[PATH_LIMIT * 2];
    int e = normalize(path, abs), n;
    if(e) return fail_with(e);
    n = lookup(abs);
    if(n < 0) return fail_with(S_ENOENT);
    if(!nodes[n].dir) return fail_with(S_ENOTDIR);
    strcpy(cwd, nodes[n].path);
    return 0;
}

char *surf_getcwd(char *buf, size_t size) {
    if(strlen(cwd) + 1 > size) {
        surf_errno = S_ENAMETOOLONG;
        return NULL;
    }
    strcpy(buf, cwd);
    return buf;
}

int surf_isatty(int fd) {
    if((fd == 0 && tty_in) || ((fd == 1 || fd == 2) && tty_out)) return 1;
    surf_errno = S_ENOTTY;
    return 0;
}

/* folders: a descriptor each, as in libsurf's dirent.c */

struct sim_dir { int fd; struct surf_dirent de; };

void *surf_opendir(const char *path) {
    struct sim_dir *d;
    int fd = surf_open(path, S_O_DIRECTORY);
    if(fd < 0) return NULL;
    if(!nodes[fds[fd].node].dir) {
        surf_close(fd);
        surf_errno = S_ENOTDIR;
        return NULL;
    }
    d = need(malloc(sizeof(*d)));
    d->fd = fd;
    return d;
}

struct surf_dirent *surf_readdir(void *dp) {
    struct sim_dir *d = dp;
    const char *base = nodes[fds[d->fd].node].path;
    size_t len = strlen(base);
    if(len == 1) len = 0;                               /* "/": what follows it starts at once */
    while(fds[d->fd].entry < nnodes) {
        const struct node *x = &nodes[fds[d->fd].entry++];
        if(x->gone || strncasecmp(x->path, base, len) || x->path[len] != '/' || !x->path[len + 1]) continue;
        if(strchr(x->path + len + 1, '/')) continue;
        memset(&d->de, 0, sizeof(d->de));
        strcpy(d->de.name, x->path + len + 1);
        d->de.type = x->dir ? S_IFDIR : S_IFREG;
        d->de.size = (unsigned)node_size(x);
        d->de.mtime = x->mtime;
        return &d->de;
    }
    return NULL;
}

int surf_closedir(void *dp) {
    struct sim_dir *d = dp;
    int r = surf_close(d->fd);
    free(d);
    return r;
}

/* stdio: only what the tools print, formatted by libsurf's own formatter */

#ifdef AR_HOST_PRINTF
/* the Mac's native build: libsurf's formatter divides with x86's divl, so the host's stands in for it there. The tools use
   nothing where the two differ (%s %d %u %llu %o, widths, %.*s); `make unittest`, at -m32, uses libsurf's own. */
int surf_vsnprintf(char *s, size_t n, const char *fmt, va_list ap) {
    return vsnprintf(s, n, fmt, ap);
}

int surf_snprintf(char *s, size_t n, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}
#else
int surf_vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
#endif

static void put_text(void *f, const char *s, size_t n) {
    if(f == surf_stderr) grow_err(s, n);
    else surf_write(1, s, n);
}

int surf_fprintf(void *f, const char *fmt, ...) {
    static char buf[1 << 16];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = surf_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put_text(f, buf, strlen(buf));
    return n;
}

int surf_printf(const char *fmt, ...) {
    static char buf[1 << 16];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = surf_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put_text(surf_stdout, buf, strlen(buf));
    return n;
}

int surf_fputs(const char *s, void *f) {
    put_text(f, s, strlen(s));
    return 0;
}

int surf_fflush(void *f) {
    (void)f;
    return 0;
}

void surf__exit(int code) {
    longjmp(finish, 1 + (code & 255));
}

void *surf_malloc(size_t n) { return malloc(n); }
void *surf_calloc(size_t n, size_t size) { return calloc(n, size); }
void *surf_realloc(void *p, size_t n) { return realloc(p, n); }
void surf_free(void *p) { free(p); }

/**** what the test calls ****/

void ars_reset(void) {
    int i;
    for(i = 0; i < nnodes; i++) {
        free(nodes[i].path);
        free(nodes[i].bytes);
    }
    nnodes = 0;
    memset(fds, 0, sizeof(fds));
    fds[0].used = fds[1].used = fds[2].used = 1;
    strcpy(cwd, "/");
    in_bytes = NULL;
    in_len = in_pos = in_chunk = 0;
    in_pipe = 1;
    tty_in = tty_out = 0;
    out_limited = 0;
    fail_nth = pull_nth = half_rename = 0;
    quota_on = 0;
    used_bytes = 0;
    change_count = 0;
    mark_from = 0;
    make_node("/", 1);
}

static void make_above(const char *abs) {
    char up[PATH_LIMIT * 2];
    char *p;
    strcpy(up, abs);
    for(p = strchr(up + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        if(lookup(up) < 0) make_node(up, 1);
        *p = '/';
    }
}

void ars_put(const char *path, const void *bytes, size_t n) {
    char abs[PATH_LIMIT * 2];
    int x;
    if(normalize(path, abs)) abort();
    make_above(abs);
    x = lookup(abs);
    if(x < 0) x = make_node(abs, 0);
    free_data(&nodes[x]);
    if(n) store(&nodes[x], 0, bytes, n);
    nodes[x].mtime = ++clock_now;
}

void ars_puts(const char *path, const char *text) {
    ars_put(path, text, strlen(text));
}

void ars_mkdir(const char *path) {
    char abs[PATH_LIMIT * 2];
    if(normalize(path, abs)) abort();
    make_above(abs);
    if(lookup(abs) < 0) make_node(abs, 1);
}

void ars_device(const char *path) {
    char abs[PATH_LIMIT * 2];
    if(normalize(path, abs)) abort();
    make_above(abs);
    nodes[make_node(abs, 0)].device = 1;
}

void ars_put_hole(const char *path, const void *bytes, size_t head, unsigned long long hole, size_t tail) {
    int x;
    char abs[PATH_LIMIT * 2];
    ars_put(path, bytes, head + tail);
    if(normalize(path, abs)) abort();
    x = lookup(abs);
    nodes[x].hole_at = head;
    nodes[x].hole = hole;
}

const unsigned char *ars_get(const char *path, size_t *n) {
    char abs[PATH_LIMIT * 2];
    int x;
    static const unsigned char none[1];
    if(normalize(path, abs)) return NULL;
    x = lookup(abs);
    if(x < 0 || nodes[x].dir) return NULL;
    if(n) *n = nodes[x].len;
    return nodes[x].bytes ? nodes[x].bytes : none;
}

int ars_is(const char *path, const char *text) {
    size_t n = 0;
    const unsigned char *p = ars_get(path, &n);
    return p && n == strlen(text) && !memcmp(p, text, n);
}

int ars_is_dir(const char *path) {
    char abs[PATH_LIMIT * 2];
    int x;
    if(normalize(path, abs)) return 0;
    x = lookup(abs);
    return x >= 0 && nodes[x].dir;
}

int ars_exists(const char *path) {
    char abs[PATH_LIMIT * 2];
    return !normalize(path, abs) && lookup(abs) >= 0;
}

int ars_count(void) {
    int i, k = 0;
    for(i = 1; i < nnodes; i++) k += !nodes[i].gone;
    return k;
}

const char *ars_list(void) {
    size_t n = 0;
    int i;
    list_buf[0] = 0;
    for(i = 1; i < nnodes; i++) {
        if(nodes[i].gone) continue;
        n += (size_t)snprintf(list_buf + n, sizeof(list_buf) - n, "%s\n", nodes[i].path);
        if(n >= sizeof(list_buf)) abort();
    }
    return list_buf;
}

void ars_mark(void) {
    mark_from = nnodes;
}

int ars_all_under(const char *dir) {
    size_t len = strlen(dir);
    int i;
    for(i = mark_from; i < nnodes; i++) {
        if(nodes[i].gone) continue;
        if(strncasecmp(nodes[i].path, dir, len) || (nodes[i].path[len] != '/' && nodes[i].path[len])) return 0;
    }
    return 1;
}

void ars_chdir(const char *path) {
    if(surf_chdir(path) < 0) abort();
}

void ars_stdin(const void *bytes, size_t n, int is_pipe, size_t chunk) {
    in_bytes = bytes;
    in_len = n;
    in_pos = 0;
    in_pipe = is_pipe;
    in_chunk = chunk;
}

void ars_ttys(int in, int out) {
    tty_in = in;
    tty_out = out;
}

void ars_stdout_closes_after(size_t n) {
    out_limited = 1;
    out_limit = n;
}

const unsigned char *ars_stdout(size_t *n) {
    static const unsigned char none[1];
    if(n) *n = out_len;
    return out_bytes ? out_bytes : none;
}

const char *ars_stderr(void) {
    return err_text ? err_text : "";
}

void ars_fail_change(int nth, int err) {
    fail_nth = nth;
    fail_err = err;
}

void ars_pull_at(int nth) {
    pull_nth = nth;
}

void ars_quota(size_t bytes) {
    quota = bytes;
    quota_on = 1;
}

void ars_rename_half(void) {
    half_rename = 1;
}

int ars_changes(void) {
    return change_count;
}

int ars_open_files(void) {
    return left_open;
}

void ars_unreadable(const char *path) {
    char x[PATH_LIMIT * 2];
    if(normalize(path, x) || lookup(x) < 0) abort();
    nodes[lookup(x)].unreadable = 1;
}

unsigned long long ars_bytes_read(void) {
    return bytes_read;
}

void ars_read_fails_after(size_t n) {
    read_left = (long long)n;
}

void ars_lie_size(const char *path, unsigned extra) {
    char x[PATH_LIMIT * 2];
    if(normalize(path, x) || lookup(x) < 0) abort();
    nodes[lookup(x)].lie = extra;
}

void ars_same_time(const char *a, const char *b) {
    char x[PATH_LIMIT * 2], y[PATH_LIMIT * 2];
    if(normalize(a, x) || normalize(b, y) || lookup(x) < 0 || lookup(y) < 0) abort();
    nodes[lookup(y)].mtime = nodes[lookup(x)].mtime;
}

int ars_most_open(void) {
    return most_open;
}

int ars_long_paths(void) {
    return long_paths;
}

int ars_run(ars_tool tool, const char *const *argv) {
    static char *copy[256];
    int argc = 0, r, i;
    while(argv[argc]) {
        if(argc == 255) abort();
        copy[argc] = need(strdup(argv[argc]));
        argc++;
    }
    copy[argc] = NULL;
    out_len = 0;
    grow_out("", 0);
    err_len = 0;
    grow_err("", 0);
    change_count = 0;
    most_open = 0;
    long_paths = 0;
    bytes_read = 0;
    r = setjmp(finish);
    if(!r) r = tool(argc, copy);
    else r--;
    for(i = 0; i < argc; i++) free(copy[i]);
    left_open = 0;
    for(i = 3; i < NR_OPEN; i++) {                      /* a process's end closes them */
        left_open += fds[i].used;
        fds[i].used = 0;
    }
    fail_nth = 0;
    pull_nth = 0;
    read_left = -1;
    return r;
}

int ars_cmd(ars_tool tool, const char *line) {
    static char words[4096];
    const char *argv[64];
    char *p, *save;
    int n = 0;
    snprintf(words, sizeof(words), "%s", line);
    for(p = strtok_r(words, " ", &save); p; p = strtok_r(NULL, " ", &save)) {
        if(n == 63) abort();
        argv[n++] = p;
    }
    argv[n] = NULL;
    return ars_run(tool, argv);
}
