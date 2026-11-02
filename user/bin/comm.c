/*
SurfOS user program: comm
--------------------
File: comm.c    Date: 11/01/26
--------------------
comm [-123] file1 file2: two sorted files side by side: the lines only in file1 in the first
column, those only in file2 in the second, those in both in the third, each column a TAB
further in. -1, -2 and -3 leave out that column. - is standard input, for one of the two.
Lines compare by their bytes, as sort without options sorts them. The order is not checked: of
input not in order comes what a merge of it gives. Only a line of each file is in memory.
*/

#include "../diff/cli.h"

struct side {
    FILE *f;
    const char *name;
    char *line;
    size_t cap;
    long len;                           /* -1: the file has ended */
};

static int shown[3] = { 1, 1, 1 };

static void advance(struct side *s) {
    int nl;
    s->len = lu_getline(s->f, &s->line, &s->cap, &nl);
}

/* a line in column c, after a TAB for each column before it that is shown */
static void put(int c, const struct side *s) {
    int k;
    if(!shown[c]) return;
    for(k = 0; k < c; k++)
        if(shown[k]) putchar('\t');
    fwrite(s->line, 1, (size_t)s->len, stdout);
    putchar('\n');
}

static int compare(const struct side *a, const struct side *b) {
    size_t n = (size_t)(a->len < b->len ? a->len : b->len);
    int c = memcmp(a->line, b->line, n);
    return c ? c : a->len < b->len ? -1 : a->len > b->len;
}

int main(int argc, char **argv) {
    struct side a = { NULL, NULL, NULL, 0, -1 }, b = { NULL, NULL, NULL, 0, -1 };
    int i = 1, rc = 0;
    lu_prog = "comm";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o < '1' || *o > '3') {
                fprintf(stderr, "usage: comm [-123] file1 file2\n");
                return 2;
            }
            shown[*o - '1'] = 0;
        }
    }
    if(argc - i != 2) {
        fprintf(stderr, "usage: comm [-123] file1 file2\n");
        return 2;
    }
    if(!strcmp(argv[i], "-") && !strcmp(argv[i + 1], "-")) {
        fprintf(stderr, "comm: only one of the files may be standard input\n");
        return 2;
    }
    a.name = argv[i];
    b.name = argv[i + 1];
    if(!(a.f = lu_open(a.name)) || !(b.f = lu_open(b.name))) return 2;
    advance(&a);
    advance(&b);
    while(a.len >= 0 || b.len >= 0) {
        int c = a.len < 0 ? 1 : b.len < 0 ? -1 : compare(&a, &b);
        if(c < 0) {
            put(0, &a);
            advance(&a);
        } else if(c > 0) {
            put(1, &b);
            advance(&b);
        } else {
            put(2, &a);
            advance(&a);
            advance(&b);
        }
    }
    if(ferror(a.f) || ferror(b.f)) {
        fprintf(stderr, "comm: %s: %s\n", ferror(a.f) ? a.name : b.name, strerror(errno));
        rc = 2;
    }
    free(a.line);
    free(b.line);
    return cli_flush("comm") ? 2 : rc;
}
