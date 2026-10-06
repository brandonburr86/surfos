/*
SurfOS user program: wc
--------------------
File: wc.c      Date: 10/5/26
--------------------
wc [-l] [-w] [-c] [file...]: lines, words and bytes (standard input without
files), and a total line for more than one file.
*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

static int show_l, show_w, show_c;
static unsigned long tot_l, tot_w, tot_c;

static void print(unsigned long l, unsigned long w, unsigned long c, const char *name) {
    if(show_l) printf("%7lu ", l);
    if(show_w) printf("%7lu ", w);
    if(show_c) printf("%7lu ", c);
    printf("%s\n", name);
}

static int count(int fd, const char *name) {
    char buf[4096];
    unsigned long l = 0, w = 0, c = 0;
    int inword = 0;
    ssize_t n, i;
    while((n = read(fd, buf, sizeof(buf))) > 0) {
        c += (unsigned long)n;
        for(i = 0; i < n; i++) {
            if(buf[i] == '\n') l++;
            if(isspace((unsigned char)buf[i])) inword = 0;
            else if(!inword) { inword = 1; w++; }
        }
    }
    if(n < 0) return -1;
    print(l, w, c, name);
    tot_l += l;
    tot_w += w;
    tot_c += c;
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0, files;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'l') show_l = 1;
            else if(*o == 'w') show_w = 1;
            else if(*o == 'c') show_c = 1;
            else { fprintf(stderr, "usage: wc [-lwc] [file...]\n"); return 2; }
        }
    }
    if(!show_l && !show_w && !show_c) show_l = show_w = show_c = 1;
    files = argc - i;
    if(!files) return count(0, "") ? 1 : 0;
    for(; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if(fd < 0 || count(fd, argv[i]) < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
        if(fd >= 0) close(fd);
    }
    if(files > 1) print(tot_l, tot_w, tot_c, "total");
    return rc;
}
