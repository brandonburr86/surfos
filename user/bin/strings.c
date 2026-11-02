/*
SurfOS user program: strings
--------------------
File: strings.c  Date: 11/01/26
--------------------
strings [-n min] [file...]: the runs of text in the files (standard input without them, - for
it), each on a line: at least min characters in a row, 4 without -n (-8 is -n 8 too). Text is
what can be printed, TAB too. Unlike strings elsewhere a character is a whole UTF-8 sequence
that prints, so accented and other names come out whole; any other byte ends a run. Streaming:
only the start of a run is held, until it is long enough, so a file may be any size.
*/

#include "../diff/cli.h"

static int usage(void) {
    fprintf(stderr, "usage: strings [-n min] [file...]\n");
    return 2;
}

/* a character that may be part of a run: U+0080 to U+009F are controls, and those two bytes C2 80..9F */
static int printable(const unsigned char *s, int n) {
    if(n == 1) return (*s >= 0x20 && *s < 0x7F) || *s == '\t';
    return !(s[0] == 0xC2 && s[1] < 0xA0);
}

int main(int argc, char **argv) {
    static struct cli_in in;
    unsigned char *held;
    int min = 4, i = 1, rc = 0;
    lu_prog = "strings";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *arg = argv[i] + 1;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if(arg[0] == 'n') {
            arg = arg[1] ? arg + 1 : ++i < argc ? argv[i] : NULL;
            if(!arg) return usage();
        } else if(arg[0] < '0' || arg[0] > '9') return usage();
        if(cli_int(arg, &min) < 0 || !min || min > (INT_MAX - 3) / 4) {
            fprintf(stderr, "strings: %s: not a length\n", arg);
            return 2;
        }
    }
    held = lu_alloc(NULL, (size_t)min * 4);     /* min characters of 4 bytes at most */
    do {
        const char *name = i < argc ? argv[i] : "-";
        FILE *f = lu_open(name);
        const unsigned char *s;
        size_t have = 0;
        int count = 0, n;
        if(!f) {
            rc = 2;
            continue;
        }
        cli_in_start(&in, f);
        while((n = cli_in_next(&in, &s)) > 0) {
            if(!printable(s, n)) {
                if(count >= min) putchar('\n');
                count = 0;
                have = 0;
                continue;
            }
            if(count >= min) {
                fwrite(s, 1, (size_t)n, stdout);
                continue;
            }
            memcpy(held + have, s, (size_t)n);
            have += (size_t)n;
            if(++count == min) fwrite(held, 1, have, stdout);
        }
        if(count >= min) putchar('\n');
        if(in.err) {
            fprintf(stderr, "strings: %s: %s\n", name, strerror(in.err));
            rc = 2;
        }
        lu_close(f);
    } while(++i < argc);
    free(held);
    return cli_flush("strings") ? 2 : rc;
}
