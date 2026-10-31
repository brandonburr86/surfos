/*
SurfOS user program: cut
--------------------
File: cut.c     Date: 10/25/26
--------------------
cut -b LIST | -c LIST | -f LIST [-d DELIM] [-s] [file...]: parts of each line of the files,
or of standard input without one (track A, A9). -b counts bytes, -c characters (a UTF-8
sequence is one), -f fields separated by DELIM (one character, a TAB without -d); the
fields chosen are joined by DELIM again, and a line without DELIM comes out whole, or not
at all with -s. LIST: numbers from 1 and ranges, separated by commas: 3, 2-4, 5- (to the
end), -3 (from the start). The parts come out in the line's order, each once.
*/

#include "lineutil.h"
#include <limits.h>

#define MAX_RANGES 64

static struct { long lo, hi; } ranges[MAX_RANGES];
static int nranges;

static int chosen(long n) {
    int i;
    for(i = 0; i < nranges; i++)
        if(n >= ranges[i].lo && n <= ranges[i].hi) return 1;
    return 0;
}

static int parse_list(const char *s) {
    char *end;
    while(*s) {
        long lo = 1, hi;
        if(nranges == MAX_RANGES) return -1;
        if(*s != '-') {
            lo = strtol(s, &end, 10);
            if(end == s || lo < 1) return -1;
            s = end;
        }
        hi = lo;
        if(*s == '-') {
            s++;
            if(*s >= '0' && *s <= '9') {
                hi = strtol(s, &end, 10);
                if(hi < lo) return -1;
                s = end;
            } else hi = LONG_MAX;
        }
        ranges[nranges].lo = lo;
        ranges[nranges++].hi = hi;
        if(*s == ',') s++;
        else if(*s) return -1;
    }
    return nranges ? 0 : -1;
}

/* the length of the UTF-8 character at s (1 for a byte that does not start one) */
static int char_len(const unsigned char *s) {
    int len = *s >= 0xF0 ? 4 : *s >= 0xE0 ? 3 : *s >= 0xC0 ? 2 : 1, i;
    for(i = 1; i < len; i++)
        if((s[i] & 0xC0) != 0x80) return 1;
    return len;
}

static void cut_line(const char *line, long len, int mode, char delim, int only_delimited) {
    long i, n = 0, start;
    int first = 1;
    if(mode == 'b') {
        for(i = 0; i < len; i++)
            if(chosen(i + 1)) putchar(line[i]);
    } else if(mode == 'c') {
        for(i = 0; i < len; i += char_len((const unsigned char *)line + i))
            if(chosen(++n)) fwrite(line + i, 1, (size_t)char_len((const unsigned char *)line + i), stdout);
    } else {
        if(!memchr(line, delim, (size_t)len)) {
            if(only_delimited) return;
            fwrite(line, 1, (size_t)len, stdout);
            putchar('\n');
            return;
        }
        for(start = i = 0; i <= len; i++) {
            if(i < len && line[i] != delim) continue;
            if(chosen(++n)) {
                if(!first) putchar(delim);
                fwrite(line + start, 1, (size_t)(i - start), stdout);
                first = 0;
            }
            start = i + 1;
        }
    }
    putchar('\n');
}

static int usage(void) {
    fprintf(stderr, "usage: cut -b LIST | -c LIST | -f LIST [-d DELIM] [-s] [file...]\n");
    return 2;
}

int main(int argc, char **argv) {
    int i = 1, mode = 0, only_delimited = 0, rc = 0, nl;
    char delim = '\t', *line = NULL;
    size_t cap = 0;
    long len;
    lu_prog = "cut";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char o = argv[i][1];
        const char *arg = argv[i][2] ? argv[i] + 2 : NULL;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if(o == 's' && !arg) {
            only_delimited = 1;
            continue;
        }
        if(o != 'b' && o != 'c' && o != 'f' && o != 'd') return usage();
        if(!arg) {
            if(++i == argc) return usage();
            arg = argv[i];
        }
        if(o == 'd') {
            if(strlen(arg) != 1) {
                fprintf(stderr, "cut: the delimiter is one character\n");
                return 2;
            }
            delim = arg[0];
        } else {
            if(mode) return usage();
            mode = o;
            if(parse_list(arg) < 0) {
                fprintf(stderr, "cut: %s: not a list (numbers from 1 and ranges: 3, 2-4, 5-, -3)\n", arg);
                return 2;
            }
        }
    }
    if(!mode) return usage();
    do {
        FILE *f = i < argc ? lu_open(argv[i]) : stdin;
        if(!f) {
            rc = 1;
            continue;
        }
        while((len = lu_getline(f, &line, &cap, &nl)) >= 0) cut_line(line, len, mode, delim, only_delimited);
        if(ferror(f)) {
            fprintf(stderr, "cut: %s: %s\n", i < argc ? argv[i] : "standard input", strerror(errno));
            rc = 1;
        }
        lu_close(f);
    } while(++i < argc);
    free(line);
    fflush(stdout);
    return rc;
}
