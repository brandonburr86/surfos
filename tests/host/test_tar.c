/* Host-side test of tar (11/01/26): user/tar/ as it is, on the simulated FAT of tests/host/ar_sim.c. Covered: the rules for
   names from archives (arpath.c), each one alone; c, x and t and their options; every form of header tarfile.c reads,
   from archives put together here byte by byte, so that no fixture depends on the code under test; what must be refused
   and what must be skipped, with the entries after it still found; every cut of an archive, mutated archives, lengths
   that lie; what happens to the disk when a change fails at each step, when the disk is pulled and when it fills; and the
   host's tar and Python's tarfile both ways, where they are installed (each comparison says so when it is skipped). Run
   under AddressSanitizer and UBSan by tests/archive-sanitize.sh on the Mac: never in `make unittest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ar_sim.h"
#include "ar_host.h"

int tar_main(int argc, char **argv);
const char *ar_safe_path(const char *name, char *out, size_t size);
void ar_show_name(const char *name, char *out, size_t size);
int ar_glob(const char *pattern, const char *s);
int ar_glob_part(const char *pattern, const char *s);
int ar_under(const char *member, const char *path);
int ar_utf8_valid(const char *s, size_t n);
extern unsigned int ar_crc32(unsigned int, const void *, size_t);

static int has(const char *text, const char *part) {
    return strstr(text, part) != NULL;
}

/**** an archive by hand ****/

#define MAX_ARCHIVE (8 << 20)
static unsigned char *A;
static size_t An;

static void a_start(void) {
    if(!A) A = malloc(MAX_ARCHIVE);
    An = 0;
}

static void a_raw(const void *p, size_t n) {
    if(An + n > MAX_ARCHIVE) abort();
    memcpy(A + An, p, n);
    An += n;
}

enum { F_USTAR, F_GNU, F_V7 };

static void set_sum(unsigned char *b) {
    unsigned sum = 0;
    int i;
    memset(b + 148, ' ', 8);
    for(i = 0; i < 512; i++) sum += b[i];
    snprintf((char *)b + 148, 8, "%06o", sum);
    b[155] = ' ';
}

/* a header block of format f */
static void header(unsigned char *b, const char *name, int type, unsigned long long size, int f) {
    memset(b, 0, 512);
    memcpy(b, name, strlen(name) > 100 ? 100 : strlen(name));
    snprintf((char *)b + 100, 8, "%07o", 0644u);
    snprintf((char *)b + 108, 8, "%07o", 0u);
    snprintf((char *)b + 116, 8, "%07o", 0u);
    snprintf((char *)b + 124, 12, "%011llo", size);
    snprintf((char *)b + 136, 12, "%011o", 1700000000u);
    b[156] = (unsigned char)type;
    if(f == F_USTAR) memcpy(b + 257, "ustar\0" "00", 8);
    else if(f == F_GNU) memcpy(b + 257, "ustar  \0", 8);
    set_sum(b);
}

static void a_data(const void *p, size_t n) {
    static const unsigned char zeros[512];
    a_raw(p, n);
    if(n & 511) a_raw(zeros, 512 - (n & 511));
}

static void a_entry(const char *name, int type, const void *data, size_t n, int f) {
    unsigned char b[512];
    header(b, name, type, n, f);
    a_raw(b, 512);
    if(n) a_data(data, n);
}

static void a_text(const char *name, const char *text) {
    a_entry(name, '0', text, strlen(text), F_USTAR);
}

static void a_end(void) {
    static const unsigned char zeros[1024];
    a_raw(zeros, 1024);
}

/* one pax record into o, its length counting its own digits: the record's size */
static size_t pax_rec(char *o, const char *key, const char *value, size_t vlen) {
    size_t body = strlen(key) + vlen + 3, len = body + 1, t;    /* the space, the =, the newline */
    char num[24];
    for(;;) {                                           /* until the length, with its own digits, says itself */
        size_t now = body + (size_t)snprintf(num, sizeof(num), "%u", (unsigned)len);
        if(now == len) break;
        len = now;
    }
    snprintf(o, 32, "%u ", (unsigned)len);
    t = strlen(o);
    strcpy(o + t, key);
    t += strlen(key);
    o[t++] = '=';
    memcpy(o + t, value, vlen);
    t += vlen;
    o[t++] = '\n';
    return t;
}

/* an 'x' header with one record */
static void a_pax(const char *key, const char *value, size_t vlen) {
    static char rec[8192];
    a_entry("PaxHeaders/x", 'x', rec, pax_rec(rec, key, value, vlen), F_USTAR);
}

/* x of the archive at A into a fresh disk's /dest: its status */
static int unpack(const char *opts) {
    char cmd[256];
    ars_reset();
    ars_put("/a.tar", A, An);
    ars_mkdir("/dest");
    ars_mark();
    snprintf(cmd, sizeof(cmd), "tar xf /a.tar -C /dest %s", opts);
    return ars_cmd(tar_main, cmd);
}

/**** names ****/

static void test_names(void) {
    static const struct { const char *in, *out; } good[] = {
        { "a", "a" }, { "a/b/c", "a/b/c" }, { "./a", "a" }, { "a/./b", "a/b" }, { "a//b", "a/b" }, { "a/", "a" },
        { "./", "" }, { ".", "" }, { "", "" }, { "././.", "" }, { "...a", "...a" }, { "a..b", "a..b" }, { "..a", "..a" },
        { "a/..b/c", "a/..b/c" }, { "h\xc3\xa9llo", "h\xc3\xa9llo" }, { "\xe2\x89\x8b/x", "\xe2\x89\x8b/x" },
        { "\xf0\x9f\x8c\x8a", "\xf0\x9f\x8c\x8a" }, { "a b", "a b" }, { ".hidden", ".hidden" }, { "~/x", "~/x" },
        { "\xc2\xa0", "\xc2\xa0" },                                              /* U+00A0, the first after C1 */
    };
    static const char *const bad[] = {
        "/etc/passwd", "//x", "..", "../x", "a/..", "a/../b", "a/b/../../..", "./../x", "..\\x", "a\\..\\b", "\\x",
        "C:\\x", "c:", "a:b", "a*", "a?", "a\"b", "a<b", "a>b", "a|b", "a\\b", "a\nb", "a\tb", "\x1b[2J", "a\x7f",
        "a.", "a ", "a /b", "a./b", "...", "a/...", ".. /x", "...\\x", "\xff", "a\xc3", "\xc0\xae\xc0\xae/x",
        "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
        "\xc2\x80", "a\xc2\x9b" "2J", "\xc2\x9f", "\xc2\x85x/y",               /* C1's controls, U+0080 to U+009F */
    };
    char out[256], longer[600];
    unsigned i;
    for(i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        const char *why = ar_safe_path(good[i].in, out, sizeof(out));
        check(!why && !strcmp(out, good[i].out), good[i].in);
    }
    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(ar_safe_path(bad[i], out, sizeof(out)) != NULL, bad[i]);
    }
    {
        /* the reason, as well: .. also ends in a dot, so a refusal alone would not show the rule for it had gone */
        static const char *const climbing[] = { "..", "../x", "a/..", "a/../b", "..\\x", "a\\..\\b", "./../x" };
        for(i = 0; i < sizeof(climbing) / sizeof(climbing[0]); i++) {
            const char *why = ar_safe_path(climbing[i], out, sizeof(out));
            check(why && has(why, "leaves the folder"), climbing[i]);
        }
        check(has(ar_safe_path("/x", out, sizeof(out)), "absolute"), "an absolute name is called that");
    }
    /* lengths */
    memset(longer, 'a', sizeof(longer));
    longer[255] = 0;
    check(!ar_safe_path(longer, out, sizeof(out)), "a component of 255 bytes, a path of 255");
    longer[255] = 'a';
    longer[256] = 0;
    check(ar_safe_path(longer, out, sizeof(out)) != NULL, "a component of 256 bytes");
    for(i = 0; i < 255; i += 2) {
        longer[i] = 'a';
        longer[i + 1] = '/';
    }
    longer[253] = 'a';
    longer[254] = 0;
    check(!ar_safe_path(longer, out, sizeof(out)), "a path of 254 bytes in 127 folders");
    longer[254] = '/';
    longer[255] = 'a';
    longer[256] = 0;
    check(ar_safe_path(longer, out, sizeof(out)) != NULL, "a path of 256 bytes");
    longer[10] = 0;
    {
        char tiny[6];
        check(ar_safe_path(longer, tiny, sizeof(tiny)) != NULL, "a path that does not fit the caller's buffer");
        check(!ar_safe_path("abcde", out, 6) && ar_safe_path("abcdef", out, 6), "the buffer's limit, exactly");
    }

    /* what reaches the terminal */
    ar_show_name("plain/name.txt", out, sizeof(out));
    check(!strcmp(out, "plain/name.txt"), "a plain name is shown as it is");
    ar_show_name("h\xc3\xa9llo \xe2\x89\x8b", out, sizeof(out));
    check(!strcmp(out, "h\xc3\xa9llo \xe2\x89\x8b"), "UTF-8 is shown as it is");
    ar_show_name("a\x1b[2Jb\n", out, sizeof(out));
    check(!strcmp(out, "a\\033[2Jb\\012"), "an escape sequence and a newline are not");
    ar_show_name("a\x9b" "2Jb", out, sizeof(out));
    check(!strcmp(out, "a\\2332Jb"), "a stray byte");
    ar_show_name("a\xc2\x9b" "2J", out, sizeof(out));
    check(!strcmp(out, "a\\302\\2332J"), "C1's CSI, well-formed UTF-8 and an escape all the same");
    ar_show_name("a\x7f", out, sizeof(out));
    check(!strcmp(out, "a\\177"), "DEL");
    ar_show_name("abcdefghijklmnop", out, 8);
    check(!strcmp(out, "abcd...") && strlen(out) < 8, "a name cut for room says so");
    ar_show_name("\x01\x02\x03", out, 8);
    check(strlen(out) < 8 && has(out, "..."), "and escapes are never cut in half");
    ar_show_name("abc", out, 3);
    check(!out[0], "a buffer too small for anything is left empty");

    /* matching */
    check(ar_glob("*.txt", "a.txt") && ar_glob("*.txt", "dir/a.txt") && !ar_glob("*.txt", "a.txt.gz"), "*: across slashes, to the end");
    check(ar_glob("A.TXT", "a.txt") && ar_glob("*.TXT", "a.txt"), "ASCII case does not count: FAT's rule");
    check(ar_glob("a?c", "abc") && !ar_glob("a?c", "ac") && ar_glob("a?c", "a\xc3\xa9" "c"), "? takes one character, a whole UTF-8 sequence");
    check(ar_glob("*", "") && ar_glob("", "") && !ar_glob("", "a") && ar_glob("**", "abc"), "the empty cases");
    check(ar_glob("*a*b*c*", "xxaxxbxxcxx") && !ar_glob("*a*b*c*", "xxaxxcxxbxx"), "several stars");
    {
        static char s[4000];
        memset(s, 'a', sizeof(s) - 1);
        check(!ar_glob("*a*a*a*a*a*a*a*a*a*b", s), "a pattern built to be slow ends: it takes no exponential time");
    }
    check(ar_glob_part("tmp", "a/tmp/b") && ar_glob_part("tmp", "tmp") && ar_glob_part("tmp", "a/tmp"), "a whole component anywhere");
    check(!ar_glob_part("tmp", "a/tmp2/b") && !ar_glob_part("mp", "a/tmp/b"), "not part of one");
    check(ar_glob_part("a/b", "x/a/b/c") && ar_glob_part("*.o", "a/b.o") && ar_glob_part("*.o", "b.o/c"), "several, and patterns");
    check(ar_under("a/b", "a") && ar_under("a", "a") && ar_under("a/b", "./a/") && ar_under("A/B", "a"), "under a name");
    check(!ar_under("ab", "a") && !ar_under("b/a", "a"), "not under it");
    check(ar_utf8_valid("abc", 3) && ar_utf8_valid("\xe2\x89\x8b", 3) && !ar_utf8_valid("\xe2\x89", 2) && !ar_utf8_valid("\xc0\xae", 2) && ar_utf8_valid("", 0),
          "UTF-8 by the length given, not past it");
}

/**** c, x, t ****/

static void tree(void) {
    ars_reset();
    ars_puts("/src/one.txt", "first file\n");
    ars_puts("/src/sub/two.txt", "second file\n");
    ars_puts("/src/sub/deep/three.txt", "third\n");
    ars_mkdir("/src/empty");
    ars_put("/src/zero", "", 0);
    {
        static char big[100000];
        memset(big, 'b', sizeof(big));
        ars_put("/src/sub/big", big, sizeof(big));
        ars_put("/src/block", big, 512);
        ars_put("/src/block1", big, 513);
    }
}

/* the tree above, back: does /dest/src hold it? */
static int tree_back(const char *root) {
    char p[128];
    size_t n = 0;
    snprintf(p, sizeof(p), "%s/one.txt", root);
    if(!ars_is(p, "first file\n")) return 0;
    snprintf(p, sizeof(p), "%s/sub/two.txt", root);
    if(!ars_is(p, "second file\n")) return 0;
    snprintf(p, sizeof(p), "%s/sub/deep/three.txt", root);
    if(!ars_is(p, "third\n")) return 0;
    snprintf(p, sizeof(p), "%s/empty", root);
    if(!ars_is_dir(p)) return 0;
    snprintf(p, sizeof(p), "%s/zero", root);
    if(!ars_is(p, "")) return 0;
    snprintf(p, sizeof(p), "%s/sub/big", root);
    if(!ars_get(p, &n) || n != 100000) return 0;
    snprintf(p, sizeof(p), "%s/block", root);
    if(!ars_get(p, &n) || n != 512) return 0;
    snprintf(p, sizeof(p), "%s/block1", root);
    if(!ars_get(p, &n) || n != 513) return 0;
    return 1;
}

static void test_tool(void) {
    size_t n, m;
    unsigned char *copy;
    const unsigned char *p;

    tree();
    check(ars_cmd(tar_main, "tar cf /a.tar /src") == 0 && has(ars_stderr(), "/"), "c: an absolute name, said to lose its /");
    check(ars_cmd(tar_main, "tar tf /a.tar") == 0, "t");
    p = ars_stdout(&n);
    {
        char *text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(has(text, "src/\n") && has(text, "src/one.txt\n") && has(text, "src/sub/deep/three.txt\n") && has(text, "src/empty/\n") && !has(text, "/src"),
              "t: the names, relative, a folder with its /");
        free(text);
    }
    ars_get("/a.tar", &n);
    check(n % 10240 == 0, "an archive is padded to its 10 KB records");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 2 && has(ars_stderr(), "/dest"), "-C to a folder that is not there: said");
    ars_mkdir("/dest");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 0 && tree_back("/dest/src"), "x: all of it back");
    check(ars_cmd(tar_main, "tar -x -f /a.tar -C /dest") == 0, "the same with dashes, again: what exists is replaced");
    check(!has(ars_list(), ".tmp"), "... and no .tmp is left");

    tree();
    ars_chdir("/src");
    check(ars_cmd(tar_main, "tar czf /a.tgz .") == 0, "cz: of the current folder");
    ars_mkdir("/dest");
    ars_chdir("/dest");
    check(ars_cmd(tar_main, "tar xzf /a.tgz") == 0 && tree_back("/dest"), "xz: into the current folder");
    ars_chdir("/");
    check(ars_cmd(tar_main, "tar tzf /a.tgz") == 0, "tz");
    check(ars_cmd(tar_main, "tar tf /a.tgz") == 2 && has(ars_stderr(), "gzip"), "t without z: not a tar archive, and z is suggested");

    /* pipes */
    tree();
    check(ars_cmd(tar_main, "tar cf - -C /src .") == 0, "c to standard output");
    p = ars_stdout(&n);
    copy = malloc(n);
    memcpy(copy, p, n);
    ars_mkdir("/dest");
    ars_stdin(copy, n, 1, 7);
    check(ars_cmd(tar_main, "tar xf - -C /dest") == 0 && tree_back("/dest"), "x from a pipe, seven bytes a read");
    ars_stdin(copy, n, 1, 0);
    check(ars_cmd(tar_main, "tar x -C /dest") == 0, "no f at all is standard input");
    free(copy);
    tree();
    check(ars_cmd(tar_main, "tar czf - -C /src .") == 0, "cz to standard output");
    p = ars_stdout(&n);
    copy = malloc(n);
    memcpy(copy, p, n);
    ars_mkdir("/dest");
    ars_stdin(copy, n, 1, 1000);
    check(ars_cmd(tar_main, "tar xzf - -C /dest") == 0 && tree_back("/dest"), "xz from a pipe");
    free(copy);
    tree();
    ars_ttys(0, 1);
    check(ars_cmd(tar_main, "tar cf - /src") == 2 && has(ars_stderr(), "terminal"), "no archive on a terminal");
    ars_ttys(0, 0);

    /* v */
    tree();
    check(ars_cmd(tar_main, "tar cvf /a.tar -C /src one.txt") == 0, "cv");
    p = ars_stdout(&n);
    check(n == 8 && !memcmp(p, "one.txt\n", 8), "v: the names on standard output");
    check(ars_cmd(tar_main, "tar cvf - -C /src one.txt") == 0 && has(ars_stderr(), "one.txt\n"), "v with the archive there: on standard error");
    check(ars_cmd(tar_main, "tar tvf /a.tar") == 0, "tv");
    p = ars_stdout(&n);
    {
        char *text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(has(text, "-rw-r--r--") && has(text, " 11 ") && has(text, "one.txt"), "tv: mode, size, name");
        free(text);
    }

    /* --exclude, names */
    tree();
    check(ars_cmd(tar_main, "tar cf /a.tar --exclude=*.txt --exclude deep --exclude=sub/big/ -C /src .") == 0, "c --exclude");
    ars_cmd(tar_main, "tar tf /a.tar");
    p = ars_stdout(&n);
    {
        char *text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(!has(text, ".txt") && !has(text, "deep") && !has(text, "big") && has(text, "./zero\n") && has(text, "./sub/\n"),
              "both forms, a folder and what is in it");
        free(text);
    }
    tree();
    ars_cmd(tar_main, "tar cf /a.tar -C /src .");
    ars_mkdir("/dest");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest --exclude=sub") == 0 && !ars_exists("/dest/sub") && ars_exists("/dest/one.txt"), "x --exclude");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest sub/deep") == 0 && ars_is("/dest/sub/deep/three.txt", "third\n") && !ars_exists("/dest/sub/two.txt"),
          "x of a named folder: only what is in it");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest ./zero nothere") == 2 && ars_exists("/dest/zero") && has(ars_stderr(), "nothere"),
          "a name that is not in the archive is said, the others are taken");
    check(ars_cmd(tar_main, "tar tf /a.tar SUB/DEEP/THREE.TXT") == 0 && !memcmp(ars_stdout(&n), "./sub/deep/three.txt\n", 21) && n == 21,
          "t of a name, whatever its case");

    /* the archive and itself */
    ars_reset();
    ars_puts("/d/a.txt", "x\n");
    ars_mkdir("/d/sub");
    ars_chdir("/d");
    check(ars_cmd(tar_main, "tar cf a.tar .") == 2 && has(ars_stderr(), "the archive"), "an archive inside its own tree is left out, and said");
    ars_cmd(tar_main, "tar tf a.tar");
    p = ars_stdout(&n);
    {
        char *text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(has(text, "./a.txt") && !has(text, "a.tar"), "... it is not in it");
        free(text);
    }
    check(ars_cmd(tar_main, "tar cf A.TAR .") == 2 && !has(ars_list(), ".tmp"), "over itself, in other capitals: its new file is left out too, and is gone");
    ars_cmd(tar_main, "tar tf a.tar");
    p = ars_stdout(&n);
    {
        char *text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(!has(text, ".tar") && !has(text, ".tmp"), "... it holds neither");
        free(text);
    }

    /* c: what is odd in the tree */
    tree();
    ars_chdir("/src");
    check(ars_cmd(tar_main, "tar cf /a.tar ../src/one.txt") == 0 && has(ars_stderr(), "../src/one.txt"), "a name with .. is cut after it, and that is said");
    ars_cmd(tar_main, "tar tf /a.tar");
    p = ars_stdout(&n);
    check(n == 12 && !memcmp(p, "src/one.txt\n", 12), "... to a name x will take");
    ars_chdir("/src/sub");
    check(ars_cmd(tar_main, "tar cf /a.tar .. --exclude=big --exclude=block*") == 0 && has(ars_stderr(), "leaving .."), "a name that is .. alone is left out itself");
    ars_cmd(tar_main, "tar tf /a.tar");
    {
        char *text;
        p = ars_stdout(&n);
        text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(n && text[0] != '/' && !has(text, "\n/") && has(text, "one.txt\n") && has(text, "sub/two.txt\n"),
              "... and what is under it comes out relative, not as /one.txt");
        free(text);
    }
    ars_chdir("/");
    check(ars_cmd(tar_main, "tar cf /a.tar /nothere /src/zero") == 2 && has(ars_stderr(), "/nothere"), "a name that is not there is said, the others are done");
    check(ars_exists("/a.tar"), "... and the archive made");
    check(ars_cmd(tar_main, "tar cf /b.tar") == 2 && !ars_exists("/b.tar"), "nothing to archive: no archive");
    check(ars_cmd(tar_main, "tar c") == 2 && has(ars_stderr(), "nothing"), "... said");
    check(ars_cmd(tar_main, "tar") == 2 && has(ars_stderr(), "usage"), "no mode: usage");
    check(ars_cmd(tar_main, "tar xc") == 2 && has(ars_stderr(), "usage"), "two modes: usage");
    check(ars_cmd(tar_main, "tar cqf /x /src") == 2 && has(ars_stderr(), "usage"), "an unknown letter: usage");
    check(ars_cmd(tar_main, "tar --what") == 2 && has(ars_stderr(), "usage"), "an unknown long option: usage");
    check(ars_cmd(tar_main, "tar cf") == 2 && has(ars_stderr(), "usage"), "f with nothing after it: usage");
    ars_device("/src/console");
    check(ars_cmd(tar_main, "tar cf /a.tar /src") == 2 && has(ars_stderr(), "device"), "a device in the tree is left out: reading it might never end");
    tree();
    ars_lie_size("/src/one.txt", 2000);
    check(ars_cmd(tar_main, "tar cf /a.tar -C /src one.txt zero") == 2 && has(ars_stderr(), "shrank"), "a file that shrinks while it is read is said");
    ars_mkdir("/dest");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 0 && ars_is("/dest/zero", ""), "... filled out, and the archive stays sound for what follows it");
    ars_get("/dest/one.txt", &m);
    check(m == 2011, "... at the size its header gave");

    /* a tree deep enough to need more descriptors than a program has, if any stayed open */
    {
        char path[200] = "/t";
        int i;
        ars_reset();
        for(i = 0; i < 30; i++) {
            strcat(path, "/d");
            ars_mkdir(path);
        }
        strcat(path, "/f");
        ars_puts(path, "at the bottom\n");
        check(ars_cmd(tar_main, "tar cf /a.tar -C /t .") == 0 && ars_most_open() <= 3, "30 folders deep, with a folder never held open going down");
        ars_mkdir("/dest");
        check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 0, "and back");
        check(has(ars_list(), "/dest/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/f\n"), "... to the bottom");
    }

    /* names at ustar's edges */
    {
        char name[300], cmd[400], d1[64], d2[96], leaf[110];
        memset(d1, 'a', 60);
        d1[60] = 0;
        memset(d2, 'b', 90);
        d2[90] = 0;
        memset(leaf, 'f', 99);
        leaf[99] = 0;
        ars_reset();
        snprintf(name, sizeof(name), "/%s/%s/%s", d1, d2, leaf);
        ars_puts(name, "long\n");
        snprintf(cmd, sizeof(cmd), "tar cf /a.tar -C / %s", d1);
        check(ars_cmd(tar_main, cmd) == 0, "a path of 251 bytes goes into prefix and name");
        {
            size_t n;
            unsigned char *a = malloc(1 << 16);
            {
                const unsigned char *from = ars_get("/a.tar", &n);
                memcpy(a, from, n);
            }
            ars_reset();
            ars_put("/a.tar", a, n);
            check(ars_cmd(tar_main, "tar xf /a.tar -C /") == 0 && ars_is(name, "long\n"), "and comes out whole");
            ars_reset();
            ars_put("/a.tar", a, n);
            ars_mkdir("/dest/deeper");
            check(ars_cmd(tar_main, "tar xf /a.tar -C /dest/deeper") == 2 && !has(ars_list(), "ffff"), "under a deeper folder it is too long: said, and nothing cut short is made");
            free(a);
        }
        /* a folder whose own name will not fit: its entry is left out, but what is in it is not */
        ars_reset();
        memset(name, 'n', 150);
        name[150] = 0;
        {
            char path[320];
            size_t n;
            snprintf(path, sizeof(path), "/%s/f.txt", name);
            ars_puts(path, "inside\n");
            check(ars_cmd(tar_main, "tar cf /a.tar -C / .") == 2 && has(ars_stderr(), "ustar"), "a folder's name of 150 bytes will not go in ustar: said");
            check(ars_cmd(tar_main, "tar tf /a.tar") == 0, "... and the archive is sound");
            ars_stdout(&n);
            {
                const unsigned char *out = ars_stdout(&n);
                check(n == 3 + 2 + 150 + 7 && !memcmp(out + n - 7, "/f.txt\n", 7), "... with what is in it there");
            }
        }
        ars_reset();
        memset(name, 'n', 200);
        name[200] = 0;
        {
            char path[320];
            snprintf(path, sizeof(path), "/%s", name);
            ars_puts(path, "one long name\n");
        }
        check(ars_cmd(tar_main, "tar cf /a.tar -C / .") == 2 && has(ars_stderr(), "ustar"), "one name of 200 bytes will not go in ustar: said");
    }
}

/**** the headers ****/

static void test_formats(void) {
    unsigned char b[512];
    static char longname[9000];
    size_t i;

    /* the three header styles */
    a_start();
    a_entry("gnu.txt", '0', "gnu\n", 4, F_GNU);
    a_entry("v7.txt", '0', "v7\n", 3, F_V7);
    a_entry("nul.txt", 0, "nul\n", 4, F_USTAR);
    a_entry("seven.txt", '7', "contiguous\n", 11, F_USTAR);
    a_entry("v7dir/", 0, NULL, 0, F_V7);
    a_entry("dir/", '5', NULL, 0, F_USTAR);
    a_entry("dir2", '5', NULL, 0, F_USTAR);
    a_end();
    check(unpack("") == 0 && ars_is("/dest/gnu.txt", "gnu\n") && ars_is("/dest/v7.txt", "v7\n") && ars_is("/dest/nul.txt", "nul\n") &&
          ars_is("/dest/seven.txt", "contiguous\n") && ars_is_dir("/dest/v7dir") && ars_is_dir("/dest/dir") && ars_is_dir("/dest/dir2"),
          "GNU, v7 and ustar headers; types 0, NUL and 7; folders each way");

    /* prefix */
    a_start();
    header(b, "leaf.txt", '0', 3, F_USTAR);
    memcpy(b + 345, "some/where", 10);
    set_sum(b);
    a_raw(b, 512);
    a_data("hi\n", 3);
    header(b, "gnu.txt", '0', 3, F_GNU);        /* GNU keeps other things there */
    memcpy(b + 345, "\x01\x02 not a prefix", 15);
    set_sum(b);
    a_raw(b, 512);
    a_data("yo\n", 3);
    header(b, "", '0', 3, F_USTAR);             /* a name field of exactly 100 bytes, no terminator; a prefix of 155 */
    memset(b, 'n', 100);
    memset(b + 345, 'p', 155);
    set_sum(b);
    a_raw(b, 512);
    a_data("xy\n", 3);
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "too long") && ars_is("/dest/some/where/leaf.txt", "hi\n") && ars_is("/dest/gnu.txt", "yo\n") &&
          ars_is("/dest/after.txt", "after\n"), "a ustar prefix is used, a GNU header's area is not; both fields full make a name too long, and said");

    /* numbers */
    a_start();
    header(b, "spaces.txt", '0', 0, F_USTAR);
    memcpy(b + 124, "     3     ", 11);           /* leading spaces, a trailing one */
    set_sum(b);
    a_raw(b, 512);
    a_data("sp\n", 3);
    header(b, "b256.txt", '0', 0, F_GNU);
    memset(b + 124, 0, 12);
    b[124] = 0x80;
    b[135] = 4;
    set_sum(b);
    a_raw(b, 512);
    a_data("256\n", 4);
    a_end();
    check(unpack("") == 0 && ars_is("/dest/spaces.txt", "sp\n") && ars_is("/dest/b256.txt", "256\n"), "a size with spaces, and in base 256");

    /* numbers that are not */
    a_start();
    header(b, "neg.txt", '0', 0, F_GNU);
    memset(b + 124, 0xff, 12);
    set_sum(b);
    a_raw(b, 512);
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "damaged") && ars_count() == 2, "a negative size: the archive cannot be read on, and nothing was made");
    a_start();
    header(b, "neg-small.txt", '0', 0, F_GNU);          /* negative, and small enough to pass for 4 if only its low bytes were read */
    memset(b + 124, 0, 12);
    b[124] = 0xff;
    b[135] = 4;
    set_sum(b);
    a_raw(b, 512);
    a_data("abcd", 4);
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "damaged") && ars_count() == 2, "a negative size with small low bytes");
    a_start();
    header(b, "high-bit.txt", '0', 0, F_GNU);           /* a number in the first byte's own bits: past any file */
    memset(b + 124, 0, 12);
    b[124] = 0x81;
    b[135] = 4;
    set_sum(b);
    a_raw(b, 512);
    a_data("abcd", 4);
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "damaged") && ars_count() == 2, "a size with bits in base 256's first byte");
    a_start();
    header(b, "huge.txt", '0', 0, F_GNU);
    memset(b + 124, 0xff, 12);
    b[124] = 0x80;
    set_sum(b);
    a_raw(b, 512);
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "damaged"), "a size of 88 bits");
    a_start();
    header(b, "junk.txt", '0', 0, F_USTAR);
    memcpy(b + 124, "12x45678901", 11);
    set_sum(b);
    a_raw(b, 512);
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "damaged"), "letters in a size");
    a_start();
    a_entry("big.bin", '0', "", 0, F_USTAR);
    An = 0;
    header(b, "big.bin", '0', 0, F_GNU);
    memset(b + 124, 0, 12);
    b[124] = 0x80;
    b[131] = 1;                                 /* 2^32 bytes */
    set_sum(b);
    a_raw(b, 512);
    check(unpack("") == 2 && has(ars_stderr(), "4 GB") && !ars_exists("/dest/big.bin"), "a file of 4 GB is skipped: FAT cannot hold one");
    a_start();
    a_entry("good.txt", '0', "kept\n", 5, F_USTAR);
    header(b, "more.txt", '0', 1000000, F_USTAR);
    a_raw(b, 512);
    a_data("short", 5);
    check(unpack("") == 2 && ars_is("/dest/good.txt", "kept\n") && !ars_exists("/dest/more.txt") && has(ars_stderr(), "ends in the middle"),
          "a size larger than the archive: what it claims is not made, and what came before stands");

    /* sums */
    a_start();
    a_text("ok.txt", "x\n");
    a_end();
    A[148] ^= 1;
    check(unpack("") == 2 && has(ars_stderr(), "not a tar archive"), "a bad sum in the first header");
    a_start();
    a_text("ok.txt", "x\n");
    a_text("next.txt", "y\n");
    a_end();
    A[1024 + 3] ^= 1;
    check(unpack("") == 2 && has(ars_stderr(), "damaged") && ars_is("/dest/ok.txt", "x\n"), "a bad sum in a later one: stops there, what came before stands");
    a_start();
    header(b, "signed\xe9.txt", '0', 0, F_V7);  /* a sum of signed characters, as old tars made it */
    {
        int sum = 0, k;
        memset(b + 148, ' ', 8);
        for(k = 0; k < 512; k++) sum += b[k] > 127 ? b[k] - 256 : b[k];
        snprintf((char *)b + 148, 8, "%06o", sum);
        b[155] = ' ';
    }
    a_raw(b, 512);
    a_end();
    check(unpack("") == 2 && !has(ars_stderr(), "not a tar archive") && has(ars_stderr(), "UTF-8"), "a sum of signed characters is a sum");
    {
        static const char junk[1024] = "this is not an archive at all, just some text";
        a_start();
        a_raw(junk, sizeof(junk));
        check(unpack("") == 2 && has(ars_stderr(), "not a tar archive"), "text");
    }

    /* the end */
    a_start();
    a_text("a.txt", "x\n");
    check(unpack("") == 2 && has(ars_stderr(), "no end") && ars_is("/dest/a.txt", "x\n"), "no end marker: the files stand, and the cut is said");
    a_start();
    a_text("a.txt", "x\n");
    {
        static const unsigned char zeros[512];
        a_raw(zeros, 512);
    }
    check(unpack("") == 0, "one zero block and the end of the file: as some tars leave it");
    a_start();
    a_text("a.txt", "x\n");
    a_end();
    a_raw("whatever follows the end marker is not looked at", 48);
    check(unpack("") == 0, "what follows the end marker");
    a_start();
    check(unpack("") == 2 && has(ars_stderr(), "no end"), "an empty file");
    a_start();
    a_text("a.txt", "x\n");
    {
        static const unsigned char zeros[512];
        a_raw(zeros, 512);
    }
    a_text("b.txt", "y\n");
    a_end();
    check(unpack("") == 0 && ars_exists("/dest/b.txt"), "one zero block between entries: no end yet");

    /* names not met because the archive stopped first */
    a_start();
    a_text("a.txt", "x\n");
    a_text("b.txt", "y\n");
    a_end();
    A[1024 + 3] ^= 1;
    check(unpack("a.txt b.txt c.txt") == 2 && ars_is("/dest/a.txt", "x\n") && has(ars_stderr(), "damaged"), "named members, and an archive that stops part way");
    check(has(ars_stderr(), "b.txt: not reached") && has(ars_stderr(), "c.txt: not reached") && !has(ars_stderr(), "not in the archive"),
          "what was not reached is not called absent");
    a_start();
    a_text("a.txt", "x\n");
    check(unpack("a.txt b.txt") == 2 && has(ars_stderr(), "b.txt: not reached"), "the same with no end marker");
    a_start();
    a_text("a.txt", "x\n");
    a_end();
    check(unpack("b.txt") == 2 && has(ars_stderr(), "b.txt: not in the archive"), "and with a whole archive it is absent");

    /* links, devices and what FAT cannot hold: skipped and said, with the next entry found */
    {
        static const struct { int type; const char *word; } kinds[] = {
            { '1', "hard link" }, { '2', "symbolic link" }, { '3', "device" }, { '4', "device" }, { '6', "named pipe" }, { 'Z', "does not know" },
        };
        for(i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            a_start();
            a_entry("thing", kinds[i].type, kinds[i].type == 'Z' ? "unknown data" : NULL, kinds[i].type == 'Z' ? 12 : 0, F_USTAR);
            a_text("after.txt", "after\n");
            a_end();
            check(unpack("") == 2 && has(ars_stderr(), kinds[i].word) && !ars_exists("/dest/thing") && ars_is("/dest/after.txt", "after\n"),
                  "a kind FAT cannot hold: skipped, said, and the next entry found");
        }
    }
    /* a link's size field does not count: POSIX, and an attacker's way to hide headers inside "data" */
    a_start();
    header(b, "link", '2', 512, F_USTAR);
    a_raw(b, 512);
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && ars_is("/dest/after.txt", "after\n"), "a link's size is no data");
    a_start();
    header(b, "dir/", '5', 512, F_USTAR);
    a_raw(b, 512);
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 0 && ars_is("/dest/after.txt", "after\n"), "nor a folder's");

    /* pax lets a hard link carry the file's data (pax -o linkdata): the header's size counts there, and only there */
    a_start();
    a_pax("mtime", "1700000000", 10);
    {
        static char body[1000];
        memset(body, 'L', sizeof(body));
        header(b, "hard", '1', 1000, F_USTAR);
        a_raw(b, 512);
        a_data(body, 1000);
    }
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "hard link") && !ars_exists("/dest/hard") && ars_is("/dest/after.txt", "after\n"), "a hard link with data, in a pax archive: read past");

    /* GNU sparse: a map, extension blocks, its data; passed over */
    a_start();
    header(b, "sparse", 'S', 600, F_GNU);
    b[482] = 1;
    set_sum(b);
    a_raw(b, 512);
    {
        unsigned char ext[512];
        memset(ext, 0, 512);
        ext[504] = 1;
        a_raw(ext, 512);
        ext[504] = 0;
        a_raw(ext, 512);
    }
    {
        static char data[600];
        a_data(data, sizeof(data));
    }
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "sparse") && !ars_exists("/dest/sparse") && ars_is("/dest/after.txt", "after\n"), "a GNU sparse file");

    /* GNU long names */
    memset(longname, 0, sizeof(longname));
    memset(longname, 'a', 120);
    a_start();
    a_entry("././@LongLink", 'L', longname, 121, F_GNU);
    a_entry("cut-short-name", '0', "long\n", 5, F_GNU);
    a_end();
    {
        char path[140] = "/dest/";
        strcat(path, longname);
        check(unpack("") == 0 && ars_is(path, "long\n") && !ars_exists("/dest/cut-short-name"), "a GNU long name");
    }
    a_start();
    a_entry("././@LongLink", 'K', "some/link/target", 17, F_GNU);
    a_entry("././@LongLink", 'L', "real-name.txt", 14, F_GNU);
    a_entry("short", '0', "both\n", 5, F_GNU);
    a_end();
    check(unpack("") == 0 && ars_is("/dest/real-name.txt", "both\n"), "a long link name is passed over");
    a_start();
    a_entry("././@LongLink", 'L', "../escape.txt", 14, F_GNU);
    a_entry("innocent", '0', "evil\n", 5, F_GNU);
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && !ars_exists("/escape.txt") && !ars_exists("/dest/innocent") && ars_is("/dest/after.txt", "after\n"), "a long name that climbs out is refused");
    a_start();
    a_entry("././@LongLink", 'L', "good.txt\0../evil", 17, F_GNU);
    a_entry("x", '0', "evil\n", 5, F_GNU);
    a_end();
    check(unpack("") == 2 && !ars_exists("/dest/good.txt") && has(ars_stderr(), "zero byte"), "a zero byte inside a long name");
    memset(longname, 'a', 8000);
    a_start();
    a_entry("././@LongLink", 'L', longname, 8001, F_GNU);
    a_entry("x", '0', "long\n", 5, F_GNU);
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && ars_is("/dest/after.txt", "after\n") && ars_count() == 3, "a long name of 8000 bytes: refused, with the rest of its data skipped");
    check(unpack("") == 2 && ars_cmd(tar_main, "tar tf /a.tar") == 0, "... and t shows it, cut");
    memset(longname, 0, sizeof(longname));
    a_start();
    a_entry("././@LongLink", 'L', "stray", 6, F_GNU);
    a_end();
    check(unpack("") == 0 && ars_count() == 2, "a long name with no entry after it");

    /* pax */
    a_start();
    a_pax("path", "pax/name/from/record.txt", 24);
    a_entry("short", '0', "pax\n", 4, F_USTAR);
    a_end();
    check(unpack("") == 0 && ars_is("/dest/pax/name/from/record.txt", "pax\n") && !ars_exists("/dest/short"), "a pax path");
    {
        char data[512];
        static char body[600];
        memset(body, 'p', sizeof(body));
        /* the header says 0 here, and the record 600, with the data really 600: the record wins */
        a_start();
        a_pax("size", "600", 3);
        header((unsigned char *)data, "sized", '0', 0, F_USTAR);
        a_raw(data, 512);
        a_data(body, 600);
        a_text("after.txt", "after\n");
        a_end();
        check(unpack("") == 0 && ars_is("/dest/after.txt", "after\n"), "a pax size wins over the header's");
        check(ars_get("/dest/sized", &i) && i == 600, "... and sizes the file");
    }
    a_start();
    {
        char rec[128];
        size_t n = pax_rec(rec, "comment", "2f6f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f", 38);
        a_entry("x", 'g', rec, n, F_USTAR);
    }
    a_text("a.txt", "after a global header\n");
    a_end();
    check(unpack("") == 0 && ars_is("/dest/a.txt", "after a global header\n"), "a global header is passed over");
    a_start();
    a_pax("path", "../climb.txt", 12);
    a_text("fine.txt", "evil\n");
    a_end();
    check(unpack("") == 2 && !ars_exists("/climb.txt") && !ars_exists("/dest/fine.txt"), "a pax path that climbs out is refused, and the name behind it is not used");
    a_start();
    a_pax("path", "good.txt\0/../x", 14);
    a_text("fine.txt", "evil\n");
    a_end();
    check(unpack("") == 2 && !ars_exists("/dest/good.txt") && has(ars_stderr(), "zero byte"), "a zero byte in a pax path");
    a_start();
    a_pax("GNU.sparse.realsize", "1000000000", 10);
    a_text("hole.txt", "condensed\n");
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "sparse") && !ars_exists("/dest/hole.txt"), "a pax sparse file is skipped: its data is not the file's");
    {
        /* a record that makes no sense: nothing about the next entry can be trusted, and it goes unwritten; the one after is taken */
        static const char *const bad[] = { "x\n", "99999 path=a\n", "5 ab\n", "3 \n", "0 =\n", "12345678901234567890123 path=x\n", " path=x\n",
                                           "9 path=x" };
        for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            a_start();
            a_entry("x", 'x', bad[i], strlen(bad[i]), F_USTAR);
            a_text("name.txt", "its own name may not be its name\n");
            a_text("after.txt", "after\n");
            a_end();
            check(unpack("") == 2 && !ars_exists("/dest/name.txt") && ars_is("/dest/after.txt", "after\n") && has(ars_stderr(), "pax") && has(ars_stderr(), "name.txt"),
                  "a pax record that makes no sense: its entry is named and skipped, and the next is taken");
        }
    }
    {
        /* a pax header of 2 MB of records nobody needs, with the path at the end: nothing allocated, nothing lost */
        static char rec[2100000];
        size_t n = 0;
        while(n < 2000000) n += pax_rec(rec + n, "LIBARCHIVE.xattr.user.junk", "0123456789012345678901234567", 28);
        n += pax_rec(rec + n, "path", "far/end.txt", 11);
        a_start();
        a_entry("x", 'x', rec, n, F_USTAR);
        a_text("short", "found\n");
        a_end();
        check(unpack("") == 0 && ars_is("/dest/far/end.txt", "found\n"), "a pax header of 2 MB");
    }
    {
        /* a pax record of 1 MB: longer than is kept */
        static char rec[1100000];
        char num[16];
        int d = snprintf(num, sizeof(num), "%d", 1000000);
        memcpy(rec, num, (size_t)d);
        rec[d] = ' ';
        memcpy(rec + d + 1, "path=", 5);
        memset(rec + d + 6, 'q', 1000000 - (size_t)d - 7);
        rec[999999] = '\n';
        a_start();
        a_entry("x", 'x', rec, 1000000, F_USTAR);
        a_text("short", "not this name\n");
        a_text("after.txt", "after\n");
        a_end();
        check(unpack("") == 2 && !ars_exists("/dest/short") && ars_is("/dest/after.txt", "after\n"), "a pax path of 1 MB is refused, whole");
    }
}

/**** what must be refused ****/

static void test_refusals(void) {
    static const char *const evil[] = {
        "/etc/passwd", "../up.txt", "a/../../up.txt", "a/b/../../../up.txt", "..", "a/..",
        "/", "//abs", "..\\up.txt", "a\\..\\..\\up.txt", "C:\\up.txt", "C:/up.txt", "a:b",
        "bad*name", "bad?name", "bad\"name", "bad<name", "bad>name", "bad|name", "dot.", "space ", "dir./x", "dir /x",
        "ctl\x01name", "esc\x1b[2J", "\xff\xfe", "\xc0\xae\xc0\xae/up.txt", "\xc0\xaf" "up.txt", "...", ".. ",
    };
    unsigned i, t;
    for(t = 0; t < 2; t++) {
        for(i = 0; i < sizeof(evil) / sizeof(evil[0]); i++) {
            a_start();
            a_text("before.txt", "before\n");
            a_entry(evil[i], t ? '5' : '0', t ? NULL : "evil\n", t ? 0 : 5, F_USTAR);
            a_text("after.txt", "after\n");
            a_end();
            check(unpack("") == 2, evil[i]);
            if(!ars_is("/dest/before.txt", "before\n") || !ars_is("/dest/after.txt", "after\n") || ars_count() != 4) {
                printf("around [%s] type %u:\n%s%s", evil[i], t, ars_list(), ars_stderr());
                check(0, "the entries around a refused one are taken, and nothing else is made");
            } else {
                check(1, "the entries around a refused one are taken, and nothing else is made");
            }
            check(ars_all_under("/dest"), "nothing is made outside the folder");
            check(!strchr(ars_stderr(), 0x1b) && !strchr(ars_stderr(), 0x01), "and the message holds no control character");
            ars_cmd(tar_main, "tar tf /a.tar");
            {
                size_t n, k;
                const unsigned char *p = ars_stdout(&n);
                int clean = 1;
                for(k = 0; k < n; k++) if(p[k] < 0x20 && p[k] != '\n') clean = 0;
                check(clean, "t lists it, with no control character reaching the terminal");
            }
        }
    }
    /* the top of the archive as an entry is no refusal */
    a_start();
    a_entry("./", '5', NULL, 0, F_USTAR);
    a_text("./a.txt", "x\n");
    a_end();
    check(unpack("") == 0 && ars_is("/dest/a.txt", "x\n"), "./ is the archive's top, and nothing to make");
    a_start();
    a_text("./", "a file named after the top\n");
    a_end();
    check(unpack("") == 2 && ars_count() == 2 && has(ars_stderr(), "holds data"), "./ with data: a folder, the top again, and its data is said to go");
    a_start();
    a_text("sub/", "data in a folder's entry\n");
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && ars_is_dir("/dest/sub") && ars_is("/dest/after.txt", "after\n") && has(ars_stderr(), "holds data"),
          "a folder by its name that holds data: made, said, status 2");
    a_start();
    a_entry("", '0', "no name\n", 8, F_USTAR);
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "no name") && ars_count() == 2, "a file with no name");

    /* in the way */
    a_start();
    a_text("x", "a file\n");
    a_text("x/y", "under it\n");
    a_end();
    check(unpack("") == 2 && ars_is("/dest/x", "a file\n"), "a file where a folder is wanted");
    a_start();
    a_entry("x/", '5', NULL, 0, F_USTAR);
    a_text("x", "over it\n");
    a_end();
    check(unpack("") == 2 && ars_is_dir("/dest/x"), "a folder where a file is wanted");
    a_start();
    a_text("x", "a file\n");
    a_entry("x/", '5', NULL, 0, F_USTAR);
    a_end();
    check(unpack("") == 2 && ars_is("/dest/x", "a file\n") && has(ars_stderr(), "in the way"), "a folder over a file");
    a_start();
    a_text("Case.txt", "first\n");
    a_text("CASE.TXT", "second\n");
    a_end();
    check(unpack("") == 0 && ars_is("/dest/case.txt", "second\n") && ars_count() == 3, "two names FAT cannot tell apart: the later one, one file");

    /* a file that exists: replaced, and through saveutil */
    a_start();
    a_text("mine.txt", "new\n");
    a_end();
    ars_reset();
    ars_put("/a.tar", A, An);
    ars_puts("/dest/mine.txt", "old\n");
    ars_puts("/dest/mine.txt.tmp", "the user's own\n");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 0 && ars_is("/dest/mine.txt", "new\n") && ars_is("/dest/mine.txt.tmp", "the user's own\n"),
          "replaced beside a .tmp of the user's, which stays");
    ars_reset();
    ars_put("/a.tar", A, An);
    ars_puts("/dest/mine.txt", "old\n");
    ars_puts("/dest/mine.txt.tmp", "old\n");
    ars_same_time("/dest/mine.txt", "/dest/mine.txt.tmp");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 2 && ars_is("/dest/mine.txt", "old\n") && has(ars_stderr(), "check the disk"),
          "a file with a twin is left, and that is said");
}

/**** damage ****/

static void test_damage(void) {
    static unsigned char plain[60000], m[1 << 18], good[1 << 18];
    size_t n, i;
    unsigned r = 5;
    for(i = 0; i < sizeof(plain); i++) {
        r = r * 1103515245 + 12345;
        plain[i] = (i >> 9) & 1 ? (unsigned char)(r >> 16) : (unsigned char)"abcd"[(r >> 16) & 3];
    }
    /* every cut of a plain archive: what was whole stands, what was not is gone */
    a_start();
    a_entry("dir/", '5', NULL, 0, F_USTAR);
    a_entry("dir/one", '0', plain, 1000, F_USTAR);
    a_entry("././@LongLink", 'L', "dir/long-name-two", 18, F_GNU);
    a_entry("two", '0', plain, 700, F_GNU);
    a_end();
    n = An;
    memcpy(good, A, n);
    for(i = 0; i < n; i += 37) {
        int st, k;
        An = i;
        memcpy(A, good, i);
        st = unpack("");
        size_t got;
        if(st != 0 && st != 2) check(0, "a cut archive: status 0 or 2");
        if(st == 0 && i < n - 1024) check(0, "a cut in the entries cannot pass");
        ars_get("/dest/dir/one", &got);
        if(ars_exists("/dest/dir/one") && got != 1000) check(0, "no partial file stands");
        if(ars_exists("/dest/dir/long-name-two") && (ars_get("/dest/dir/long-name-two", &got), got != 700)) check(0, "no partial file stands");
        (void)k;
    }
    check(1, "every cut of an archive: no partial file");
    An = n;
    memcpy(A, good, n);

    /* a cut must not hurt a file that was there */
    An = 512 + 512 + 200;
    ars_reset();
    ars_put("/a.tar", A, An);
    ars_puts("/dest/dir/one", "the old one\n");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 2 && ars_is("/dest/dir/one", "the old one\n") && !has(ars_list(), ".tmp"),
          "a cut in the middle of a file that exists: the old one stands");
    An = n;

    /* mutated archives, plain and through gzip: they end, status 0 or 2, and nothing goes outside */
    for(i = 0; i < 1500; i++) {
        int st, k;
        memcpy(m, good, n);
        r = r * 1103515245 + 12345;
        for(k = 1 + (int)(r >> 28); k > 0; k--) {
            r = r * 1103515245 + 12345;
            m[(r >> 8) % n] = (i & 1) ? (unsigned char)(r >> 24) : (m[(r >> 8) % n] ^ (unsigned char)(1u << (r & 7)));
        }
        memcpy(A, m, n);
        An = n;
        st = unpack("");
        if((st != 0 && st != 2) || !ars_all_under("/dest")) check(0, "a mutated archive: status 0 or 2, nothing outside");
    }
    check(1, "1500 mutated archives: nothing outside the folder");
    /* mutated, with every header's sum put right: the fields themselves are what is hostile */
    for(i = 0; i < 1500; i++) {
        size_t h, at;
        int st;
        memcpy(m, good, n);
        r = r * 1103515245 + 12345;
        h = ((r >> 8) % (n / 512)) * 512;
        at = h + (r >> 20) % 512;
        m[at] = (unsigned char)(r >> 3);
        if(m[h + 148] >= '0' && m[h + 148] <= '7') set_sum(m + h);
        memcpy(A, m, n);
        An = n;
        st = unpack("");
        if((st != 0 && st != 2) || !ars_all_under("/dest")) check(0, "a mutated header with a right sum: status 0 or 2, nothing outside");
        ars_cmd(tar_main, "tar tvf /a.tar");
    }
    check(1, "1500 mutated headers with right sums: x and tv hold");
    An = n;
    memcpy(A, good, n);

    /* through gzip: a gzip fault is said, and a file in progress goes */
    tree();
    ars_cmd(tar_main, "tar czf /a.tgz -C /src .");
    {
        size_t z;
        const unsigned char *p = ars_get("/a.tgz", &z);
        memcpy(m, p, z);
        for(i = 0; i < z; i += 1 + i / 30) {
            ars_reset();
            ars_put("/a.tgz", m, i);
            ars_mkdir("/dest");
            if(ars_cmd(tar_main, "tar xzf /a.tgz -C /dest") != 2) check(0, "a cut .tgz: status 2");
        }
        ars_reset();
        ars_put("/a.tgz", m, z);
        m[z - 6] ^= 1;
        ars_put("/b.tgz", m, z);
        ars_mkdir("/dest");
        check(ars_cmd(tar_main, "tar xzf /b.tgz -C /dest") == 2 && has(ars_stderr(), "CRC"),
              "a .tgz whose CRC is wrong: found though the tar end marker came before it");
        m[z - 6] ^= 1;
        memcpy(m + z, "tail", 4);
        ars_put("/c.tgz", m, z + 4);
        check(ars_cmd(tar_main, "tar tzf /c.tgz") == 2 && has(ars_stderr(), "after the end"), "garbage after a .tgz");
        memset(m + z, 0, 10240);
        ars_put("/d.tgz", m, z + 10240);
        check(ars_cmd(tar_main, "tar tzf /d.tgz") == 0, "zeros after a .tgz: what a piped GNU tar leaves");
    }
}

/**** the disk ****/

static void test_failures(void) {
    int total, k, m;

    /* x with a change failing at each step, and the disk pulled at each step */
    a_start();
    a_text("sub/one.txt", "new one\n");
    a_text("two.txt", "new two\n");
    a_end();
    ars_reset();
    ars_put("/a.tar", A, An);
    ars_puts("/dest/two.txt", "old two\n");
    ars_cmd(tar_main, "tar xf /a.tar -C /dest");
    total = ars_changes();
    check(total >= 8, "x changes the disk in many steps");
    for(m = 0; m < 2; m++) {
        for(k = 1; k <= total; k++) {
            int st;
            ars_reset();
            ars_put("/a.tar", A, An);
            ars_puts("/dest/two.txt", "old two\n");
            if(m) ars_pull_at(k);
            else ars_fail_change(k, ARS_EIO);
            st = ars_cmd(tar_main, "tar xf /a.tar -C /dest");
            check(ars_is("/dest/two.txt", "old two\n") || ars_is("/dest/two.txt", "new two\n"), "a file that existed is the old one or the new one");
            if(ars_exists("/dest/sub/one.txt") && !ars_is("/dest/sub/one.txt", "new one\n") && !m) check(0, "a new file that stands is whole");
            if(!m && st == 0) check(ars_is("/dest/two.txt", "new two\n") && ars_is("/dest/sub/one.txt", "new one\n"), "status 0 means done");
            if(!m && has(ars_list(), ".tmp") && !has(ars_stderr(), "check the disk")) check(0, "no .tmp stays, unless a rename left it and that is said");
        }
    }

    /* c over an old archive */
    tree();
    ars_puts("/a.tar", "the old archive");
    ars_cmd(tar_main, "tar cf /a.tar -C /src one.txt");
    total = ars_changes();
    for(k = 1; k <= total; k++) {
        int st;
        size_t n = 0;
        tree();
        ars_puts("/a.tar", "the old archive");
        ars_fail_change(k, ARS_EIO);
        st = ars_cmd(tar_main, "tar cf /a.tar -C /src one.txt");
        check(ars_is("/a.tar", "the old archive") || (ars_get("/a.tar", &n) && n % 10240 == 0), "an archive is the old one or the whole new one");
        if(st == 0) check(!ars_is("/a.tar", "the old archive"), "status 0 means done");
    }
    /* c of a new archive: it goes when it cannot be finished */
    tree();
    ars_cmd(tar_main, "tar cf /b.tar -C /src one.txt");
    total = ars_changes();
    for(k = 1; k <= total; k++) {
        tree();
        ars_fail_change(k, ARS_EIO);
        if(ars_cmd(tar_main, "tar cf /b.tar -C /src one.txt") && ars_exists("/b.tar")) check(0, "a new archive that failed is gone");
    }
    check(1, "a new archive that failed is gone");
    /* -C moves the program: the clean-up must find the file by its own path, and no other */
    tree();
    ars_puts("/src/b.tar", "a file of the user's in the folder -C goes to");
    ars_quota(0);
    ars_chdir("/");
    check(ars_cmd(tar_main, "tar cf b.tar -C /src one.txt") == 2 && ars_is("/src/b.tar", "a file of the user's in the folder -C goes to") && !ars_exists("/b.tar"),
          "a failure after -C deletes its own file, not one of that name where -C went");

    /* full disks */
    tree();
    ars_quota(100);
    check(ars_cmd(tar_main, "tar cf /a.tar /src") == 2 && !ars_exists("/a.tar"), "a full disk: no archive is left");
    a_start();
    a_text("one.txt", "first\n");
    a_text("two.txt", "second, a longer file than the first\n");
    a_text("three.txt", "x\n");
    a_end();
    ars_reset();
    ars_put("/a.tar", A, An);
    ars_quota(An + 8);
    ars_mkdir("/dest");
    check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 2 && has(ars_stderr(), "No space") && ars_is("/dest/one.txt", "first\n") && !ars_exists("/dest/two.txt"),
          "x on a full disk: the file that does not fit goes, and it is said");
    check(ars_is("/dest/three.txt", "x\n") || !ars_exists("/dest/three.txt"), "... and what follows is tried");

    /* a reader that goes */
    tree();
    ars_stdout_closes_after(600);
    check(ars_cmd(tar_main, "tar cf - /src") == 141, "c to a pipe whose reader goes: the program ends there");
    tree();
    ars_stdout_closes_after(10);
    ars_puts("/a", "x");
    ars_cmd(tar_main, "tar cf /a.tar -C /src .");
    ars_stdout_closes_after(10);
    check(ars_cmd(tar_main, "tar tf /a.tar") == 141, "a listing whose reader goes");
}

/**** a pax header that cannot be read ****/

static void pax_fixture(const char *raw, size_t rawn, const char *key, const char *value, size_t vlen, unsigned header_size, size_t data) {
    static char body[4000];
    unsigned char b[512];
    memset(body, 'A', sizeof(body));
    a_start();
    if(key) a_pax(key, value, vlen);
    else a_entry("PaxHeaders/x", 'x', raw, rawn, F_USTAR);
    header(b, "important.txt", '0', header_size, F_USTAR);
    a_raw(b, 512);
    a_data(body, data);
    a_text("next.txt", "next\n");
    a_end();
}

/* x over a file the user has: its status */
static int over_mine(void) {
    ars_reset();
    ars_put("/a.tar", A, An);
    ars_puts("/dest/important.txt", "the user's good file\n");
    ars_mark();
    return ars_cmd(tar_main, "tar xf /a.tar -C /dest");
}

static int mine_stands(void) {
    return ars_is("/dest/important.txt", "the user's good file\n") && !has(ars_list(), ".tmp");
}

static void test_pax_entry(void) {
    static const char *const values[] = { "20x0", "-5", "", "2000 ", "99999999999999999999999", "0x7d0" };
    static char digits[6000];
    unsigned i;
    /* a size that cannot be read: where the next entry starts is not known */
    for(i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        pax_fixture(NULL, 0, "size", values[i], strlen(values[i]), 512, 2000);
        check(over_mine() == 2, "an unreadable pax size: status 2");
        check(mine_stands(), "an unreadable pax size: the user's file stands");
        check(!ars_exists("/dest/next.txt"), "an unreadable pax size: nothing after it is believed");
        check(has(ars_stderr(), "important.txt") && has(ars_stderr(), "size"), "an unreadable pax size: the entry is named, and the reason");
    }
    pax_fixture(NULL, 0, "size", "20x0", 4, 512, 2000);
    ars_reset();
    ars_put("/a.tar", A, An);
    check(ars_cmd(tar_main, "tar tf /a.tar") == 2 && has(ars_stderr(), "size") && !has((const char *)ars_stdout(NULL), "next.txt"), "t stops there too");
    {
        static const char raw[] = "xx size=2000\n";
        pax_fixture(raw, sizeof(raw) - 1, NULL, NULL, 0, 512, 2000);
        check(over_mine() == 2 && mine_stands() && !ars_exists("/dest/next.txt"), "a size in a record that cannot be framed: the same");
    }
    memset(digits, '1', sizeof(digits));
    pax_fixture(NULL, 0, "size", digits, 5000, 512, 2000);
    check(over_mine() == 2 && mine_stands() && !ars_exists("/dest/next.txt"), "a size longer than is kept of a record: the same");

    /* a path that cannot be read: the entry's own name is a stand-in, and it may be a file the user has */
    {
        static const char *const raws[] = { "xx path=long/new.txt\n", "9 path=x", "99999 path=long/new.txt\n", "path=long/new.txt\n", "5 ab\n" };
        for(i = 0; i < sizeof(raws) / sizeof(raws[0]); i++) {
            pax_fixture(raws[i], strlen(raws[i]), NULL, NULL, 0, 20, 20);
            check(over_mine() == 2, "an unreadable pax path: status 2");
            check(mine_stands(), "an unreadable pax path: the user's file stands");
            check(ars_is("/dest/next.txt", "next\n"), "an unreadable pax path: the next entry is taken");
            check(has(ars_stderr(), "important.txt") && has(ars_stderr(), "pax"), "an unreadable pax path: the entry is named");
        }
    }
    pax_fixture(NULL, 0, "path", "good.txt\0../x", 13, 20, 20);
    check(over_mine() == 2 && mine_stands() && ars_is("/dest/next.txt", "next\n") && !ars_exists("/dest/good.txt"), "a zero byte in a pax path: the same");
    memset(digits, 'p', sizeof(digits));
    pax_fixture(NULL, 0, "path", digits, 5000, 20, 20);
    check(over_mine() == 2 && mine_stands() && ars_is("/dest/next.txt", "next\n"), "a pax path longer than is kept: the same");

    /* it is per entry: what follows is judged on its own */
    a_start();
    a_entry("PaxHeaders/x", 'x', "5 ab\n", 5, F_USTAR);
    a_text("skipped.txt", "skipped\n");
    a_pax("path", "really.txt", 10);
    a_text("stand-in", "fine\n");
    a_text("plain.txt", "plain\n");
    a_end();
    check(unpack("") == 2 && !ars_exists("/dest/skipped.txt") && ars_is("/dest/really.txt", "fine\n") && ars_is("/dest/plain.txt", "plain\n"),
          "a good pax header after a bad one is taken, and so is a plain entry");
    {
        const char *e = ars_stderr();
        const char *first = strstr(e, "skipped.txt");
        check(first && !strstr(first + 1, "skipped.txt") && !has(e, "a name may be wrong"), "the bad one is named once, and nothing general is said of the rest");
    }
    /* a damaged header with no entry after it */
    a_start();
    a_text("a.txt", "x\n");
    a_entry("PaxHeaders/x", 'x', "5 ab\n", 5, F_USTAR);
    a_end();
    check(unpack("") == 2 && has(ars_stderr(), "pax") && ars_is("/dest/a.txt", "x\n"), "a damaged pax header with no entry after it is said");
}

/**** what reaches the terminal through messages that take a path raw ****/

static void test_c1(void) {
    static const char c1[] = "\xc2\x9b" "2J\xc2\x9b" "31mowned";
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/", c1);
    /* of_say and su_say print a path as it is: such a path must never reach them */
    a_start();
    a_entry(dir, '5', NULL, 0, F_USTAR);
    a_text(c1, "data\n");
    a_text("after.txt", "after\n");
    a_end();
    check(unpack("") == 2 && ars_is("/dest/after.txt", "after\n") && ars_count() == 3, "a name with C1's CSI is refused, as a folder and as a file");
    check(!strstr(ars_stderr(), "\xc2\x9b"), "and no message holds it bare");
}

/**** a member named like the archive ****/

static void test_self(void) {
    static const char *const names[] = { "a.tar", "A.TAR", "./a.tar" };
    unsigned char *copy;
    size_t n, m;
    unsigned i;
    for(i = 0; i < 3; i++) {
        const unsigned char *now;
        a_start();
        a_text(names[i], "NEW CONTENTS\n");
        a_text("after.txt", "after\n");
        a_end();
        n = An;
        ars_reset();
        ars_put("/d/a.tar", A, n);
        ars_chdir("/d");
        check(ars_cmd(tar_main, "tar xf a.tar") == 2, names[i]);
        check(has(ars_stderr(), "the archive"), "a member that is the archive itself is refused by what it is, not by the rename's EBUSY");
        now = ars_get("/d/a.tar", &m);
        check(now && m == n && !memcmp(now, A, n), "the archive is as it was");
        check(!has(ars_list(), ".tmp"), "and nothing was written for that member");
        check(ars_is("/d/after.txt", "after\n"), "and the next member is taken");
    }
    /* through -C, and through gzip */
    a_start();
    a_text("x/a.tar", "NEW CONTENTS\n");
    a_end();
    n = An;
    ars_reset();
    ars_put("/d/x/a.tar", A, n);
    check(ars_cmd(tar_main, "tar xf /d/x/a.tar -C /d") == 2 && has(ars_stderr(), "the archive"), "by another path, from another folder");
    ars_reset();
    ars_puts("/s/b.tgz", "NEW CONTENTS\n");
    check(ars_cmd(tar_main, "tar czf /b.tgz -C /s b.tgz") == 0, "a .tgz with a member of its own name");
    {
        const unsigned char *p = ars_get("/b.tgz", &n);
        copy = malloc(n);
        memcpy(copy, p, n);
    }
    ars_reset();
    ars_put("/d/b.tgz", copy, n);
    check(ars_cmd(tar_main, "tar xzf /d/b.tgz -C /d") == 2 && has(ars_stderr(), "the archive"), "through gzip");
    free(copy);
    /* a file of its size, under another name, is no such thing */
    a_start();
    a_text("other.tar", "replaced\n");
    a_end();
    ars_reset();
    ars_put("/d/a.tar", A, An);
    ars_put("/d/other.tar", A, An);
    check(ars_cmd(tar_main, "tar xf /d/a.tar -C /d") == 0 && ars_is("/d/other.tar", "replaced\n"), "another file is replaced as ever");
}

/**** an archive that exists ****/

/* It is replaced only by one that went right: the old one may be the only copy of what the new one could not read. */
static void old_stands(const char *cmd, const char *what) {
    char msg[160];
    check(ars_cmd(tar_main, cmd) == 2, what);
    snprintf(msg, sizeof(msg), "%s: the archive that was there stands", what);
    check(ars_is("/d/a.tar", "the old archive"), msg);
    snprintf(msg, sizeof(msg), "%s: and that is said", what);
    check(has(ars_stderr(), "stays as it was"), msg);
    snprintf(msg, sizeof(msg), "%s: and no .tmp is left", what);
    check(!has(ars_list(), ".tmp"), msg);
}

static void old_tree(void) {
    ars_reset();
    ars_puts("/d/a.tar", "the old archive");
    ars_puts("/src/one.txt", "first\n");
    ars_puts("/src/two.txt", "second\n");
    ars_chdir("/d");
}

static void test_old_archive(void) {
    old_tree();
    old_stands("tar cf /d/a.tar /missing", "a name that is not there");
    old_tree();
    old_stands("tar cf /d/a.tar /missing /src", "one good name and one bad");
    old_tree();
    old_stands("tar czf /d/a.tar /missing", "the same through gzip");
    old_tree();
    ars_lie_size("/src/one.txt", 5000);
    old_stands("tar cf /d/a.tar /src", "a file that shrank");
    old_tree();
    ars_mkdir("/src/damaged");
    ars_unreadable("/src/damaged");
    old_stands("tar cf /d/a.tar /src", "a folder that cannot be read");
    old_tree();
    ars_unreadable("/src/two.txt");
    old_stands("tar cf /d/a.tar /src", "a file that cannot be read");
    old_tree();
    old_stands("tar cf a.tar a.tar", "the archive as its only name");
    old_tree();
    {
        char name[220], path[240];
        memset(name, 'n', 200);
        name[200] = 0;
        snprintf(path, sizeof(path), "/src/%s", name);
        ars_puts(path, "a name that will not go in ustar\n");
    }
    old_stands("tar cf /d/a.tar -C / src", "a name that does not fit");

    /* nothing wrong: replaced */
    old_tree();
    check(ars_cmd(tar_main, "tar cf /d/a.tar -C / src") == 0 && !ars_is("/d/a.tar", "the old archive"), "with nothing wrong an archive is replaced");
    /* no archive there: what was made is all there is, and it stays, with the trouble said */
    ars_reset();
    ars_puts("/src/one.txt", "first\n");
    check(ars_cmd(tar_main, "tar cf /new.tar /missing /src") == 2 && ars_exists("/new.tar"), "a new archive stays, trouble or not: it replaces nothing");
}

/**** the host's tools ****/

static void make_export(const char *dir) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/tree/sub/deep' '%s/tree/empty'", dir, dir);
    if(system(cmd)) abort();
    {
        char sub[512];
        static char big[70000];
        memset(big, 'z', sizeof(big));
        snprintf(sub, sizeof(sub), "%s/tree", dir);
        host_write(sub, "one.txt", "first file\n", 11);
        host_write(sub, "zero", "", 0);
        host_write(sub, "sub/two.txt", "second file\n", 12);
        host_write(sub, "sub/big", big, sizeof(big));
        host_write(sub, "sub/deep/three.txt", "third\n", 6);
        host_write(sub, "h\xc3\xa9llo.txt", "accented\n", 9);
    }
}

/* the export's tree in the simulation under /src */
static void import_tree(void) {
    static char big[70000];
    memset(big, 'z', sizeof(big));
    ars_reset();
    ars_puts("/src/one.txt", "first file\n");
    ars_put("/src/zero", "", 0);
    ars_puts("/src/sub/two.txt", "second file\n");
    ars_put("/src/sub/big", big, sizeof(big));
    ars_puts("/src/sub/deep/three.txt", "third\n");
    ars_puts("/src/h\xc3\xa9llo.txt", "accented\n");
    ars_mkdir("/src/empty");
}

static int imported_ok(const char *root) {
    char p[128];
    size_t n = 0;
    const unsigned char *q;
    snprintf(p, sizeof(p), "%s/one.txt", root);
    if(!ars_is(p, "first file\n")) return 0;
    snprintf(p, sizeof(p), "%s/zero", root);
    if(!ars_is(p, "")) return 0;
    snprintf(p, sizeof(p), "%s/sub/two.txt", root);
    if(!ars_is(p, "second file\n")) return 0;
    snprintf(p, sizeof(p), "%s/sub/deep/three.txt", root);
    if(!ars_is(p, "third\n")) return 0;
    snprintf(p, sizeof(p), "%s/h\xc3\xa9llo.txt", root);
    if(!ars_is(p, "accented\n")) return 0;
    snprintf(p, sizeof(p), "%s/sub/big", root);
    q = ars_get(p, &n);
    if(!q || n != 70000 || q[69999] != 'z') return 0;
    snprintf(p, sizeof(p), "%s/empty", root);
    return ars_is_dir(p);
}

static void test_host_tar(void) {
    const char *tar = host_tool("tar"), *py = host_tool("python3");
    char dir[256], cmd[2048];
    int z;
    if(!tar) {
        printf("test_tar: no tar on this host: the comparison is skipped\n");
    } else {
        host_tmpdir(dir);
        make_export(dir);
        /* the host's, read by ours */
        for(z = 0; z < 2; z++) {
            static const char *const formats[] = { "ustar", "pax" };
            unsigned f;
            for(f = 0; f < 2; f++) {
                unsigned char *a;
                size_t n;
                snprintf(cmd, sizeof(cmd), "COPYFILE_DISABLE=1 tar --format=%s -c%sf '%s/host.tar' -C '%s/tree' . 2>/dev/null", formats[f], z ? "z" : "", dir, dir);
                check(system(cmd) == 0, "the host's tar makes an archive");
                a = host_read(dir, "host.tar", &n);
                ars_reset();
                ars_put("/a.tar", a, n);
                ars_mkdir("/dest");
                check(ars_cmd(tar_main, z ? "tar xzf /a.tar -C /dest" : "tar xf /a.tar -C /dest") == 0 && imported_ok("/dest"), "ours unpacks what the host's tar made");
                if(!imported_ok("/dest")) printf("(%s)\n%s\n", formats[f], ars_stderr());
                free(a);
            }
        }
        /* a piped tar's gzip, which GNU tar pads with zeros */
        snprintf(cmd, sizeof(cmd), "COPYFILE_DISABLE=1 tar -czf - -C '%s/tree' . 2>/dev/null > '%s/piped.tgz'", dir, dir);
        check(system(cmd) == 0, "a piped tar czf -");
        {
            size_t n;
            unsigned char *a = host_read(dir, "piped.tgz", &n);
            ars_reset();
            ars_put("/a.tgz", a, n);
            ars_mkdir("/dest");
            check(ars_cmd(tar_main, "tar xzf /a.tgz -C /dest") == 0 && imported_ok("/dest"), "ours unpacks it");
            free(a);
        }
        /* ours, read by the host's */
        for(z = 0; z < 2; z++) {
            const unsigned char *p;
            size_t n;
            import_tree();
            check(ars_cmd(tar_main, z ? "tar czf /a.tar -C /src ." : "tar cf /a.tar -C /src .") == 0, "ours makes an archive");
            p = ars_get("/a.tar", &n);
            host_write(dir, "ours.tar", p, n);
            snprintf(cmd, sizeof(cmd), "rm -rf '%s/out' && mkdir '%s/out' && tar -x%sf '%s/ours.tar' -C '%s/out' 2>/dev/null && diff -r '%s/tree' '%s/out'",
                     dir, dir, z ? "z" : "", dir, dir, dir, dir);
            check(system(cmd) == 0, "the host's tar unpacks what ours made, the same tree");
        }
        host_rmdir(dir);
    }
    if(!py) {
        printf("test_tar: no python3 on this host: the tarfile comparison is skipped\n");
        return;
    }
    host_tmpdir(dir);
    {
        /* Python writes each format the same way on any host: long names, pax records, GNU's. */
        static const char *const formats[] = { "tarfile.USTAR_FORMAT", "tarfile.GNU_FORMAT", "tarfile.PAX_FORMAT" };
        unsigned f;
        for(f = 0; f < 3; f++) {
            size_t n;
            unsigned char *a;
            const unsigned char *p;
            snprintf(cmd, sizeof(cmd),
                     "'%s' -I -c \"import sys, tarfile, io\n"
                     "f = tarfile.open(sys.argv[1], 'w', format=%s)\n"
                     "def add(name, data):\n"
                     "    t = tarfile.TarInfo(name); t.size = len(data); t.mtime = 1700000000\n"
                     "    f.addfile(t, io.BytesIO(data))\n"
                     "add('short.txt', b'short\\n')\n"
                     "add('d' * 60 + '/' + 'e' * 60 + '/' + 'f' * 60 + '.txt', b'long\\n')\n"
                     "t = tarfile.TarInfo('caf\\u00e9/na\\u00efve.txt'); t.size = 4; f.addfile(t, io.BytesIO(b'utf\\n'))\n"
                     "t = tarfile.TarInfo('link'); t.type = tarfile.SYMTYPE; t.linkname = 'short.txt'; f.addfile(t)\n"
                     "t = tarfile.TarInfo('folder'); t.type = tarfile.DIRTYPE; f.addfile(t)\n"
                     "add('after.txt', b'after\\n')\n"
                     "f.close()\n\" '%s/py.tar'", py, formats[f], dir);
            check(system(cmd) == 0, "Python makes an archive");
            a = host_read(dir, "py.tar", &n);
            ars_reset();
            ars_put("/a.tar", a, n);
            ars_mkdir("/dest");
            check(ars_cmd(tar_main, "tar xf /a.tar -C /dest") == 2 && has(ars_stderr(), "symbolic link"), "ours reads it, and skips the link");
            check(ars_is("/dest/short.txt", "short\n") && ars_is("/dest/after.txt", "after\n") && ars_is_dir("/dest/folder"), "... the plain entries");
            check(ars_is("/dest/caf\xc3\xa9/na\xc3\xafve.txt", "utf\n"), "... UTF-8 names");
            {
                char name[200];
                memset(name, 0, sizeof(name));
                strcpy(name, "/dest/");
                memset(name + 6, 'd', 60);
                name[66] = '/';
                memset(name + 67, 'e', 60);
                name[127] = '/';
                memset(name + 128, 'f', 60);
                strcpy(name + 188, ".txt");
                check(ars_is(name, "long\n"), "... the long name, in the format's own way");
            }
            free(a);
            /* ours, read by Python */
            import_tree();
            check(ars_cmd(tar_main, "tar cf /a.tar -C /src .") == 0, "ours makes one");
            p = ars_get("/a.tar", &n);
            host_write(dir, "ours.tar", p, n);
            snprintf(cmd, sizeof(cmd),
                     "'%s' -I -c \"import sys, tarfile\n"
                     "f = tarfile.open(sys.argv[1])\n"
                     "n = {m.name: m for m in f}\n"
                     "assert f.extractfile(n['./sub/two.txt']).read() == b'second file\\n'\n"
                     "assert n['./sub/big'].size == 70000 and n['./empty'].isdir() and n['./zero'].size == 0\n"
                     "assert n['./h\\u00e9llo.txt'].size == 9\n\" '%s/ours.tar'", py, dir);
            check(system(cmd) == 0, "Python reads what ours made");
        }
    }
    host_rmdir(dir);
}

int main(void) {
    test_names();
    test_tool();
    test_formats();
    test_refusals();
    test_damage();
    test_failures();
    test_old_archive();
    test_pax_entry();
    test_c1();
    test_self();
    test_host_tar();
    free(A);
    return report("test_tar");
}
