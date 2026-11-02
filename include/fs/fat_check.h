/*
SurfOS FAT Checker
------------------
File: fat_check.h  Date: 11/01/26 (the command-line suite, track fsck)
------------------
The checker behind the kernel command fsck (shell/fsckcmd.c), which only looks: the repairs
(FAT_CHECK_REPAIR) are reached by nothing in the kernel since 11/02/26, and stay for the host
tests and a later round (fs/fat_check.c). It knows
nothing of block devices, mounts or the shell: its reads and writes, what it says
and its one question go through struct fat_check_io, so that tests/host/test_fsck.c
runs it over images, as fs/fat_mkfs.c is run. A run that only looks is given no write
function at all: what it cannot call it cannot call by mistake.
*/

#ifndef _FS_FAT_CHECK_H
#define _FS_FAT_CHECK_H

#include <surfos/types.h>

struct fat_check_io {
    void *ctx;
    u32 blocks;                                                 /* the device's size, in blocks of 512 bytes */
    int (*read)(void *ctx, u32 lba, u32 count, void *buf);      /* 0, or < 0 */
    int (*write)(void *ctx, u32 lba, u32 count, const void *buf);   /* NULL: the run only looks */
    int (*flush)(void *ctx);                                    /* what was written is on the medium; may be NULL */
    bool (*stop)(void *ctx);                                    /* the call is to end (Ctrl+C, a kill); may be NULL */
    void (*say)(void *ctx, const char *line);                   /* one line, without its end */
    bool (*ask)(void *ctx, const char *plan);                   /* FAT_CHECK_REPAIR: go on? plan says what will change, and
                                                                   the caller shows it where it is seen, whatever is redirected */
};

#define FAT_CHECK_VERBOSE 1             /* every problem and the counts, not only the first 40 */
#define FAT_CHECK_REPAIR  2             /* repair (io->write set), after io->ask(): the host tests only */

/* the statuses of the command (shell.h's CMD_* have the same values) */
#define FAT_CHECK_CLEAN    0            /* no problem found, or all of them repaired */
#define FAT_CHECK_PROBLEMS 1            /* problems are left */
#define FAT_CHECK_TROUBLE  2            /* it could not check: no FAT volume, no memory, a device that fails */

struct fat_check_result {
    int status;
    bool stopped;                       /* io->stop() ended it */
    bool cancelled;                     /* io->ask() said no */
    bool wrote;                         /* something was written */
    u32 found;                          /* problems the first look found */
    u32 left;                           /* problems left at the end */
    u32 cuts, sizes, saved;             /* chains ended, sizes set, lost chains saved */
    u32 synced;                         /* FAT sectors copied */
    u32 writes;                         /* write requests made */
    u32 dirs, files;                    /* the tree the last look walked */
    u32 clusters, free_clusters, bad_clusters;
};

/* check the volume, and repair it when asked to; res->status, also the return value. When in doubt it changes nothing:
   fs/fat_check.c says what bars repair, what is repaired, and what is only reported. Nothing is ever deleted: no entry in
   use in the first FAT is set free (copying the first over the second does free entries in the second, which nothing reads). */
int fat_check(const struct fat_check_io *io, int flags, struct fat_check_result *res);

#endif
