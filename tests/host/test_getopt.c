/* Host-side tests of user/libsurf/getopt.c (11/01/26, the command-line suite): the same calls through the library's getopt() and
   the host's, compared at each step, with the host told to keep to POSIX (it stops at the first word that is no
   option), and the library's messages, which are SurfOS's own. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

int surf_getopt(int argc, char *const argv[], const char *optstring);
extern char *surf_optarg;
extern int surf_optind, surf_opterr, surf_optopt;

/* getopt.c's messages come here */
static char message[256];
int surf_dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    size_t n = strlen(message);
    (void)fd;
    va_start(ap, fmt);
    vsnprintf(message + n, sizeof(message) - n, fmt, ap);
    va_end(ap);
    return 0;
}

void *surf_malloc(size_t n) { return malloc(n); }

static int failures, checks;

static void compare(const char *opts, int argc, char *const argv[]) {
    int step;
    optind = 1;
#ifdef __GLIBC__
    optind = 0;                                 /* glibc's way of starting over */
#else
    extern int optreset;
    optreset = 1;
#endif
    surf_optind = 1;
    opterr = 0;
    surf_opterr = 0;
    for(step = 0; step < 40; step++) {
        int h = getopt(argc, argv, opts), s = surf_getopt(argc, argv, opts);
        const char *ha = optarg, *sa = surf_optarg;
        checks++;
        if(h != s || optind != surf_optind || (h == -1 ? 0 : (ha ? 1 : 0) != (sa ? 1 : 0)) ||
           (h != -1 && ha && ha != sa) || ((h == '?' || h == ':') && optopt != surf_optopt)) {
            failures++;
            printf("FAIL: \"%s\" step %d with %s...: host %d optind %d optarg %s optopt %d; here %d optind %d optarg %s optopt %d\n",
                   opts, step, argc > 1 && argv[1] ? argv[1] : "", h, optind, ha ? ha : "-", optopt, s, surf_optind, sa ? sa : "-",
                   surf_optopt);
            return;
        }
        if(h == -1) return;
    }
}

#define CASE(opts, ...) do { \
        char *argv[] = { "prog", __VA_ARGS__, NULL }; \
        compare(opts, (int)(sizeof(argv) / sizeof(argv[0])) - 1, argv); \
    } while(0)

static void words(void) {
    CASE("ab", "-a");
    CASE("ab", "-a", "-b");
    CASE("ab", "-ab");
    CASE("ab", "-ba", "file");
    CASE("ab", "file", "-a");                   /* stops at the first word that is no option */
    CASE("ab", "-a", "file", "-b");
    CASE("ab", "-", "-a");                      /* - alone is a file */
    CASE("ab", "-a", "-", "-b");
    CASE("ab", "--", "-a");
    CASE("ab", "-a", "--", "-b");
    CASE("ab", "--", "--");
    CASE("ab", "-x");
    CASE("ab", "-ax", "-b");
    CASE("ab", "-xa");
    CASE("ab", "-x", "-a");
    CASE("a:b", "-a", "value");
    CASE("a:b", "-avalue");
    CASE("a:b", "-bavalue");
    CASE("a:b", "-ab");                         /* b is the argument */
    CASE("a:b", "-a", "-b");                    /* so is -b */
    CASE("a:b", "-a", "--");
    CASE("a:b", "-a");
    CASE("a:b", "-ba");
    CASE(":a:b", "-a");
    CASE(":a:b", "-x");
    CASE(":a:b", "-b", "-a");
    CASE("a:b:c", "-a", "1", "-b", "2", "-c", "rest");
    CASE("a:b:c", "-a1", "-b2", "-c");
    CASE("abc", "-abc", "-cba", "x");
    CASE("a", "-:");                            /* : is no option letter */
    CASE("a:", "-:");
    CASE("", "-a");
    CASE("", "x");
}

static void more(void) {
    char *argv[] = { "/initrd/bin/prog", "-x", "-a", NULL };
    char *argv2[] = { "prog", "-ab", "-c", NULL };
    char *argv3[] = { "prog", "-b", "x", NULL };
    /* tool: what: why, the tool without its path */
    surf_optind = 1;
    surf_opterr = 1;
    message[0] = 0;
    checks++;
    if(surf_getopt(3, argv, "a:") != '?' || strcmp(message, "prog: -x: unknown option\n")) {
        failures++;
        printf("FAIL: message: [%s]\n", message);
    }
    message[0] = 0;
    checks++;
    if(surf_getopt(3, argv, "a:") != '?' || strcmp(message, "prog: -a: needs an argument\n")) {
        failures++;
        printf("FAIL: message: [%s]\n", message);
    }
    /* quiet when asked, either way */
    surf_optind = 1;
    surf_opterr = 0;
    message[0] = 0;
    surf_getopt(3, argv, "a:");
    checks++;
    if(message[0]) {
        failures++;
        printf("FAIL: opterr 0 said [%s]\n", message);
    }
    surf_optind = 1;
    surf_opterr = 1;
    message[0] = 0;
    surf_getopt(3, argv, ":a:");
    checks++;
    if(message[0]) {
        failures++;
        printf("FAIL: a leading : said [%s]\n", message);
    }
    /* a new scan from the middle of a word of the last one; 0 starts over as glibc's does */
    surf_optind = 1;
    surf_opterr = 0;
    surf_getopt(3, argv2, "abc");
    surf_optind = 1;
    checks++;
    if(surf_getopt(3, argv3, "ab") != 'b') {
        failures++;
        printf("FAIL: starting over\n");
    }
    surf_optind = 0;
    checks++;
    if(surf_getopt(3, argv3, "ab") != 'b' || surf_optind != 2) {
        failures++;
        printf("FAIL: starting over from 0\n");
    }
    /* + at the front is skipped, not an option */
    surf_optind = 1;
    checks++;
    if(surf_getopt(3, argv3, "+b") != 'b') {
        failures++;
        printf("FAIL: a leading +\n");
    }
}

int main(void) {
    setenv("POSIXLY_CORRECT", "1", 1);
    words();
    more();
    printf("test_getopt: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
