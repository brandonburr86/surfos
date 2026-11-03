/*
SurfOS user program: cmp
--------------------
File: cmp.c     Date: 11/01/26
--------------------
cmp [-l] [-s] [-n count] file1 file2: do two files hold the same bytes? - is standard input.
Without options the first difference is said, with its byte and line (both from 1), on
standard output. -l lists every byte that differs: its number in decimal and the two values
in octal. -s says nothing. -n compares the first count bytes at most. When one file ends
first that goes to the standard error, unless -s. Status 0 the same, 1 different, 2 trouble.

Both files are read a block at a time, so their size does not matter; counts are 64 bits, as
two files of nearly 4 GB may go a long way.
*/

#include "../diff/cli.h"
#include <fcntl.h>

#define BLOCK 32768

static unsigned char block1[BLOCK], block2[BLOCK];

static int usage(void) {
    fprintf(stderr, "usage: cmp [-l] [-s] [-n count] file1 file2\n");
    return 2;
}

static int open_file(const char *name) {
    int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
    if(fd < 0) fprintf(stderr, "cmp: %s: %s\n", name, strerror(errno));
    return fd;
}

/* n bytes unless the file ends first: a pipe gives them in pieces, and the two reads must stay in step */
static long read_block(int fd, unsigned char *p, size_t n) {
    size_t got = 0;
    while(got < n) {
        long r = read(fd, p + got, n - got);
        if(r < 0) return -1;
        if(!r) break;
        got += (size_t)r;
    }
    return (long)got;
}

static unsigned long long lines_in(const unsigned char *p, size_t n) {
    unsigned long long count = 0;
    const unsigned char *end = p + n, *q;
    while(p < end && (q = memchr(p, '\n', (size_t)(end - p)))) {
        count++;
        p = q + 1;
    }
    return count;
}

int main(int argc, char **argv) {
    unsigned long long limit = 0, at = 0, line = 1;
    int list = 0, silent = 0, limited = 0, differ = 0, i = 1, fd1, fd2;
    lu_prog = "cmp";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'l') list = 1;
            else if(*o == 's') silent = 1;
            else if(*o == 'n') {
                const char *arg = o[1] ? o + 1 : argv[++i];
                if(i >= argc) return usage();
                if(cli_number(arg, &limit) < 0) {
                    fprintf(stderr, "cmp: %s: not a count of bytes\n", arg);
                    return 2;
                }
                limited = 1;
                break;
            } else return usage();
        }
    }
    if(argc - i != 2 || (list && silent)) return usage();
    if(!strcmp(argv[i], "-") && !strcmp(argv[i + 1], "-")) return 0;    /* the one input against itself */
    if((fd1 = open_file(argv[i])) < 0 || (fd2 = open_file(argv[i + 1])) < 0) return 2;
    for(;;) {
        size_t want = !limited || limit > BLOCK ? BLOCK : (size_t)limit, same, k;
        long n1, n2;
        if(!want) break;
        n1 = read_block(fd1, block1, want);
        if(n1 < 0) {
            fprintf(stderr, "cmp: %s: %s\n", argv[i], strerror(errno));
            return 2;
        }
        n2 = read_block(fd2, block2, want);
        if(n2 < 0) {
            fprintf(stderr, "cmp: %s: %s\n", argv[i + 1], strerror(errno));
            return 2;
        }
        same = (size_t)(n1 < n2 ? n1 : n2);
        if(memcmp(block1, block2, same)) {
            for(k = 0; k < same; k++) {
                if(block1[k] == block2[k]) continue;
                differ = 1;
                if(list) printf("%6llu %3o %3o\n", at + k + 1, block1[k], block2[k]);
                else {
                    if(!silent) printf("%s %s differ: byte %llu, line %llu\n", argv[i], argv[i + 1], at + k + 1,
                                       line + lines_in(block1, k));
                    return cli_flush("cmp") ? 2 : 1;
                }
            }
        }
        at += same;
        line += lines_in(block1, same);
        if(n1 != n2) {
            const char *shorter = n1 < n2 ? argv[i] : argv[i + 1];
            if(cli_flush("cmp")) return 2;
            if(silent) return 1;
            if(at) fprintf(stderr, "cmp: EOF on %s after byte %llu\n", shorter, at);
            else fprintf(stderr, "cmp: EOF on %s which is empty\n", shorter);
            return 1;
        }
        if((size_t)n1 < want) break;
        if(limited) limit -= same;
    }
    return cli_flush("cmp") ? 2 : differ;
}
