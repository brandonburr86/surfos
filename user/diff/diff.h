/*
SurfOS user programs: comparing two files
----------------------
File: diff.h    Date: 11/01/26
----------------------
dfile.c finds what changed and dout.c prints it; user/bin/diff.c has the options, the directories and
the file names. The steps, none of which holds a file in memory:

  - the bytes the two files start and end with in common are passed over, a block at a time,
    which is all a pair of huge files that differ in a few lines needs;
  - each line between gets a 32-bit hash, reading once through (4 bytes a line, and one of flags);
  - lines whose hash the other file lacks are changed for certain and drop out, and dcore.c
    compares what is left;
  - both files are read once more side by side, and each pair of lines that dcore.c called equal
    is compared byte by byte. A pair two hashes fooled becomes a change, so what comes out is always
    a correct edit, and the hash can stay small. (Unix's first diff stood on the same check.)

Output reads the files forward once more. Host-tested by tests/host/test_diff.c.
*/

#ifndef _DIFF_H
#define _DIFF_H

#include "dinput.h"

#define DIFF_SAME 0
#define DIFF_DIFFER 1                   /* say so; with brief only that may be known */
#define DIFF_BINARY 2                   /* they differ, and one of them is not text */
#define DIFF_FAILED 3                   /* errno says why, diff_failed_side() where */

struct dopts {
    int style;                          /* 'n' normal, 'u' unified, 'c' context */
    unsigned context;                   /* lines around a change, for u and c */
    int brief;                          /* -q: only whether they differ */
    int icase;                          /* -i */
    int space;                          /* -b: 'b'; -w: 'w' */
    int blanks;                         /* -B */
};

#define D_CHANGED 1
#define D_BLANK 2

/* one file: the part of it not passed over, and its lines */
struct dside {
    struct dsrc *src;
    unsigned start, end;                /* those bytes */
    unsigned first;                     /* the lines before them */
    unsigned n, cap;                    /* the lines in them */
    unsigned *hash;
    unsigned char *flag;
};

struct dscript {
    struct dside a, b;
    unsigned tail;                      /* the last lines of both that are only there as context for what is before them */
    int failed_side;                    /* 0 or 1, when a read failed; -1: no memory */
};

/* DIFF_SAME, DIFF_DIFFER, DIFF_BINARY or DIFF_FAILED. s holds the changes until diff_free() */
int diff_compare(struct dsrc *a, struct dsrc *b, const struct dopts *o, struct dscript *s);
void diff_free(struct dscript *s);
/* the changes that are to be seen: with -B some may not be. Compares as diff_print() prints */
int diff_visible(const struct dscript *s, const struct dopts *o);
/* the edit, with the file lines (label1, label2) for u and c. 0, or DIFF_FAILED */
int diff_print(struct dscript *s, const struct dopts *o, const char *label1, const char *label2, FILE *out);

#endif
