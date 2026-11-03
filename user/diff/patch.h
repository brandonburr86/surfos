/*
SurfOS user programs: reading and applying a patch
----------------------
File: patch.h   Date: 11/01/26
----------------------
phunk.c reads a patch, unified, context or normal, into sections (a file's headers) and hunks, all held the
same way: lines marked as in both files, only the old or only the new. papply.c finds where a
hunk goes in a file and writes the result. Both are pure but for reading through dinput.h: output
goes through a callback, and user/bin/patch.c has the options and every change to the disk.

Only one hunk is in memory at a time. The file being patched is not: it is 8 bytes a line, each
line's offset and hash, and a hunk is looked for among the hashes. A place that fits is then
compared byte by byte before it is believed; the unchanged parts of the file are copied through
in blocks. All counts and lengths from a patch are bounded by what is really there: the
numbers in a header are only compared with the lines that come.
*/

#ifndef _PATCH_H
#define _PATCH_H

#include "dinput.h"
#include <limits.h>

#define PATCH_NAME_MAX 256              /* a name in a patch, terminator included: the kernel's longest path */
#define PATCH_FUZZ 2                    /* context lines of each end that may differ */
#define PATCH_NO_NEWLINE "\\ No newline at end of file\n"

struct pline {
    unsigned at, len;                   /* its text in the hunk's bytes, without the newline */
    unsigned hash;                      /* of those bytes and the newline, if it has one */
    char kind;                          /* ' ' in both files, '-' only in the old, '+' only in the new */
    char no_newline;
};

struct phunk {
    unsigned old_start, old_count;      /* the old lines: the first's number from 1, also when there are none */
    unsigned new_start, new_count;
    unsigned number;                    /* in its section, from 1 */
    unsigned long patch_line;           /* where it starts in the patch, for messages */
    struct pline *lines;
    unsigned nlines, lines_cap;
    char *text;
    size_t text_len, text_cap;
    unsigned lead, trail;               /* the lines at each end that are in both files */
};

struct psection {
    int format;                         /* 'u', 'c' or 'n' */
    char old_name[PATCH_NAME_MAX];      /* "": none */
    char new_name[PATCH_NAME_MAX];
    int old_epoch, new_epoch;           /* that side dated the first moment of 1970, UTC: diff -N's file that is not there */
    char index_name[PATCH_NAME_MAX];
    char command[32];                   /* a normal hunk's first line, read to see what it is */
    unsigned hunks;                     /* read so far */
};

struct pinput {
    struct dreader r;
    char *line;
    size_t cap, len;
    int had_newline;
    int again;                          /* the current line is to be read again */
    int started;                        /* a section has been found in it */
    unsigned long stray;                /* a line past a hunk's counts that looked like part of it: its number; the caller
                                           says so and clears this */
    unsigned long lineno;
    char why[120];                      /* what is wrong, when a call says -1 */
};

/* the input: 0, or -1 (no memory) */
int pi_start(struct pinput *p, struct dsrc *src);
void pi_stop(struct pinput *p);
/* the next section: 1, 0 at the end of the input, -1 with p->why */
int pp_next_section(struct pinput *p, struct psection *s);
/* the section's next hunk into h: 1, 0 when it has no more, -1 with p->why */
int pp_next_hunk(struct pinput *p, struct psection *s, struct phunk *h);
void ph_free(struct phunk *h);
void ph_reverse(struct phunk *h);       /* the hunk that undoes it */
void ps_reverse(struct psection *s);

/* the file being patched */
struct ptarget {
    struct dsrc *src;                   /* NULL: a file that does not exist yet */
    struct dreader rd;
    unsigned nlines, cap;
    unsigned *start;                    /* each line's offset; start[nlines] is the end */
    unsigned *hash;
};

typedef int (*pa_put)(void *ctx, const void *p, unsigned n);    /* 0, or -1: the callback knows why */

struct papply {
    struct ptarget *t;
    pa_put put;
    void *ctx;
    unsigned done;                      /* the file's lines dealt with */
    long long offset;                   /* where the last hunk was from where it said: the next is looked for there */
    long long last_offset;              /* the same, for a message */
    long long added;                    /* lines the hunks so far put in, less those they took out */
    long long last_line;                /* where the last hunk starts in the new file, from 1 */
    int owe_newline;                    /* the last line out had none, and more is coming */
    int failed;                         /* a read: its errno */
    int made;                           /* the file is one the patch makes: see pa_locate() */
};

/* the file's line index: 0, or -1 and errno. src NULL: empty */
int pt_load(struct ptarget *t, struct dsrc *src);
void pt_free(struct ptarget *t);

void pa_start(struct papply *a, struct ptarget *t, pa_put put, void *ctx);
/* where h goes: 1 and *at (the file's line, from 0, of the first old line compared) and *fuzz; 0 it fits nowhere; -1 a
   read failed. With reversed given it is looked for the other way round too: at each fuzz, forward where it says, the
   other way round where it says, and then each nearby. A place that fits exactly backwards says more than one that needs
   fuzz forwards. A hunk with no old lines is looked for backwards only where it says, and with made set (the file is one
   the patch makes) only as the whole file. When the patch is found backwards first, *reversed is 1 and the result 0 */
int pa_locate(struct papply *a, struct phunk *h, unsigned *at, unsigned *fuzz, int *reversed);
/* the file up to there and then the hunk go out: 0, or -1 */
int pa_apply(struct papply *a, const struct phunk *h, unsigned at, unsigned fuzz);
/* the rest of the file: 0, or -1 */
int pa_finish(struct papply *a);
/* the hunk in unified form, for a .rej: 0, or -1 */
int pr_write(const struct phunk *h, pa_put put, void *ctx);

#endif
