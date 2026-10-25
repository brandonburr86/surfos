/*
SurfOS shell: surfsh
--------------------
File: test.c    Date: 10/25/26
--------------------
test EXPRESSION and [ EXPRESSION ]: status 0 when it is true, 1 when it is false, 2 when it
is no expression (said). With 0 to 4 arguments POSIX's rules by their count decide (so
"test -n -e" is about the string -e, and "test ! = x" compares two strings); with more the
operators bind as XSI has them: ! before an expression, then -a, then -o; ( ) groups.

    -e f   f exists               -f f  a file           -d f  a directory
    -s f   a file that is not empty                       -x f  a file or a directory
    -r f, -w f   it exists (SurfOS has no permissions)    -c f  a device (/dev)
    -h f, -L f, -p f, -b f, -S f, -g f, -u f, -k f   false: SurfOS has none of those
    -t n   descriptor n is a terminal
    -n s   s is not empty        -z s   s is empty       s alone: not empty
    s1 = s2, s1 == s2, s1 != s2, s1 < s2, s1 > s2   strings (byte order for < >)
    n1 -eq n2, -ne, -lt, -le, -gt, -ge             integers (decimal, a sign allowed)
    f1 -nt f2, f1 -ot f2   newer, older by the time of the last change (one that does not
                           exist is older than one that does); f1 -ef f2  the same file
*/

#include "surfsh.h"

struct tst {
    char **v;
    int n, i;                       /* the arguments, and the next one */
    int bad;                        /* said: status 2 */
};

static int bad(struct tst *t, const char *fmt, const char *a) {
    if(!t->bad) {
        fprintf(stderr, "surfsh: test: ");
        fprintf(stderr, fmt, a);
        fputc('\n', stderr);
        t->bad = 1;
    }
    return 0;
}

static int integer(struct tst *t, const char *s, long *v) {
    char *end;
    const char *p = s;
    while(*p == ' ' || *p == '\t') p++;
    *v = strtol(p, &end, 10);
    while(*end == ' ' || *end == '\t') end++;
    if(!*p || *end || end == p) return bad(t, "%s: not a number", s);
    return 1;
}

static int unary_op(const char *s) {
    return s[0] == '-' && s[1] && !s[2] && strchr("bcdefghknprstuwxzLS", s[1]);
}

static int binary_op(const char *s) {
    static const char *const ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
                                       "-nt", "-ot", "-ef", NULL };
    int i;
    for(i = 0; ops[i]; i++) if(!strcmp(s, ops[i])) return 1;
    return 0;
}

static int unary(struct tst *t, const char *op, const char *a) {
    struct stat st;
    long fd;
    int is = stat(a, &st) == 0;
    switch(op[1]) {
    case 'n': return a[0] != 0;
    case 'z': return a[0] == 0;
    case 'e': case 'r': case 'w': return is;
    case 'f': return is && S_ISREG(st.type);
    case 'd': return is && S_ISDIR(st.type);
    case 's': return is && S_ISREG(st.type) && st.size > 0;
    case 'x': return is && (S_ISREG(st.type) || S_ISDIR(st.type));
    case 'c': return is && !strcmp(st.fs, "devfs") && !S_ISDIR(st.type);
    case 't': return integer(t, a, &fd) && fd >= 0 && fd < 1024 && isatty((int)fd);
    default: return 0;                          /* -b -g -h -k -L -p -S -u: no such files */
    }
}

static int binary(struct tst *t, const char *a, const char *op, const char *b) {
    struct stat sa, sb;
    long x, y;
    if(!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
    if(!strcmp(op, "!=")) return strcmp(a, b) != 0;
    if(!strcmp(op, "<")) return strcmp(a, b) < 0;
    if(!strcmp(op, ">")) return strcmp(a, b) > 0;
    if(!strcmp(op, "-nt") || !strcmp(op, "-ot") || !strcmp(op, "-ef")) {
        int ia = stat(a, &sa) == 0, ib = stat(b, &sb) == 0;
        if(op[1] == 'e') return ia && ib && sa.ino == sb.ino && !strcmp(sa.fs, sb.fs) && !strcmp(sa.dev, sb.dev);
        if(op[1] == 'n') return ia && (!ib || sa.mtime > sb.mtime);
        return ib && (!ia || sa.mtime < sb.mtime);
    }
    if(!integer(t, a, &x) || !integer(t, b, &y)) return 0;
    if(!strcmp(op, "-eq")) return x == y;
    if(!strcmp(op, "-ne")) return x != y;
    if(!strcmp(op, "-lt")) return x < y;
    if(!strcmp(op, "-le")) return x <= y;
    if(!strcmp(op, "-gt")) return x > y;
    return x >= y;
}

/**** more than 4 arguments: an expression with ! -a -o ( ) ****/

static int or_expr(struct tst *t);

static const char *next(struct tst *t) {
    return t->i < t->n ? t->v[t->i] : NULL;
}

/* a binary comparison first (so "!" = "!" compares), then ! and ( ), a unary test, a string */
static int primary(struct tst *t) {
    const char *a = next(t);
    if(!a) return bad(t, "%s", "an expression is missing");
    if(t->i + 2 < t->n && binary_op(t->v[t->i + 1])) {
        t->i += 3;
        return binary(t, a, t->v[t->i - 2], t->v[t->i - 1]);
    }
    if(!strcmp(a, "!")) {
        t->i++;
        return !primary(t);
    }
    if(!strcmp(a, "(")) {
        int r;
        t->i++;
        r = or_expr(t);
        if(!next(t) || strcmp(next(t), ")")) return bad(t, "%s", "a ( is not closed");
        t->i++;
        return r;
    }
    if(unary_op(a) && t->i + 1 < t->n) {
        t->i += 2;
        return unary(t, a, t->v[t->i - 1]);
    }
    t->i++;
    return a[0] != 0;
}

static int and_expr(struct tst *t) {
    int r = primary(t);
    while(next(t) && !strcmp(next(t), "-a")) {
        int r2;
        t->i++;
        r2 = primary(t);
        r = r && r2;
    }
    return r;
}

static int or_expr(struct tst *t) {
    int r = and_expr(t);
    while(next(t) && !strcmp(next(t), "-o")) {
        int r2;
        t->i++;
        r2 = and_expr(t);
        r = r || r2;
    }
    return r;
}

/**** POSIX's rules by the count ****/

static int by_count(struct tst *t, char **v, int n) {
    switch(n) {
    case 0:
        return 0;
    case 1:
        return v[0][0] != 0;
    case 2:
        if(!strcmp(v[0], "!")) return !by_count(t, v + 1, 1);
        if(unary_op(v[0])) return unary(t, v[0], v[1]);
        return bad(t, "%s: a unary operator was to come", v[0]);
    case 3:
        if(binary_op(v[1])) return binary(t, v[0], v[1], v[2]);
        if(!strcmp(v[0], "!")) return !by_count(t, v + 1, 2);
        if(!strcmp(v[0], "(") && !strcmp(v[2], ")")) return by_count(t, v + 1, 1);
        if(!strcmp(v[1], "-a")) return v[0][0] && v[2][0];
        if(!strcmp(v[1], "-o")) return v[0][0] || v[2][0];
        return bad(t, "%s: a binary operator was to come", v[1]);
    case 4:
        if(!strcmp(v[0], "!")) return !by_count(t, v + 1, 3);
        if(!strcmp(v[0], "(") && !strcmp(v[3], ")")) return by_count(t, v + 1, 2);
        break;
    }
    t->v = v;
    t->n = n;
    t->i = 0;
    {
        int r = or_expr(t);
        if(!t->bad && t->i < t->n) bad(t, "%s: not understood here", t->v[t->i]);
        return r;
    }
}

/* the arguments after test (or [, without its ]): 0 true, 1 false, 2 said */
int test_eval(int n, char **v) {
    struct tst t;
    int r;
    memset(&t, 0, sizeof(t));
    r = by_count(&t, v, n);
    return t.bad ? STATUS_USAGE : !r;
}

int b_test(int argc, char **argv) {
    if(!strcmp(argv[0], "[")) {
        if(argc < 2 || strcmp(argv[argc - 1], "]")) {
            fprintf(stderr, "surfsh: [: the ] at its end is missing\n");
            return STATUS_USAGE;
        }
        argc--;
    }
    return test_eval(argc - 1, argv + 1);
}
