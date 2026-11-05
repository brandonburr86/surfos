/*
SurfOS user program: wcode
--------------------
File: grep.h    Date: 11/05/26
--------------------
Find in files (grep.c): every line that matches, in the files of a folder and the folders in it,
with the pattern and the choices of Find (find.h), as a list to open each from. It walks the
folders without recursion, through a stack of the paths still to read, and works a piece at a
time (wc_grep_step()), so the window answers while it runs and Stop ends it.

A program has 16 descriptors, and the window and the standard ones take four: the walk holds one
folder and one file open at a time, never more. Everything it reads is bounded: the folders deep
and in all, the files, the bytes, a line's length, the matches listed and the paths waiting; past
one it stops or leaves the rest out, and says which in why. It reads the files as they are on the
disk, not as the editor has them unsaved, and never /dev (devices) or /.netman (the network
manager's).

The calls to the file system are main.c's (struct wc_fsops), so tests/host/test_wcode.c gives it
one in memory.
*/
#ifndef _WCODE_GREP_H
#define _WCODE_GREP_H

#include "find.h"

#define WC_GREP_DEPTH   24              /* folders inside the one searched */
#define WC_GREP_DIRS    4096            /* folders read */
#define WC_GREP_FILES   20000           /* files read */
#define WC_GREP_BYTES   (256u << 20)    /* bytes read in all */
#define WC_GREP_HITS    2000            /* matching lines listed */
#define WC_GREP_LINE    (64u << 10)     /* a longer line is searched in its first 64 KB */
#define WC_GREP_CHUNK   (64u << 10)     /* bytes a read */
#define WC_GREP_SHOW    120             /* of a matching line, the bytes kept around its match */
#define WC_GREP_PENDING (256u << 10)    /* the paths of the folders waiting, their bytes */
#define WC_GREP_FILTER  64

struct wc_fsops {
    void *ctx;
    int (*opendir)(void *ctx, const char *path);            /* a handle, or -1 */
    /* the next entry: 1 with its name (at most size - 1 bytes), whether it is a folder and its bytes; 0 at the
       end; -1 for an error */
    int (*readdir)(void *ctx, int h, char *name, unsigned size, int *dir, unsigned *bytes);
    void (*closedir)(void *ctx, int h);
    int (*open)(void *ctx, const char *path);               /* a handle, or -1 */
    int (*read)(void *ctx, int h, char *buf, unsigned n);   /* bytes, 0 at the end, -1 for an error */
    void (*close)(void *ctx, int h);
};

struct wc_hit {
    unsigned file;                      /* its path, an offset in paths */
    unsigned line;                      /* from 1 */
    unsigned col;                       /* the match's byte in the line, from 0 */
    char text[WC_GREP_SHOW];            /* the line around the match, tabs and all */
    unsigned short a, b;                /* the match in text */
};

enum { WC_GREP_IDLE, WC_GREP_RUN, WC_GREP_DONE };

struct wc_grep {
    struct wc_find f;
    char filter[WC_GREP_FILTER];        /* the names to look in: "*.c *.h", "" for all */
    char root[WC_PATH];
    const struct wc_fsops *fs;
    int state;
    /* the walk */
    char *pend;                         /* the folders waiting: paths after one another, each with its NUL */
    unsigned pend_used;
    char dir[WC_PATH];                  /* the folder being read, and its handle */
    int dh, depth;
    unsigned dir_mark;                  /* where its subfolders start in pend, pushed in its order */
    char file[WC_PATH];                 /* the file being read, its handle and its line */
    int fh;
    unsigned fline, flen, fskip, fhit;
    char *buf;                          /* a line carried over a read, and the read */
    unsigned carry;
    /* what it found */
    struct wc_hit *hits;
    unsigned nhits;
    char *paths;                        /* the paths of the files with matches */
    unsigned paths_used, paths_cap;
    unsigned dirs, files, skipped, binary, left_out;
    unsigned long bytes;
    char why[128];                      /* what stopped it early, or was left out */
};

void wc_grep_init(struct wc_grep *g);
/* a search of folder with g->f's pattern and g->filter: 0, or -1 with g->why (a pattern that is none, no memory) */
int wc_grep_start(struct wc_grep *g, const struct wc_fsops *fs, const char *folder);
int wc_grep_step(struct wc_grep *g, unsigned long budget);      /* about budget bytes of work: 1 while more is left */
void wc_grep_stop(struct wc_grep *g);                           /* what is open is closed; the list stays */
void wc_grep_free(struct wc_grep *g);
const char *wc_grep_path(const struct wc_grep *g, const struct wc_hit *h);
int wc_grep_name_ok(const char *filter, const char *name);      /* the filter takes this name */

#endif
