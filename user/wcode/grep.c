/*
SurfOS user program: wcode
--------------------
File: grep.c    Date: 11/05/26
--------------------
Find in files (grep.h). The walk is a loop over a stack of folder paths, each with its depth: a
folder is read an entry at a time, a file in it is read and searched before the next entry, and
its subfolders go on the stack, in the order they came, to be read after it. A file is read in
chunks of WC_GREP_CHUNK; a line that a chunk cut is carried over to the next, up to WC_GREP_LINE,
past which the rest of that line is not searched. A file whose first chunk has a NUL byte is no
text and is left. Every limit stops the search or leaves something out and counts it, never more
work or memory than grep.h's numbers.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grep.h"

void wc_grep_init(struct wc_grep *g) {
    memset(g, 0, sizeof(*g));
    g->dh = g->fh = -1;
    wc_find_init(&g->f);
}

static int fold(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* * and ? against a name, either case as FAT's names are: the two-pointer way, with no recursion */
static int glob(const char *p, const char *s) {
    const char *star = NULL, *back = NULL;
    while(*s) {
        if(*p == '*') {
            star = p++;
            back = s;
        } else if(*p && (*p == '?' || fold((unsigned char)*p) == fold((unsigned char)*s))) {
            p++;
            s++;
        } else if(star) {
            p = star + 1;
            s = ++back;
        } else return 0;
    }
    while(*p == '*') p++;
    return !*p;
}

int wc_grep_name_ok(const char *filter, const char *name) {
    char w[WC_GREP_FILTER];
    const char *p = filter;
    unsigned n;
    int any = 0;
    for(;;) {
        while(*p == ' ' || *p == ',' || *p == ';') p++;
        if(!*p) return !any;
        for(n = 0; p[n] && p[n] != ' ' && p[n] != ',' && p[n] != ';'; n++);
        any = 1;
        if(n < sizeof(w)) {
            memcpy(w, p, n);
            w[n] = 0;
            if(glob(w, name)) return 1;
        }
        p += n;
    }
}

/* the devices and the network manager's mount are never read */
static int system_path(const char *p) {
    return !strncmp(p, "/dev", 4) && (!p[4] || p[4] == '/') ? 1 :
           !strncmp(p, "/.netman", 8) && (!p[8] || p[8] == '/');
}

static int join(char *out, const char *dir, const char *name) {
    size_t n = strlen(dir);
    int w = snprintf(out, WC_PATH, "%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/", name);
    return w > 0 && w < WC_PATH;
}

static void finish(struct wc_grep *g, const char *why) {
    if(g->fh >= 0) g->fs->close(g->fs->ctx, g->fh);
    if(g->dh >= 0) g->fs->closedir(g->fs->ctx, g->dh);
    g->fh = g->dh = -1;
    g->state = WC_GREP_DONE;
    if(why && !g->why[0]) snprintf(g->why, sizeof(g->why), "%s", why);
}

void wc_grep_stop(struct wc_grep *g) {
    if(g->state == WC_GREP_RUN) finish(g, "Stopped");
}

void wc_grep_free(struct wc_grep *g) {
    wc_grep_stop(g);
    free(g->pend);
    free(g->buf);
    free(g->hits);
    free(g->paths);
    wc_find_free(&g->f);
    g->pend = g->buf = g->paths = NULL;
    g->hits = NULL;
    g->nhits = g->paths_used = g->paths_cap = 0;
}

const char *wc_grep_path(const struct wc_grep *g, const struct wc_hit *h) {
    return g->paths + h->file;
}

/**** the folders waiting: [depth + 1][path][NUL] after one another ****/

static int push(struct wc_grep *g, const char *path, int depth) {
    unsigned n = (unsigned)strlen(path) + 2;
    if(g->pend_used + n > WC_GREP_PENDING) return -1;
    g->pend[g->pend_used] = (char)(depth + 1);
    memcpy(g->pend + g->pend_used + 1, path, n - 1);
    g->pend_used += n;
    return 0;
}

/* the subfolders a folder pushed, turned round, so that they come off the stack in the order they came */
static void turn_round(struct wc_grep *g) {
    unsigned seg = g->pend_used - g->dir_mark, k, e, out = g->dir_mark;
    char *tmp;
    if(!seg) return;
    tmp = malloc(seg);
    if(!tmp) return;                            /* they come in the other order */
    memcpy(tmp, g->pend + g->dir_mark, seg);
    for(e = seg; e > 0; e = k) {
        for(k = e - 1; k > 0 && tmp[k - 1]; k--);
        memcpy(g->pend + out, tmp + k, e - k);
        out += e - k;
    }
    free(tmp);
}

/* the next folder off the stack, opened: 0 when there is none */
static int pop(struct wc_grep *g) {
    unsigned k;
    if(!g->pend_used) return 0;
    for(k = g->pend_used - 1; k > 0 && g->pend[k - 1]; k--);
    g->depth = g->pend[k] - 1;
    snprintf(g->dir, sizeof(g->dir), "%s", g->pend + k + 1);
    g->pend_used = k;
    if(++g->dirs > WC_GREP_DIRS) {
        snprintf(g->why, sizeof(g->why), "Stopped after %u folders", WC_GREP_DIRS);
        finish(g, NULL);
        return 1;
    }
    g->dh = g->fs->opendir(g->fs->ctx, g->dir);
    if(g->dh < 0) g->left_out++;
    g->dir_mark = g->pend_used;
    return 1;
}

/**** what it found ****/

static void add_hit(struct wc_grep *g, const char *s, unsigned n, unsigned a, unsigned b) {
    struct wc_hit *h;
    unsigned from, to, k, len;
    char *p;
    if(g->nhits >= WC_GREP_HITS) {
        snprintf(g->why, sizeof(g->why), "The first %u matching lines", WC_GREP_HITS);
        finish(g, NULL);
        return;
    }
    if(!g->fhit) {                              /* the file's path, once */
        len = (unsigned)strlen(g->file) + 1;
        if(g->paths_used + len > g->paths_cap) {
            unsigned cap = g->paths_cap ? g->paths_cap * 2 : 4096;
            while(cap < g->paths_used + len) cap *= 2;
            p = realloc(g->paths, cap);
            if(!p) {
                finish(g, "Stopped: there is not enough memory");
                return;
            }
            g->paths = p;
            g->paths_cap = cap;
        }
        memcpy(g->paths + g->paths_used, g->file, len);
        g->fhit = g->paths_used + 1;
        g->paths_used += len;
    }
    h = &g->hits[g->nhits++];
    h->file = g->fhit - 1;
    h->line = g->fline;
    h->col = a;
    /* the line around the match, cut between characters */
    from = a > 40 ? a - 40 : 0;
    while(from < a && ((unsigned char)s[from] & 0xC0) == 0x80) from++;
    to = from + WC_GREP_SHOW - 1 < n ? from + WC_GREP_SHOW - 1 : n;
    while(to > from && to < n && ((unsigned char)s[to] & 0xC0) == 0x80) to--;
    for(k = from; k < to; k++) h->text[k - from] = s[k] ? s[k] : ' ';
    h->text[to - from] = 0;
    h->a = (unsigned short)(a - from);
    h->b = (unsigned short)((b < to ? b : to) - from);
}

static void search_line(struct wc_grep *g, const char *s, unsigned n) {
    unsigned a, b;
    if(n && s[n - 1] == '\r') n--;
    if(wc_find_in(&g->f, s, n, 0, &a, &b)) add_hit(g, s, n, a, b);
}

static void close_file(struct wc_grep *g) {
    g->fs->close(g->fs->ctx, g->fh);
    g->fh = -1;
}

/* a chunk of the open file: the work it was */
static unsigned long read_some(struct wc_grep *g) {
    char *p, *q, *end;
    int r = g->fs->read(g->fs->ctx, g->fh, g->buf + g->carry, WC_GREP_CHUNK);
    if(r < 0) {
        g->skipped++;
        close_file(g);
        return 64;
    }
    if(!r) {                                    /* the end: the last line, if it has no line end */
        if(g->carry && !g->fskip) search_line(g, g->buf, g->carry);
        if(g->fh >= 0) close_file(g);
        return 64;
    }
    if(!g->flen && memchr(g->buf + g->carry, 0, (size_t)r)) {
        g->binary++;
        close_file(g);
        return (unsigned long)r;
    }
    g->flen += (unsigned)r;
    g->bytes += (unsigned long)r;
    for(p = g->buf, end = g->buf + g->carry + r; g->state == WC_GREP_RUN && (q = memchr(p, '\n', (size_t)(end - p))); p = q + 1) {
        if(!g->fskip) search_line(g, p, (unsigned)(q - p));
        g->fskip = 0;
        g->fline++;
    }
    g->carry = (unsigned)(end - p);
    if(g->fskip) g->carry = 0;                  /* still the long line's rest */
    else if(g->carry >= WC_GREP_LINE) {
        search_line(g, p, WC_GREP_LINE);
        g->fskip = 1;
        g->carry = 0;
    } else if(g->carry) memmove(g->buf, p, g->carry);
    if(g->bytes > WC_GREP_BYTES && g->state == WC_GREP_RUN) {
        snprintf(g->why, sizeof(g->why), "Stopped after %u MB", WC_GREP_BYTES >> 20);
        finish(g, NULL);
    }
    return (unsigned long)r;
}

/* the open folder's next entry: the work it was */
static unsigned long next_entry(struct wc_grep *g) {
    char name[WC_PATH], path[WC_PATH];
    unsigned bytes = 0;
    int dir = 0, r = g->fs->readdir(g->fs->ctx, g->dh, name, sizeof(name), &dir, &bytes);
    if(r <= 0) {
        g->fs->closedir(g->fs->ctx, g->dh);
        g->dh = -1;
        turn_round(g);
        return 64;
    }
    if(!strcmp(name, ".") || !strcmp(name, "..")) return 16;
    if(!join(path, g->dir, name)) {
        g->left_out++;
        return 16;
    }
    if(dir) {
        if(system_path(path)) return 16;
        if(g->depth + 1 > WC_GREP_DEPTH || push(g, path, g->depth + 1) < 0) g->left_out++;
        return 16;
    }
    if(!wc_grep_name_ok(g->filter, name)) return 16;
    if(bytes > WC_TEXT_MAX) {
        g->skipped++;
        return 16;
    }
    if(++g->files > WC_GREP_FILES) {
        snprintf(g->why, sizeof(g->why), "Stopped after %u files", WC_GREP_FILES);
        finish(g, NULL);
        return 16;
    }
    g->fh = g->fs->open(g->fs->ctx, path);
    if(g->fh < 0) {
        g->skipped++;
        return 16;
    }
    snprintf(g->file, sizeof(g->file), "%s", path);
    g->fline = 1;
    g->flen = g->carry = g->fskip = g->fhit = 0;
    return 64;
}

int wc_grep_start(struct wc_grep *g, const struct wc_fsops *fs, const char *folder) {
    size_t n;
    wc_grep_stop(g);
    g->fs = fs;
    g->state = WC_GREP_IDLE;
    g->why[0] = 0;
    g->nhits = g->paths_used = g->pend_used = 0;
    g->dirs = g->files = g->skipped = g->binary = g->left_out = 0;
    g->bytes = 0;
    if(wc_find_prepare(&g->f) < 0) {
        snprintf(g->why, sizeof(g->why), "%s", g->f.err);
        return -1;
    }
    if(!folder[0] || system_path(folder)) {
        snprintf(g->why, sizeof(g->why), "Not a folder to search: %s", folder[0] ? folder : "(none)");
        return -1;
    }
    if(!g->pend) g->pend = malloc(WC_GREP_PENDING);
    if(!g->buf) g->buf = malloc(WC_GREP_LINE + WC_GREP_CHUNK);
    if(!g->hits) g->hits = malloc(WC_GREP_HITS * sizeof(struct wc_hit));
    if(!g->pend || !g->buf || !g->hits) {
        snprintf(g->why, sizeof(g->why), "There is not enough memory");
        return -1;
    }
    snprintf(g->root, sizeof(g->root), "%s", folder);
    n = strlen(g->root);
    while(n > 1 && g->root[n - 1] == '/') g->root[--n] = 0;
    push(g, g->root, 0);
    g->state = WC_GREP_RUN;
    return 0;
}

int wc_grep_step(struct wc_grep *g, unsigned long budget) {
    unsigned long work = 0;
    while(g->state == WC_GREP_RUN && work < budget) {
        if(g->fh >= 0) work += read_some(g);
        else if(g->dh >= 0) work += next_entry(g);
        else if(!pop(g)) finish(g, NULL);
        else work += 64;
    }
    return g->state == WC_GREP_RUN;
}
