/*
SurfOS user programs: the tar format
----------------------
File: tarfile.h Date: 11/01/26
----------------------
Reading an archive entry by entry, and making the headers of one, with no system calls: bytes come
from a function, so tests/host/ runs this over memory and tar over a file, a pipe or gzip alike.

    struct tar_reader t;                        about 13 KB
    tar_start(&t, source, ctx);
    while((r = tar_next(&t)) > 0) {
        ... t.e is the entry ...
        while((n = tar_data(&t, buf, sizeof(buf))) > 0) ...     optional: tar_next() skips what was not read
    }

tar_next() returns 1 for an entry, 0 at the end of the archive (t.no_end when the two zero blocks
were missing) and -1 when it can go no further: t.why says why, NULL meaning the source failed and
errno says how. tar_data() returns bytes, 0 at the end of the entry's data, -1 in the same way.

A pax header that cannot be read is judged for the entry after it alone. If only that entry's name
is in doubt, it comes with t.e.pax_bad and the caller writes nothing of it. If its size is in doubt
nobody knows where the next entry starts, so tar_next() returns -1 with t.lost_entry set and t.e
naming it.
Everything reads forward, so a pipe will do; what is skipped is read and dropped.

ustar, GNU and old (v7) headers are read, GNU long names ('L', 'K'), pax 'path' and 'size' records
('x'; a 'g' header is passed over), base-256 numbers and GNU sparse headers' extension blocks. Nothing
in a header is believed further than its field: a length only says how much to read, the names
stop at their fields' ends, and a name too long for e.name is marked rather than cut silently.
*/

#ifndef _TARFILE_H
#define _TARFILE_H

#include "../gzip/gzfile.h"

#define TAR_BLOCK 512
#define TAR_NAME_MAX 4096               /* longer than any path SurfOS has: a longer name is marked name_bad */
#define TAR_RECORD 10240                /* an archive is padded with zeros to a multiple of this */
#define TAR_RECORD_KEEP (TAR_NAME_MAX + 32)     /* of a pax record, what is looked at */

/* the type byte of a header, as it comes; what is not one of these is treated as a file of an unknown kind */
#define TAR_FILE     '0'                /* also '\0' and '7' (contiguous): tar_next() gives '0' for all */
#define TAR_HARDLINK '1'
#define TAR_SYMLINK  '2'
#define TAR_CHARDEV  '3'
#define TAR_BLOCKDEV '4'
#define TAR_DIR      '5'                /* also a '0' whose name ends in '/', as old archives have it */
#define TAR_FIFO     '6'
#define TAR_SPARSE   'S'                /* GNU's: its data is not the file's */

struct tar_entry {
    char name[TAR_NAME_MAX];            /* prefix/name, or the long form, terminated */
    int name_bad;                       /* it was too long for name, or held a zero byte: no file may be made from it */
    int type;
    int sparse;                         /* GNU.sparse.* in its pax header: the data is not the file's */
    int pax_bad;                        /* a pax header before it could not be read: its name may not be its name */
    unsigned long long size;            /* of its data in the archive */
    unsigned int mode;
    unsigned int mtime;
};

struct tar_reader {
    struct tar_entry e;
    const char *why;
    int no_end;                         /* the input ended without the two zero blocks */
    int pax_stray;                      /* a pax header that could not be read, with no entry after it */
    int lost_entry;                     /* -1 from tar_next() is about t.e: its size cannot be read */

    /* the rest is tarfile.c's */
    gz_source source;
    void *ctx;
    int failed;
    int headers;                        /* blocks seen that were headers */
    unsigned long long left;            /* the entry's data to go */
    unsigned int pad;                   /* zeros after it, to the block's end */
    char long_name[TAR_NAME_MAX];       /* what 'L' or an 'x' path said of the next entry */
    int have_long, long_bad, have_size, next_sparse;
    int next_bad, size_lost, next_pax;  /* of the pax header before the next entry: see read_pax() */
    unsigned long long pax_size;
    char scratch[TAR_RECORD_KEEP];      /* a pax record, a long link name */
    unsigned char block[TAR_BLOCK];
};

void tar_start(struct tar_reader *t, gz_source source, void *ctx);
int tar_next(struct tar_reader *t);
int tar_data(struct tar_reader *t, void *buf, unsigned int n);

/* A ustar header for a member into block (TAR_BLOCK bytes): 0, or -1 when the name will not go into ustar's two
   fields. A directory's name is given with its '/'. */
int tar_make_header(unsigned char *block, const char *name, int type, unsigned int size, unsigned int mtime);

#endif
