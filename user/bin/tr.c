/*
SurfOS user program: tr
--------------------
File: tr.c      Date: 10/25/26
--------------------
tr [-d] [-s] set1 [set2]: standard input to standard output with the characters of set1 made
those of set2, place by place (set2's last one for the rest of set1); -d deletes the
characters of set1 instead, and -s squeezes each run of one character of the last set given
into one. A set is characters as they are, a-z for a range, and \n \t \r \\ \- for those.
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* a set as its characters in order, ranges spelled out: how many (at most 256) */
static int expand(const char *s, unsigned char *out) {
    int n = 0;
    while(*s && n < 256) {
        unsigned char c = (unsigned char)*s++;
        if(c == '\\' && *s) {
            char e = *s++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : (unsigned char)e;
        }
        if(*s == '-' && s[1]) {                 /* a range */
            unsigned char hi = (unsigned char)s[1];
            int k;
            s += 2;
            for(k = c; k <= hi && n < 256; k++) out[n++] = (unsigned char)k;
            continue;
        }
        out[n++] = c;
    }
    return n;
}

int main(int argc, char **argv) {
    unsigned char set1[256], set2[256], map[256], in1[256], squeeze[256], buf[4096], out[4096];
    int del = 0, sq = 0, i = 1, n1, n2 = 0, k, last = -1;
    ssize_t n;
    for(; i < argc && argv[i][0] == '-' && argv[i][1] && argv[i][2] == 0; i++) {
        if(argv[i][1] == 'd') del = 1;
        else if(argv[i][1] == 's') sq = 1;
        else break;
    }
    if(i >= argc || (!del && !sq && i + 2 != argc) || i + 2 < argc) {
        fprintf(stderr, "usage: tr [-d] [-s] set1 [set2]\n");
        return 2;
    }
    n1 = expand(argv[i], set1);
    if(i + 1 < argc) n2 = expand(argv[i + 1], set2);
    memset(in1, 0, sizeof(in1));
    memset(squeeze, 0, sizeof(squeeze));
    for(k = 0; k < 256; k++) map[k] = (unsigned char)k;
    for(k = 0; k < n1; k++) in1[set1[k]] = 1;
    if(!del && n2) for(k = 0; k < n1; k++) map[set1[k]] = set2[k < n2 ? k : n2 - 1];
    if(sq) {
        const unsigned char *s = n2 ? set2 : set1;
        int ns = n2 ? n2 : n1;
        for(k = 0; k < ns; k++) squeeze[s[k]] = 1;
    }
    while((n = read(0, buf, sizeof(buf))) > 0) {
        int m = 0;
        ssize_t j;
        for(j = 0; j < n; j++) {
            int c = buf[j];
            if(del && in1[c]) continue;
            c = map[c];
            if(sq && squeeze[c] && c == last) continue;
            last = c;
            out[m++] = (unsigned char)c;
        }
        if(m && write(1, out, (size_t)m) != m) return 1;
    }
    return 0;
}
