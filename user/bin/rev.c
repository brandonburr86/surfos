/*
SurfOS user program: rev
--------------------
File: rev.c     Date: 11/01/26
--------------------
rev [file...]: each line of the files (standard input without them) with its characters in the
other order. A character is a UTF-8 sequence, whole; any other byte goes as one. A last line
without a newline stays without one. Status 0, or 2 when a file could not be read.
*/

#include "../diff/cli.h"

static void flip(char *a, char *b) {
    while(a < b) {
        char t = *a;
        *a++ = *--b;
        *b = t;
    }
}

/* each sequence backwards first, and then the whole line: the sequences come out right */
static void reverse(char *line, size_t n) {
    size_t i = 0;
    while(i < n) {
        int k = cli_seq((const unsigned char *)line + i, n - i);
        if(k > 1) flip(line + i, line + i + k);
        i += (size_t)k;
    }
    flip(line, line + n);
}

int main(int argc, char **argv) {
    char *line = NULL;
    size_t cap = 0;
    int i = 1, rc = 0, nl;
    long n;
    lu_prog = "rev";
    if(i < argc && !strcmp(argv[i], "--")) i++;
    else if(i < argc && argv[i][0] == '-' && argv[i][1]) {
        fprintf(stderr, "usage: rev [file...]\n");
        return 2;
    }
    do {
        const char *name = i < argc ? argv[i] : "-";
        FILE *f = lu_open(name);
        if(!f) {
            rc = 2;
            continue;
        }
        while((n = lu_getline(f, &line, &cap, &nl)) >= 0) {
            reverse(line, (size_t)n);
            fwrite(line, 1, (size_t)n, stdout);
            if(nl) putchar('\n');
        }
        if(ferror(f)) {
            fprintf(stderr, "rev: %s: %s\n", name, strerror(errno));
            rc = 2;
        }
        lu_close(f);
    } while(++i < argc);
    free(line);
    return cli_flush("rev") ? 2 : rc;
}
