/*
SurfOS user program: head
--------------------
File: head.c    Date: 10/5/26
--------------------
head [-n N | -N] [file...]: the first N lines (10) of each file or of standard
input; with more than one file each gets a "==> name <==" heading.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int head(FILE *f, long lines) {
    int c;
    while(lines > 0 && (c = getc(f)) != EOF) {
        putchar(c);
        if(c == '\n') lines--;
    }
    return ferror(f) ? -1 : 0;
}

int main(int argc, char **argv) {
    long lines = 10;
    int i = 1, rc = 0, files;
    char *end;
    if(i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) {
        lines = strtol(argv[i + 1], &end, 10);
        if(*end || lines < 0) { fprintf(stderr, "head: %s: not a line count\n", argv[i + 1]); return 2; }
        i += 2;
    } else if(i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        lines = strtol(argv[i] + 1, &end, 10);
        if(*end) { fprintf(stderr, "usage: head [-n N] [file...]\n"); return 2; }
        i++;
    }
    files = argc - i;
    if(!files) return head(stdin, lines) ? 1 : 0;
    for(; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if(!f) { fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if(files > 1) printf("%s==> %s <==\n", i > argc - files ? "\n" : "", argv[i]);
        if(head(f, lines)) { fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
        fclose(f);
    }
    return rc;
}
