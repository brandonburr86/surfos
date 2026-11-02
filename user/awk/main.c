/*
SurfOS user program: awk
--------------------
File: main.c    Date: 11/01/26
--------------------
awk [-F fs] [-v var=value]... [program | -f file...] [argument...]: POSIX's pattern scanning and
processing language. Each argument after the program is a file to read (- for standard input,
which is also read when there are none) or a var=value to assign when it is reached.

-F fs sets FS (-Ft is a tab, as in the one true awk). -v var=value assigns before BEGIN. -f file
reads the program from the file, and more of them add up. Status 0, or what exit gave, and 2 for a
mistake: one in the program, a file that cannot be read, or anything else that stops a run. The
language is in rootfs/man/awk.txt, and the parts of the program in user/awk/awk.h.
*/

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "awk.h"

/* in the order of awk.h's S_ names */
static const char *const special[] = { "NF", "NR", "FNR", "FS", "OFS", "ORS", "RS", "FILENAME", "SUBSEP", "RSTART",
                                       "RLENGTH", "CONVFMT", "OFMT", "ENVIRON", "ARGC", "ARGV" };

static void set_str(int id, const char *s) {
    A.global[id].kind = V_STR;
    A.global[id].u.s = str_new(s, (unsigned)strlen(s));
}

static void set_num(int id, double d) {
    A.global[id].kind = V_NUM;
    A.global[id].u.num = d;
}

static void array_set(struct array *a, const char *key, const char *value) {
    struct str *k = str_new(key, (unsigned)strlen(key));
    struct value *v = array_elem(a, k, 1);
    str_put(k);
    v->kind = V_INPUT;
    v->u.s = str_new(value, (unsigned)strlen(value));
}

static void init_variables(void) {
    unsigned i;
    for(i = 0; i < S_SPECIALS; i++) global_id(special[i], (unsigned)strlen(special[i]));
    set_num(S_NF, 0);
    set_num(S_NR, 0);
    set_num(S_FNR, 0);
    set_str(S_FS, " ");
    set_str(S_OFS, " ");
    set_str(S_ORS, "\n");
    set_str(S_RS, "\n");
    set_str(S_FILENAME, "");
    set_str(S_SUBSEP, "\034");
    set_num(S_RSTART, 0);
    set_num(S_RLENGTH, 0);
    set_str(S_CONVFMT, "%.6g");
    set_str(S_OFMT, "%.6g");
    record_set(str_new("", 0));
}

static void init_environment(void) {
    extern char **environ;
    struct array *a = array_new();
    char **e;
    A.global[S_ENVIRON].kind = V_ARRAY;
    A.global[S_ENVIRON].u.a = a;
    for(e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        struct str *k;
        struct value *v;
        if(!eq) continue;
        k = str_new(*e, (unsigned)(eq - *e));
        v = array_elem(a, k, 1);
        str_put(k);
        val_put(v);
        v->kind = V_INPUT;
        v->u.s = str_new(eq + 1, (unsigned)strlen(eq + 1));
    }
}

static void init_arguments(int argc, char **argv) {
    struct array *a = array_new();
    int i;
    A.global[S_ARGV].kind = V_ARRAY;
    A.global[S_ARGV].u.a = a;
    set_num(S_ARGC, argc);
    for(i = 0; i < argc; i++) {
        char t[16];
        snprintf(t, sizeof(t), "%d", i);
        array_set(a, t, i ? argv[i] : "awk");
    }
}

/* the whole of a program file, after what is already there */
static void read_program(struct buf *b, const char *name) {
    FILE *f = !strcmp(name, "-") ? stdin : fopen(name, "r");
    char t[4096];
    size_t n;
    if(!f) fatal("%s: %s", name, strerror(errno));
    while((n = fread(t, 1, sizeof(t), f)) > 0) buf_add(b, t, (unsigned)n);
    if(ferror(f)) fatal("%s: %s", name, strerror(errno));
    if(f != stdin) fclose(f);
    buf_ch(b, '\n');
}

static int usage(void) {
    fprintf(stderr, "usage: awk [-F fs] [-v var=value] [program | -f file] [argument...]\n");
    return 2;
}

int main(int argc, char **argv) {
    struct buf prog = { NULL, 0, 0 };
    char **files = NULL;
    unsigned *starts;
    const char *fs = NULL;
    int nfiles = 0, c, i;
    init_variables();
    files = xalloc((size_t)(argc + 1) * sizeof(*files));
    /* + stops at the program, which a GNU getopt would otherwise look past; libsurf's takes it and stops anyway */
    while((c = getopt(argc, argv, "+F:v:f:")) != -1) {
        if(c == 'F') {
            fs = optarg;
        } else if(c == 'f') {
            files[nfiles++] = optarg;
        } else if(c == 'v') {
            const char *p = optarg;
            if(!strchr(p, '=') || !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) {
                fprintf(stderr, "awk: -v %s: not a var=value\n", p);
                return 2;
            }
            assign_operand(p);
        } else {
            return usage();
        }
    }
    if(fs) {
        struct buf b = { NULL, 0, 0 };
        struct value v;
        parse_assign_value(!strcmp(fs, "t") ? "\\t" : fs, (unsigned)strlen(!strcmp(fs, "t") ? "\\t" : fs), &b);
        v.kind = V_INPUT;
        v.u.s = str_new(b.p, b.len);
        free(b.p);
        global_set(S_FS, &v);
        val_put(&v);
    }
    starts = xalloc(((size_t)nfiles + 1) * sizeof(*starts));
    if(nfiles) {
        A.sources = xalloc((size_t)nfiles * sizeof(*A.sources));
        for(i = 0; i < nfiles; i++) {
            starts[i] = prog.len;
            A.sources[i] = files[i];
            read_program(&prog, files[i]);
        }
        A.nsources = (unsigned)nfiles;
    } else {
        if(optind >= argc) return usage();
        buf_add(&prog, argv[optind], (unsigned)strlen(argv[optind]));
        optind++;
        starts[0] = 0;
        A.nsources = 1;
    }
    buf_add(&prog, "", 0);
    parse_program(prog.p, prog.len, starts, A.nsources);
    free(prog.p);
    free(starts);
    free(files);
    init_environment();
    init_arguments(argc - optind + 1, argv + optind - 1);
    run_program();
    streams_end();
    if(fflush(stdout) == EOF || ferror(stdout)) fatal("write error: %s", strerror(errno));
    return A.exit_status;
}
