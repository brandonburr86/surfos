/*
SurfOS user programs: gzip, gunzip and zcat
--------------------
File: tool.c    Date: 11/01/26
--------------------
gzip [-cdfkt] [-1..-9] [file...]; gunzip [-cfkt] [file...]; zcat [-t] [file...]. Each is a main of
one line around gz_tool(), so that each is a program of its own: there are no links on FAT for one
program to answer to three names, and none has to look at argv[0].

gzip makes file.gz from file, gunzip makes file from file.gz (or file.tar from file.tgz); a name
that differs from those only in case is the same to FAT. -c writes to standard output and keeps
the input, as does -k without changing where the output goes. -d decompresses, -t only checks the
data, -1 to -9 choose speed against size (6 by default). Without files, or for -, standard input
goes to standard output. zcat is gunzip -c.

A file is never left half replaced: outfile.h says how, and an output that exists stays unless -f
says to replace it. The input goes only after the output has been written, synced and closed; a
file that looks like one half of a FAT rename left unfinished (su_twin_beside()) is kept, since
deleting it would free the clusters its twin holds. Being killed part way (Ctrl+C) leaves the
output as far as it got and the input as it was.

gunzip takes every member of a file, checks each one's CRC and length, and refuses what follows
the last one, but for the zeros gzfile.h explains. Status 0, or 2 if anything went wrong with any
file; the others are still done.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "gzfile.h"
#include "tool.h"
#include "outfile.h"

#define BUF_SIZE 32768

struct options {
    const char *prog;
    int to_stdout, decompress, force, keep, test, level;
};

static unsigned char *data;
static struct gz_reader *reader;
static struct gz_writer *writer;

static int fd_source(void *ctx, void *buf, unsigned int n) {
    return read(*(int *)ctx, buf, n);
}

static int fd_sink(void *ctx, const void *buf, unsigned int n) {
    const unsigned char *p = buf;
    while(n) {
        ssize_t w = write(*(int *)ctx, p, n);
        if(w <= 0) {
            if(!w) errno = ENOSPC;
            return -1;
        }
        p += w;
        n -= (unsigned int)w;
    }
    return 0;
}

static void say(const struct options *o, const char *name, const char *why) {
    fprintf(stderr, "%s: %s: %s\n", o->prog, name, why);
}

static void say_errno(const struct options *o, const char *name) {
    say(o, name, strerror(errno));
}

/* in compressed, to out; out -1: only checked. 0, or -1 said */
static int expand(const struct options *o, int in, int out, const char *name, const char *out_name) {
    gzr_start(reader, fd_source, &in);
    for(;;) {
        int n = gzr_read(reader, data, BUF_SIZE);
        if(!n) return 0;
        if(n < 0) {
            if(reader->why) say(o, name, reader->why);
            else say_errno(o, name);
            return -1;
        }
        if(out >= 0 && fd_sink(&out, data, (unsigned int)n) < 0) {
            say_errno(o, out_name);
            return -1;
        }
    }
}

/* in plain, to out, as a member named stored (NULL: none) with the time mtime. 0, or -1 said */
static int compress(const struct options *o, int in, int out, const char *name, const char *out_name, const char *stored,
                    unsigned int mtime) {
    ssize_t n;
    if(gzw_start(writer, fd_sink, &out, o->level, stored, mtime) < 0) goto bad_write;
    while((n = read(in, data, BUF_SIZE)) > 0) if(gzw_write(writer, data, (unsigned int)n) < 0) goto bad_write;
    if(n < 0) {
        say_errno(o, name);
        return -1;
    }
    if(gzw_finish(writer) < 0) goto bad_write;
    return 0;
bad_write:
    say_errno(o, out_name);
    return -1;
}

/* the name of the output, into buf: 0, or -1 said */
static int output_name(const struct options *o, const char *name, char *buf, size_t size) {
    size_t n = strlen(name);
    if(o->decompress) {
        int r;
        if(n > 3 && !strcasecmp(name + n - 3, ".gz")) r = snprintf(buf, size, "%.*s", (int)(n - 3), name);
        else if(n > 4 && !strcasecmp(name + n - 4, ".tgz")) r = snprintf(buf, size, "%.*s.tar", (int)(n - 4), name);
        else {
            say(o, name, "not a name that ends in .gz or .tgz: -c gets the data anyway");
            return -1;
        }
        if(n > 3 && buf[r - 1] == '/') {                /* ".gz" alone, in a folder */
            say(o, name, "that would leave no name");
            return -1;
        }
        return 0;
    }
    if(n > 3 && !strcasecmp(name + n - 3, ".gz") && !o->force) {
        say(o, name, "it already ends in .gz: unchanged (-f goes on)");
        return -1;
    }
    if(snprintf(buf, size, "%s.gz", name) >= (int)size) {
        errno = ENAMETOOLONG;
        say_errno(o, name);
        return -1;
    }
    return 0;
}

/* The output is safe. A twin of the input keeps it: see the header. 0, or -1 said */
static int drop_input(const struct options *o, const char *name) {
    char twin[PATH_MAX];
    if(su_twin_beside(name, twin, sizeof(twin))) {
        fprintf(stderr, "%s: %s: kept: %s may share its data: check the disk first\n", o->prog, name, twin);
        return -1;
    }
    if(unlink(name) < 0) {
        say_errno(o, name);
        return -1;
    }
    return 0;
}

/* standard input to standard output, or only checked */
static int do_stdin(const struct options *o) {
    if(o->decompress || o->test) {
        if(isatty(0) && !o->force) {
            fprintf(stderr, "%s: compressed data is not read from a terminal: -f insists\n", o->prog);
            return -1;
        }
        return expand(o, 0, o->test ? -1 : 1, "standard input", "standard output");
    }
    if(isatty(1) && !o->force) {
        fprintf(stderr, "%s: compressed data is not written to a terminal: -f insists\n", o->prog);
        return -1;
    }
    return compress(o, 0, 1, "standard input", "standard output", NULL, 0);
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int do_file(const struct options *o, const char *name) {
    char target[PATH_MAX];
    struct out_file out;
    struct stat st;
    int in, r;
    if(!strcmp(name, "-")) return do_stdin(o);
    in = open(name, O_RDONLY);
    if(in < 0 || fstat(in, &st) < 0) {
        say_errno(o, name);
        if(in >= 0) close(in);
        return -1;
    }
    if(S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        say_errno(o, name);
        close(in);
        return -1;
    }
    if(o->test) {
        r = expand(o, in, -1, name, NULL);
    } else if(o->to_stdout) {
        if(!o->decompress && isatty(1) && !o->force) {
            fprintf(stderr, "%s: compressed data is not written to a terminal: -f insists\n", o->prog);
            close(in);
            return -1;
        }
        r = o->decompress ? expand(o, in, 1, name, "standard output")
                          : compress(o, in, 1, name, "standard output", base_name(name), st.st_mtime);
    } else {
        if(output_name(o, name, target, sizeof(target)) < 0) {
            close(in);
            return -1;
        }
        r = of_open(&out, o->prog, target, o->force);
        if(r == OF_EXISTS) say(o, target, "it already exists: -f replaces it");
        if(r) {
            close(in);
            return -1;
        }
        r = o->decompress ? expand(o, in, out.fd, name, target)
                          : compress(o, in, out.fd, name, target, base_name(name), st.st_mtime);
        close(in);
        if(r < 0) {
            of_drop(&out, o->prog);
            return -1;
        }
        if(of_commit(&out, o->prog, 1) < 0) return -1;
        return o->keep ? 0 : drop_input(o, name);
    }
    close(in);
    return r;
}

static int usage(const char *prog) {
    fprintf(stderr, "usage: %s [-cdfkt] [-1..-9] [file...]\n", prog);
    return 2;
}

int gz_tool(int argc, char **argv, const char *prog, int decompress, int to_stdout) {
    struct options o;
    int i = 1, bad = 0;
    memset(&o, 0, sizeof(o));
    o.prog = prog;
    o.decompress = decompress;
    o.to_stdout = to_stdout;
    o.level = 6;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i] + 1;
        if(!strcmp(a, "-")) {
            i++;
            break;
        }
        for(; *a; a++) {
            if(*a == 'c') o.to_stdout = 1;
            else if(*a == 'd') o.decompress = 1;
            else if(*a == 'f') o.force = 1;
            else if(*a == 'k') o.keep = 1;
            else if(*a == 't') o.test = 1;
            else if(*a >= '1' && *a <= '9') o.level = *a - '0';
            else return usage(prog);
        }
    }
    data = malloc(BUF_SIZE);
    reader = o.decompress || o.test ? malloc(sizeof(*reader)) : NULL;
    writer = o.decompress || o.test ? NULL : malloc(sizeof(*writer));
    if(!data || (!reader && !writer)) {
        fprintf(stderr, "%s: out of memory\n", prog);
        bad = 1;
    } else if(i == argc) {
        bad = do_stdin(&o) < 0;
    } else {
        for(; i < argc; i++) if(do_file(&o, argv[i]) < 0) bad = 1;
    }
    free(data);
    free(reader);
    free(writer);
    return bad ? 2 : 0;
}
