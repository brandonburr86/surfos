/*
SurfOS user program: join
--------------------
File: join.c    Date: 11/01/26
--------------------
join [-t char] [-1 field] [-2 field] [-a 1|2]... [-v 1|2]... [-e text] [-o list] file1 file2: the
lines of two files, each sorted on its join field, joined where that field is the same. The
field is the first without -1 and -2. Fields are separated by blanks (any number, and those
at a line's start ignored), or by each single char with -t, one character, a UTF-8 sequence
whole. A joined line is the field, then the rest of file1's line, then the rest of file2's,
separated by a space or the -t char.

-a 1 also gives the lines of file1 that joined nothing, -a 2 those of file2. -v gives only
those. -o chooses what comes out: a list of 0 (the join field) and FILE.FIELD (1.3 is file1's
third), separated by commas or blanks. -e gives the text for a field that is missing or empty.
- is standard input, for one of the files. Lines compare by their bytes.

In memory: all the lines of one join value at once, of both files, since each of one file
meets each of the other. A value shared by a million lines needs them all.
*/

#include "../diff/cli.h"

struct field {
    size_t at, len;
};

struct line {
    char *text;
    size_t len;
    struct field *fields;
    size_t nfields;
};

struct group {
    struct line *lines;
    size_t n, cap;
};

struct side {
    FILE *f;
    const char *name;
    size_t key;                         /* the join field, from 0 */
    char *buf;
    size_t cap;
    struct line cur;                    /* the next line; cur.text NULL: none */
    struct group group;
    int unpaired;                       /* -a or -v asked for its lines that join nothing */
};

struct out_field {
    int file;                           /* 0: the join field; 1, 2: that file's */
    size_t field;                       /* from 0 */
};

static const char *delim;               /* -t */
static size_t delim_len;
static const char *filler;              /* -e */
static struct out_field *outs;
static size_t nouts;
static int only_unpaired;               /* -v */

static void split(struct line *l) {
    size_t cap = 8, i = 0, n = 0;
    l->fields = lu_alloc(NULL, cap * sizeof(*l->fields));
    if(delim) {
        const char *p = l->text, *end = p + l->len;
        while(l->len) {
            const char *q = p;
            while(q < end && ((size_t)(end - q) < delim_len || memcmp(q, delim, delim_len))) q++;
            if(n == cap) l->fields = lu_alloc(l->fields, (cap *= 2) * sizeof(*l->fields));
            l->fields[n].at = (size_t)(p - l->text);
            l->fields[n++].len = (size_t)(q - p);
            if(q >= end) break;
            p = q + delim_len;
        }
    } else {
        while(i < l->len) {
            size_t start;
            while(i < l->len && (l->text[i] == ' ' || l->text[i] == '\t')) i++;
            if(i == l->len) break;
            start = i;
            while(i < l->len && l->text[i] != ' ' && l->text[i] != '\t') i++;
            if(n == cap) l->fields = lu_alloc(l->fields, (cap *= 2) * sizeof(*l->fields));
            l->fields[n].at = start;
            l->fields[n++].len = i - start;
        }
    }
    l->nfields = n;
}

static void advance(struct side *s) {
    int nl;
    long n = lu_getline(s->f, &s->buf, &s->cap, &nl);
    if(n < 0) {
        s->cur.text = NULL;
        return;
    }
    s->cur.text = lu_alloc(NULL, (size_t)n + 1);
    memcpy(s->cur.text, s->buf, (size_t)n + 1);
    s->cur.len = (size_t)n;
    split(&s->cur);
}

static void put_field(const struct line *l, size_t k) {
    if(l && k < l->nfields && l->fields[k].len) fwrite(l->text + l->fields[k].at, 1, l->fields[k].len, stdout);
    else if(filler) fputs(filler, stdout);
}

static void put_delim(void) {
    if(delim) fwrite(delim, 1, delim_len, stdout);
    else putchar(' ');
}

/* the fields of one line, without the join field */
static void put_rest(const struct line *l, size_t key) {
    size_t k;
    for(k = 0; k < l->nfields; k++) {
        if(k == key) continue;
        put_delim();
        put_field(l, k);
    }
}

/* a line out: a and b the lines joined, or one of them for a line that joined nothing */
static void put_line(const struct line *a, const struct line *b, const struct side *s1, const struct side *s2) {
    if(nouts) {
        size_t i;
        for(i = 0; i < nouts; i++) {
            if(i) put_delim();
            if(outs[i].file == 1) put_field(a, outs[i].field);
            else if(outs[i].file == 2) put_field(b, outs[i].field);
            else if(a) put_field(a, s1->key);
            else put_field(b, s2->key);
        }
    } else {
        if(a) put_field(a, s1->key);
        else put_field(b, s2->key);
        if(a) put_rest(a, s1->key);
        if(b) put_rest(b, s2->key);
    }
    putchar('\n');
}

static void drop(struct line *l) {
    free(l->text);
    free(l->fields);
    l->text = NULL;
    l->fields = NULL;
}

static int key_cmp(const struct line *a, size_t ka, const struct line *b, size_t kb) {
    const char *pa = "", *pb = "";
    size_t la = 0, lb = 0;
    int c;
    if(ka < a->nfields) {
        pa = a->text + a->fields[ka].at;
        la = a->fields[ka].len;
    }
    if(kb < b->nfields) {
        pb = b->text + b->fields[kb].at;
        lb = b->fields[kb].len;
    }
    c = memcmp(pa, pb, la < lb ? la : lb);
    return c ? c : la < lb ? -1 : la > lb;
}

/* all the lines with cur's join value into the group; cur becomes the line after them */
static void gather(struct side *s) {
    struct group *g = &s->group;
    g->n = 0;
    for(;;) {
        if(g->n == g->cap) g->lines = lu_alloc(g->lines, (g->cap = g->cap ? g->cap * 2 : 8) * sizeof(*g->lines));
        g->lines[g->n++] = s->cur;
        advance(s);
        if(!s->cur.text || key_cmp(&g->lines[0], s->key, &s->cur, s->key)) return;
    }
}

static void pass_unpaired(struct side *s, const struct side *s1, const struct side *s2) {
    if(s->unpaired) {
        if(s == s1) put_line(&s->cur, NULL, s1, s2);
        else put_line(NULL, &s->cur, s1, s2);
    }
    drop(&s->cur);
    advance(s);
}

static void join(struct side *s1, struct side *s2) {
    advance(s1);
    advance(s2);
    while(s1->cur.text && s2->cur.text) {
        int c = key_cmp(&s1->cur, s1->key, &s2->cur, s2->key);
        size_t i, k;
        if(c < 0) pass_unpaired(s1, s1, s2);
        else if(c > 0) pass_unpaired(s2, s1, s2);
        else {
            gather(s1);
            gather(s2);
            for(i = 0; i < s1->group.n; i++) {
                for(k = 0; k < s2->group.n; k++)
                    if(!only_unpaired) put_line(&s1->group.lines[i], &s2->group.lines[k], s1, s2);
                drop(&s1->group.lines[i]);
            }
            for(k = 0; k < s2->group.n; k++) drop(&s2->group.lines[k]);
        }
    }
    while(s1->cur.text) pass_unpaired(s1, s1, s2);
    while(s2->cur.text) pass_unpaired(s2, s1, s2);
}

static int usage(void) {
    fprintf(stderr, "usage: join [-t char] [-1 field] [-2 field] [-a 1|2] [-v 1|2] [-e text] [-o list] file1 file2\n");
    return 2;
}

/* the list of -o added to outs (a second -o goes on from the first): 0, or -1 */
static int parse_outs(const char *s) {
    size_t cap = nouts + strlen(s) + 1;
    outs = lu_alloc(outs, cap * sizeof(*outs));
    while(*s) {
        char *end;
        unsigned long v;
        if(*s == ',' || *s == ' ' || *s == '\t') {
            s++;
            continue;
        }
        if(*s == '0' && (s[1] == 0 || s[1] == ',' || s[1] == ' ' || s[1] == '\t')) {
            outs[nouts].file = 0;
            outs[nouts++].field = 0;
            s++;
            continue;
        }
        if((*s != '1' && *s != '2') || s[1] != '.') return -1;
        outs[nouts].file = *s - '0';
        s += 2;
        if(*s < '1' || *s > '9') return -1;
        v = strtoul(s, &end, 10);
        if(*end && *end != ',' && *end != ' ' && *end != '\t') return -1;
        outs[nouts++].field = v - 1;
        s = end;
    }
    return nouts ? 0 : -1;
}

/* the number of a file, 1 or 2 */
static int file_number(const char *s) {
    return (s[0] == '1' || s[0] == '2') && !s[1] ? s[0] - '0' : 0;
}

int main(int argc, char **argv) {
    static struct side s1, s2;
    int i = 1, rc = 0;
    lu_prog = "join";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char o = argv[i][1];
        const char *arg = argv[i][2] ? argv[i] + 2 : NULL;
        int v;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if(!strchr("t12aveo", o)) return usage();
        if(!arg) {
            if(++i >= argc) return usage();
            arg = argv[i];
        }
        switch(o) {
        case 't':
            delim = arg;
            delim_len = strlen(arg);
            if(!delim_len || (size_t)cli_seq((const unsigned char *)arg, delim_len) != delim_len) {
                fprintf(stderr, "join: %s: the separator is one character\n", arg);
                return 2;
            }
            break;
        case '1':
        case '2':
            if(cli_int(arg, &v) < 0 || !v) {
                fprintf(stderr, "join: %s: not a field number\n", arg);
                return 2;
            }
            (o == '1' ? &s1 : &s2)->key = (size_t)v - 1;
            break;
        case 'v':
            only_unpaired = 1;
            /* fall through */
        case 'a':
            v = file_number(arg);
            if(!v) return usage();
            (v == 1 ? &s1 : &s2)->unpaired = 1;
            break;
        case 'e':
            filler = arg;
            break;
        case 'o':
            if(parse_outs(arg) < 0) {
                fprintf(stderr, "join: %s: not a list of 0 and FILE.FIELD\n", arg);
                return 2;
            }
            break;
        }
    }
    if(argc - i != 2) return usage();
    if(!strcmp(argv[i], "-") && !strcmp(argv[i + 1], "-")) {
        fprintf(stderr, "join: only one of the files may be standard input\n");
        return 2;
    }
    s1.name = argv[i];
    s2.name = argv[i + 1];
    if(!(s1.f = lu_open(s1.name)) || !(s2.f = lu_open(s2.name))) return 2;
    join(&s1, &s2);
    if(ferror(s1.f) || ferror(s2.f)) {
        fprintf(stderr, "join: %s: %s\n", ferror(s1.f) ? s1.name : s2.name, strerror(errno));
        rc = 2;
    }
    return cli_flush("join") ? 2 : rc;
}
