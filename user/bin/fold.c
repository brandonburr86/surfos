/*
SurfOS user program: fold
--------------------
File: fold.c    Date: 11/01/26
--------------------
fold [-w width] [-s] [-b] [file...]: long lines of the files (standard input without them)
broken so that none is wider than width columns, 80 without -w. Columns are characters (a
UTF-8 sequence is one, and is never split), with TAB going on to the next multiple of 8,
backspace one back and carriage return to the start. -b counts bytes instead, each byte
one, but a sequence that does not fit still goes whole to the next line. -s breaks after
the last blank that fits, when there is one. The newlines already in the text stay.

Without -s nothing is held back, so a line may be any length; with -s the segment since the last
break is, and only that.
*/

#include "../diff/cli.h"
#include <ctype.h>

static unsigned long width = 80;
static int by_bytes, at_spaces;

static unsigned long column_after(unsigned long col, const unsigned char *s, int n) {
    if(by_bytes) return col + (unsigned long)n;
    if(n == 1 && *s == '\b') return col ? col - 1 : 0;
    if(n == 1 && *s == '\r') return 0;
    if(n == 1 && *s == '\t') return col + 8 - col % 8;
    return col + 1;
}

/* no -s: each character goes out as it comes, after a newline when it would pass the width */
static void plain(struct cli_in *in) {
    unsigned long col = 0;
    const unsigned char *s;
    int n, have = 0;                    /* something is on this line: a TAB wider than the width still goes */
    while((n = cli_in_next(in, &s)) > 0) {
        unsigned long next;
        if(n == 1 && *s == '\n') {
            putchar('\n');
            col = 0;
            have = 0;
            continue;
        }
        next = column_after(col, s, n);
        if(next > width && have) {
            putchar('\n');
            next = column_after(0, s, n);
        }
        fwrite(s, 1, (size_t)n, stdout);
        col = next;
        have = 1;
    }
}

/* -s, as POSIX has it: a segment of a line with a blank among its first width columns breaks after the last such blank;
   one with none is broken at the width, as without -s. A segment cannot go out until its end is known, so it waits in a buffer,
   and two marks go through it as characters come: seen, up to where it has been measured, and after_blank, just past the last
   blank in that. At a break the rest moves to the front, and is measured again: a TAB's width depends on where it starts. */
static unsigned char *segment;
static size_t segment_len, segment_cap, seen, after_blank;
static unsigned long seen_column;

static void add_to_segment(const unsigned char *s, int n) {
    if(segment_len + (size_t)n > segment_cap) {
        segment_cap = segment_cap ? segment_cap * 2 : 256;
        segment = lu_alloc(segment, segment_cap);
    }
    memcpy(segment + segment_len, s, (size_t)n);
    segment_len += (size_t)n;
}

static void write_segment(size_t n) {
    if(n) {                                     /* (with nothing held there may be no buffer yet) */
        fwrite(segment, 1, n, stdout);
        segment_len -= n;
        memmove(segment, segment + n, segment_len);
    }
    seen = 0;
    after_blank = 0;
    seen_column = 0;
}

/* measure what is new, breaking each time a character would pass the width. A first character goes whatever its width */
static void measure_segment(void) {
    while(seen < segment_len) {
        int n = cli_seq(segment + seen, segment_len - seen);
        unsigned long next = column_after(seen_column, segment + seen, n);
        if(next > width && seen) {
            write_segment(after_blank ? after_blank : seen);
            putchar('\n');
            continue;
        }
        seen += (size_t)n;
        seen_column = next;
        if(n == 1 && (segment[seen - 1] == ' ' || segment[seen - 1] == '\t')) after_blank = seen;
    }
}

static void spaced(struct cli_in *in) {
    const unsigned char *s;
    int n;
    while((n = cli_in_next(in, &s)) > 0) {
        if(n == 1 && *s == '\n') {
            write_segment(segment_len);
            putchar('\n');
            continue;
        }
        add_to_segment(s, n);
        measure_segment();
    }
    write_segment(segment_len);
}

static int usage(void) {
    fprintf(stderr, "usage: fold [-w width] [-s] [-b] [file...]\n");
    return 2;
}

int main(int argc, char **argv) {
    static struct cli_in in;
    int i = 1, rc = 0;
    lu_prog = "fold";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 's') at_spaces = 1;
            else if(*o == 'b') by_bytes = 1;
            else if(*o == 'w') {
                const char *arg = o[1] ? o + 1 : argv[++i];
                int w;
                if(i >= argc) return usage();
                if(cli_int(arg, &w) < 0 || !w) {
                    fprintf(stderr, "fold: %s: not a width\n", arg);
                    return 2;
                }
                width = (unsigned long)w;
                break;
            } else return usage();
        }
    }
    do {
        const char *name = i < argc ? argv[i] : "-";
        FILE *f = lu_open(name);
        if(!f) {
            rc = 2;
            continue;
        }
        cli_in_start(&in, f);
        if(at_spaces) spaced(&in);
        else plain(&in);
        if(in.err) {
            fprintf(stderr, "fold: %s: %s\n", name, strerror(in.err));
            rc = 2;
        }
        lu_close(f);
    } while(++i < argc);
    free(segment);
    return cli_flush("fold") ? 2 : rc;
}
