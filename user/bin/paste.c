/*
SurfOS user program: paste
--------------------
File: paste.c   Date: 11/01/26
--------------------
paste [-d list] [-s] [file...]: the lines of the files side by side, the first lines together,
then the second, separated by a TAB. - is standard input, and may be named several times to
take a line from it for each. -d takes the separators from list in turn, starting again on
each line: a list entry is one character (a whole UTF-8 sequence), or \n \t \\ or \0, the last
meaning nothing. -s puts all of each file's lines on one line, a file at a time.

Nothing is kept of a line: it goes through a byte at a time, so a line may be any length. A
program has 16 descriptors, so side by side about a dozen files can be open; -s opens one at a
time and takes any number.
*/

#include "../diff/cli.h"

struct sep {
    char bytes[4];
    int len;
};

static struct sep *seps;
static int nseps;

/* the list as separators: 0, or -1 said */
static int parse_list(const char *s) {
    size_t n = strlen(s);
    seps = lu_alloc(NULL, (n ? n : 1) * sizeof(*seps));
    if(!n) {                            /* -d '': nothing between */
        seps[0].len = 0;
        nseps = 1;
        return 0;
    }
    while(*s) {
        struct sep *o = &seps[nseps++];
        if(*s == '\\') {
            char e = s[1];
            if(!e) {
                fprintf(stderr, "paste: the list ends in a backslash\n");
                return -1;
            }
            o->len = e != '0';
            o->bytes[0] = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            s += 2;
        } else {
            o->len = cli_seq((const unsigned char *)s, strlen(s));
            memcpy(o->bytes, s, (size_t)o->len);
            s += o->len;
        }
    }
    return 0;
}

static void put_sep(int k) {
    const struct sep *d = &seps[k % nseps];
    fwrite(d->bytes, 1, (size_t)d->len, stdout);
}

/* one file's lines on one line */
static int serial(const char *name) {
    FILE *f = lu_open(name);
    int c, k = 0, rc = 0;
    if(!f) return 2;
    while((c = getc(f)) != EOF) {
        if(c != '\n') {
            putchar(c);
            continue;
        }
        if((c = getc(f)) == EOF) break;         /* the last newline ends the line, as below */
        put_sep(k);
        k = (k + 1) % nseps;
        ungetc(c, f);
    }
    putchar('\n');
    if(ferror(f)) {
        fprintf(stderr, "paste: %s: %s\n", name, strerror(errno));
        rc = 2;
    }
    lu_close(f);
    return rc;
}

static int parallel(int n, char **names) {
    FILE **files;
    int i, rc = 0, open_now = 0;
    files = lu_alloc(NULL, (size_t)n * sizeof(*files));
    for(i = 0; i < n; i++) {
        if(!(files[i] = lu_open(names[i]))) return 2;
        open_now++;
    }
    while(open_now) {
        int any = 0, waiting = 0;               /* separators not out yet: at the very end there is no line */
        for(i = 0; i < n; i++) {
            int c = EOF;
            if(files[i] && (c = getc(files[i])) == EOF) {
                if(ferror(files[i])) {
                    fprintf(stderr, "paste: %s: %s\n", names[i], strerror(errno));
                    rc = 2;
                }
                lu_close(files[i]);
                /* standard input named twice: it has ended for each */
                if(files[i] == stdin) {
                    int k;
                    for(k = i + 1; k < n; k++) {
                        if(files[k] == stdin) {
                            files[k] = NULL;
                            open_now--;
                        }
                    }
                }
                files[i] = NULL;
                open_now--;
            }
            if(c != EOF) {
                int k;
                if(!any) {
                    for(k = 0; k < waiting; k++) put_sep(k);
                    any = 1;
                }
                while(c != EOF && c != '\n') {
                    putchar(c);
                    c = getc(files[i]);
                }
            }
            if(i < n - 1) {
                if(any) put_sep(i);
                else waiting++;
            }
        }
        if(any) putchar('\n');
    }
    free(files);
    return rc;
}

int main(int argc, char **argv) {
    static char stdin_name[] = "-";
    static char *only_stdin[1] = { stdin_name };
    const char *list = "\\t";
    int i = 1, one_line = 0, rc = 0;
    lu_prog = "paste";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 's') one_line = 1;
            else if(*o == 'd' && (o[1] || i + 1 < argc)) {
                list = o[1] ? o + 1 : argv[++i];
                break;
            } else {
                fprintf(stderr, "usage: paste [-d list] [-s] [file...]\n");
                return 2;
            }
        }
    }
    if(parse_list(list) < 0) return 2;
    if(one_line) {
        int k = i;
        do rc |= serial(k < argc ? argv[k] : "-");
        while(++k < argc);
    } else rc = i < argc ? parallel(argc - i, argv + i) : parallel(1, only_stdin);
    free(seps);
    return cli_flush("paste") ? 2 : rc ? 2 : 0;
}
