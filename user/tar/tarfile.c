/*
SurfOS user programs: the tar format
--------------------
File: tarfile.c Date: 11/01/26
--------------------
What tarfile.h says. A pax header can be megabytes of a Mac's extended attributes around the two
records that matter, so it is read a record at a time with only a record's first bytes kept, and a
GNU long name past TAR_NAME_MAX is read and dropped: neither ever allocates, nor believes a length
further than the input goes.

The 64-bit sizes here are only added to, compared and masked: a 32-bit program has no library
function to divide them with.
*/

#include <string.h>
#include <stdio.h>
#include "tarfile.h"

#define O_NAME     0
#define O_MODE     100
#define O_SIZE     124
#define O_MTIME    136
#define O_CHKSUM   148
#define O_TYPE     156
#define O_MAGIC    257
#define O_PREFIX   345
#define N_NAME     100
#define N_PREFIX   155
#define O_ISEXTENDED 482                /* a GNU sparse header's: its map goes on in the next block */
#define O_EXT_MORE 504                  /* and an extension block's */
#define NUMBER_MAX 0x7fffffffffffffffULL

static const char DAMAGED[] = "the archive is damaged: a header is not a header";
static const char CUT_SHORT[] = "the archive ends in the middle of an entry";

static int bad(struct tar_reader *t, const char *why) {
    t->why = why;
    return -1;
}

/* n bytes of input: n; fewer only at its end; -1 when the source failed */
static int fill(struct tar_reader *t, unsigned char *p, unsigned int n) {
    unsigned int got = 0;
    while(got < n) {
        int r = t->source(t->ctx, p + got, n - got);
        if(r < 0) {
            t->failed = 1;
            return -1;
        }
        if(!r) break;
        got += (unsigned int)r;
    }
    return (int)got;
}

/* n bytes that must be there: 0, or -1 said */
static int must_read(struct tar_reader *t, unsigned char *p, unsigned int n) {
    int r = fill(t, p, n);
    if(r < 0) return bad(t, NULL);
    if((unsigned int)r < n) return bad(t, CUT_SHORT);
    return 0;
}

/* n bytes, dropped: 0, or -1 said */
static int skip(struct tar_reader *t, unsigned long long n) {
    unsigned char junk[TAR_BLOCK];
    while(n) {
        unsigned int k = n < sizeof(junk) ? (unsigned int)n : (unsigned int)sizeof(junk);
        if(must_read(t, junk, k) < 0) return -1;
        n -= k;
    }
    return 0;
}

static unsigned int padding(unsigned long long size) {
    return (TAR_BLOCK - ((unsigned int)size & (TAR_BLOCK - 1))) & (TAR_BLOCK - 1);
}

/* a number in a field of n bytes: octal, with spaces before it and a space or zero after, or GNU's base 256. 0, or -1
   for what is neither, a negative number among them: no length comes of one */
static int number(const unsigned char *p, int n, unsigned long long *out) {
    unsigned long long v = 0;
    int i = 0;
    if(p[0] & 0x80) {
        if(p[0] != 0x80) return -1;
        for(i = 1; i < n; i++) {
            if(v > (NUMBER_MAX >> 8)) return -1;
            v = v << 8 | p[i];
        }
        *out = v;
        return 0;
    }
    while(i < n && p[i] == ' ') i++;
    for(; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v << 3 | (unsigned int)(p[i] - '0');
    if(i < n && p[i] != ' ' && p[i]) return -1;
    *out = v;
    return 0;
}

/* The stored sum against the block's, the field itself counting as spaces. Old tars summed signed characters. */
static int sum_right(const unsigned char *b) {
    unsigned long long stored;
    unsigned int plain = 0;
    int sign = 0, i;
    if(number(b + O_CHKSUM, 8, &stored) < 0) return 0;
    for(i = 0; i < TAR_BLOCK; i++) {
        int c = i >= O_CHKSUM && i < O_CHKSUM + 8 ? ' ' : b[i];
        plain += (unsigned int)c;
        sign += c > 127 ? c - 256 : c;
    }
    return stored == plain || stored == (unsigned long long)(long long)sign;
}

static int all_zero(const unsigned char *b) {
    int i;
    for(i = 0; i < TAR_BLOCK; i++) if(b[i]) return 0;
    return 1;
}

/* a field that fills its bytes has no terminator */
static size_t field_len(const unsigned char *p, size_t n) {
    const unsigned char *z = memchr(p, 0, n);
    return z ? (size_t)(z - p) : n;
}

/* a GNU long name: the entry's data, as long as size says */
static int read_long_name(struct tar_reader *t, unsigned long long size, char *out, int *bad_name) {
    unsigned int keep = size < TAR_NAME_MAX ? (unsigned int)size : TAR_NAME_MAX - 1, n = keep;
    if(must_read(t, (unsigned char *)out, keep) < 0) return -1;
    if(skip(t, size - keep + padding(size)) < 0) return -1;
    while(n && !out[n - 1]) n--;
    out[keep] = 0;
    *bad_name = size >= TAR_NAME_MAX || memchr(out, 0, n) != NULL;
    return 0;
}

/* One pax record is "<length> <key>=<value>\n", the length counting all of it. */
static void pax_record(struct tar_reader *t, const char *rec, size_t n, int whole) {
    const char *eq = memchr(rec, '=', n), *value;
    size_t key, len, i;
    if(!eq) {
        t->pax_bad = 1;
        return;
    }
    key = (size_t)(eq - rec);
    value = eq + 1;
    len = n - key - 1;
    if(key == 4 && !memcmp(rec, "path", 4)) {
        t->have_long = 1;
        t->long_bad = !whole || len >= TAR_NAME_MAX || memchr(value, 0, len) != NULL;
        if(len >= TAR_NAME_MAX) len = TAR_NAME_MAX - 1;
        memcpy(t->long_name, value, len);
        t->long_name[len] = 0;
    } else if(key == 4 && !memcmp(rec, "size", 4)) {
        unsigned long long v = 0;
        if(!len) t->pax_bad = 1;
        for(i = 0; i < len; i++) {
            if(value[i] < '0' || value[i] > '9' || v > (NUMBER_MAX - 9) / 10) {
                t->pax_bad = 1;
                return;
            }
            v = v * 10 + (unsigned int)(value[i] - '0');
        }
        t->pax_size = v;
        t->have_size = 1;
    } else if(key > 11 && !memcmp(rec, "GNU.sparse.", 11)) {
        t->next_sparse = 1;
    }
}

/* the records of a pax header of this size, and the padding after them */
static int read_pax(struct tar_reader *t, unsigned long long size) {
    char *keep = t->scratch;
    unsigned long long left = size;
    while(left) {
        unsigned long long len = 0;
        unsigned int digits = 0, have;
        unsigned char c = 0;
        for(;;) {                                       /* the length, up to its space */
            if(!left) {
                t->pax_bad = 1;
                return skip(t, padding(size));
            }
            if(must_read(t, &c, 1) < 0) return -1;
            left--;
            if(c == ' ') break;
            if(c < '0' || c > '9' || ++digits > 18) {
                t->pax_bad = 1;
                return skip(t, left + padding(size));
            }
            len = len * 10 + (unsigned int)(c - '0');
        }
        if(!digits || len < digits + 2ULL || len - digits - 1 > left) { /* "N k=v\n": at least the newline follows */
            t->pax_bad = 1;
            return skip(t, left + padding(size));
        }
        len -= digits + 1;                              /* key=value\n */
        have = len < TAR_RECORD_KEEP ? (unsigned int)len : TAR_RECORD_KEEP;
        if(must_read(t, (unsigned char *)keep, have) < 0) return -1;
        if(skip(t, len - have) < 0) return -1;
        left -= len;
        if(have == len && keep[have - 1] != '\n') t->pax_bad = 1;
        else pax_record(t, keep, have == len ? have - 1 : have, have == len);
    }
    return skip(t, padding(size));
}

/* a GNU sparse header may be followed by blocks that go on with its map */
static int skip_sparse_map(struct tar_reader *t, const unsigned char *header) {
    int more = header[O_ISEXTENDED];
    while(more) {
        if(must_read(t, t->block, TAR_BLOCK) < 0) return -1;
        more = t->block[O_EXT_MORE];
    }
    return 0;
}

/* the block in t->block, a real entry, as t->e */
static int take_entry(struct tar_reader *t, unsigned long long size, unsigned long long mtime, unsigned long long mode) {
    struct tar_entry *e = &t->e;
    const unsigned char *b = t->block;
    int type = b[O_TYPE];
    size_t n = field_len(b + O_NAME, N_NAME);
    e->name_bad = 0;
    e->sparse = t->next_sparse;
    if(t->have_long) {
        memcpy(e->name, t->long_name, strlen(t->long_name) + 1);
        e->name_bad = t->long_bad;
    } else if(!memcmp(b + O_MAGIC, "ustar\0", 6) && b[O_PREFIX]) {
        size_t p = field_len(b + O_PREFIX, N_PREFIX);
        memcpy(e->name, b + O_PREFIX, p);
        e->name[p] = '/';
        memcpy(e->name + p + 1, b + O_NAME, n);
        e->name[p + 1 + n] = 0;
    } else {
        memcpy(e->name, b + O_NAME, n);
        e->name[n] = 0;
    }
    if(!type || type == '7') type = TAR_FILE;
    /* These have no data, whatever the size field holds. */
    if(type == TAR_DIR || type == TAR_HARDLINK || type == TAR_SYMLINK || type == TAR_CHARDEV || type == TAR_BLOCKDEV ||
       type == TAR_FIFO) size = 0;
    /* A file by its type and a folder by its name, as old archives have it, still has its size read past. */
    if(type == TAR_FILE && e->name[0] && e->name[strlen(e->name) - 1] == '/') type = TAR_DIR;
    e->type = type;
    e->size = size;
    e->mode = (unsigned int)mode & 07777;
    e->mtime = mtime > 0xffffffffULL ? 0xffffffffu : (unsigned int)mtime;
    if(type == TAR_SPARSE && skip_sparse_map(t, b) < 0) return -1;
    t->left = e->size;
    t->pad = padding(e->size);
    t->have_long = t->long_bad = t->have_size = t->next_sparse = 0;
    return 1;
}

void tar_start(struct tar_reader *t, gz_source source, void *ctx) {
    memset(t, 0, sizeof(*t));
    t->source = source;
    t->ctx = ctx;
}

int tar_next(struct tar_reader *t) {
    int zeros = 0;
    if(skip(t, t->left) < 0 || skip(t, t->pad) < 0) return -1;
    t->left = 0;
    t->pad = 0;
    for(;;) {
        unsigned long long size, mtime = 0, mode = 0;
        int r = fill(t, t->block, TAR_BLOCK), type;
        if(r < 0) return bad(t, NULL);
        if(!r) {
            t->no_end = !zeros;
            return 0;
        }
        if(r < TAR_BLOCK) return bad(t, CUT_SHORT);
        if(all_zero(t->block)) {
            if(++zeros == 2) return 0;
            continue;
        }
        zeros = 0;
        if(!sum_right(t->block)) return bad(t, t->headers ? DAMAGED : "not a tar archive (a gzip one needs z)");
        t->headers++;
        type = t->block[O_TYPE];
        if(number(t->block + O_SIZE, 12, &size) < 0) return bad(t, DAMAGED);
        if(type == 'L') {
            if(read_long_name(t, size, t->long_name, &t->long_bad) < 0) return -1;
            t->have_long = 1;
            continue;
        }
        if(type == 'K') {                               /* a link's target: links are never made */
            int dropped;
            if(read_long_name(t, size, t->scratch, &dropped) < 0) return -1;
            continue;
        }
        if(type == 'x') {
            if(read_pax(t, size) < 0) return -1;
            continue;
        }
        if(type == 'g') {
            if(skip(t, size + padding(size)) < 0) return -1;
            continue;
        }
        if(t->have_size) size = t->pax_size;
        if(number(t->block + O_MTIME, 12, &mtime) < 0) mtime = 0;       /* for show: not worth stopping for */
        if(number(t->block + O_MODE, 8, &mode) < 0) mode = 0;
        return take_entry(t, size, mtime, mode);
    }
}

int tar_data(struct tar_reader *t, void *buf, unsigned int n) {
    int r;
    if(!t->left) return 0;
    if(n > t->left) n = (unsigned int)t->left;
    r = fill(t, buf, n);
    if(r < 0) return bad(t, NULL);
    if((unsigned int)r < n) return bad(t, CUT_SHORT);
    t->left -= (unsigned int)r;
    return r;
}

/* Split a long name at a / so that the part after it fits name and the part before it prefix. The name field is used
   as far as it goes: most of a long path. */
static int split_name(const char *name, size_t *prefix) {
    size_t n = strlen(name), i;
    *prefix = 0;
    if(n <= N_NAME) return 0;
    if(n > N_PREFIX + 1 + N_NAME) return -1;
    i = n > N_NAME + 1 ? n - N_NAME - 1 : 0;
    for(; i < n && i <= N_PREFIX; i++) {
        if(name[i] == '/' && i && n - i - 1 <= N_NAME && n - i - 1) {
            *prefix = i;
            return 0;
        }
    }
    return -1;
}

int tar_make_header(unsigned char *block, const char *name, int type, unsigned int size, unsigned int mtime) {
    size_t prefix, i;
    unsigned int sum = 0;
    if(split_name(name, &prefix) < 0) return -1;
    memset(block, 0, TAR_BLOCK);
    if(prefix) {
        memcpy(block + O_PREFIX, name, prefix);
        memcpy(block + O_NAME, name + prefix + 1, strlen(name) - prefix - 1);
    } else {
        memcpy(block + O_NAME, name, strlen(name));
    }
    /* SurfOS has no permissions and no owners: what a Unix expects of a file and a folder, and root */
    snprintf((char *)block + O_MODE, 8, "%07o", type == TAR_DIR ? 0755u : 0644u);
    snprintf((char *)block + 108, 8, "%07o", 0u);       /* uid */
    snprintf((char *)block + 116, 8, "%07o", 0u);       /* gid */
    snprintf((char *)block + O_SIZE, 12, "%011o", size);
    snprintf((char *)block + O_MTIME, 12, "%011o", mtime);
    memset(block + O_CHKSUM, ' ', 8);
    block[O_TYPE] = (unsigned char)type;
    memcpy(block + O_MAGIC, "ustar\0" "00", 8);
    snprintf((char *)block + 329, 8, "%07o", 0u);       /* the device numbers */
    snprintf((char *)block + 337, 8, "%07o", 0u);
    for(i = 0; i < TAR_BLOCK; i++) sum += block[i];
    snprintf((char *)block + O_CHKSUM, 8, "%06o", sum);
    block[O_CHKSUM + 7] = ' ';
    return 0;
}
