/* Host tests for awk (user/awk/): every file as it is, main.c too, on the host's C library, with libsurf's regular
   expressions and tests/host/awk_host.h in front. Each run is a fork, so that a status, a message and a crash are seen as
   they are, and a run that goes wrong costs that run. A table reviewed against the Mac's awk; what SurfOS asks of a tool
   (characters are UTF-8, a NUL or a bad byte passes, the statuses, the messages); each form of getline, redirection and
   close, and writes that fail; a corpus against the system's awk where there is one (the one true awk on the Mac, mawk in
   the Docker image); limits on memory, which also find a leak; a program and input cut, damaged and oversized.

   Built and run by `make unittest`. `test_awk long` runs far more damaged input. The sanitizers are never part of
   `make unittest` (a -fsanitize build at -m32 hangs under the Docker image's emulation); run them on the Mac by hand,
   with tests/host/awk/host64.c, as libsurf's string.c is not 64-bit safe:

       cc="clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined"
       for f in value field io fmt func parse vm main; do
           $cc -Itests/host/awk -Iuser/awk -include tests/host/awk_host.h -c user/awk/$f.c -o /tmp/awk_$f.o; done
       $cc -ffreestanding -fno-builtin -nostdinc -Iuser/include -include tests/host/libsurf_names.h \
           -include tests/host/text_names.h -c user/libsurf/regex.c -o /tmp/awk_regex.o
       $cc -c tests/host/awk/host64.c -o /tmp/awk_host64.o
       $cc tests/host/test_awk.c /tmp/awk_*.o -lm -o /tmp/test_awk && ASAN_OPTIONS=abort_on_error=1 /tmp/test_awk long
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include "awk/regex.h"

int awk_main(int argc, char **argv);

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/**** what awk's files call instead ****/

struct hdr { size_t size; size_t pad; };

static size_t live_bytes, mem_limit;

void *t_malloc(size_t n) {
    struct hdr *h;
    if(mem_limit && live_bytes + n > mem_limit) return NULL;
    if(!(h = malloc(sizeof(*h) + n))) return NULL;
    h->size = n;
    live_bytes += n;
    return h + 1;
}

void *t_realloc(void *p, size_t n) {
    struct hdr *h, *q;
    if(!p) return t_malloc(n);
    h = (struct hdr *)p - 1;
    if(mem_limit && live_bytes - h->size + n > mem_limit) return NULL;
    live_bytes -= h->size;
    if(!(q = realloc(h, sizeof(*h) + n))) return NULL;
    q->size = n;
    live_bytes += n;
    return q + 1;
}

void t_free(void *p) {
    if(p) {
        struct hdr *h = (struct hdr *)p - 1;
        live_bytes -= h->size;
        free(h);
    }
}

/* libsurf's regex.c, built for the host, calls these */
void *surf_malloc(size_t n) { return t_malloc(n); }
void *surf_realloc(void *p, size_t n) { return t_realloc(p, n); }
void surf_free(void *p) { t_free(p); }

void *surf_calloc(size_t n, size_t k) {
    void *p = t_malloc(n * k);
    if(p) memset(p, 0, n * k);
    return p;
}

static int exit_status(int st) {
    if(st == -1) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int t_system(const char *cmd) {
    fflush(NULL);
    return exit_status(system(cmd));
}

FILE *t_popen(const char *cmd, const char *mode) {
    fflush(NULL);
    return popen(cmd, mode);
}

int t_pclose(FILE *f) {
    return exit_status(pclose(f));
}

static int close_fails;                 /* the error fclose() ends with, as when a disk has filled up */

int t_fclose(FILE *f) {
    int rc = fclose(f);
    if(close_fails) {
        errno = close_fails;
        return EOF;
    }
    return rc;
}

static volatile unsigned long long *searched;   /* bytes given to regexec, in memory that a run's fork shares */

int counted_regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t *pmatch, int eflags) {
    if(eflags & REG_STARTEND) *searched += (unsigned long long)(pmatch[0].rm_eo - pmatch[0].rm_so);
    return surf_regexec(preg, string, nmatch, pmatch, eflags);
}

/**** a run ****/

static char dir[256], in_path[300], out_path[300], err_path[300];

static struct {
    char *out, *err;
    size_t nout, nerr;
    int status;                         /* -1: a signal */
    int sig;
} R;

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "r");
    char *p = NULL;
    size_t cap = 0, len = 0, k;
    if(f) {
        do {
            if(len + 65536 + 1 > cap) {
                cap = cap ? cap * 2 : 131072;
                if(!(p = realloc(p, cap))) abort();
            }
            k = fread(p + len, 1, 65536, f);
            len += k;
        } while(k);
        fclose(f);
    }
    if(!p) p = calloc(1, 1);
    p[len] = 0;
    *n = len;
    return p;
}

static void write_file(const char *path, const char *data, size_t n) {
    FILE *f = fopen(path, "w");
    if(!f || fwrite(data, 1, n, f) != n || fclose(f)) {
        perror(path);
        exit(1);
    }
}

static const char *join(const char *name) {
    static char t[4][320];
    static int i;
    i = (i + 1) % 4;
    snprintf(t[i], sizeof(t[i]), "%s/%s", dir, name);
    return t[i];
}

/* awk with these arguments, this standard input and a limit on time and memory; R has what came of it */
static void run_args(int argc, char **argv, const char *input, size_t ninput, int ms, size_t limit, int keep_output) {
    pid_t pid;
    int st;
    write_file(in_path, input ? input : "", input ? ninput : 0);
    fflush(NULL);
    pid = fork();
    if(pid < 0) {
        perror("fork");
        exit(1);
    }
    if(!pid) {
        struct itimerval t;
        int fd = open(in_path, O_RDONLY);
        dup2(fd, 0);
        close(fd);
        fd = open(keep_output ? out_path : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 1);
        close(fd);
        fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 2);
        close(fd);
        memset(&t, 0, sizeof(t));
        t.it_value.tv_sec = ms / 1000;
        t.it_value.tv_usec = (ms % 1000) * 1000;
        setitimer(ITIMER_REAL, &t, NULL);       /* its signal ends a program that loops */
        mem_limit = limit;
        exit(awk_main(argc, argv));
    }
    waitpid(pid, &st, 0);
    R.sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    R.status = R.sig ? -1 : WEXITSTATUS(st);
    free(R.out);
    free(R.err);
    R.out = read_file(out_path, &R.nout);
    R.err = read_file(err_path, &R.nerr);
    if(!keep_output) R.nout = 0;
}

static void run_prog(const char *prog, const char *input) {
    char *argv[3];
    argv[0] = "awk";
    argv[1] = (char *)prog;
    argv[2] = NULL;
    run_args(2, argv, input, input ? strlen(input) : 0, 20000, 0, 1);
}

static void show(const char *prog, const char *want) {
    failed++;
    printf("FAIL: %.300s\n  got  [%.500s] status %d signal %d\n  want [%.500s]\n  err  [%.200s]\n", prog, R.out, R.status, R.sig, want, R.err);
}

static void is(const char *prog, const char *input, const char *want) {
    run_prog(prog, input);
    checks++;
    if(strcmp(R.out, want) || R.status) show(prog, want);
}

static void status_is(const char *prog, const char *input, const char *want, int status) {
    run_prog(prog, input);
    checks++;
    if(strcmp(R.out, want) || R.status != status) show(prog, want);
}

static void fails(const char *prog, const char *input, const char *why) {
    run_prog(prog, input);
    checks++;
    if(R.status != 2 || !strstr(R.err, why)) show(prog, why);
}

/* with more arguments than the program: a NULL ends them, and { are the program's */
static void args_are(const char *input, const char *want, int status, char *a0, char *a1, char *a2, char *a3, char *a4, char *a5) {
    char *argv[8];
    int argc = 1;
    argv[0] = "awk";
    argv[1] = a0; argv[2] = a1; argv[3] = a2; argv[4] = a3; argv[5] = a4; argv[6] = a5; argv[7] = NULL;
    while(argv[argc]) argc++;
    run_args(argc, argv, input, input ? strlen(input) : 0, 20000, 0, 1);
    checks++;
    if(strcmp(R.out, want) || R.status != status) show(a0, want);
}

/**** the reviewed table ****/

static const struct { const char *prog, *input, *out; int status; } table[] = {
#include "awk/table.inc"
};

/**** what SurfOS asks ****/

static void characters(void) {
    is("{ print length($0) }", "h\303\251llo\n", "5\n");
    is("BEGIN { print length(\"\343\201\202\360\237\214\212\") }", NULL, "2\n");            /* three bytes and four */
    is("{ print substr($0, 2, 2) }", "h\303\251llo\n", "\303\251l\n");
    is("{ print substr($0, 2) }", "\343\201\202\303\251x\n", "\303\251x\n");
    is("BEGIN { x = 1e300 * 1e300; y = x - x; print substr(\"abc\", y, 2) \"|\" substr(\"abc\", 2, y) \"|\" substr(\"abc\", x) \"|\" substr(\"abc\", 2, x) \"|\" substr(\"abc\", -x, x) \"|\" substr(\"abc\", 2, -x) }", NULL, "ab|||bc|abc|\n");
    is("{ print index($0, \"l\") }", "h\303\251llo\n", "3\n");
    is("{ print match($0, /l+/), RSTART, RLENGTH }", "h\303\251llo\n", "3 3 2\n");
    is("{ print match($0, /\303\251./), RLENGTH }", "h\303\251llo\n", "2 2\n");
    is("{ print toupper($0) }", "h\303\251llo\n", "H\303\251LLO\n");                        /* A-Z only: there is no locale */
    is("BEGIN { printf \"%c|%c|%c\\n\", 233, 12354, 65 }", NULL, "\303\251|\343\201\202|A\n");
    is("BEGIN { printf \"%c\\n\", \"\303\251x\" }", NULL, "\303\251\n");                    /* the first character, whole */
    is("BEGIN { printf \"%3s|%-3s|%.1s|\\n\", \"\303\251\", \"\303\251\", \"\303\251x\" }", NULL, "  \303\251|\303\251  |\303\251|\n");
    is("BEGIN { printf \"%3c|\\n\", \"\303\251\" }", NULL, "  \303\251|\n");
    is("BEGIN { n = split(\"a\303\251b\", x, \"\"); print n, x[2] }", NULL, "3 \303\251\n");
    is("BEGIN { FS = \"\" } { print NF, $2 }", "a\303\251b\n", "3 \303\251\n");
    is("{ gsub(/x*/, \"-\"); print }", "\303\251\n", "-\303\251-\n");                       /* an empty match never splits one */
    is("{ gsub(/./, \"<&>\"); print }", "\343\201\202a\n", "<\343\201\202><a>\n");
    /* a byte that is in no sequence is a character, and goes through as it is */
    is("{ print length($0) }", "a\377b\n", "3\n");
    is("{ print length($0) }", "\303\n", "1\n");
    is("{ print length($0) }", "\340\200\200\n", "3\n");                                    /* overlong */
    is("{ print length($0) }", "\355\240\200\n", "3\n");                                    /* a surrogate */
    is("{ print length($0) }", "\364\220\200\200\n", "4\n");                                /* above U+10FFFF */
    is("{ print substr($0, 2, 1) }", "a\377b\n", "\377\n");
    is("{ print }", "a\377b\n", "a\377b\n");
    is("{ print toupper($0) }", "a\377b\n", "A\377B\n");
}

static void binary_data(void) {
    char *argv[3];
    argv[0] = "awk";
    /* a NUL in the input is a byte like any other */
    argv[1] = "{ print length($0); print }";
    argv[2] = NULL;
    run_args(2, argv, "a\0b\n", 4, 20000, 0, 1);
    CHECK(R.nout == 6 && !memcmp(R.out, "3\na\0b\n", 6));
    argv[1] = "{ gsub(/b/, \"X\"); print }";
    run_args(2, argv, "a\0b\n", 4, 20000, 0, 1);
    CHECK(R.nout == 4 && !memcmp(R.out, "a\0X\n", 4));
    argv[1] = "{ print $2 }";
    run_args(2, argv, "a\0b c\n", 6, 20000, 0, 1);
    CHECK(R.nout == 2 && !memcmp(R.out, "c\n", 2));
    argv[1] = "BEGIN { printf \"%c\", 0 }";
    run_args(2, argv, NULL, 0, 20000, 0, 1);
    CHECK(R.nout == 1 && !R.out[0]);
    argv[1] = "BEGIN { RS = \"\\0\" } { print }";
    run_args(2, argv, "a\0b\0", 4, 20000, 0, 1);
    CHECK(R.nout == 4 && !memcmp(R.out, "a\nb\n", 4));
    /* no newline at the end; carriage returns are the record's */
    is("{ print NR, $0 }", "a\nb", "1 a\n2 b\n");
    is("{ print length($0) }", "a\r\n", "2\n");
}

static void records(void) {
    is("BEGIN { RS = \"[0-9]+\" } { print }", "a1b22c\n", "a\nb\nc\n\n");
    is("BEGIN { RS = \"\\r\\n\" } { print length($0) }", "ab\r\ncd\r\n", "2\n2\n");
    is("BEGIN { RS = \"aa|b\" } { print NR \":\" $0 }", "xaay", "1:x\n2:y\n");
    is("BEGIN { RS = \"x*\" } END { print NR }", "ab", "1\n");                              /* an empty match is no separator */
    is("BEGIN { RS = \"\" } { print $NF }", "\n\n\na b\nc\n\n\n", "c\n");
    is("BEGIN { RS = \"\"; FS = \"[,]\" } { print NF }", "a,b\nc\n\nd\n", "3\n1\n");
    is("{ RS = \";\" } { print }", "a\nb;c;\n", "a\nb\nc\n\n\n");                         /* the last record is the newline */
    is("{ $3 = \"\"; print; print NF }", "a b\n", "a b \n3\n");
    is("{ print $(1+1) }", "a b\n", "b\n");
    is("{ print $NF; NF--; print }", "a b c\n", "c\na b\n");
    is("{ $(NF+2) = \"x\"; print; print NF }", "a\n", "a  x\n3\n");
    is("{ i = 1; print $i++; print i }", "5 6\n", "5\n1\n");                                /* $i++ is ($i)++ */
    is("{ i = 1; print $++i }", "5 6\n", "6\n");
    is("{ print $$1 }", "2 x\n", "x\n");
    is("{ $2 = 3.0; print; x = $2; print x + 1 }", "a b\n", "a 3\n4\n");
    is("BEGIN { OFS = \":\" } { $1 = $1; OFS = \"-\"; print; print $1, $2 }", "a b\n", "a:b\na-b\n");     /* $0 is made with the OFS of the change */
    is("{ FS = \":\"; print $1 }", "a:b c\nd:e f\n", "a:b\nd\n");                           /* FS is for the next record */
    /* large */
    {
        char *big = malloc(2000001);
        size_t i;
        for(i = 0; i < 2000000; i++) big[i] = i % 2 ? ' ' : 'x';
        big[2000000] = 0;
        is("{ print NF, length($0), $1000000 }", big, "1000000 2000000 x\n");               /* a million fields, no newline */
        free(big);
    }
    fails("{ print $(-1) }", "a\n", "negative field");
    fails("{ $(-1) = 2 }", "a\n", "negative field");
    fails("{ NF = -1 }", "a\n", "negative field");
    fails("{ print $(2^40) }", "a\n", "too large");
    fails("{ $(2^31) = 1 }", "a\n", "too large");
    is("{ print $1.5 }", "a b\n", "a\n");
    is("{ print $+1, $!0, $-0 }", "a b\n", "a a a b\n");
    is("BEGIN { FS = \"x*\" } { print NF, $1 }", "aab\n", "1 aab\n");                     /* an empty match separates nothing */
    is("BEGIN { FS = \"x*\" } { print NF, $2 }", "axxb\n", "2 b\n");
    is("BEGIN { RS = \"\"; FS = \"[,]\" } { print NF, $2 }", "a\nb,c\n", "3 b\n");         /* a newline before the match counts first */
}

static void files_and_commands(void) {
    char prog[2000];
    const char *f = join("f.txt"), *g = join("g.txt");
    unlink(f);
    /* > makes the file once, and then the stream goes on; close ends it, and >> adds */
    snprintf(prog, sizeof(prog), "BEGIN { print \"a\" > \"%s\"; print \"b\" > \"%s\"; close(\"%s\"); print \"c\" >> \"%s\"; close(\"%s\");"
             " while((getline x < \"%s\") > 0) print \"got\", x; print close(\"%s\"), close(\"%s\") }", f, f, f, f, f, f, f, f);
    is(prog, NULL, "got a\ngot b\ngot c\n0 -1\n");
    snprintf(prog, sizeof(prog), "BEGIN { print \"new\" > \"%s\"; close(\"%s\"); getline x < \"%s\"; print x }", f, f, f);
    is(prog, NULL, "new\n");
    /* each getline */
    write_file(g, "one two\nthree\n", 14);
    snprintf(prog, sizeof(prog), "BEGIN { getline < \"%s\"; print $2, NF, NR; getline y < \"%s\"; print y, NF, NR, (getline z < \"%s\") }", g, g, g);
    is(prog, NULL, "two 2 0\nthree 2 0 0\n");
    is("{ getline; print NR, $0 }", "a\nb\nc\n", "2 b\n3 c\n");                             /* the last has no next: $0 stays */
    is("{ while((getline l) > 0) n++ } END { print n, NR }", "a\nb\nc\n", "2 3\n");
    is("BEGIN { \"echo a b\" | getline; print $2, NF, NR, FNR }", NULL, "b 2 0 0\n");     /* a command's records are not counted */
    is("BEGIN { \"echo a b\" | getline x; print x, NF, NR, FNR }", NULL, "a b 0 0 0\n");
    is("BEGIN { \"echo 1\" \" 2\" | getline x; print x }", NULL, "1 2\n");                  /* the whole concatenation is the command */
    is("BEGIN { c = \"echo 7\"; c | getline a[1]; print a[1] + 1 }", NULL, "8\n");
    is("BEGIN { c = \"echo 7 8\"; c | getline $2; print $0, NF }", NULL, " 7 8 2\n");
    is("BEGIN { c = \"echo x\"; c | getline; c | getline y; print \"[\" y \"]\"; print (c | getline) }", NULL, "[]\n0\n");
    is("BEGIN { c = \"echo x\"; c | getline; close(c); c | getline y; print y }", NULL, "x\n");
    is("BEGIN { print \"b\\na\" | \"sort\"; close(\"sort\"); print \"c\" }", NULL, "a\nb\nc\n");
    is("BEGIN { printf \"%s\\n\", \"x\" | \"cat\" }", NULL, "x\n");
    is("BEGIN { print close(\"nothing\") }", NULL, "-1\n");
    is("BEGIN { print system(\"exit 3\"), system(\"true\") }", NULL, "3 0\n");
    is("BEGIN { printf \"a\"; system(\"echo b\"); print \"c\" }", NULL, "ab\nc\n");         /* what awk printed comes first */
    is("BEGIN { c = \"exit 5\"; print \"x\" | c; print close(c) }", NULL, "5\n");
    is("BEGIN { c = \"exit 6\"; c | getline; print close(c) }", NULL, "6\n");
    is("BEGIN { print \"x\" > \"/dev/stderr\" } END { }", "", "");
    CHECK(!strcmp(R.err, "x\n"));
    is("BEGIN { print \"x\" > \"/dev/stdout\"; print \"y\" }", NULL, "x\ny\n");
    is("BEGIN { print fflush(), fflush(\"nothing\") }", NULL, "0 -1\n");
    snprintf(prog, sizeof(prog), "BEGIN { print \"x\" > \"%s\"; print fflush(\"%s\"); getline y < \"%s\"; print y }", f, f, f);
    is(prog, NULL, "0\nx\n");
    is("BEGIN { print (getline x < \"/nonexistent/f\") }", NULL, "-1\n");
    is("BEGIN { print (getline x < \"/\") }", NULL, "-1\n");                                /* a directory */
    snprintf(prog, sizeof(prog), "BEGIN { print \"x\" > \"%s/no/such\" }", dir);
    fails(prog, NULL, "No such file");
    fails("BEGIN { print \"x\" > \"\" }", NULL, "empty");
    /* > and >> of a name are one stream, and a file and a command of that name are two, each way */
    snprintf(prog, sizeof(prog), "BEGIN { print \"a\" > \"%s\"; print \"b\" >> \"%s\"; close(\"%s\"); getline x < \"%s\"; getline y < \"%s\"; print x, y }", f, f, f, f, f);
    is(prog, NULL, "a b\n");
    {
        const char *s = join("both.sh");
        char here[1024];
        write_file(s, "#!/bin/sh\necho from-command\n", 28);
        chmod(s, 0700);
        snprintf(prog, sizeof(prog), "BEGIN { c = \"%s\"; getline a < c; c | getline b; print a; print b; print close(c), close(c) }", s);
        is(prog, NULL, "#!/bin/sh\nfrom-command\n0 -1\n");
        snprintf(prog, sizeof(prog), "BEGIN { c = \"%s\"; c | getline b; getline a < c; print a; print b }", s);
        is(prog, NULL, "#!/bin/sh\nfrom-command\n");
        if(!getcwd(here, sizeof(here)) || chdir(dir)) abort();
        unlink("cat");
        is("BEGIN { print \"to the file\" > \"cat\"; print \"to the command\" | \"cat\"; close(\"cat\"); getline x < \"cat\"; print x }", NULL, "to the command\nto the file\n");
        unlink("cat");
        if(chdir(here)) abort();
    }
    /* descriptors run out with a message, here at the host's limit, and close gives them back */
    {
        const char *p = "BEGIN { for(i = 0; i < 200; i++) { f = d \"/m\" i; print i > f; close(f); getline x < f; close(f); n += x }; print n }";
        char assign[320];
        snprintf(assign, sizeof(assign), "d=%s", dir);
        args_are(NULL, "19900\n", 0, "-v", assign, (char *)p, NULL, NULL, NULL);
    }
}

static void file_holds(const char *path, const char *want) {
    size_t n;
    char *t = read_file(path, &n);
    checks++;
    if(strcmp(t, want)) {
        failed++;
        printf("FAIL: %s holds [%.200s], want [%.200s]\n", path, t, want);
    }
    free(t);
}

/* What print sent to a file may leave only when the file is closed: a disk that has filled up says so there. */
static void writes_that_fail(void) {
    char prog[1000];
    const char *f = join("w.txt"), *g = join("w2.txt");
    close_fails = ENOSPC;
    snprintf(prog, sizeof(prog), "BEGIN { print \"data\" > \"%s\" }", f);
    fails(prog, NULL, "No space left");
    CHECK(strstr(R.err, f) != NULL);
    snprintf(prog, sizeof(prog), "BEGIN { print \"data\" >> \"%s\"; exit 3 }", f);
    fails(prog, NULL, "No space left");                     /* the failure outranks what exit said */
    snprintf(prog, sizeof(prog), "BEGIN { print \"data\" > \"%s\"; close(\"%s\"); print \"after\" }", f, f);
    fails(prog, NULL, "No space left");                     /* and close does not go on as if it had worked */
    CHECK(!strstr(R.out, "after"));
    snprintf(prog, sizeof(prog), "BEGIN { print 1 > \"%s\"; print 2 > \"%s\" }", f, g);
    fails(prog, NULL, "No space left");                     /* each file that lost something is named */
    CHECK(strstr(R.err, f) != NULL && strstr(R.err, g) != NULL);
    snprintf(prog, sizeof(prog), "BEGIN { getline x < \"%s\"; close(\"%s\"); getline x < \"%s\"; print \"read\" }", g, g, g);
    is(prog, NULL, "read\n");                               /* a file only read has lost nothing */
    close_fails = 0;
    /* the real thing, where the system has a device that is always full */
    if(!access("/dev/full", W_OK)) {
        fails("BEGIN { print \"data\" > \"/dev/full\" }", NULL, "/dev/full: No space left");
        fails("BEGIN { print \"data\" > \"/dev/full\"; close(\"/dev/full\"); print \"after\" }", NULL, "/dev/full: No space left");
        CHECK(!strstr(R.out, "after"));
        fails("BEGIN { print \"data\" > \"/dev/full\"; print (fflush(\"/dev/full\") != 0) }", NULL, "/dev/full: No space left");
        CHECK(!strcmp(R.out, "1\n"));
    }
    /* a mistake ends awk as the end of a program does: what was sent arrives, and commands are waited for */
    unlink(g);
    snprintf(prog, sizeof(prog), "BEGIN { print \"x\" | \"sleep 0.3; cat > %s\"; print 1 / 0 }", g);
    fails(prog, NULL, "division by zero");
    file_holds(g, "x\n");
    unlink(f);
    snprintf(prog, sizeof(prog), "BEGIN { print \"y\" > \"%s\"; printf \"%%d %%d\", 1 }", f);
    fails(prog, NULL, "not enough arguments");
    file_holds(f, "y\n");
    /* and one that fails then is said too */
    close_fails = ENOSPC;
    snprintf(prog, sizeof(prog), "BEGIN { print \"y\" > \"%s\"; print 1 / 0 }", f);
    fails(prog, NULL, "division by zero");
    CHECK(strstr(R.err, "No space left") != NULL);
    close_fails = 0;
}

static void command_line(void) {
    const char *a = join("a.txt"), *b = join("b.txt"), *p1 = join("p1.awk"), *p2 = join("p2.awk");
    write_file(a, "a1\na2\n", 6);
    write_file(b, "b1\n", 3);
    write_file(p1, "function f(x) { return x + 1 }\n", 31);
    write_file(p2, "BEGIN { print f(1) }\n", 21);
    args_are(NULL, "1 a a1 1\n2 a a2 2\n1 b b1 3\n", 0, "{ print FNR, (FILENAME ~ /a.txt/ ? \"a\" : \"b\"), $0, NR }", (char *)a, (char *)b, NULL, NULL, NULL);
    args_are(NULL, "2\n", 0, "-f", (char *)p1, "-f", (char *)p2, NULL, NULL);
    args_are("x y\n", "y\n", 0, "-F", " ", "{ print $2 }", NULL, NULL, NULL);
    args_are("a:b\n", "b\n", 0, "-F:", "{ print $2 }", NULL, NULL, NULL, NULL);
    args_are("atb\tc\n", "c\n", 0, "-Ft", "{ print $2 }", NULL, NULL, NULL, NULL);          /* -Ft is a tab */
    args_are("a\tb\n", "b\n", 0, "-F\\t", "{ print $2 }", NULL, NULL, NULL, NULL);
    args_are("a1b\n", "b\n", 0, "-F[0-9]", "{ print $2 }", NULL, NULL, NULL, NULL);
    args_are(NULL, "x\ty\n", 0, "-v", "s=x\\ty", "BEGIN { print s }", NULL, NULL, NULL);    /* the escapes of a string */
    args_are(NULL, "1\n", 0, "-v", "n=10", "BEGIN { print (n > 9) }", NULL, NULL, NULL);    /* and it compares as a number */
    args_are(NULL, ":\n", 0, "-v", "OFS=:", "BEGIN { print \"\", \"\" }", NULL, NULL, NULL);
    args_are(NULL, "2\n", 0, "-v", "NF=2", "BEGIN { print NF }", NULL, NULL, NULL);
    /* assignments among the files, when they are reached */
    args_are("in\n", "1 in\n2 b1\n", 0, "{ print x, $0 }", "x=1", "-", "x=2", (char *)b, NULL);
    args_are("z\n", "5\n", 0, "END { print x }", "x=5", NULL, NULL, NULL, NULL);            /* after the last file too, before END */
    args_are(NULL, "0\n", 0, "BEGIN { print x + 0 }", "x=5", NULL, NULL, NULL, NULL);       /* not yet in BEGIN */
    args_are("s\n", "s\n", 0, "{ print }", "", NULL, NULL, NULL, NULL);                     /* an empty one is passed over */
    args_are(NULL, "3 awk p q\n", 0, "BEGIN { print ARGC, ARGV[0], ARGV[1], ARGV[2] }", "p", "q", NULL, NULL, NULL);
    /* the program may change ARGV */
    args_are("in\n", "b1\n", 0, "BEGIN { ARGV[1] = ARGV[2]; ARGC = 2 } { print }", "/nonexistent", (char *)b, NULL, NULL, NULL);
    args_are("in\n", "in\n", 0, "BEGIN { delete ARGV[1] } { print }", "/nonexistent", NULL, NULL, NULL, NULL);
    args_are("in\n", "1\n", 0, "--", "END { print NR }", NULL, NULL, NULL, NULL);
    args_are("in\n", "1\n", 0, "BEGIN { ARGC = 1e300 } END { print NR }", NULL, NULL, NULL, NULL, NULL);     /* past the end of ARGV is nothing */
    args_are("in\n", "1\n", 0, "BEGIN { ARGC = -5 } END { print NR }", NULL, NULL, NULL, NULL, NULL);
    /* nextfile */
    args_are(NULL, "a1\nb1\n", 0, "FNR == 2 { nextfile } { print }", (char *)a, (char *)b, NULL, NULL, NULL);
    args_are(NULL, "a1\nb1\n", 0, "{ print; nextfile }", (char *)a, (char *)b, NULL, NULL, NULL);
    /* a name that cannot take a value is refused */
    args_are("in\n", "", 2, "{ print }", "ARGV=1", (char *)a, NULL, NULL, NULL);
    CHECK(strstr(R.err, "ARGV=1: that name cannot take a value") != NULL);
    args_are("in\n", "", 2, "BEGIN { ARGV[1] = \"ARGV=1\" } { print }", "x", "y", NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are("in\n", "", 2, "{ print }", "ENVIRON=1", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are("in\n", "", 2, "function f() { } { print }", "f=1", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are("in\n", "", 2, "BEGIN { x[1] } { print }", "x=1", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are("in\n", "", 2, "{ print }", "length=1", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are("in\n", "", 2, "{ print }", "if=1", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are(NULL, "", 2, "-v", "ARGV=1", "BEGIN { }", NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are(NULL, "", 2, "-v", "ENVIRON=1", "BEGIN { }", NULL, NULL, NULL);
    CHECK(strstr(R.err, "cannot take a value") != NULL);
    args_are(NULL, "", 2, "-v", "x=1", "function x() { } BEGIN { }", NULL, NULL, NULL);
    CHECK(strstr(R.err, "both a function and a variable") != NULL);
    args_are(NULL, "", 2, "-v", "x=1", "BEGIN { x[1] = 2 }", NULL, NULL, NULL);
    CHECK(strstr(R.err, "scalar used as an array") != NULL);
    args_are("in\n", "", 2, "{ x[1] = 2 }", "x=1", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "scalar used as an array") != NULL);
    args_are("in\n", "", 2, "function f(a) { getline; a[1] = 1 } BEGIN { f(x) }", "x=1", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "scalar used as an array") != NULL);
    args_are("in\n", "in 1 2\n", 0, "{ print $0, NF0, ARGC0 }", "NF0=1", "ARGC0=2", NULL, NULL, NULL);
    args_are("in\n", "1 in\n", 0, "{ print NR, $0 }", "NR=0", NULL, NULL, NULL, NULL);
    /* the ways it refuses */
    args_are(NULL, "", 2, "{ print }", "/nonexistent/file", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "awk: /nonexistent/file: No such file") != NULL);
    args_are(NULL, "", 2, "{ print }", (char *)dir, NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "awk: ") == R.err);
    args_are(NULL, "", 2, "-f", "/nonexistent/prog", NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "/nonexistent/prog: No such file") != NULL);
    args_are(NULL, "", 2, "-v", "1x=2", "BEGIN { }", NULL, NULL, NULL);
    CHECK(strstr(R.err, "not a var=value") != NULL);
    args_are(NULL, "", 2, "-x", NULL, NULL, NULL, NULL, NULL);
    CHECK(strstr(R.err, "usage: awk") != NULL);
    args_are(NULL, "", 2, "-F", NULL, NULL, NULL, NULL, NULL);
    {
        char *argv[2];
        argv[0] = "awk";
        argv[1] = NULL;
        run_args(1, argv, NULL, 0, 20000, 0, 1);
        CHECK(R.status == 2 && strstr(R.err, "usage: awk"));
    }
    /* a mistake in the second program file names it */
    write_file(p2, "BEGIN {\n  print )\n}\n", 20);
    args_are(NULL, "", 2, "-f", (char *)p1, "-f", (char *)p2, NULL, NULL);
    CHECK(strstr(R.err, "p2.awk: line 2: syntax error near ')'") != NULL);
    /* the environment */
    setenv("AWK_TEST_VALUE", "12", 1);
    is("BEGIN { print ENVIRON[\"AWK_TEST_VALUE\"] + 1, (ENVIRON[\"AWK_TEST_VALUE\"] > 9) }", NULL, "13 1\n");
}

static void language(void) {
    /* numbers and strings */
    is("BEGIN { print 3 \"\", 3.0 \"\", 0.1 \"\", 1e6 \"\", 1e21 \"\", 123456789012 \"\" }", NULL, "3 3 0.1 1000000 1000000000000000000000 123456789012\n");
    is("BEGIN { print 2^53, 2^53 + 2, -2^53 }", NULL, "9007199254740992 9007199254740994 -9007199254740992\n");
    is("BEGIN { print 1e300 * 1e300, -1e300 * 1e300 }", NULL, "inf -inf\n");
    is("BEGIN { x = 1e300 * 1e300; print x - x, -(x - x); printf \"%f %d\\n\", x - x, x - x }", NULL, "nan nan\nnan nan\n");   /* no sign, on any machine */
    is("BEGIN { print \"3x\" + 1, \"x3\" + 1, \".5\" + 0, \"1e2x\" + 0, \"1e\" + 0, \"+5\" + 0, \"-\" + 0, \" 7 \" + 0 }", NULL, "4 1 0.5 100 1 5 0 7\n");
    is("BEGIN { print \"0x10\" + 0, \"inf\" + 0, \"nan\" + 0, \"infinity\" + 0 }", NULL, "0 0 0 0\n");     /* decimal only */
    is("BEGIN { s = \"5\"; for(i = 0; i < 100; i++) s = \"0\" s; print s + 1 }", NULL, "6\n");       /* a number of any length */
    is("{ print ($1 == 16), ($2 < 1), ($3 < 1) }", "0x10 inf nan\n", "0 0 0\n");
    is("{ print ($1 == 100), ($2 == 0.5), ($3 == 5) }", "1e2 .5 +5\n", "1 1 1\n");
    is("{ print ($1 < $2) }", "10 9\n", "0\n");                                             /* as numbers */
    is("{ print ($1 \"\" < $2 \"\") }", "10 9\n", "1\n");                                   /* as strings */
    is("{ x = $1; print (x < 9) }", "10\n", "0\n");                                         /* an assignment keeps what it is */
    is("{ a[1] = $1; print (a[1] < 9) }", "10\n", "0\n");
    is("BEGIN { split(\"10 9\", a); print (a[1] < a[2]) }", NULL, "0\n");
    is("BEGIN { print (x < 1), (x < \"a\"), (x == 0), (x == \"\") }", NULL, "1 1 1 1\n");
    is("BEGIN { print length(1e6), length(0.1), index(100, 0) }", NULL, "7 3 2\n");
    is("BEGIN { CONVFMT = \"%d\"; a[1.5] = 1; for(k in a) print k; print 12 \"\" }", NULL, "1\n12\n");
    is("BEGIN { OFMT = \"%.1f\"; print 1.25, 1.25 \"\"; print 7 }", NULL, "1.2 1.25\n7\n");
    /* a format that would ask for this same conversion again: a number is no format, as on the Mac's awk and mawk, and %s
       gets what the default gives */
    is("BEGIN { CONVFMT = 0.5; x = 3.14 \"\"; print x }", NULL, "3.14\n");
    is("BEGIN { CONVFMT = 0.5; x = (1.5 + 1.64) \"\"; print x }", NULL, "3.14\n");
    is("BEGIN { OFMT = 0.5; print 3.14 }", NULL, "3.14\n");
    is("BEGIN { OFMT = 2; print 3.14 }", NULL, "3.14\n");
    is("BEGIN { CONVFMT = \"%s\"; x = 3.14 \"\"; print x }", NULL, "3.14\n");
    is("BEGIN { OFMT = \"%s\"; print 3.14 }", NULL, "3.14\n");
    is("BEGIN { OFMT = \"[%s]\"; print 3.14 }", NULL, "[3.14]\n");
    is("BEGIN { CONVFMT = \"%s\"; a[0.5] = 1; for(k in a) print k }", NULL, "0.5\n");
    is("BEGIN { CONVFMT = \"%c\"; x = 65.5 \"\"; print x }", NULL, "A\n");
    is("BEGIN { CONVFMT = \"\"; x = 3.14 \"\"; print \"[\" x \"]\" }", NULL, "[]\n");
    args_are(NULL, "0.5\n", 0, "-v", "CONVFMT=0.5", "BEGIN { x = 3.14 \"\"; print x }", NULL, NULL, NULL);      /* a string is a format */
    fails("BEGIN { CONVFMT = \"%s %s\"; x = 3.14 \"\" }", NULL, "not enough arguments");
    is("BEGIN { printf \"%d %s\\n\", 1e18, 1e18 }", NULL, "1000000000000000000 1000000000000000000\n");
    is("BEGIN { printf \"%d\\n\", 1e30 }", NULL, "1000000000000000019884624838656\n");
    is("BEGIN { printf \"%i|%5.3d|%x|%o\\n\", 3.9, 7, 255.9, 8 }", NULL, "3|  007|ff|10\n");
    is("BEGIN { printf \"%x\\n\", -1 }", NULL, "ffffffffffffffff\n");
    is("BEGIN { printf \"%x %c\\n\", 1e30, 128522 }", NULL, "1000000000000000019884624838656 \360\237\230\212\n");
    is("BEGIN { printf \"%s %s\\n\", 1e300 * 1e300, -1e300 * 1e300; printf \"%d\\n\", 1e300 * 1e300 }", NULL, "inf -inf\ninf\n");
    is("BEGIN { printf \"%.3s|%.0s|%s\\n\", \"abcdef\", \"x\", \"\" }", NULL, "abc||\n");
    is("BEGIN { printf \"%-+5d|%#o|%#x|%e\\n\", 3, 8, 255, 0 }", NULL, "+3   |010|0xff|0.000000e+00\n");
    is("BEGIN { printf \"%ld %lld %hd\\n\", 1, 2, 3 }", NULL, "1 2 3\n");                   /* C's lengths are taken and mean nothing */
    is("BEGIN { printf \"%*d|\\n\", -4, 1 }", NULL, "1   |\n");
    is("BEGIN { printf \"%\" }", NULL, "%");
    is("BEGIN { printf \"%5\" }", NULL, "%5");
    is("BEGIN { printf \"a%\" \"%b\\n\" }", NULL, "a%b\n");
    fails("BEGIN { printf \"%d %d\\n\", 1 }", NULL, "not enough arguments");
    fails("BEGIN { printf \"%*d\\n\" }", NULL, "not enough arguments");
    fails("BEGIN { printf \"%999999999d\\n\", 1 }", NULL, "too large");
    fails("BEGIN { printf \"%.999999999f\\n\", 1 }", NULL, "too large");
    /* the arithmetic of ^ and % */
    is("BEGIN { print 2^10, 2^-1, 0^0, (-2)^3, 2^0.5, 2^1000 > \"/dev/null\" }", NULL, "");
    is("BEGIN { print 3^40 == 12157665459056928801, 10^15 }", NULL, "1 1000000000000000\n");
    is("BEGIN { x = 2; x ^= 3; x **= 2; print x }", NULL, "64\n");
    fails("BEGIN { print 1 / 0 }", NULL, "division by zero");
    fails("BEGIN { print 1 % 0 }", NULL, "division by zero");
    fails("BEGIN { x = 1; x /= 0 }", NULL, "division by zero");
    /* the grammar */
    is("BEGIN { print 1 + 2 \" \" 3 * 4 }", NULL, "3 12\n");
    is("BEGIN { print 1 \" \" -1 }", NULL, "1-1\n");                                     /* 1 (" " - 1) */
    is("BEGIN { print -12 \" \" -24 }", NULL, "-12-24\n");
    is("BEGIN { print 2 * -3, 2 - -3, 2 ^ -1, - - 2, !!2, -!0 }", NULL, "-6 5 0.5 2 1 -1\n");
    is("BEGIN { print +\"3x\", + 2, +x }", NULL, "3 2 0\n");
    is("BEGIN { if(1); else print \"x\"; print \"y\"; while(i++ < 2); print i; for(;j < 2;j++); print j }", NULL, "y\n3\n2\n");
    is("BEGIN { print !0 + 1, !1 ~ 0 }", NULL, "2 1\n");
    is("BEGIN { x = 3; y = 4; print x y, x+y, x -y, x - y }", NULL, "34 7 -1 -1\n");
    is("BEGIN { a = 1; a += a += 2; print a }", NULL, "6\n");
    is("BEGIN { print 1 + x = 2, x }", NULL, "3 2\n");                                      /* an assignment where the name stands */
    is("BEGIN { print 1 ? 2 : 3, 0 ? 2 : 3, 1 ? 0 ? 4 : 5 : 6 }", NULL, "2 3 5\n");
    is("BEGIN { x = 0 ? 1 : y = 2; print x, y }", NULL, "2 2\n");
    is("BEGIN { print 5 in a, (5 in a) + 1, !(5 in a) }", NULL, "0 1 1\n");
    is("BEGIN { a[1]; print 1 in a && 2 in a, 1 in a || 2 in a }", NULL, "0 1\n");
    is("BEGIN { print \"a\" ~ \"a\" \"b\", \"ab\" ~ \"a\" \"b\" }", NULL, "0 1\n");         /* concatenation first */
    is("BEGIN { print (1 < 2) (3 < 2), 1 == 1 }", NULL, "10 1\n");
    is("BEGIN { print length, length \"x\", length() }", NULL, "0 0x 0\n");
    is("{ print length + 1, length * 2 }", "abc\n", "4 6\n");
    is("BEGIN { n = 4; print n / 2 / 1, n /2/ 1 }", NULL, "2 2\n");                         /* a / after an operand divides */
    is("{ print /a/ + /b/, !/c/ }", "ab\n", "2 1\n");
    is("BEGIN { print /=/ ? 1 : 0 }", NULL, "0\n");
    is("BEGIN { a = 6; a /= 2; print a }", NULL, "3\n");
    is("{ print match($0, /=/) }", "a=b\n", "2\n");
    is("BEGIN { print length(\"a\" \"b\"), substr(\"abc\", 1 + 1) }", NULL, "2 bc\n");
    /* a word that starts with getline is a name */
    is("BEGIN { getline_cmd = \"cat\"; print \"hello\" | getline_cmd }", NULL, "hello\n");
    is("BEGIN { getline2 = \"cat\"; print \"hello\" | getline2 }", NULL, "hello\n");
    is("BEGIN { getlineX = \"cat\"; print \"hello\" | getlineX }", NULL, "hello\n");
    is("BEGIN { \"echo a\" |getline; print }", NULL, "a\n");
    is("BEGIN { \"echo a\" | getline\nprint }", NULL, "a\n");
    is("BEGIN { \"echo a\" | getline x; print x }", NULL, "a\n");
    is("BEGIN { while((\"echo a\" | getline) > 0) print }", NULL, "a\n");
    is("function f(a) { return a * 2 } BEGIN { print f(2) f(3), f(f(2)) }", NULL, "46 8\n");
    is("function f(a) { a = 1 } BEGIN { print f(2) \"x\" }", NULL, "x\n");
    /* lines */
    is("BEGIN {\n# a comment\n  x = 1 # another\n\n  print x\n}\n\n", NULL, "1\n");
    is("BEGIN { print 1 ; ; print 2 ; }", NULL, "1\n2\n");
    is("BEGIN { if (1) print 1; else print 2; if (0) print 3\nelse print 4 }", NULL, "1\n4\n");
    is("BEGIN { if (0) { } else { print 1 } ; if (0)\n\n  print 2\n\nelse\n\n  print 3 }", NULL, "1\n3\n");
    is("BEGIN { for (i = 0;\n i < 2;\n i++)\n print i }", NULL, "0\n1\n");
    is("BEGIN { do print 1; while (0); do { print 2 } while (0)\ndo\nprint 3\nwhile (0) }", NULL, "1\n2\n3\n");
    is("BEGIN { while (i < 2)\n{ i++ }\nprint i }", NULL, "2\n");
    is("BEGIN { print 1,\n2; x = 1 ?\n2 :\n3; print x; y = 1 &&\n0; print y }", NULL, "1 2\n2\n0\n");
    is("function f(a,\n b) { return a + b }\nBEGIN { print f(1,\n 2) }", NULL, "3\n");
    is("function f(a)\n{\n return a\n}\nBEGIN { print f(1) }", NULL, "1\n");
    is("BEGIN { print \"a\\\nb\" }", NULL, "ab\n");
    is("BEGIN { print 1 \\\n+ 2 }", NULL, "3\n");
    is("BEGIN { print 1 }\r\nEND { print 2 }\r\n", "", "1\n2\n");                           /* a file from elsewhere */
    is("BEGIN { print 1 } ; END { print 2 } ; ; ", "", "1\n2\n");
    /* strings */
    is("BEGIN { print \"a\\tb\\\\c\\\"d\\/e\\101\\60\" }", NULL, "a\tb\\c\"d/eA0\n");
    is("BEGIN { print \"\\a\\b\\f\\v\\r\" }", NULL, "\a\b\f\v\r\n");
    is("BEGIN { print \"\\.\", \"\\q\" }", NULL, "\\. \\q\n");                              /* no escape: the backslash stays */
    /* regular expressions: awk's escapes first */
    is("BEGIN { print \"a]\" ~ /[\\]]/, \"a\\\\\" ~ /[\\\\]/, \"-\" ~ /[a\\-z]/, \"b\" ~ /[a\\-z]/, \"^\" ~ /[\\^]/ }", NULL, "1 1 1 0 1\n");
    is("BEGIN { print \"\\r\" ~ /\\r/, \"\\f\" ~ /\\f/, \"\\v\" ~ /\\v/, \"\\a\" ~ /\\a/, \"/\" ~ \"\\\\/\" }", NULL, "1 1 1 1 1\n");
    is("BEGIN { print \"\\t\" ~ /[\\t]/, \"\\n\" ~ /\\n/, \"/\" ~ /\\//, \"\\\"\" ~ /\\\"/, \"A\" ~ /\\101/, \"\\b\" ~ /\\b/ }", NULL, "1 1 1 1 1 1\n");
    is("/[[:alpha:]/]/", "1/2\n3\n", "1/2\n");                                             /* a class in brackets, and a / after it */
    is("/[[.-.]/]/", "a/\n-\nb\n", "a/\n-\n");
    is("/[]/]/", "]\n/\nx\n", "]\n/\n");
    is("/[^]/]/", "]\n/\nx\n", "x\n");
    is("BEGIN { print \"a.b\" ~ /a\\.b/, \"axb\" ~ /a\\.b/, \"a\\\\b\" ~ /a\\\\b/, \"+\" ~ /\\+/, \"*\" ~ /\\052/ }", NULL, "1 0 1 1 1\n");
    is("BEGIN { r = \"[\\\\]]\"; print \"]\" ~ r; print \"x\" ~ \"[[:alpha:]]\", \"1\" ~ /[[:alpha:]]/ }", NULL, "1\n1 0\n");
    is("BEGIN { print \"ab\" ~ /^(a|b)+$/, \"a{2}\" ~ /a\\{2\\}/, \"aa\" ~ /^a{2}$/, \"{\" ~ /{/ }", NULL, "1 1 1 1\n");
    is("BEGIN { s = \"aaa\"; print gsub(\"a\", \"b\", s), s; t = \"a.b\"; gsub(\".\", \"x\", t); print t; gsub(/x/, \"\\\\\\\\&\", t); print t }", NULL, "3 bbb\nxxx\n\\x\\x\\x\n");
    is("BEGIN { s = \"a\"; sub(/a/, \"\\\\\\\\\\\\&\", s); print s; t = \"a\"; sub(/a/, \"\\\\q\", t); print t }", NULL, "\\&\n\\q\n");
    is("{ sub(/$/, \"!\"); print; sub(/^/, \">\"); print }", "a\n", "a!\n>a!\n");
    is("{ $2 = \"x\"; gsub(/x/, \"y\"); print $2, NF }", "a b c\n", "y 3\n");
    is("{ sub(/b/, \"X Y\", $2); print; print NF }", "a b c\n", "a X Y c\n3\n");
    is("{ gsub(/ /, \"\"); print NF, $1 }", "a b\n", "1 ab\n");
    is("function f(s) { sub(/a/, \"b\", s); return s } BEGIN { x = \"a\"; print f(x), x }", NULL, "b a\n");
    is("BEGIN { a[1] = \"xx\"; sub(/x/, \"y\", a[1]); print a[1] }", NULL, "yx\n");
    is("BEGIN { n = split(\"a b\", x); sub(/a/, \"c\", x[1]); print x[1], n }", NULL, "c 2\n");
    is("BEGIN { print match(\"abc\", \"$\"), RSTART, RLENGTH; print match(\"\", /x*/), RSTART, RLENGTH }", NULL, "4 4 0\n1 1 0\n");
    fails("/(/", "", "/(/:");
    fails("BEGIN { x = \"(\"; print \"a\" ~ x }", NULL, "/(/:");
    fails("BEGIN { print match(\"a\", \"[\") }", NULL, "/[/:");
    fails("BEGIN { n = split(\"a\", x, \"(a\") }", NULL, "/(a/:");
    /* functions and their names */
    is("function f(a) { a[1] = 1 } BEGIN { f(x); f(x); print length(x) }", NULL, "1\n");
    is("function f(a) { g(a); return a[1] } function g(b) { b[1] = 5 } BEGIN { print f(x), x[1] }", NULL, "5 5\n");
    is("function f(a, b) { b[1] = 2; return g(b) } function g(c) { return c[1] } BEGIN { print f(); print f() }", NULL, "2\n2\n");
    is("function f(x, l) { l[1] = x; if(x > 0) f(x - 1); return l[1] } BEGIN { print f(3) }", NULL, "3\n");
    is("function f(a) { a = 5; return a } BEGIN { print f(x), \"[\" x \"]\" }", NULL, "5 []\n");
    is("function f(a) { x = 7; return a } BEGIN { print \"[\" f(x) \"]\", x }", NULL, "[] 7\n");   /* by value, though it was unset */
    is("function f(n) { if(n == 0) return 0; return 1 + f(n - 1) } BEGIN { print f(200000) }", NULL, "200000\n");
    is("function f(k, s) { for(k in x) s = s k; return s } BEGIN { x[1]; print f() }", NULL, "1\n");
    is("function f(n) { for(k in x) if(n) return 1; return 0 } BEGIN { x[1]; x[2]; print f(1), f(0) }", NULL, "1 0\n");
    is("function f() { next } { f(); print \"no\" } END { print NR }", "a\nb\n", "2\n");
    status_is("function f() { exit 3 } BEGIN { f(); print \"no\" } END { print \"end\" }", NULL, "end\n", 3);
    fails("function f() { next } BEGIN { f() }", NULL, "next or nextfile");
    fails("function f(a) { a[1] = 1 } BEGIN { x = 1; f(x) }", NULL, "scalar used as an array");
    fails("function f(a) { return a + 1 } BEGIN { x[1]; f(x) }", NULL, "array used as a scalar");
    fails("BEGIN { x[1]; x = 1 }", NULL, "array used as a scalar");
    fails("BEGIN { x = 1; x[1] = 2 }", NULL, "scalar used as an array");
    fails("BEGIN { x[1]; print x }", NULL, "array used as a scalar");
    fails("BEGIN { x[1]; print x + 1 }", NULL, "array used as a scalar");
    /* arrays and for-in */
    is("BEGIN { for(i = 0; i < 100; i++) a[i] = i; for(k in a) { delete a[k]; n++ } print n, length(a) }", NULL, "100 0\n");
    is("BEGIN { a[1]; for(k in a) { a[2]; n++ } print n, length(a) }", NULL, "1 2\n");      /* the keys when it began */
    is("BEGIN { for(i = 0; i < 100000; i++) a[i] = i; for(i = 0; i < 100000; i++) s += a[i]; print s, length(a) }", NULL, "4999950000 100000\n");
    is("BEGIN { a[\"\"] = 1; print (\"\" in a), a[x] }", NULL, "1 1\n");
    is("BEGIN { a[1, 2]; for(k in a) { n = split(k, p, SUBSEP); print n, p[1], p[2] } }", NULL, "2 1 2\n");
    is("BEGIN { x[1] = 5; split(x[1], x); print x[1] }", NULL, "5\n");                      /* the text is taken before the array goes */
    is("BEGIN { delete x; delete x[1]; print length(x) }", NULL, "0\n");
    is("BEGIN { for(k in x) print k; print length(x) }", NULL, "0\n");
    is("BEGIN { for(NF in x) print 1; x[3]; for(NF in x) print NF }", NULL, "3\n");
    /* patterns */
    is("BEGIN { print 1 } BEGIN { print 2 } END { print 3 } END { print 4 }", "", "1\n2\n3\n4\n");
    is("NR == 1 { print \"a\" } NR == 1 { print \"b\" }", "x\n", "a\nb\n");
    is("/a/ { print 1 } /b/", "ab\n", "1\nab\n");
    is("END { print $0, NF, NR }", "a b\n", "a b 2 1\n");
    is("NR==1,NR==2 { print \"r\", $0 } NR==2,NR==3 { print \"s\", $0 }", "1\n2\n3\n", "r 1\nr 2\ns 2\ns 3\n");
    is("/x/,/y/", "a\nx\nb\n", "x\nb\n");                                                   /* it stays open to the end */
    is("/x/,0 { n++ } END { print n }", "a\nx\nb\n", "2\n");
    is("NR > 1, 0", "a\nb\n", "b\n");
    is("x = NR % 2", "a\nb\nc\n", "a\nc\n");
    /* BEGIN alone reads nothing, and exit */
    is("BEGIN { print \"x\" }", "ignored\n", "x\n");
    status_is("BEGIN { exit 3 }", NULL, "", 3);
    status_is("BEGIN { exit 1 } END { exit }", NULL, "", 1);
    status_is("{ exit 2 + 3 }", "a\n", "", 5);
    status_is("END { exit 7; print \"no\" }", "", "", 7);
    status_is("BEGIN { exit 3 } END { print \"e\"; exit 4 }", NULL, "e\n", 4);
    status_is("BEGIN { exit \"6x\" }", NULL, "", 6);
    status_is("BEGIN { exit 256 + 9 }", NULL, "", 9);
    status_is("BEGIN { x = 1e300 * 1e300; exit x - x }", NULL, "", 0);
    status_is("BEGIN { exit 1e300 * 1e300 }", NULL, "", 0);
    is("BEGIN { exit } END { print \"end\" }", NULL, "end\n");
    /* rand */
    is("BEGIN { a = rand(); b = rand(); print (a != b), (a >= 0 && a < 1); print srand(5), srand(5); x = rand(); srand(5); print (x == rand()) }", NULL, "1 1\n0 5\n1\n");
    /* mistakes, with their lines */
    fails("BEGIN { print 1", NULL, "unexpected end of the program");
    fails("BEGIN {\n\n  x = )\n}", NULL, "line 3: syntax error near ')'");
    fails("{ print \"abc }", "", "a string without its closing");
    fails("/abc", "", "a regular expression without its closing");
    fails("BEGIN { x = 1 @ 2 }", NULL, "unexpected character '@'");
    fails("BEGIN { x = 1 \\ 2 }", NULL, "unexpected character '\\'");
    fails("BEGIN", NULL, "BEGIN and END need an action");
    fails("BEGIN\n{ }", NULL, "BEGIN and END need an action");
    fails("END", NULL, "BEGIN and END need an action");
    fails("BEGIN { next }", NULL, "next in BEGIN or END");
    fails("END { nextfile }", NULL, "nextfile in BEGIN or END");
    fails("BEGIN { return }", NULL, "return outside a function");
    fails("BEGIN { break }", NULL, "break outside a loop");
    fails("BEGIN { continue }", NULL, "continue outside a loop");
    fails("BEGIN { f(1) }", NULL, "the function f is not defined");
    fails("function f(a) { } BEGIN { f(1, 2) }", NULL, "the function f takes 1 arguments");
    fails("function f() { } function f() { }", NULL, "a function defined twice");
    fails("function f(a, a) { }", NULL, "a name twice");
    fails("function f(f) { }", NULL, "the function's name");
    fails("function f() { } BEGIN { f = 1 }", NULL, "both a function and a variable");
    fails("function g(if) { }", NULL, "syntax error near 'if'");
    fails("BEGIN { if = 1 }", NULL, "syntax error near '='");
    fails("BEGIN { length = 1 }", NULL, "syntax error near '='");
    fails("BEGIN { 1 = 2 }", NULL, "syntax error near '='");
    fails("BEGIN { x++ = 2 }", NULL, "syntax error near '='");
    fails("BEGIN { ++1 }", NULL, "unexpected number");
    fails("BEGIN { substr(\"a\") }", NULL, "substr takes 2 to 3 arguments, not 1");
    fails("BEGIN { sin() }", NULL, "sin takes 1 to 1 arguments, not 0");
    fails("BEGIN { sub(/a/, \"b\", 1) }", NULL, "must be a variable");
    fails("BEGIN { split(\"a\", 1) }", NULL, "unexpected number");
    fails("BEGIN { printf }", NULL, "printf without a format");
    fails("BEGIN { print (1, 2) + 1 }", NULL, "syntax error near '+'");
    fails("BEGIN { x = (1, 2) }", NULL, "a list where a value should be");
    fails("BEGIN { getline < }", NULL, "syntax error near '}'");
    fails("BEGIN { 1 in 2 }", NULL, "unexpected number");
    fails("BEGIN { for(i in 5) x }", NULL, "unexpected number");
    fails("BEGIN { delete 1 }", NULL, "unexpected number");
    fails("BEGIN { do x }", NULL, "syntax error near '}'");
    fails("BEGIN { do x; while }", NULL, "syntax error near '}'");
    fails("{ print } }", "", "syntax error near '}'");
}

/**** the corpus, against the system's awk ****/

static unsigned long long prng = 88172645463325252ull;

static unsigned next_rand(void) {
    prng ^= prng << 13;
    prng ^= prng >> 7;
    prng ^= prng << 17;
    return (unsigned)(prng >> 32);
}

static unsigned below(unsigned n) {
    return next_rand() % n;
}

/* what the programs read: words, whole numbers and decimals in columns, and lines of other shapes */
static char *make_corpus(size_t *n) {
    static const char *const words[] = { "alpha", "beta", "gamma", "delta", "epsilon", "x", "yy", "the", "quick", "brown", "fox" };
    size_t cap = 200000, len = 0;
    char *s = malloc(cap);
    int i;
    for(i = 0; i < 1500; i++) {
        int k, cols = 1 + (int)below(7);
        if(len + 400 > cap) break;
        if(!below(50)) {
            s[len++] = '\n';
            continue;
        }
        for(k = 0; k < cols; k++) {
            switch(below(4)) {
            case 0: len += (size_t)snprintf(s + len, cap - len, "%s", words[below(11)]); break;
            case 1: len += (size_t)snprintf(s + len, cap - len, "%u", below(100000)); break;
            case 2: {                   /* one call each: the order of a call's arguments is the compiler's */
                unsigned whole = below(1000), part = below(100);
                len += (size_t)snprintf(s + len, cap - len, "%u.%02u", whole, part);
                break;
            }
            default: len += (size_t)snprintf(s + len, cap - len, "-%u", below(100)); break;
            }
            s[len++] = k + 1 == cols ? '\n' : " \t,:"[below(4) == 0 ? below(4) : 0];
        }
    }
    s[len] = 0;
    *n = len;
    return s;
}

/* Programs on which the awks agree. Not here: what POSIX leaves open, the order of for-in (sorted through sort), character
   counts, a number past 2^31 as an integer (mawk prints 2.14748e+09), %c of an empty string (mawk prints a NUL),
   "a" b | getline without brackets (mawk reads it as "a" (b | getline)), and what the account's table says differs. */
static const char *const corpus_programs[] = {
    "{ print }", "{ print NF }", "{ print $1 }", "{ print $NF }", "{ print NR, NF, $0 }", "NF", "!NF { n++ } END { print n + 0 }",
    "NR % 7 == 0", "END { print NR }", "{ n += NF } END { print n, n / NR }", "{ $1 = $1; print }", "{ $2 = \"X\"; print }",
    "NF > 2 { print $2, $1 }", "{ print length($0), length($1) }", "{ print toupper($1), tolower($NF) }",
    "{ s = 0; for(i = 1; i <= NF; i++) s += $i; print s }", "{ for(i = NF; i > 0; i--) printf \"%s%s\", $i, (i > 1 ? OFS : ORS) }",
    "BEGIN { FS = \",\" } { print NF, $1 }", "BEGIN { FS = \":\"; OFS = \"|\" } { $1 = $1 } 1", "BEGIN { FS = \"\\t\" } { print $2 }",
    "BEGIN { FS = \"[ ,:]+\" } { print NF, $NF }", "BEGIN { FS = \"a\" } { print NF }", "BEGIN { OFS = \"-\" } { print $1, $2 }",
    "BEGIN { ORS = \"|\" } NR < 50", "BEGIN { RS = \"\" } { print NR, NF }", "BEGIN { RS = \"\"; FS = \"\\n\" } { print $1 }",
    "BEGIN { RS = \"a\" } { print length($0) }", "BEGIN { RS = \" \" } NR < 200",
    "/a/", "!/a/", "/^[a-z]+/", "/[0-9]$/", "/ab|th/", "/(a|e)l/", "/^$/", "$1 ~ /^[0-9]+$/", "$1 !~ /a/", "$2 ~ \"e\"",
    "$1 > 500", "$1 == 7", "NF > 1 && $1 < $2", "$1 == \"fox\"", "/alpha/,/gamma/", "NR == 10, NR == 20", "/x/,/fox/ { n++ } END { print n + 0 }",
    "{ gsub(/a/, \"<&>\"); print }", "{ n = gsub(/[aeiou]/, \"\"); print n, $0 }", "{ sub(/[0-9]+/, \"#\"); print }",
    "{ gsub(/[0-9]+/, \"[&]\", $1); print }", "{ print gsub(/x*/, \"-\"), $0 }", "{ gsub(/ +/, \" \"); gsub(/^ | $/, \"\"); print }",
    "{ if(match($0, /[0-9]+/)) print RSTART, RLENGTH, substr($0, RSTART, RLENGTH) }", "{ print index($0, \"a\"), index($0, \"th\") }",
    "{ print substr($0, 3), substr($0, 2, 3), substr($1, 1, 1) }", "{ n = split($0, a); print n, a[1], a[n] }", "{ n = split($0, a, /[ ,]/); print n }",
    "{ n = split($0, a, \"a\"); print n, a[1] }", "{ n = split($1, a, \"\"); print n, a[1] }",
    "{ printf \"%-10s|%5s|\\n\", $1, $2 }", "{ printf \"%5.2f %d %x\\n\", $1, $2, length($0) }", "NF { printf \"%c%c\\n\", $1 \"\", 65 + NR % 26 }",
    "{ printf \"%e %g\\n\", $1, $2 }", "{ printf \"%05d|%-5d|%+d\\n\", $1, $1, $1 }", "{ x = sprintf(\"%s:%d\", $1, NR); print x, length(x) }",
    "{ c[$1]++ } END { for(k in c) print k, c[k] | \"sort\" }", "{ for(i = 1; i <= NF; i++) w[$i]++ } END { for(k in w) if(w[k] > 3) print k, w[k] | \"sort\" }",
    "{ a[NR] = $0 } END { for(i = NR; i > NR - 40; i--) print a[i] }", "$1 in seen { n++ } { seen[$1] } END { print n, length(seen) }",
    "{ k = $1 SUBSEP NF; c[k]++ } END { for(x in c) { split(x, p, SUBSEP); print p[1], p[2], c[x] | \"sort\" } }",
    "{ c[$1, NF]++ } END { n = 0; for(x in c) n++; print n }", "!seen[$1]++", "{ m = $1 > m ? $1 : m } END { print m }",
    "function f(n) { return n < 2 ? 1 : n * f(n - 1) } NR < 13 { print f(NR) }", "function g(s, i, r) { for(i = length(s); i > 0; i--) r = r substr(s, i, 1); return r } { print g($1) }",
    "function h(a, n, i, j, t) { for(i = 2; i <= n; i++) { t = a[i]; for(j = i - 1; j > 0 && a[j] > t; j--) a[j + 1] = a[j]; a[j + 1] = t } } NF && n < 200 { x[++n] = $1 } END { h(x, n); for(i = 1; i <= n; i++) print x[i] }",
    "{ print ($1 < 100 ? \"small\" : \"large\") }", "{ print $1 * 2, $1 + $2, $1 - 1, $1 % 7, $1 / 4 }", "{ print int($1 / 3), -$1, !$1 }", "{ print sqrt($1 * $1) }",
    "NR % 2 { next } { print }", "NR == 5 { exit } { print }", "{ print | \"cat\" } END { close(\"cat\"); print \"done\" }",
    "n++ < 3 { (\"echo \" n) | getline x; print x }", "NR == 1 { while((getline l) > 0) n++; print n }", "{ getline x; print x }",
    "BEGIN { while((\"echo a; echo b\" | getline l) > 0) print l }", "BEGIN { printf \"%d %s %5.1f\\n\", 42, \"str\", 3.14159 }",
    "BEGIN { for(i = 1; i <= 12; i++) print i, 2 ^ i, i * i, 1 / i }", "BEGIN { x[1]; delete x[1]; print length(x) }",
    "BEGIN { print length(\"abc\"), substr(\"hello\", 2, 3), index(\"hello\", \"l\"), toupper(\"x\") }",
    "BEGIN { print 1e3, 1.5e3, 0.1 + 0.2, 100 / 3, 2 ^ 30, 1234567 }", "BEGIN { OFMT = \"%.3f\"; print 3.14159, 2.5; CONVFMT = \"%.2f\"; x = 3.14159 \"\"; print x }",
};

static void against_system_awk(void) {
    size_t n, i;
    char *data = make_corpus(&n), cmd[900];
    const char *data_path = join("corpus.txt"), *prog_path = join("prog.awk"), *ref_path = join("ref.out");
    unsigned same = 0;
    if(system("command -v awk > /dev/null 2>&1")) {
        printf("test_awk: no awk here: the comparison is skipped\n");
        free(data);
        return;
    }
    write_file(data_path, data, n);
    for(i = 0; i < sizeof(corpus_programs) / sizeof(corpus_programs[0]); i++) {
        const char *prog = corpus_programs[i];
        char *argv[4], *ref;
        size_t nref;
        write_file(prog_path, prog, strlen(prog));
        snprintf(cmd, sizeof(cmd), "awk -f '%s' '%s' < /dev/null > '%s' 2> /dev/null", prog_path, data_path, ref_path);
        system(cmd);
        ref = read_file(ref_path, &nref);
        argv[0] = "awk";
        argv[1] = (char *)prog;
        argv[2] = (char *)data_path;
        argv[3] = NULL;
        run_args(3, argv, NULL, 0, 60000, 0, 1);
        checks++;
        if(R.status || R.nout != nref || memcmp(R.out, ref, nref)) {
            size_t k = 0, line = 1, start = 0;
            while(k < R.nout && k < nref && R.out[k] == ref[k]) {
                if(ref[k] == '\n') {
                    line++;
                    start = k + 1;
                }
                k++;
            }
            failed++;
            printf("FAIL: differs from the system's awk at line %lu:\n  program %.200s\n  mine  %.120s\n  awk   %.120s\n  (status %d: %.200s)\n",
                   (unsigned long)line, prog, R.out + start, ref + start, R.status, R.err);
        } else {
            same++;
        }
        free(ref);
    }
    printf("test_awk: %u programs print the same as the system's awk, on %lu bytes\n", same, (unsigned long)n);
    free(data);
}

/**** memory ****/

static void limited(const char *prog, const char *input, size_t limit, int want_fail) {
    char *argv[3];
    argv[0] = "awk";
    argv[1] = (char *)prog;
    argv[2] = NULL;
    run_args(2, argv, input, input ? strlen(input) : 0, 60000, limit, 1);
    checks++;
    if(want_fail ? R.status != 2 || !strstr(R.err, "out of memory") : R.status != 0) {
        failed++;
        printf("FAIL: under %lu bytes: %.200s\n  status %d signal %d [%.200s]\n", (unsigned long)limit, prog, R.status, R.sig, R.err);
    }
}

static void memory(void) {
    char *many = malloc(40001);
    size_t i;
    /* the limit, and a clear message: a string, an array, the calls, the input */
    limited("BEGIN { s = \"x\"; while(1) s = s s }", NULL, 1u << 20, 1);
    limited("BEGIN { while(1) a[i++] = i }", NULL, 1u << 20, 1);
    limited("function f(n) { return f(n + 1) + 1 } BEGIN { f(0) }", NULL, 1u << 20, 1);
    limited("function f(n, l) { l[n] = n; return f(n + 1) } BEGIN { f(0) }", NULL, 1u << 20, 1);
    limited("BEGIN { $1000000 = 1 }", NULL, 1u << 20, 1);
    limited("BEGIN { printf \"%90000000s\", \"\" }", NULL, 1u << 20, 1);
    limited("BEGIN { while(1) s = s sprintf(\"%1000d\", 1) }", NULL, 1u << 20, 1);
    {
        char *big = malloc(2000001);
        memset(big, 'x', 2000000);
        big[2000000] = 0;
        limited("{ n++ }", big, 1u << 20, 1);           /* one record larger than the memory */
        free(big);
    }
    /* Loops under a small limit: a leak would reach it. */
    for(i = 0; i < 40000; i += 2) {
        many[i] = 'a';
        many[i + 1] = '\n';
    }
    many[40000] = 0;
    limited("{ $3 = \"x\"; n += NF; s = $0; sub(/a/, \"b\", s); gsub(/x/, \"y\"); x[1] = s; split(s, p); t = t + length(s) } END { print n }", many, 1u << 18, 0);
    limited("BEGIN { for(i = 0; i < 200000; i++) { s = \"a\" i; a[i % 10] = s; b = s s; n += length(b) } print n }", NULL, 1u << 16, 0);
    limited("function f(a, b) { b[1] = a; return b[1] + 1 } BEGIN { for(i = 0; i < 100000; i++) n = f(n) ; print n }", NULL, 1u << 16, 0);
    limited("BEGIN { for(i = 0; i < 2000; i++) if((\"x\" i) ~ (\"y\" i \"+\")) n++; print n + 0 }", NULL, 1u << 22, 0);       /* more patterns than are kept */
    limited("BEGIN { for(i = 0; i < 50000; i++) { a[i] = 1; delete a[i]; for(k in a) n++ } print n + 0 }", NULL, 1u << 16, 0);
    limited("BEGIN { for(i = 0; i < 300; i++) { c = \"echo \" i; c | getline x; close(c); n += x } print n }", NULL, 1u << 20, 0);
    free(many);
}

/**** cut, damaged and oversized ****/

static const char *const seeds[] = {
    "BEGIN { FS = \":\"; OFS = \"-\"; x[1, 2] = 3 }\n/a+b/ { n++; next }\n$1 ~ \"b\", NF > 2 { print $1, $NF | \"cat\" }\nEND { for(k in x) print k, x[k] > \"/dev/null\"; printf \"%5.2f %s\\n\", n / 3, substr(\"hello\", 2, 3) }\n",
    "function f(a, b, c) { if(a <= 0) return b; c[a] = a; return f(a - 1, b + 1) }\nBEGIN { print f(5, 0); s = \"aXbX\"; gsub(/X/, \"[&]\", s); print s; while((\"echo hi\" | getline l) > 0) print l; print match(s, /b+/), RSTART, RLENGTH }\n",
    "{ for(i = NF; i > 0; i--) { if(i == 2) continue; do { t = t $i; j++ } while(j < 3); if(length(t) > 9) break } $2 = t; print; t = \"\"; j = 0 }\nEND { print NR, (1 in a) ? \"y\" : \"n\", -2 ^ 2, !x, 7 % 3; delete a; exit 2 }\n",
    "BEGIN { RS = \"\"; split(\"a b c\", p); getline l < \"/nonexistent\"; print length(p), toupper(p[1]), int(3.9), sqrt(16), sin(0); printf(\"%c%d%x%s\\n\", 65, 2, 255, \"s\") }\nNR == 1, /z/ { sub(/^/, \">\"); print > \"/dev/stderr\" }\n",
};

static void crash_check(const char *what, const char *text, size_t n) {
    checks++;
    if(R.sig && R.sig != SIGALRM) {
        failed++;
        printf("FAIL: signal %d on %s:\n%.*s\n", R.sig, what, (int)n, text);
    }
}

static const char corpus_input[] = "a b c\nab:1:2\n\n1 2 3 4 5\nthe end z\n";

static void run_damaged(const char *prog, size_t n, const char *input, size_t ninput) {
    char *t = malloc(n + 1), *argv[3];
    size_t i;
    /* a program is one argument, so it has no NUL */
    for(i = 0; i < n; i++) t[i] = prog[i] ? prog[i] : ' ';
    t[n] = 0;
    argv[0] = "awk";
    argv[1] = t;
    argv[2] = NULL;
    run_args(2, argv, input, ninput, 400, 1u << 20, 0);
    crash_check("this program", t, n);
    free(t);
}

static void damaged(int rounds) {
    static const char *const tokens[] = { "(", ")", "[", "]", "{", "}", ";", ",", "\n", "\"", "\\\n", "/", "#", "++", "--", "=", "^", "%", "==", "&&", "||", "!", ".", "9", "$", "$0",
                                          "x", "a[1]", "in", "getline", "next", "exit", "return", "function", "if", "else", "for", "while", "do", "print", "printf", "delete",
                                          "BEGIN", "END", "NF", "NR", "length", "substr(", "split(", "sub(", "gsub(", "match(", "sprintf(", "\\", "99999", "-1", ">", ">>", "|", "<", "?", ":", "~", "\377", "\001" };
    char t[2048];
    unsigned s, i;
    int r;
    /* cut: any end of the program is taken */
    for(s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
        size_t n = strlen(seeds[s]), k;
        for(k = 0; k < n; k += rounds > 1000 ? 1 : 13) run_damaged(seeds[s], k, corpus_input, sizeof(corpus_input) - 1);
    }
    /* tokens changed */
    for(r = 0; r < rounds; r++) {
        const char *seed = seeds[below(sizeof(seeds) / sizeof(seeds[0]))];
        size_t n = strlen(seed);
        int m;
        memcpy(t, seed, n);
        for(m = 1 + (int)below(4); m > 0 && n; m--) {
            size_t at = below((unsigned)n), len = 1 + below(3);
            const char *put = tokens[below(sizeof(tokens) / sizeof(tokens[0]))];
            size_t pl = strlen(put);
            if(at + len > n) len = n - at;
            if(n - len + pl >= sizeof(t)) continue;
            memmove(t + at + pl, t + at + len, n - at - len);
            memcpy(t + at, put, pl);
            n = n - len + pl;
        }
        run_damaged(t, n, corpus_input, sizeof(corpus_input) - 1);
    }
    /* any bytes as the input of a program that does much with them */
    for(r = 0; r < rounds / 4; r++) {
        size_t n = below(600);
        for(i = 0; i < n; i++) t[i] = below(3) ? "ab \n\t:1.\303\251\377\0"[below(13)] : (char)next_rand();
        run_damaged(seeds[r % 4], strlen(seeds[r % 4]), t, n);
    }
}

static char *repeated(const char *a, unsigned n, const char *mid, const char *b, unsigned m, const char *end) {
    size_t la = strlen(a), lb = strlen(b), lm = strlen(mid);
    char *s = malloc(la * n + lm + lb * m + strlen(end) + 1), *p = s;
    unsigned i;
    for(i = 0; i < n; i++, p += la) memcpy(p, a, la);
    memcpy(p, mid, lm);
    p += lm;
    for(i = 0; i < m; i++, p += lb) memcpy(p, b, lb);
    strcpy(p, end);
    return s;
}

static void too_deep(const char *before, const char *open, const char *mid, const char *close, const char *end) {
    char *t = repeated(open, 100000, mid, close, 100000, end), *f = strdup(join("deep.awk"));
    char *s = malloc(strlen(before) + strlen(t) + 1);
    strcpy(s, before);
    strcat(s, t);
    free(t);
    write_file(f, s, strlen(s));
    args_are(NULL, "", 2, "-f", f, NULL, NULL, NULL, NULL);
    checks++;
    if(!strstr(R.err, "nested too deeply")) {
        failed++;
        printf("FAIL: %s...: [%.200s] status %d signal %d\n", open, R.err, R.status, R.sig);
    }
    free(s);
    free(f);
}

static void from_file(const char *text, const char *input, const char *want) {
    char *f = strdup(join("big.awk"));
    write_file(f, text, strlen(text));
    args_are(input, want, 0, "-f", f, NULL, NULL, NULL, NULL);
    free(f);
}

static void oversized(void) {
    char *s;
    too_deep("BEGIN { ", "x = ", "1", "", " }");
    too_deep("BEGIN { x = ", "(", "1", ")", " }");
    too_deep("BEGIN { ", "{ ", "", " }", " }");
    too_deep("BEGIN { ", "if(1) ", "x", "", " }");
    too_deep("BEGIN { ", "while(1) ", "x", "", " }");
    too_deep("BEGIN { ", "for(;;) ", "x", "", " }");
    too_deep("BEGIN { ", "do ", "x", " while(0)", " }");
    too_deep("BEGIN { x = ", "- ", "1", "", " }");
    too_deep("BEGIN { x = ", "! ", "1", "", " }");
    too_deep("BEGIN { x = ", "2 ^ ", "1", "", " }");
    too_deep("BEGIN { x = ", "1 ? ", "1", " : 2", " }");
    too_deep("BEGIN { x = ", "a[", "1", "]", " }");
    too_deep("function f(a) { } BEGIN { ", "f(", "1", ")", " }");
    too_deep("BEGIN { x = ", "length(", "1", ")", " }");
    too_deep("BEGIN { x = ", "$", "1", "", " }");
    /* and the most that is sensible runs */
    s = repeated("(", 300, "7", ")", 300, "");
    {
        size_t room = strlen(s) + 20;
        char *p = malloc(room);
        snprintf(p, room, "BEGIN { print %s }", s);
        is(p, NULL, "7\n");
        free(p);
    }
    free(s);
    /* lists are no nesting */
    s = repeated("BEGIN { x = 0", 1, "", " + 1", 100000, "; print x }");
    from_file(s, NULL, "100000\n");
    free(s);
    s = repeated("BEGIN { x = \"\"", 1, "", " \"a\"", 50000, "; print length(x) }");
    from_file(s, NULL, "50000\n");
    free(s);
    s = repeated("BEGIN { print 1", 1, "", ";x=1", 100000, " }");
    from_file(s, NULL, "1\n");
    free(s);
    /* a long name, string, regular expression, comment */
    s = repeated("BEGIN { ", 1, "", "ab", 500000, " = 4; print length(\"x\") }");
    from_file(s, NULL, "1\n");
    free(s);
    s = repeated("BEGIN { print length(\"", 1, "", "ab", 500000, "\") }");
    from_file(s, NULL, "1000000\n");
    free(s);
    s = repeated("BEGIN { print \"", 1, "", "ab", 2000, "\" ~ /^(ab)+$/ }");
    from_file(s, NULL, "1\n");
    free(s);
    s = repeated("# ", 1, "", "ab", 500000, "\nBEGIN { print 5 }");
    from_file(s, NULL, "5\n");
    free(s);
    /* many functions, rules, parameters */
    {
        size_t cap = 3000000, n = 0;
        int i;
        s = malloc(cap);
        for(i = 0; i < 3000; i++) n += (size_t)snprintf(s + n, cap - n, "function f%d(a) { return a + %d }\n", i, i);
        n += (size_t)snprintf(s + n, cap - n, "BEGIN { print f2999(1) }\n");
        from_file(s, NULL, "3000\n");
        n = 0;
        for(i = 0; i < 3000; i++) n += (size_t)snprintf(s + n, cap - n, "NR == %d { n++ }\n", i);
        n += (size_t)snprintf(s + n, cap - n, "END { print n }\n");
        from_file(s, "x\n", "1\n");
        n = (size_t)snprintf(s, cap, "function f(");
        for(i = 0; i < 1000; i++) n += (size_t)snprintf(s + n, cap - n, "%sp%d", i ? "," : "", i);
        n += (size_t)snprintf(s + n, cap - n, ") { return p999 } BEGIN { print f(");
        for(i = 0; i < 1000; i++) n += (size_t)snprintf(s + n, cap - n, "%s%d", i ? "," : "", i);
        snprintf(s + n, cap - n, ") }\n");
        from_file(s, NULL, "999\n");
        free(s);
    }
    /* one long line through the string functions */
    {
        char *big = malloc(1000002);
        memset(big, 'a', 1000000);
        big[1000000] = '\n';
        big[1000001] = 0;
        is("{ print length($0), length(substr($0, 500000)), index($0, \"b\"), gsub(/a/, \"bb\"), length($0) }", big, "1000000 500001 0 1000000 2000000\n");
        free(big);
    }
}

int main(int argc, char **argv) {
    int rounds = argc > 1 && !strcmp(argv[1], "long") ? 20000 : 300;
    const char *tmp = getenv("TMPDIR");
    size_t i;
    snprintf(dir, sizeof(dir), "%s/test_awk_XXXXXX", tmp ? tmp : "/tmp");
    if(!mkdtemp(dir)) {
        perror(dir);
        return 1;
    }
    snprintf(in_path, sizeof(in_path), "%s/stdin", dir);
    snprintf(out_path, sizeof(out_path), "%s/stdout", dir);
    snprintf(err_path, sizeof(err_path), "%s/stderr", dir);
    for(i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        run_prog(table[i].prog, table[i].input);
        checks++;
        if(strcmp(R.out, table[i].out) || R.status != table[i].status) show(table[i].prog, table[i].out);
    }
    characters();
    binary_data();
    records();
    files_and_commands();
    writes_that_fail();
    command_line();
    language();
    against_system_awk();
    memory();
    damaged(rounds);
    oversized();
    {
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        system(cmd);
    }
    printf("test_awk: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
