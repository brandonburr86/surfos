/* Host-side test of gzip, gunzip and zcat (11/01/26): user/gzip/ as it is, on the simulated FAT of tests/host/ar_sim.c.
   Covered: what the options do; what happens to the files: an output that exists, a refused name, the input going only
   once the output is safe, with a change to the disk failing at every step, the disk pulled at every step and the disk
   filling up; a rename left half done, a twin of the input; the format: each header flag, and each kind of damage,
   every cut of a file, mutated files; a reader that goes away; and the host's own gzip both ways, and Python's zlib for
   the block types gzip does not make, where they are installed (each comparison says so when it is skipped). The tools
   are run under AddressSanitizer and UBSan by tests/archive-sanitize.sh on the Mac: never in `make unittest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ar_sim.h"
#include "ar_host.h"

int gz_tool(int argc, char **argv, const char *prog, int decompress, int to_stdout);

static int gzip_main(int argc, char **argv) { return gz_tool(argc, argv, "gzip", 0, 0); }
static int gunzip_main(int argc, char **argv) { return gz_tool(argc, argv, "gunzip", 1, 0); }
static int zcat_main(int argc, char **argv) { return gz_tool(argc, argv, "zcat", 1, 1); }

static const char sample[] = "the quick brown fox jumps over the lazy dog, the quick brown fox again\n";

/* what gzip makes of these bytes, in *n; the buffer is the test's until the next call */
static unsigned char *packed(const void *data, size_t size, size_t *n) {
    static unsigned char *keep;
    const unsigned char *p;
    ars_reset();
    ars_put("/in", data, size);
    if(ars_cmd(gzip_main, "gzip -c /in")) check(0, "the fixture is made");
    p = ars_stdout(n);
    keep = realloc(keep, *n + 1);
    memcpy(keep, p, *n);
    return keep;
}

static int has(const char *text, const char *part) {
    return strstr(text, part) != NULL;
}

static void test_options(void) {
    size_t n, m;
    const unsigned char *p;
    unsigned char *one;

    ars_reset();
    ars_puts("/a.txt", sample);
    check(ars_cmd(gzip_main, "gzip /a.txt") == 0 && ars_exists("/a.txt.gz") && !ars_exists("/a.txt"), "gzip: a.txt becomes a.txt.gz");
    check(!ars_get("/a.txt.gz", &n) ? 0 : n > 18, "... with something in it");
    check(ars_cmd(gunzip_main, "gunzip /a.txt.gz") == 0 && ars_is("/a.txt", sample) && !ars_exists("/a.txt.gz"),
          "gunzip: and back, the same bytes");
    check(ars_count() == 1, "nothing else is left on the disk");
    check(!ars_open_files(), "and no descriptor open");

    ars_reset();
    ars_puts("/a", sample);
    check(ars_cmd(gzip_main, "gzip -k /a") == 0 && ars_is("/a", sample) && ars_exists("/a.gz"), "-k keeps the input");
    check(ars_cmd(gzip_main, "gzip -kf /a") == 0 && ars_is("/a", sample), "-kf together");
    ars_reset();
    ars_puts("/a", sample);
    check(ars_cmd(gzip_main, "gzip -c /a") == 0 && ars_is("/a", sample) && !ars_exists("/a.gz"), "-c: to standard output, input kept");
    p = ars_stdout(&n);
    check(n > 18 && p[0] == 0x1f && p[1] == 0x8b, "... and what came out is gzip");
    ars_put("/b.gz", p, n);
    check(ars_cmd(gunzip_main, "gunzip -c /b.gz") == 0 && ars_exists("/b.gz"), "gunzip -c keeps its input");
    p = ars_stdout(&m);
    check(m == strlen(sample) && !memcmp(p, sample, m), "... and writes the data");
    check(ars_cmd(zcat_main, "zcat /b.gz") == 0 && ars_exists("/b.gz"), "zcat");
    p = ars_stdout(&m);
    check(m == strlen(sample) && !memcmp(p, sample, m), "... the same");
    check(ars_cmd(gunzip_main, "gunzip -k /b.gz") == 0 && ars_is("/b", sample) && ars_exists("/b.gz"), "gunzip -k");
    check(ars_cmd(gzip_main, "gzip -d /b.gz") == 2 && has(ars_stderr(), "already exists"), "gzip -d is gunzip, and refuses as it does");

    ars_reset();
    ars_puts("/a", sample);
    check(ars_cmd(gzip_main, "gzip /a") == 0, "made");
    {
        size_t k;
        const unsigned char *q = ars_get("/a.gz", &k);
        unsigned char *copy = malloc(k);
        memcpy(copy, q, k);
        ars_put("/A.TGZ", copy, k);
        ars_put("/C.GZ", copy, k);
        ars_put("/d.Gz", copy, k);
        ars_put("/noext", copy, k);
        ars_mkdir("/sub");
        ars_put("/sub/.gz", copy, k);
        check(ars_cmd(gunzip_main, "gunzip /A.TGZ") == 0 && ars_exists("/A.tar") && !ars_exists("/A.TGZ"), ".tgz becomes .tar");
        check(ars_cmd(gunzip_main, "gunzip /C.GZ") == 0 && ars_exists("/C"), "the suffix in capitals: FAT has no case");
        check(ars_cmd(gunzip_main, "gunzip /d.Gz") == 0 && ars_exists("/d"), "or in both");
        check(ars_cmd(gunzip_main, "gunzip /noext") == 2 && ars_exists("/noext") && has(ars_stderr(), ".gz"), "no suffix: refused, and said");
        check(ars_cmd(gunzip_main, "gunzip /sub/.gz") == 2 && ars_exists("/sub/.gz"), "a suffix and no name: refused");
        check(ars_cmd(zcat_main, "zcat /noext") == 0, "zcat wants no suffix");
        ars_stdin(copy, k, 1, 0);
        check(ars_cmd(gunzip_main, "gunzip") == 0, "gunzip with no file: a pipe in, standard output out");
        p = ars_stdout(&m);
        check(m == strlen(sample) && !memcmp(p, sample, m), "... the data");
        ars_stdin(copy, k, 1, 1);
        check(ars_cmd(zcat_main, "zcat -") == 0, "- is standard input, one byte a read");
        p = ars_stdout(&m);
        check(m == strlen(sample) && !memcmp(p, sample, m), "... the data");
        ars_stdin(copy, k, 1, 0);
        check(ars_cmd(gunzip_main, "gunzip -t") == 0, "-t from standard input");
        ars_stdout(&m);
        check(!m, "... says nothing");
        free(copy);
    }

    ars_reset();
    ars_puts("/a", sample);
    ars_puts("/a.gz", "mine");
    check(ars_cmd(gzip_main, "gzip /a") == 2 && ars_is("/a.gz", "mine") && ars_is("/a", sample) && has(ars_stderr(), "-f"),
          "an output that exists stays, and so does the input; -f is named");
    check(ars_cmd(gzip_main, "gzip -f /a") == 0 && !ars_is("/a.gz", "mine") && !ars_exists("/a") && ars_count() == 1,
          "-f replaces it, and no .tmp is left");
    check(ars_cmd(gzip_main, "gzip /a.gz") == 2 && has(ars_stderr(), "already ends in .gz") && ars_exists("/a.gz"),
          "a name that ends in .gz is left alone");
    check(ars_cmd(gzip_main, "gzip -f /a.gz") == 0 && ars_exists("/a.gz.gz"), "-f goes on");
    ars_reset();
    ars_mkdir("/d");
    ars_puts("/e", sample);
    check(ars_cmd(gzip_main, "gzip /d /nothere /e") == 2 && ars_exists("/e.gz") && has(ars_stderr(), "/d") && has(ars_stderr(), "/nothere"),
          "a folder and a missing file are said, and the file after them is done");
    ars_reset();
    ars_puts("/a", sample);
    ars_mkdir("/a.gz");
    check(ars_cmd(gzip_main, "gzip -f /a") == 2 && ars_is("/a", sample) && ars_is_dir("/a.gz") && ars_count() == 2,
          "-f does not replace a folder");

    ars_reset();
    ars_puts("/a", sample);
    ars_ttys(1, 1);
    check(ars_cmd(gzip_main, "gzip -c /a") == 2 && has(ars_stderr(), "terminal"), "-c does not put compressed data on a terminal");
    check(ars_cmd(gzip_main, "gzip -cf /a") == 0, "unless -f");
    ars_stdin(sample, strlen(sample), 1, 0);
    check(ars_cmd(gzip_main, "gzip") == 2 && has(ars_stderr(), "terminal"), "compressed data is not written to a terminal");
    check(ars_cmd(gzip_main, "gzip -f") == 0, "unless -f");
    check(ars_cmd(gunzip_main, "gunzip") == 2 && has(ars_stderr(), "terminal"), "nor read from one");
    ars_ttys(0, 0);
    check(ars_cmd(gzip_main, "gzip -x") == 2 && has(ars_stderr(), "usage"), "an unknown option: usage, status 2");

    /* an option is an option wherever it stands: all are read before anything is touched */
    ars_reset();
    ars_puts("/x", sample);
    check(ars_cmd(gzip_main, "gzip /x -k") == 0 && ars_is("/x", sample) && ars_exists("/x.gz"), "gzip x -k keeps x");
    ars_reset();
    ars_puts("/x", sample);
    ars_puts("/y", sample);
    check(ars_cmd(gzip_main, "gzip /x -k /y") == 0 && ars_is("/x", sample) && ars_is("/y", sample), "and between two names keeps both");
    ars_reset();
    ars_puts("/x", sample);
    check(ars_cmd(gzip_main, "gzip /x -c") == 0 && ars_is("/x", sample) && !ars_exists("/x.gz"), "gzip x -c writes to standard output");
    ars_reset();
    ars_puts("/x", sample);
    check(ars_cmd(gzip_main, "gzip /x -Q") == 2 && has(ars_stderr(), "usage") && ars_is("/x", sample) && !ars_exists("/x.gz"), "a bad option after a name: usage, and nothing touched");
    ars_reset();
    ars_puts("/x", sample);
    ars_cmd(gzip_main, "gzip /x");
    check(ars_cmd(gunzip_main, "gunzip /x.gz -k") == 0 && ars_exists("/x.gz") && ars_is("/x", sample), "gunzip x.gz -k keeps it");
    check(ars_cmd(zcat_main, "zcat /x.gz -t") == 0, "zcat x.gz -t only checks");
    ars_stdout(&n);
    check(!n, "... and writes nothing");
    check(ars_cmd(gunzip_main, "gunzip /x.gz -f -t") == 0 && ars_exists("/x.gz"), "two options after a name");
    ars_reset();
    ars_puts("/d/-k", sample);
    ars_chdir("/d");
    check(ars_cmd(gzip_main, "gzip -- -k") == 0 && ars_exists("/d/-k.gz") && !ars_exists("/d/-k"), "after -- a name is a name, however it looks");

    /* several members */
    {
        size_t a, b;
        unsigned char *pa = packed("first ", 6, &a), *both = malloc(a + 200);
        memcpy(both, pa, a);
        pa = packed("second\n", 7, &b);
        memcpy(both + a, pa, b);
        ars_reset();
        ars_put("/two.gz", both, a + b);
        check(ars_cmd(gunzip_main, "gunzip /two.gz") == 0 && ars_is("/two", "first second\n"), "two members: both, in turn");
        free(both);
        ars_reset();
        ars_puts("/a", "one\n");
        ars_puts("/b", "two\n");
        check(ars_cmd(gzip_main, "gzip -c /a /b") == 0, "-c with two files");
        one = malloc(4096);
        {
            const unsigned char *from = ars_stdout(&a);
            memcpy(one, from, a);
        }
        ars_put("/c.gz", one, a);
        check(ars_cmd(zcat_main, "zcat /c.gz") == 0, "... makes two members");
        p = ars_stdout(&m);
        check(m == 8 && !memcmp(p, "one\ntwo\n", 8), "... one after the other");
        free(one);
    }

    /* levels */
    {
        static unsigned char text[200000];
        size_t i, last = 0;
        const char *const levels[] = { "-1", "-2", "-3", "-4", "-5", "-6", "-7", "-8", "-9" };
        unsigned r = 1;
        for(i = 0; i < sizeof(text); i++) {
            r = r * 1103515245 + 12345;
            text[i] = (unsigned char)"aaaabbcdeefghiijklmnoopqrstuuvwxyz  \n"[(r >> 16) % 37];
        }
        for(i = 0; i < 9; i++) {
            const char *argv[] = { "gzip", levels[i], "-c", "/t", NULL };
            size_t n1, n2;
            unsigned char *z;
            ars_reset();
            ars_put("/t", text, sizeof(text));
            check(ars_run(gzip_main, argv) == 0, "a level");
            n1 = 0;
            z = malloc(sizeof(text) * 2);
            {
                const unsigned char *from = ars_stdout(&n1);
                memcpy(z, from, n1);
            }
            ars_put("/t.gz", z, n1);
            check(ars_cmd(zcat_main, "zcat /t.gz") == 0, "... that unpacks");
            check(!memcmp(ars_stdout(&n2), text, sizeof(text)) && n2 == sizeof(text), "... to the data");
            if(i == 0) last = n1;
            if(i == 8) check(n1 < last, "-9 is smaller than -1 here");
            free(z);
        }
    }
}

/* a gzip file by hand: flags, the optional fields, one stored block of "hi" */
static size_t by_hand(unsigned char *o, int flags, const char *name, const char *comment, int extra, int hcrc_wrong) {
    extern unsigned int ar_crc32(unsigned int, const void *, size_t);
    size_t n = 0;
    unsigned int c;
    int i;
    o[n++] = 0x1f; o[n++] = 0x8b; o[n++] = 8; o[n++] = (unsigned char)flags;
    for(i = 0; i < 6; i++) o[n++] = 0;
    if(extra >= 0) {
        o[n++] = (unsigned char)extra; o[n++] = (unsigned char)(extra >> 8);
        memset(o + n, 'x', (size_t)extra);
        n += (size_t)extra;
    }
    if(name) { strcpy((char *)o + n, name); n += strlen(name) + 1; }
    if(comment) { strcpy((char *)o + n, comment); n += strlen(comment) + 1; }
    if(flags & 2) {
        c = ar_crc32(0, o, n) ^ (unsigned)hcrc_wrong;
        o[n++] = (unsigned char)c; o[n++] = (unsigned char)(c >> 8);
    }
    o[n++] = 1; o[n++] = 2; o[n++] = 0; o[n++] = 0xfd; o[n++] = 0xff; o[n++] = 'h'; o[n++] = 'i';
    c = ar_crc32(0, "hi", 2);
    for(i = 0; i < 4; i++) o[n++] = (unsigned char)(c >> (8 * i));
    o[n++] = 2; o[n++] = 0; o[n++] = 0; o[n++] = 0;
    return n;
}

static int takes(const unsigned char *f, size_t n) {
    ars_reset();
    ars_put("/x.gz", f, n);
    return ars_cmd(zcat_main, "zcat /x.gz");
}

/* refused: status 2, and gunzip leaves the input and no output */
static void refuses(const unsigned char *f, size_t n, const char *why, const char *what) {
    ars_reset();
    ars_put("/x.gz", f, n);
    check(ars_cmd(gunzip_main, "gunzip /x.gz") == 2 && ars_exists("/x.gz") && !ars_exists("/x"), what);
    if(why) check(has(ars_stderr(), why), what);
}

static void test_format(void) {
    static unsigned char f[1 << 21], g[1 << 21];
    size_t n, i;
    n = by_hand(f, 0, NULL, NULL, -1, 0);
    check(takes(f, n) == 0, "a plain header");
    n = by_hand(f, 1, NULL, NULL, -1, 0);
    check(takes(f, n) == 0, "FTEXT means nothing here");
    n = by_hand(f, 8, "name.txt", NULL, -1, 0);
    check(takes(f, n) == 0, "FNAME");
    n = by_hand(f, 16, NULL, "a comment", -1, 0);
    check(takes(f, n) == 0, "FCOMMENT");
    n = by_hand(f, 4, NULL, NULL, 300, 0);
    check(takes(f, n) == 0, "FEXTRA");
    n = by_hand(f, 4, NULL, NULL, 0, 0);
    check(takes(f, n) == 0, "FEXTRA of nothing");
    n = by_hand(f, 2, NULL, NULL, -1, 0);
    check(takes(f, n) == 0, "FHCRC, right");
    n = by_hand(f, 2 | 4 | 8 | 16, "n", "c", 5, 0);
    check(takes(f, n) == 0, "all of them, FHCRC over all of them");
    {
        size_t m;
        const unsigned char *p = ars_stdout(&m);
        check(m == 2 && !memcmp(p, "hi", 2), "... and the data comes through");
    }
    n = by_hand(f, 2, NULL, NULL, -1, 1);
    refuses(f, n, "header", "FHCRC, wrong");
    n = by_hand(f, 2 | 8, "n", NULL, -1, 0x100);
    refuses(f, n, "header", "FHCRC, wrong in its high byte");
    for(i = 0x20; i <= 0x80; i <<= 1) {
        n = by_hand(f, (int)i, NULL, NULL, -1, 0);
        refuses(f, n, "flags", "a reserved flag");
    }
    n = by_hand(f, 0, NULL, NULL, -1, 0);
    f[2] = 7;
    refuses(f, n, "method", "a compression method other than deflate");
    n = by_hand(f, 0, NULL, NULL, -1, 0);
    f[1] = 0x8c;
    refuses(f, n, "not in gzip format", "the second magic byte");
    f[0] = 0x1e;
    refuses(f, n, "not in gzip format", "the first magic byte");
    refuses((const unsigned char *)"", 0, "not in gzip format", "an empty file");
    refuses((const unsigned char *)"\x1f", 1, "not in gzip format", "one byte of magic");
    refuses((const unsigned char *)"plain text, not compressed at all\n", 34, "not in gzip format", "plain text");

    /* the trailer */
    n = by_hand(f, 0, NULL, NULL, -1, 0);
    f[n - 8] ^= 1;
    refuses(f, n, "CRC", "a CRC off by one bit");
    f[n - 8] ^= 1;
    f[n - 1] ^= 0x80;
    refuses(f, n, "length", "a length off by one bit");
    f[n - 1] ^= 0x80;

    /* what follows */
    memcpy(f + n, "garbage", 7);
    refuses(f, n + 7, "after the end", "trailing garbage");
    memset(f + n, 0, 512);
    check(takes(f, n + 512) == 0, "zeros after the end: a piped tar's padding, as GNU gzip takes it");
    f[n + 511] = 1;
    refuses(f, n + 512, "after the end", "zeros and then something");
    f[n + 511] = 0;
    f[n] = 0;
    f[n + 1] = 0x1f;
    f[n + 2] = 0x8b;
    refuses(f, n + 512, "after the end", "a member after zeros");
    memcpy(f + n, f, n);                                /* a second member */
    check(takes(f, n * 2) == 0, "two members by hand");
    refuses(f, n * 2 - 1, "too early", "the second member cut short");
    f[2 * n - 8] ^= 4;
    refuses(f, n * 2, "CRC", "the second member's CRC");

    /* every cut */
    n = by_hand(f, 2 | 4 | 8 | 16, "name", "comment", 5, 0);
    for(i = 0; i < n; i++) refuses(f, i, NULL, "a file cut at each byte");
    {
        static char plain[300000];
        unsigned r = 7;
        unsigned char *z;
        for(i = 0; i < sizeof(plain); i++) {
            r = r * 1103515245 + 12345;
            plain[i] = (i >> 10) & 1 ? (char)(r >> 16) : "abcdefgh"[(r >> 16) & 7];
        }
        z = packed(plain, sizeof(plain), &n);
        memcpy(f, z, n);
        for(i = 0; i < n; i += 1 + i / 20) refuses(f, i, NULL, "a large file cut along its length");
        refuses(f, n - 1, "too early", "cut one byte from the end");

        /* mutated: refused, or the same data (the CRC is what stands between); nothing else */
        for(i = 0; i < 400; i++) {
            size_t m, at;
            int k;
            memcpy(g, f, n);
            r = r * 1103515245 + 12345;
            for(k = 1 + (int)(r >> 28); k > 0; k--) {
                r = r * 1103515245 + 12345;
                at = (r >> 8) % (i & 1 ? n : 64);
                g[at] ^= (unsigned char)(1u << (r & 7));
            }
            ars_reset();
            ars_put("/x.gz", g, n);
            k = ars_cmd(zcat_main, "zcat /x.gz");
            if(k != 2 && k != 0) check(0, "a mutated file: status 0 or 2");
            if(k == 0) {
                const unsigned char *p = ars_stdout(&m);
                if(m != sizeof(plain) || memcmp(p, plain, m)) check(0, "a mutated file accepted must be the same data");
            }
        }
        check(1, "400 mutated files: refused, or the same data");
    }

    /* a header's name of 1 MB is only read past */
    n = 0;
    memcpy(f, "\x1f\x8b\x08\x08\0\0\0\0\0\xff", 10);
    n = 10;
    memset(f + n, 'n', 1 << 20);
    n += 1 << 20;
    f[n++] = 0;
    memcpy(f + n, "\x01\x02\0\xfd\xff" "hi", 7);
    n += 7;
    {
        extern unsigned int ar_crc32(unsigned int, const void *, size_t);
        unsigned c = ar_crc32(0, "hi", 2);
        int k;
        for(k = 0; k < 4; k++) f[n++] = (unsigned char)(c >> (8 * k));
        f[n++] = 2; f[n++] = 0; f[n++] = 0; f[n++] = 0;
    }
    check(takes(f, n) == 0, "a header's name of 1 MB");

    /* a bomb stops where the reader stops */
    {
        static char zeros[4 << 20];
        unsigned char *z = packed(zeros, sizeof(zeros), &n);
        ars_reset();
        ars_put("/b.gz", z, n);
        ars_stdout_closes_after(100);
        check(ars_cmd(zcat_main, "zcat /b.gz") == 141, "a reader that goes: the program ends, as on a closed pipe (status 141)");
        check(ars_count() == 1, "... leaving nothing behind");
    }
}

/* one run with the kth change failing, or the disk gone from it */
static void test_failures(void) {
    static const int modes[] = { 0, 1 };
    size_t zn, whole = 0;
    unsigned char *z;
    int total, k, m;

    z = malloc(4096);
    {
        const unsigned char *made = packed(sample, strlen(sample), &zn);        /* it sets zn: not in memcpy's arguments */
        memcpy(z, made, zn);
    }
    check(zn > 18 && z[0] == 0x1f, "the fixture for the failures is a gzip file");

    /* gzip, a new output */
    ars_reset();
    ars_puts("/a", sample);
    ars_cmd(gzip_main, "gzip /a");
    total = ars_changes();
    check(total >= 4, "gzip changes the disk in several steps");
    for(m = 0; m < 2; m++) {
        for(k = 1; k <= total; k++) {
            int st;
            ars_reset();
            ars_puts("/a", sample);
            if(modes[m]) ars_pull_at(k);
            else ars_fail_change(k, ARS_EIO);
            st = ars_cmd(gzip_main, "gzip /a");
            if(st == 0) {
                check(!ars_exists("/a") && ars_exists("/a.gz"), "gzip that says it worked did");
            } else {
                check(st == 2 && ars_is("/a", sample), "gzip that failed left the input");
                check(k == total || !ars_exists("/a.gz") || m, "... and no output, unless the input's own deletion failed");
            }
            if(ars_exists("/a") == 0 && !ars_exists("/a.gz")) check(0, "the data is somewhere, at every step");
        }
    }

    /* gunzip likewise */
    ars_reset();
    ars_put("/a.gz", z, zn);
    ars_cmd(gunzip_main, "gunzip /a.gz");
    total = ars_changes();
    for(m = 0; m < 2; m++) {
        for(k = 1; k <= total; k++) {
            int st;
            ars_reset();
            ars_put("/a.gz", z, zn);
            if(modes[m]) ars_pull_at(k);
            else ars_fail_change(k, ARS_EIO);
            st = ars_cmd(gunzip_main, "gunzip /a.gz");
            if(st == 0) check(ars_is("/a", sample) && !ars_exists("/a.gz"), "gunzip that says it worked did");
            else check(st == 2 && ars_exists("/a.gz"), "gunzip that failed left the input");
            if(ars_exists("/a.gz") == 0 && !ars_is("/a", sample)) check(0, "the data is somewhere, at every step");
        }
    }

    /* -f over a file: it is the old one or the new one, never a half */
    ars_reset();
    ars_puts("/a", sample);
    ars_puts("/a.gz", "the old one");
    ars_cmd(gzip_main, "gzip -f /a");
    total = ars_changes();
    for(k = 1; k <= total; k++) {
        int st;
        size_t n = 0;
        const unsigned char *p;
        ars_reset();
        ars_puts("/a", sample);
        ars_puts("/a.gz", "the old one");
        ars_fail_change(k, ARS_EIO);
        st = ars_cmd(gzip_main, "gzip -f /a");
        p = ars_get("/a.gz", &n);
        check(p && (ars_is("/a.gz", "the old one") || (n > 18 && p[0] == 0x1f)), "-f: the old file or the whole new one");
        if(st) check(ars_is("/a", sample), "... and the input stays when it says it failed");
    }

    /* a full disk */
    ars_reset();
    ars_puts("/a", sample);
    ars_quota(strlen(sample) + 5);
    check(ars_cmd(gzip_main, "gzip /a") == 2 && ars_is("/a", sample) && !ars_exists("/a.gz") && has(ars_stderr(), "No space"),
          "a full disk: said, the input stays, the partial output goes");
    ars_reset();
    ars_put("/a.gz", z, zn);
    ars_quota(zn + 3);
    check(ars_cmd(gunzip_main, "gunzip /a.gz") == 2 && ars_exists("/a.gz") && !ars_exists("/a"), "the same for gunzip");

    /* the disk gone with the output part made: it cannot be deleted either, and nobody is left to think it is whole */
    ars_reset();
    ars_puts("/a", sample);
    ars_cmd(gzip_main, "gzip /a");
    total = ars_changes();
    ars_get("/a.gz", &whole);
    for(k = 1; k <= total; k++) {
        size_t now = 0;
        int st;
        ars_reset();
        ars_puts("/a", sample);
        ars_pull_at(k);
        st = ars_cmd(gzip_main, "gzip /a");
        if(st == 2 && ars_get("/a.gz", &now) && now < whole) check(has(ars_stderr(), "stays"), "a short .gz that could not be deleted is said");
    }
    ars_reset();
    ars_puts("/a", sample);
    ars_puts("/a.gz", "the old one");
    ars_cmd(gzip_main, "gzip -f /a");
    total = ars_changes();
    for(k = 1; k <= total; k++) {
        size_t now = 0;
        int st;
        ars_reset();
        ars_puts("/a", sample);
        ars_puts("/a.gz", "the old one");
        ars_pull_at(k);
        st = ars_cmd(gzip_main, "gzip -f /a");
        if(st == 2 && ars_exists("/a.gz.tmp")) check(has(ars_stderr(), "stays") || has(ars_stderr(), "a.gz.tmp"), "a .tmp that could not be deleted is said");
        check(ars_is("/a.gz", "the old one") || ars_get("/a.gz", &now), "and the file is the old one or the whole new one");
        if(!ars_is("/a.gz", "the old one")) check(now == whole, "and the file is the old one or the whole new one");
    }

    /* a damaged input never replaces the output */
    {
        unsigned char *bad = malloc(zn);
        memcpy(bad, z, zn);
        bad[zn - 5] ^= 1;
        ars_reset();
        ars_put("/a.gz", bad, zn);
        ars_puts("/a", "precious");
        check(ars_cmd(gunzip_main, "gunzip -f /a.gz") == 2 && ars_is("/a", "precious") && ars_exists("/a.gz") && ars_count() == 2,
              "a bad CRC under -f: the old file stands, and no .tmp is left");
        free(bad);
    }

    /* a rename that half happened: the input stays, and the message says to check the disk */
    ars_reset();
    ars_puts("/a", sample);
    ars_puts("/a.gz", "the old one");
    ars_rename_half();
    check(ars_cmd(gzip_main, "gzip -f /a") == 2 && ars_is("/a", sample) && has(ars_stderr(), "check the disk"),
          "a rename left half done: the input stays, and the disk is to be checked");

    /* a twin of the input: deleting it would free what its twin holds */
    ars_reset();
    ars_puts("/a", sample);
    ars_puts("/a.tmp", sample);
    ars_same_time("/a", "/a.tmp");
    check(ars_cmd(gzip_main, "gzip /a") == 2 && ars_is("/a", sample) && ars_exists("/a.gz") && has(ars_stderr(), "check the disk"),
          "an input with a twin is kept, and that is said");
    /* an output that has a twin is not touched */
    ars_reset();
    ars_puts("/a", sample);
    ars_puts("/a.gz", "the old one");
    ars_puts("/a.gz.tmp", "the old one");
    ars_same_time("/a.gz", "/a.gz.tmp");
    check(ars_cmd(gzip_main, "gzip -f /a") == 2 && ars_is("/a.gz", "the old one") && ars_is("/a", sample) && has(ars_stderr(), "check the disk"),
          "an output with a twin is left, with both its names");
    free(z);
}

/**** the host's gzip ****/

static void corpus(int k, unsigned char **data, size_t *n) {
    static unsigned char *d;
    size_t i;
    unsigned r = 99;
    static const size_t sizes[] = { 0, 1, 2, 100, 5000, 70000, 300000, 1000000 };
    *n = sizes[k];
    d = realloc(d, *n + 1);
    for(i = 0; i < *n; i++) {
        r = r * 1103515245 + 12345;
        if(k == 7) d[i] = (unsigned char)(r >> 16);                     /* random: stored blocks */
        else if(k == 6) d[i] = (unsigned char)(i >> 8);                 /* long runs */
        else d[i] = (unsigned char)"to be, or not to be: that is the question\n"[(i + (r >> 28)) % 42];
    }
    *data = d;
}

#define CORPUS 8

static void test_host_gzip(void) {
    const char *gz = host_tool("gzip");
    int k, level;
    char dir[256], cmd[1024];
    if(!gz) {
        printf("test_gztool: no gzip on this host: both comparisons skipped\n");
        return;
    }
    host_tmpdir(dir);
    for(k = 0; k < CORPUS; k++) {
        unsigned char *data;
        size_t n, m;
        corpus(k, &data, &n);
        /* ours, the host's reader */
        for(level = 1; level <= 9; level += 4) {
            const unsigned char *p;
            char opt[8];
            const char *argv[] = { "gzip", opt, "-c", "/in", NULL };
            unsigned char *back;
            size_t got;
            snprintf(opt, sizeof(opt), "-%d", level);
            ars_reset();
            ars_put("/in", data, n);
            check(ars_run(gzip_main, argv) == 0, "ours compresses");
            p = ars_stdout(&m);
            host_write(dir, "ours.gz", p, m);
            snprintf(cmd, sizeof(cmd), "'%s' -dc '%s/ours.gz' > '%s/back' 2>/dev/null", gz, dir, dir);
            check(system(cmd) == 0, "the host's gzip reads what ours wrote, and finds no fault");
            back = host_read(dir, "back", &got);
            check(got == n && !memcmp(back, data, n), "... the same bytes");
            free(back);
        }
        /* the host's, our reader */
        host_write(dir, "plain", data, n);
        for(level = 1; level <= 9; level += 4) {
            unsigned char *z;
            size_t zn;
            const unsigned char *p;
            snprintf(cmd, sizeof(cmd), "'%s' -%d -c '%s/plain' > '%s/host.gz'", gz, level, dir, dir);
            check(system(cmd) == 0, "the host's gzip compresses");
            z = host_read(dir, "host.gz", &zn);
            ars_reset();
            ars_put("/host.gz", z, zn);
            check(ars_cmd(gunzip_main, "gunzip /host.gz") == 0, "ours reads what the host's wrote");
            p = ars_get("/host", &m);
            check(p && m == n && !memcmp(p, data, n), "... the same bytes");
            free(z);
        }
    }
    host_rmdir(dir);
}

/* Python's zlib makes what gzip does not: fixed blocks only, a run-length strategy, level 0's stored blocks, and a sync
   flush every 1000 bytes, which is an empty stored block. */
static void test_host_zlib(void) {
    static const char *const kinds[] = {
        "zlib.compressobj(6, zlib.DEFLATED, 31, 8, zlib.Z_FIXED)",
        "zlib.compressobj(6, zlib.DEFLATED, 31, 8, zlib.Z_RLE)",
        "zlib.compressobj(6, zlib.DEFLATED, 31, 8, zlib.Z_HUFFMAN_ONLY)",
        "zlib.compressobj(0, zlib.DEFLATED, 31)",
        "zlib.compressobj(9, zlib.DEFLATED, 31, 9)",
    };
    const char *py = host_tool("python3");
    char dir[256], cmd[2048];
    unsigned i;
    int k;
    if(!py) {
        printf("test_gztool: no python3 on this host: the zlib comparison is skipped\n");
        return;
    }
    host_tmpdir(dir);
    for(k = 3; k < CORPUS - 1; k++) {
        unsigned char *data;
        size_t n;
        corpus(k, &data, &n);
        host_write(dir, "plain", data, n);
        for(i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            int flushes;
            for(flushes = 0; flushes < 2; flushes++) {
                unsigned char *z;
                size_t zn, m;
                const unsigned char *p;
                snprintf(cmd, sizeof(cmd),
                         "'%s' -I -c \"import sys,zlib\n"
                         "d=open(sys.argv[1],'rb').read(); c=%s; o=b''\n"
                         "for i in range(0,len(d),1000):\n"
                         "    o+=c.compress(d[i:i+1000])\n"
                         "    if %d: o+=c.flush(zlib.Z_SYNC_FLUSH)\n"
                         "open(sys.argv[2],'wb').write(o+c.flush())\n\" '%s/plain' '%s/z.gz'",
                         py, kinds[i], flushes, dir, dir);
                check(system(cmd) == 0, "Python makes the stream");
                z = host_read(dir, "z.gz", &zn);
                ars_reset();
                ars_put("/z.gz", z, zn);
                check(ars_cmd(gunzip_main, "gunzip /z.gz") == 0, "ours reads it");
                p = ars_get("/z", &m);
                check(p && m == n && !memcmp(p, data, n), "... the same bytes");
                free(z);
            }
        }
    }
    host_rmdir(dir);
}

int main(void) {
    test_options();
    test_format();
    test_failures();
    test_host_gzip();
    test_host_zlib();
    return report("test_gztool");
}
