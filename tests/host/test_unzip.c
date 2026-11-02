/* Host-side test of unzip (11/01/26): user/unzip/ as it is, on the simulated FAT of tests/host/ar_sim.c. Archives are put
   together here byte by byte, field by field, so that each can lie in one way; a zip has no checksum over its headers, so
   every mutated field reaches the parser. Covered: what the options do; names in UTF-8 and code page 437; each refusal,
   with the entries around it still taken; sizes, offsets and counts that lie, in each direction, and entries laid over
   each other; every cut of an archive and mutated archives; the disk failing at each step, pulled and full; an archive
   past 2 GB, which lseek cannot reach; and Python's zipfile, Info-ZIP's zip and macOS's ditto where they are installed
   (each comparison says so when it is skipped). Run under AddressSanitizer and UBSan by tests/archive-sanitize.sh on the
   Mac: never in `make unittest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ar_sim.h"
#include "ar_host.h"

int unzip_main(int argc, char **argv);
extern unsigned int ar_crc32(unsigned int, const void *, size_t);

static int has(const char *text, const char *part) {
    return strstr(text, part) != NULL;
}

/**** a zip by hand ****/

#define MAX_ZIP (4 << 20)
static unsigned char *Z, *C;            /* the archive, and its central directory as it grows */
static size_t Zn, Cn;
static unsigned entries;
static const char *eocd_comment;

struct ent {
    const char *name;
    size_t name_len;                    /* 0: strlen(name) */
    const void *data;                   /* what is stored */
    size_t csize;
    size_t usize;
    unsigned crc;
    unsigned method, flags, made_by;
    unsigned attrs;
    /* lies: what the central directory says, when these are set */
    int lie_csize, lie_usize, lie_crc, lie_offset, lie_method;
    unsigned c_csize, c_usize, c_crc, c_offset, c_method;
    const char *local_name;             /* the local header's name, when it differs */
    int lie_local_method;
    unsigned local_method, local_flags;
    int lie_local_sig;
    size_t extra;                       /* bytes in the local extra field */
};

static void put8(unsigned char **p, unsigned v) { *(*p)++ = (unsigned char)v; }
static void put16(unsigned char **p, unsigned v) { put8(p, v); put8(p, v >> 8); }
static void put32(unsigned char **p, unsigned v) { put16(p, v); put16(p, v >> 16); }

static void z_start(void) {
    if(!Z) {
        Z = malloc(MAX_ZIP);
        C = malloc(MAX_ZIP);
    }
    Zn = Cn = 0;
    entries = 0;
    eocd_comment = NULL;
}

static void z_entry(const struct ent *e) {
    size_t nlen = e->name_len ? e->name_len : strlen(e->name);
    unsigned char *p = Z + Zn;
    unsigned offset = (unsigned)Zn;
    const char *local = e->local_name ? e->local_name : e->name;
    size_t llen = e->local_name ? strlen(local) : nlen;
    if(Zn + 30 + llen + e->extra + e->csize > MAX_ZIP) abort();
    put32(&p, e->lie_local_sig ? 0x04034b51u : 0x04034b50u);
    put16(&p, 20);
    put16(&p, e->local_flags ? e->local_flags : e->flags);
    put16(&p, e->lie_local_method ? e->local_method : e->method);
    put16(&p, 0x6000);                  /* 12:00 */
    put16(&p, (46u << 9) | (11u << 5) | 1u);    /* 2026-11-01 */
    put32(&p, e->crc);
    put32(&p, (unsigned)e->csize);
    put32(&p, (unsigned)e->usize);
    put16(&p, (unsigned)llen);
    put16(&p, (unsigned)e->extra);
    memcpy(p, local, llen);
    p += llen;
    memset(p, 'e', e->extra);
    p += e->extra;
    if(e->csize) memcpy(p, e->data, e->csize);
    p += e->csize;
    Zn = (size_t)(p - Z);

    p = C + Cn;
    put32(&p, 0x02014b50u);
    put16(&p, e->made_by ? e->made_by : 20);
    put16(&p, 20);
    put16(&p, e->flags);
    put16(&p, e->lie_method ? e->c_method : e->method);
    put16(&p, 0x6000);
    put16(&p, (46u << 9) | (11u << 5) | 1u);
    put32(&p, e->lie_crc ? e->c_crc : e->crc);
    put32(&p, e->lie_csize ? e->c_csize : (unsigned)e->csize);
    put32(&p, e->lie_usize ? e->c_usize : (unsigned)e->usize);
    put16(&p, (unsigned)nlen);
    put16(&p, 0);
    put16(&p, 0);
    put16(&p, 0);
    put16(&p, 0);
    put32(&p, e->attrs);
    put32(&p, e->lie_offset ? e->c_offset : offset);
    memcpy(p, e->name, nlen);
    p += nlen;
    Cn = (size_t)(p - C);
    entries++;
}

/* a stored entry that tells the truth */
static void z_stored(const char *name, const char *text) {
    struct ent e;
    memset(&e, 0, sizeof(e));
    e.name = name;
    e.data = text;
    e.csize = e.usize = strlen(text);
    e.crc = ar_crc32(0, text, e.csize);
    z_entry(&e);
}

/* the data as a deflate stream of one stored block: real deflate with no dependence on the code under test */
static size_t wrap(unsigned char *o, const void *data, size_t n) {
    if(n > 65535) abort();
    o[0] = 1;
    o[1] = (unsigned char)n;
    o[2] = (unsigned char)(n >> 8);
    o[3] = (unsigned char)~n;
    o[4] = (unsigned char)(~n >> 8);
    if(n) memcpy(o + 5, data, n);
    return n + 5;
}

static void z_deflated(const char *name, const char *text) {
    static unsigned char buf[70000];
    struct ent e;
    memset(&e, 0, sizeof(e));
    e.name = name;
    e.method = 8;
    e.data = buf;
    e.usize = strlen(text);
    e.csize = wrap(buf, text, e.usize);
    e.crc = ar_crc32(0, text, e.usize);
    z_entry(&e);
}

/* the end record, with its fields as they should be or as *over says */
struct over { int set_entries, set_size, set_offset; unsigned entries, size, offset; unsigned this_disk, cd_disk, total; int set_total; };

static void z_end_with(const struct over *o) {
    unsigned at = (unsigned)Zn;
    unsigned char *p;
    size_t comment = eocd_comment ? strlen(eocd_comment) : 0;
    memcpy(Z + Zn, C, Cn);
    Zn += Cn;
    p = Z + Zn;
    put32(&p, 0x06054b50u);
    put16(&p, o ? o->this_disk : 0);
    put16(&p, o ? o->cd_disk : 0);
    put16(&p, o && o->set_entries ? o->entries : entries);
    put16(&p, o && o->set_total ? o->total : o && o->set_entries ? o->entries : entries);
    put32(&p, o && o->set_size ? o->size : (unsigned)Cn);
    put32(&p, o && o->set_offset ? o->offset : at);
    put16(&p, (unsigned)comment);
    if(comment) memcpy(p, eocd_comment, comment);
    p += comment;
    Zn = (size_t)(p - Z);
}

static void z_end(void) {
    z_end_with(NULL);
}

/* unzip with these options of the archive in Z, in a fresh disk's /dest: its status */
static int run(const char *opts) {
    char cmd[256];
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    ars_mkdir("/dest");
    ars_mark();
    ars_chdir("/dest");
    snprintf(cmd, sizeof(cmd), "unzip %s /a.zip", opts);
    return ars_cmd(unzip_main, cmd);
}

static void sample(void) {
    z_start();
    z_stored("one.txt", "first file\n");
    z_stored("sub/", "");
    z_deflated("sub/two.txt", "second file\n");
    z_deflated("sub/deep/three.txt", "third\n");
    z_stored("empty", "");
    z_deflated("nothing", "");
    z_end();
}

static int sample_back(void) {
    return ars_is("/dest/one.txt", "first file\n") && ars_is("/dest/sub/two.txt", "second file\n") && ars_is("/dest/sub/deep/three.txt", "third\n") &&
           ars_is("/dest/empty", "") && ars_is("/dest/nothing", "");
}

/**** the options ****/

static void test_options(void) {
    size_t n;
    const unsigned char *p;
    sample();
    check(run("") == 0 && sample_back(), "unzip: all of it, stored and deflated, with the folders above");
    check(has((const char *)ars_stdout(&n), "inflating: sub/two.txt") && has((const char *)ars_stdout(&n), "extracting: one.txt"), "and what it does is said");
    check(!ars_open_files(), "with no descriptor left open");
    check(run("-q") == 0 && sample_back(), "-q");
    ars_stdout(&n);
    check(!n, "... says nothing");
    check(run("-l") == 0 && ars_count() == 2, "-l writes nothing");
    p = ars_stdout(&n);
    {
        char *text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(has(text, "2026-11-01 12:00   sub/deep/three.txt\n") && has(text, "        11  2026") && has(text, "6 files"), "-l: size, date, name, and the total");
        free(text);
    }
    check(run("-t") == 0 && ars_count() == 2, "-t writes nothing");
    p = ars_stdout(&n);
    {
        char *text = malloc(n + 1);
        memcpy(text, p, n);
        text[n] = 0;
        check(has(text, "testing: sub/two.txt  OK") && has(text, "No errors"), "-t says each, and the result");
        free(text);
    }
    check(run("-tq") == 0, "-tq");
    ars_stdout(&n);
    check(!n, "... says nothing");
    check(run("-p") == 0 && ars_count() == 2, "-p writes no file");
    p = ars_stdout(&n);
    check(n == 29 && !memcmp(p, "first file\nsecond file\nthird\n", 29), "-p: the data, in order, and nothing else");

    /* names */
    sample();
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    ars_mkdir("/dest");
    ars_chdir("/dest");
    check(ars_cmd(unzip_main, "unzip /a.zip one.txt") == 0 && ars_is("/dest/one.txt", "first file\n") && ars_count() == 3, "a name takes that file");
    check(ars_cmd(unzip_main, "unzip -o /a.zip *three*") == 0 && ars_is("/dest/sub/deep/three.txt", "third\n") && !ars_exists("/dest/sub/two.txt"), "a pattern");
    check(ars_cmd(unzip_main, "unzip -o /a.zip ONE.TXT") == 0, "whatever the case");
    check(ars_cmd(unzip_main, "unzip -o /a.zip one.txt nothere") == 2 && has(ars_stderr(), "nothere"), "a name that is not there is said");

    /* -d, before and after */
    sample();
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    check(ars_cmd(unzip_main, "unzip -d /new/deep /a.zip") == 0 && ars_is("/new/deep/one.txt", "first file\n"), "-d makes its folder, however deep");
    check(ars_cmd(unzip_main, "unzip /a.zip -d /other") == 0 && ars_is("/other/one.txt", "first file\n"), "-d after the archive, as people write it");
    check(ars_cmd(unzip_main, "unzip /a.zip -d/third") == 0 && ars_is("/third/one.txt", "first file\n"), "-d glued to its folder");
    check(ars_cmd(unzip_main, "unzip -l /a.zip -d /notmade") == 0 && !ars_exists("/notmade"), "-l makes no folder");
    ars_puts("/afile", "x");
    check(ars_cmd(unzip_main, "unzip /a.zip -d /afile") == 2 && has(ars_stderr(), "/afile"), "-d onto a file: said");

    /* a file that exists */
    sample();
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    ars_puts("/dest/one.txt", "mine\n");
    ars_chdir("/dest");
    check(ars_cmd(unzip_main, "unzip /a.zip") == 2 && ars_is("/dest/one.txt", "mine\n") && has(ars_stderr(), "-o") && ars_is("/dest/sub/two.txt", "second file\n"),
          "a file that exists stays; -o is named; the others are done");
    ars_puts("/dest/one.txt", "mine\n");
    check(ars_cmd(unzip_main, "unzip -n /a.zip") == 0 && ars_is("/dest/one.txt", "mine\n"), "-n leaves it quietly");
    check(ars_cmd(unzip_main, "unzip -o /a.zip") == 0 && ars_is("/dest/one.txt", "first file\n") && !has(ars_list(), ".tmp"), "-o replaces it, and no .tmp is left");
    check(ars_cmd(unzip_main, "unzip -on /a.zip") == 2 && has(ars_stderr(), "usage"), "-o and -n together: usage");
    check(ars_cmd(unzip_main, "unzip") == 2 && has(ars_stderr(), "usage"), "no archive: usage");
    check(ars_cmd(unzip_main, "unzip -x /a.zip") == 2 && has(ars_stderr(), "usage"), "an unknown option: usage");
    check(ars_cmd(unzip_main, "unzip -d") == 2 && has(ars_stderr(), "usage"), "-d with nothing after it: usage");
    check(ars_cmd(unzip_main, "unzip /nothere.zip") == 2 && has(ars_stderr(), "/nothere.zip"), "a missing archive: said");
    ars_mkdir("/d.zip");
    check(ars_cmd(unzip_main, "unzip /d.zip") == 2 && has(ars_stderr(), "/d.zip"), "a folder: said");

    /* standard input */
    sample();
    ars_reset();
    ars_stdin(Z, Zn, 1, 0);
    check(ars_cmd(unzip_main, "unzip -l -") == 2 && has(ars_stderr(), "pipe"), "a pipe: said, since a zip is read from its end");
    ars_stdin(Z, Zn, 0, 0);
    ars_mkdir("/dest");
    ars_chdir("/dest");
    check(ars_cmd(unzip_main, "unzip -") == 0 && sample_back(), "- from a file: fine");
}

/**** names ****/

static void test_names(void) {
    struct ent e;
    memset(&e, 0, sizeof(e));
    /* code page 437, as old tools wrote it */
    z_start();
    e.name = "caf\x82.txt";
    e.data = "cp437\n";
    e.csize = e.usize = 6;
    e.crc = ar_crc32(0, "cp437\n", 6);
    z_entry(&e);
    e.name = "\xe1\xe2\x9b.txt";                        /* ß Γ ¢ */
    z_entry(&e);
    z_end();
    check(run("") == 0 && ars_is("/dest/caf\xc3\xa9.txt", "cp437\n") && ars_is("/dest/\xc3\x9f\xce\x93\xc2\xa2.txt", "cp437\n"), "code page 437 names become UTF-8");
    /* the flag */
    z_start();
    e.name = "caf\xc3\xa9.txt";
    e.flags = 0x800;
    z_entry(&e);
    z_end();
    check(run("") == 0 && ars_is("/dest/caf\xc3\xa9.txt", "cp437\n"), "the UTF-8 flag");
    /* Unix and the Mac write UTF-8 without saying so */
    z_start();
    e.flags = 0;
    e.made_by = 3 << 8 | 30;
    e.name = "caf\xc3\xa9-unix.txt";
    z_entry(&e);
    e.made_by = 19 << 8 | 20;
    e.name = "caf\xc3\xa9-mac.txt";
    z_entry(&e);
    z_end();
    check(run("") == 0 && ars_is("/dest/caf\xc3\xa9-unix.txt", "cp437\n") && ars_is("/dest/caf\xc3\xa9-mac.txt", "cp437\n"), "UTF-8 from Unix or a Mac, flag or no flag");
    /* from Unix, not valid UTF-8: code page 437 after all */
    z_start();
    e.made_by = 3 << 8 | 30;
    e.name = "caf\x82.txt";
    z_entry(&e);
    z_end();
    check(run("") == 0 && ars_is("/dest/caf\xc3\xa9.txt", "cp437\n"), "and when it is not valid UTF-8, code page 437");
    /* the same bytes from DOS */
    z_start();
    e.made_by = 0 << 8 | 20;
    e.name = "\xc3\xa9.txt";
    z_entry(&e);
    z_end();
    check(run("") == 0 && ars_exists("/dest/\xe2\x94\x9c\xe2\x8c\x90.txt"), "from DOS, bytes that look like UTF-8 are code page 437 all the same");
    e.made_by = 0;
    /* a UTF-8 flag on what is not UTF-8 */
    z_start();
    e.name = "\xff\xfe.txt";
    e.flags = 0x800;
    z_entry(&e);
    e.flags = 0;
    z_stored("after.txt", "after\n");
    z_end();
    check(run("") == 2 && has(ars_stderr(), "UTF-8") && ars_is("/dest/after.txt", "after\n") && !strchr(ars_stderr(), 0xff), "invalid UTF-8 under the flag: refused, and not printed raw");
    /* an overlong "../" */
    z_start();
    e.name = "\xc0\xae\xc0\xae\xc0\xafup.txt";
    e.flags = 0x800;
    z_entry(&e);
    e.flags = 0;
    z_end();
    check(run("") == 2 && ars_count() == 2, "an overlong ../ under the flag");
    /* a zero byte: the name in the file differs from the name a C string would see */
    z_start();
    e.name = "good.txt\0.exe";
    e.name_len = 13;
    z_entry(&e);
    e.name_len = 0;
    z_end();
    check(run("") == 2 && has(ars_stderr(), "zero byte") && !ars_exists("/dest/good.txt"), "a zero byte inside a name");
    /* the longest name the format has */
    {
        static char longest[65536];
        memset(longest, 'n', 65535);
        z_start();
        e.name = longest;
        z_entry(&e);
        z_stored("after.txt", "after\n");
        z_end();
        check(run("") == 2 && has(ars_stderr(), "too long") && ars_is("/dest/after.txt", "after\n"), "a name of 65535 bytes");
        check(run("-t") == 2, "... -t");
        check(run("-l") == 0, "... -l lists it");
        check(run("-p") == 2, "... -p");
        ars_stdout(NULL);
        memset(longest, 'n', 300);
        longest[300] = 0;
        z_start();
        e.name = longest;
        z_entry(&e);
        z_end();
        check(run("") == 2 && has(ars_stderr(), "too long"), "a name of 300 bytes");
    }
    /* a control character in a name, listed */
    z_start();
    e.name = "esc\x1b[2J.txt";
    z_entry(&e);
    z_end();
    check(run("-l") == 0 && !strchr((const char *)ars_stdout(NULL), 0x1b), "-l does not send a name's escape to the terminal");
    check(run("-t") == 0 && !strchr((const char *)ars_stdout(NULL), 0x1b), "-t likewise");
    check(run("") == 2 && !strchr(ars_stderr(), 0x1b), "and the refusal does not");
    /* C1's CSI as UTF-8 is a legal FAT name, and must not reach the terminal bare */
    z_start();
    e.name = "c1\xc2\x9b" "2J.txt";
    e.flags = 0x800;
    z_entry(&e);
    e.flags = 0;
    z_end();
    check(run("") == 2 && ars_count() == 2 && !strstr(ars_stderr(), "\xc2\x9b"), "a name with C1's CSI is refused, and not shown bare");
    check(run("-l") == 0 && !strchr((const char *)ars_stdout(NULL), 0x9b), "-l shows it escaped");
}

/**** refusals ****/

static void test_refusals(void) {
    static const char *const evil[] = {
        "/etc/passwd", "../up.txt", "a/../../up.txt", "..", "a/..", "//abs", "..\\up.txt", "a\\..\\..\\up.txt", "\\abs",
        "C:\\up.txt", "C:/up.txt", "a:b", "bad*name", "bad?name", "bad\"name", "bad<name", "bad>name", "bad|name",
        "dot.", "space ", "dir./x", "dir /x", "ctl\x01name", "...", ".. ",
    };
    struct ent e;
    unsigned i;
    size_t n;
    for(i = 0; i < sizeof(evil) / sizeof(evil[0]); i++) {
        z_start();
        z_stored("before.txt", "before\n");
        z_stored(evil[i], "evil\n");
        z_stored("after.txt", "after\n");
        z_end();
        check(run("") == 2, evil[i]);
        if(!ars_is("/dest/before.txt", "before\n") || !ars_is("/dest/after.txt", "after\n") || ars_count() != 4 || !ars_all_under("/dest")) {
            printf("around [%s]:\n%s%s", evil[i], ars_list(), ars_stderr());
            check(0, "the entries around a refused one are taken, and nothing else is made");
        } else {
            check(1, "the entries around a refused one are taken, and nothing else is made");
        }
        check(!strchr(ars_stderr(), 0x01), "the message holds no control character");
        check(run("-p") == 0, "-p writes no file, so the name does not matter");
        check(run("-t") == 0, "nor does -t");
    }
    /* no name to make, and data that would go nowhere: said, and status 2, as tar does */
    {
        static const char *const none[] = { "", ".", "./", "././" };
        for(i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
            z_start();
            z_stored(none[i], "data with no name\n");
            z_stored("ok.txt", "ok\n");
            z_end();
            check(run("") == 2 && ars_is("/dest/ok.txt", "ok\n") && ars_count() == 3, "an entry with no name that holds data: status 2, and the next is taken");
            check(has(ars_stderr(), "no name") || has(ars_stderr(), "holds data"), "and it is said");
        }
        z_start();
        z_stored("", "");
        z_end();
        check(run("") == 2 && has(ars_stderr(), "no name"), "an empty file with no name is said as well");
        z_start();
        z_stored("./", "");
        z_stored("ok.txt", "ok\n");
        z_end();
        check(run("") == 0 && ars_is("/dest/ok.txt", "ok\n"), "./ with nothing in it is only the top");
    }
    /* a folder that holds data: the folder is made, and the data must not go silently */
    z_start();
    z_stored("sub/", "data in a folder's entry\n");
    z_stored("ok.txt", "ok\n");
    z_end();
    check(run("") == 2 && ars_is_dir("/dest/sub") && ars_is("/dest/ok.txt", "ok\n") && has(ars_stderr(), "holds data"), "a folder that holds data: made, said, status 2");
    check(run("-t") == 2 && has(ars_stderr(), "holds data"), "-t says so too");
    check(run("-p") == 2 && has(ars_stderr(), "holds data"), "and -p");
    {
        struct ent d;                                   /* a folder stored as an empty deflate stream: two bytes, and nothing in them */
        static const unsigned char empty[2] = { 0x03, 0x00 };
        memset(&d, 0, sizeof(d));
        d.name = "sub/";
        d.method = 8;
        d.data = empty;
        d.csize = 2;
        z_start();
        z_entry(&d);
        z_end();
        check(run("") == 0 && ars_is_dir("/dest/sub"), "a folder deflated to nothing is only a folder");
    }

    /* "./" and folders in the name */
    z_start();
    z_stored("./a.txt", "x\n");
    z_stored("./", "");
    z_stored("b//c.txt", "y\n");
    z_end();
    check(run("") == 0 && ars_is("/dest/a.txt", "x\n") && ars_is("/dest/b/c.txt", "y\n"), "./ and // are only spelling");

    /* encrypted, other methods */
    memset(&e, 0, sizeof(e));
    z_start();
    e.name = "secret.txt";
    e.flags = 1;
    e.data = "xxxxxxxxxxxx";
    e.csize = e.usize = 12;
    z_entry(&e);
    z_stored("after.txt", "after\n");
    z_end();
    check(run("") == 2 && has(ars_stderr(), "encrypted") && !ars_exists("/dest/secret.txt") && ars_is("/dest/after.txt", "after\n"), "encrypted: skipped, the next entry taken");
    check(run("-t") == 2 && has(ars_stderr(), "encrypted"), "-t");
    check(run("-p") == 2 && has(ars_stderr(), "encrypted"), "-p");
    memset(&e, 0, sizeof(e));
    z_start();
    e.name = "local-only.txt";
    e.local_flags = 1;
    e.data = "abc";
    e.csize = e.usize = 3;
    e.crc = ar_crc32(0, "abc", 3);
    z_entry(&e);
    z_end();
    check(run("") == 2 && has(ars_stderr(), "encrypted") && !ars_exists("/dest/local-only.txt"), "encrypted by the local header alone");
    memset(&e, 0, sizeof(e));
    z_start();
    e.name = "bzip.txt";
    e.method = 12;
    e.data = "BZh91AY&SY";
    e.csize = 10;
    e.usize = 100;
    z_entry(&e);
    z_stored("after.txt", "after\n");
    z_end();
    check(run("") == 2 && has(ars_stderr(), "does not know") && !ars_exists("/dest/bzip.txt") && ars_is("/dest/after.txt", "after\n"), "a method unzip does not know");

    /* links */
    memset(&e, 0, sizeof(e));
    z_start();
    e.name = "link";
    e.made_by = 3 << 8 | 30;
    e.attrs = (0120777u) << 16;
    e.data = "/etc/passwd";
    e.csize = e.usize = 11;
    e.crc = ar_crc32(0, "/etc/passwd", 11);
    z_entry(&e);
    e.name = "pipe";
    e.attrs = (0010644u) << 16;
    z_entry(&e);
    e.name = "plain";
    e.attrs = (0100644u) << 16;
    z_entry(&e);
    e.name = "dos";
    e.made_by = 20;
    e.attrs = (0120777u) << 16;                         /* from DOS, these bits mean nothing */
    z_entry(&e);
    z_end();
    check(run("") == 2 && has(ars_stderr(), "link") && !ars_exists("/dest/link") && !ars_exists("/dest/pipe") && ars_exists("/dest/plain") && ars_exists("/dest/dos"),
          "a Unix link or pipe is skipped; a plain file and DOS's attributes are not");

    /* in the way */
    z_start();
    z_stored("x", "a file\n");
    z_stored("x/y", "under it\n");
    z_stored("after.txt", "after\n");
    z_end();
    check(run("") == 2 && ars_is("/dest/x", "a file\n") && ars_is("/dest/after.txt", "after\n"), "a file where a folder is wanted");
    z_start();
    z_stored("x/", "");
    z_stored("x", "over it\n");
    z_end();
    check(run("") == 2 && ars_is_dir("/dest/x"), "a folder where a file is wanted");
    z_start();
    z_stored("x", "a file\n");
    z_stored("x/", "");
    z_end();
    check(run("-o") == 2 && ars_is("/dest/x", "a file\n") && has(ars_stderr(), "in the way"), "a folder over a file");
    z_start();
    z_stored("Case.txt", "first\n");
    z_stored("CASE.TXT", "second\n");
    z_end();
    check(run("-o") == 0 && ars_is("/dest/case.txt", "second\n") && ars_count() == 3, "two names FAT cannot tell apart, with -o: the later one, one file");
    ars_stdout(&n);
}

/**** a member named like the archive ****/

static void test_self(void) {
    static const char *const names[] = { "a.zip", "A.ZIP", "./a.zip" };
    unsigned i;
    for(i = 0; i < 3; i++) {
        const unsigned char *now;
        size_t m;
        z_start();
        z_stored(names[i], "NEW CONTENTS\n");
        z_stored("after.txt", "after\n");
        z_end();
        ars_reset();
        ars_put("/d/a.zip", Z, Zn);
        ars_chdir("/d");
        check(ars_cmd(unzip_main, "unzip -o a.zip") == 2, names[i]);
        check(has(ars_stderr(), "the archive"), "a member that is the archive itself is refused by what it is, not by the rename's EBUSY");
        now = ars_get("/d/a.zip", &m);
        check(now && m == Zn && !memcmp(now, Z, Zn), "the archive is as it was");
        check(!has(ars_list(), ".tmp"), "and nothing was written for that member");
        check(ars_is("/d/after.txt", "after\n"), "and the next member is taken");
    }
    z_start();
    z_stored("a.zip", "NEW CONTENTS\n");
    z_end();
    ars_reset();
    ars_put("/d/a.zip", Z, Zn);
    ars_chdir("/d");
    check(ars_cmd(unzip_main, "unzip -n a.zip") == 0 && !has(ars_stderr(), "the archive"), "-n leaves it quietly, as any file that exists");
    check(ars_cmd(unzip_main, "unzip a.zip") == 2, "and without either it stays, and counts as trouble");
}

/**** lengths that lie ****/

/* a deflated entry of "hello hello hello..." whose headers say otherwise; nothing may stand of it */
static void lying(struct ent *e, const char *what) {
    z_entry(e);
    z_stored("after.txt", "after\n");
    z_end();
    check(run("") == 2, what);
    if(ars_exists("/dest/lie.txt") || ars_exists("/dest/lie.txt.tmp")) check(0, "nothing stands of a file that lied");
    if(!ars_is("/dest/after.txt", "after\n")) {
        printf("%s\n%s", what, ars_stderr());
        check(0, "the entry after one that lied is taken");
    }
    check(run("-t") == 2, "-t says so too");
}

static void test_lies(void) {
    static const char text[] = "hello hello hello hello hello\n";
    static unsigned char packed[200], buf[64];
    size_t size = strlen(text), pn = wrap(packed, text, size);
    unsigned crc = ar_crc32(0, text, size);
    struct ent e;

#define FRESH(M) do { memset(&e, 0, sizeof(e)); z_start(); e.name = "lie.txt"; e.method = (M); e.crc = crc; e.usize = size; \
        if(M) { e.data = packed; e.csize = pn; } else { e.data = text; e.csize = size; } } while(0)

    FRESH(8);
    e.lie_usize = 1;
    e.c_usize = (unsigned)size - 1;
    lying(&e, "deflated: it holds more than the central directory says");
    /* the bound itself: not one byte past the claim goes out, though the entry is refused in the end anyway */
    FRESH(8);
    e.lie_usize = 1;
    e.c_usize = 10;
    z_entry(&e);
    z_end();
    {
        size_t got;
        const unsigned char *out;
        check(run("-p") == 2 && has(ars_stderr(), "more than it says"), "-p: more data than claimed is said");
        out = ars_stdout(&got);
        check(got == 10 && !memcmp(out, text, 10), "... and what went out stops at the claim");
    }
    FRESH(8);
    e.lie_usize = 1;
    e.c_usize = (unsigned)size + 1;
    lying(&e, "deflated: it holds less");
    FRESH(8);
    e.lie_usize = 1;
    e.c_usize = 0;
    lying(&e, "deflated: says 0 and holds something");
    FRESH(8);
    e.lie_usize = 1;
    e.c_usize = 0xfffffff0u;
    lying(&e, "deflated: says 4 GB and holds little: nothing is set aside for what it claims");
    FRESH(8);
    e.lie_csize = 1;
    e.c_csize = (unsigned)pn - 3;
    lying(&e, "deflated: its compressed size is short of the stream: it starves");
    FRESH(8);
    e.lie_csize = 1;
    e.c_csize = 0;
    lying(&e, "deflated: a compressed size of 0");
    {
        static unsigned char more[220];                 /* the stream, and bytes after it that its size counts */
        memcpy(more, packed, pn);
        memset(more + pn, 0x55, 4);
        FRESH(8);
        e.data = more;
        e.csize = pn + 4;
        lying(&e, "deflated: its compressed size goes past the stream");
    }
    FRESH(8);
    e.lie_csize = 1;
    e.c_csize = (unsigned)pn + 4000;
    z_entry(&e);
    z_end();
    check(run("") == 2 && has(ars_stderr(), "outside") && ars_count() == 2, "a compressed size past the directory: the archive is refused whole");
    FRESH(0);
    e.lie_usize = 1;
    e.c_usize = (unsigned)size + 1;
    lying(&e, "stored: its two sizes differ");
    FRESH(0);
    e.lie_csize = 1;
    e.lie_usize = 1;
    e.c_csize = e.c_usize = (unsigned)size - 1;
    lying(&e, "stored: the sizes agree and are short: the CRC says so");
    FRESH(8);
    e.lie_crc = 1;
    e.c_crc = crc ^ 1;
    lying(&e, "a CRC off by a bit");
    FRESH(0);
    e.lie_crc = 1;
    e.c_crc = crc ^ 0x80000000u;
    lying(&e, "the same, stored");
    FRESH(0);
    e.lie_method = 1;
    e.c_method = 8;
    lying(&e, "the headers disagree on the method");
    FRESH(0);
    e.local_name = "other.txt";
    lying(&e, "the headers disagree on the name, as a scanner would be fooled");
    FRESH(0);
    e.local_name = "lie.txu";
    lying(&e, "the same with a name of one length");
    FRESH(0);
    e.lie_local_sig = 1;
    lying(&e, "a local header that is not one");
    /* corrupt deflate data */
    {
        static const unsigned char junk[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        FRESH(8);
        e.data = junk;
        e.csize = sizeof(junk);
        lying(&e, "data that is not deflate");
    }
    /* offsets */
    FRESH(0);
    e.lie_offset = 1;
    e.c_offset = 0xfffffff0u;
    z_entry(&e);
    z_end();
    check(run("") == 2 && has(ars_stderr(), "outside") && ars_count() == 2, "an offset past the file: the archive is refused before anything is written");
    FRESH(0);
    e.lie_csize = 1;
    e.c_csize = 0xfffffff0u;
    e.lie_usize = 1;
    e.c_usize = 0xfffffff0u;
    z_entry(&e);
    z_end();
    check(run("") == 2 && has(ars_stderr(), "outside"), "a compressed size past the file");
    FRESH(0);
    e.extra = 3;
    e.lie_offset = 1;
    e.c_offset = 0;
    z_entry(&e);
    z_end();
    check(run("") == 0 && ars_is("/dest/lie.txt", text), "(an extra field is passed over)");
    /* a local extra field that reaches the central directory */
    z_start();
    memset(&e, 0, sizeof(e));
    e.name = "lie.txt";
    e.data = text;
    e.csize = e.usize = size;
    e.crc = crc;
    e.extra = 20;
    z_entry(&e);
    z_end();
    Z[28] = 0xff;                                       /* the local extra's length */
    Z[29] = 0xff;
    check(run("") == 2 && !ars_exists("/dest/lie.txt") && has(ars_stderr(), "outside"), "a local extra length that puts the data outside the archive");

    /* the zip bomb that lays entries over each other */
    {
        static unsigned char bomb[60000];
        int i;
        char name[32];
        memset(bomb, 'b', sizeof(bomb));
        memset(&e, 0, sizeof(e));
        z_start();
        e.data = bomb;
        e.csize = e.usize = sizeof(bomb);
        e.crc = ar_crc32(0, bomb, sizeof(bomb));
        e.name = "kernel";
        z_entry(&e);
        for(i = 0; i < 40; i++) {                       /* central entries only, all pointing at the first's data */
            unsigned char *p = C + Cn;
            size_t nlen = (size_t)snprintf(name, sizeof(name), "copy%d", i);
            memcpy(p, C, 46);
            p[28] = (unsigned char)nlen;
            memcpy(p + 46, name, nlen);
            Cn += 46 + nlen;
            entries++;
        }
        z_end();
        check(run("") == 2 && has(ars_stderr(), "overlap") && ars_count() == 2, "entries laid over each other: refused before anything is written");
        check(run("-l") == 2, "-l too");
    }

    /* the end record */
    {
        struct over o;
        sample();
        z_start();
        z_stored("a", "a");
        z_stored("b", "b");
        memset(&o, 0, sizeof(o));
        o.set_entries = 1;
        o.entries = 1;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "more than"), "fewer entries in the end record than in the directory");
        z_start();
        z_stored("a", "a");
        o.entries = 2;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "ends in the middle"), "more entries in it than in the directory");
        z_start();
        z_stored("a", "a");
        memset(&o, 0, sizeof(o));
        o.this_disk = 1;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "disk"), "several disks");
        memset(&o, 0, sizeof(o));
        z_start();
        z_stored("a", "a");
        o.cd_disk = 2;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "disk"), "the directory on another disk");
        memset(&o, 0, sizeof(o));
        z_start();
        z_stored("a", "a");
        o.set_total = 1;
        o.total = 5;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "disk"), "this disk's count against the total");
        memset(&o, 0, sizeof(o));
        z_start();
        z_stored("a", "a");
        o.set_size = 1;
        o.size = 0xfffffff0u;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "not where"), "a directory larger than the file");
        memset(&o, 0, sizeof(o));
        z_start();
        z_stored("a", "a");
        o.set_offset = 1;
        o.offset = 0xfffffff0u;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "not where"), "a directory past the file: the sum must not wrap");
        memset(&o, 0, sizeof(o));
        z_start();
        z_stored("a", "a");
        o.set_entries = 1;
        o.entries = 0xffff;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "Zip64"), "Zip64's marker in the count");
        memset(&o, 0, sizeof(o));
        z_start();
        o.set_size = 1;
        o.size = 0xffffffffu;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "Zip64"), "in the size");
        memset(&o, 0, sizeof(o));
        z_start();
        o.set_offset = 1;
        o.offset = 0xffffffffu;
        z_end_with(&o);
        check(run("-l") == 2 && has(ars_stderr(), "Zip64"), "in the offset");
        FRESH(0);
        e.lie_csize = 1;
        e.c_csize = 0xffffffffu;
        z_entry(&e);
        z_end();
        check(run("-l") == 2 && has(ars_stderr(), "Zip64"), "in an entry's size");
        /* a directory of 17 MB */
        {
            unsigned char *big = malloc(18 << 20);
            unsigned char *p;
            size_t k = (17 << 20) + 22;
            memset(big, 0, k);
            p = big + (17 << 20);
            put32(&p, 0x06054b50u);
            put16(&p, 0);
            put16(&p, 0);
            put16(&p, 1);
            put16(&p, 1);
            put32(&p, 17 << 20);
            put32(&p, 0);
            put16(&p, 0);
            ars_reset();
            ars_put("/a.zip", big, k);
            check(ars_cmd(unzip_main, "unzip -l /a.zip") == 2 && has(ars_stderr(), "16 MB"), "a central directory over the limit: refused, not loaded");
            free(big);
        }
    }

    /* what is not a zip */
    z_start();
    check(run("-l") == 2 && has(ars_stderr(), "not a zip"), "an empty file");
    z_start();
    {
        static const char text2[] = "plain text, well over twenty-two bytes long, and no zip at all\n";
        memcpy(Z, text2, sizeof(text2) - 1);
        Zn = sizeof(text2) - 1;
    }
    check(run("-l") == 2 && has(ars_stderr(), "not a zip"), "plain text");
    z_start();
    z_stored("a", "a");
    z_end();
    memcpy(Z + Zn, "x", 1);
    Zn++;
    check(run("-l") == 2 && has(ars_stderr(), "not a zip"), "a byte after the end record");
    z_start();
    z_end();
    check(run("") == 0 && ars_count() == 2, "an empty zip: nothing to do");

    /* comments */
    z_start();
    z_stored("a", "a");
    eocd_comment = "a comment";
    z_end();
    check(run("") == 0 && ars_is("/dest/a", "a"), "a comment after the end record");
    {
        static char comment[65536];
        memset(comment, 'c', 65535);
        z_start();
        z_stored("a", "a");
        eocd_comment = comment;
        z_end();
        check(run("") == 0 && ars_is("/dest/a", "a"), "the longest comment there is");
        /* a comment that holds an end record of its own, for a scanner to find first */
        memset(buf, 0, sizeof(buf));
        z_start();
        z_stored("a", "a");
        memcpy(comment, "PK\x05\x06", 4);
        memset(comment + 4, 'c', 100);
        comment[104] = 0;
        eocd_comment = comment;
        z_end();
        check(run("") == 0 && ars_is("/dest/a", "a"), "an end record's signature inside the comment does not fool it");
    }
#undef FRESH
}

/**** damage ****/

static void test_damage(void) {
    static unsigned char good[1 << 20], m[1 << 20];
    static char body[3000];
    size_t n, i;
    unsigned r = 11;
    for(i = 0; i < sizeof(body) - 1; i++) {
        r = r * 1103515245 + 12345;
        body[i] = "the quick brown fox "[(r >> 16) % 20];
    }
    z_start();
    z_stored("sub/", "");
    z_deflated("sub/one.txt", body);
    z_stored("two.txt", "second\n");
    z_deflated("three.txt", "third\n");
    z_end();
    n = Zn;
    memcpy(good, Z, n);
    for(i = 0; i < n; i++) {
        int st;
        memcpy(Z, good, i);
        Zn = i;
        st = run("");
        if(st != 2 || ars_count() != 2) check(0, "any cut of a zip loses its end record: refused, nothing written");
    }
    check(1, "every cut of a zip");
    /* the tail cut off by less than a record's worth of comment does not pass either: done above; now mutations */
    for(i = 0; i < 4000; i++) {
        int st, k;
        memcpy(m, good, n);
        r = r * 1103515245 + 12345;
        for(k = 1 + (int)(r >> 29); k > 0; k--) {
            size_t at;
            r = r * 1103515245 + 12345;
            /* the headers are what is hostile, and they sit at the end */
            at = (i & 1) ? n - 1 - (r >> 8) % 300 : (r >> 8) % n;
            m[at] = (i & 2) ? (unsigned char)(r >> 24) : (unsigned char)(m[at] ^ (1u << (r & 7)));
        }
        memcpy(Z, m, n);
        Zn = n;
        st = run("");
        if((st != 0 && st != 2) || !ars_all_under("/dest")) check(0, "a mutated zip: status 0 or 2, nothing outside the folder");
        if(st == 0 && !ars_is("/dest/two.txt", "second\n") && ars_exists("/dest/two.txt")) check(0, "a file that passes has its CRC right");
        run("-l");
        run("-t");
        run("-p");
    }
    check(1, "4000 mutated zips, each extracted, listed, tested and piped");
    memcpy(Z, good, n);
    Zn = n;

    /* a file that exists is not hurt by one that fails */
    z_start();
    {
        struct ent e;
        static unsigned char packed[64];
        memset(&e, 0, sizeof(e));
        e.name = "mine.txt";
        e.method = 8;
        e.data = packed;
        e.usize = 6;
        e.csize = wrap(packed, "third\n", 6);
        e.crc = 12345;
        z_entry(&e);
    }
    z_end();
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    ars_puts("/dest/mine.txt", "old\n");
    ars_chdir("/dest");
    check(ars_cmd(unzip_main, "unzip -o /a.zip") == 2 && ars_is("/dest/mine.txt", "old\n") && !has(ars_list(), ".tmp") && has(ars_stderr(), "CRC"),
          "-o with a bad CRC: the old file stands");
}

/**** the disk ****/

static void test_failures(void) {
    int total, k, m;
    sample();
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    ars_puts("/dest/one.txt", "old\n");
    ars_chdir("/dest");
    ars_cmd(unzip_main, "unzip -o /a.zip");
    total = ars_changes();
    check(total >= 12, "unzip changes the disk in many steps");
    for(m = 0; m < 2; m++) {
        for(k = 1; k <= total; k++) {
            int st;
            ars_reset();
            ars_put("/a.zip", Z, Zn);
            ars_puts("/dest/one.txt", "old\n");
            ars_chdir("/dest");
            if(m) ars_pull_at(k);
            else ars_fail_change(k, ARS_EIO);
            st = ars_cmd(unzip_main, "unzip -o /a.zip");
            check(ars_is("/dest/one.txt", "old\n") || ars_is("/dest/one.txt", "first file\n"), "a file that existed is the old one or the new one");
            if(!m) {
                if(ars_exists("/dest/sub/two.txt") && !ars_is("/dest/sub/two.txt", "second file\n")) check(0, "a new file that stands is whole");
                if(st == 0) check(sample_back(), "status 0 means all of it");
            }
        }
    }
    sample();
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    ars_quota(Zn + 14);
    ars_mkdir("/dest");
    ars_chdir("/dest");
    check(ars_cmd(unzip_main, "unzip /a.zip") == 2 && has(ars_stderr(), "No space") && ars_is("/dest/one.txt", "first file\n") && !ars_exists("/dest/sub/two.txt"),
          "a full disk: said, and what did not fit does not stand");
    sample();
    ars_reset();
    ars_put("/a.zip", Z, Zn);
    ars_stdout_closes_after(5);
    check(ars_cmd(unzip_main, "unzip -p /a.zip") == 141, "-p whose reader goes: the program ends there");
}

/* An archive past 2 GB: its central directory and last file are where lseek cannot go, and read goes on. */
static void test_past_2gb(void) {
    static unsigned char tail[4096];
    const unsigned long long hole = 2200000000ULL;
    unsigned char *a = malloc(8192), *p;
    size_t head, tail_n;
    unsigned crc_low = ar_crc32(0, "low\n", 4), crc_high = ar_crc32(0, "high\n", 5), crc_pad;
    static unsigned char zeros[65536];
    unsigned long long left = hole;
    unsigned pad_off, high_off, cd_off, cd_n;
    int slow = getenv("AR_SLOW") != NULL;               /* tests/archive-sanitize.sh: seconds here, minutes under emulation */
    (void)tail;

    /* the CRC of 2.2 GB of zeros, for a file that holds them */
    crc_pad = 0;
    while(slow && left) {
        size_t k = left < sizeof(zeros) ? (size_t)left : sizeof(zeros);
        extern unsigned int crc32_fast(unsigned int, const void *, size_t);
        crc_pad = crc32_fast(crc_pad, zeros, k);
        left -= k;
    }

    /* low.txt, stored; then pad.bin, whose data is the hole */
    p = a;
    put32(&p, 0x04034b50u); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0); put16(&p, 0x21); put32(&p, crc_low);
    put32(&p, 4); put32(&p, 4); put16(&p, 7); put16(&p, 0);
    memcpy(p, "low.txt", 7); p += 7;
    memcpy(p, "low\n", 4); p += 4;
    pad_off = (unsigned)(p - a);
    put32(&p, 0x04034b50u); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0); put16(&p, 0x21); put32(&p, crc_pad);
    put32(&p, (unsigned)hole); put32(&p, (unsigned)hole); put16(&p, 7); put16(&p, 0);
    memcpy(p, "pad.bin", 7); p += 7;
    head = (size_t)(p - a);
    /* after the hole: high.txt, the directory, the end record */
    high_off = (unsigned)(head + hole);
    put32(&p, 0x04034b50u); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0); put16(&p, 0x21); put32(&p, crc_high);
    put32(&p, 5); put32(&p, 5); put16(&p, 8); put16(&p, 0);
    memcpy(p, "high.txt", 8); p += 8;
    memcpy(p, "high\n", 5); p += 5;
    cd_off = high_off + (unsigned)(p - (a + head));
    {
        unsigned char *q = p;
        struct { const char *n; unsigned crc, size, off; } list[3] = {
            { "low.txt", 0, 4, 0 }, { "pad.bin", 0, 0, 0 }, { "high.txt", 0, 5, 0 },
        };
        int i;
        list[0].crc = crc_low;
        list[1].crc = crc_pad;
        list[1].size = (unsigned)hole;
        list[1].off = pad_off;
        list[2].crc = crc_high;
        list[2].off = high_off;
        for(i = 0; i < 3; i++) {
            size_t l = strlen(list[i].n);
            put32(&p, 0x02014b50u); put16(&p, 20); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0); put16(&p, 0x21);
            put32(&p, list[i].crc); put32(&p, list[i].size); put32(&p, list[i].size); put16(&p, (unsigned)l); put16(&p, 0);
            put16(&p, 0); put16(&p, 0); put16(&p, 0); put32(&p, 0); put32(&p, list[i].off);
            memcpy(p, list[i].n, l);
            p += l;
        }
        cd_n = (unsigned)(p - q);
    }
    put32(&p, 0x06054b50u); put16(&p, 0); put16(&p, 0); put16(&p, 3); put16(&p, 3); put32(&p, cd_n); put32(&p, cd_off); put16(&p, 0);
    tail_n = (size_t)(p - a) - head;

    ars_reset();
    ars_put_hole("/big.zip", a, head, hole, tail_n);
    ars_mkdir("/dest");
    ars_chdir("/dest");
    check(ars_cmd(unzip_main, "unzip -l /big.zip") == 0, "a zip of 2.2 GB: its end, past lseek's reach, is found by reading forward");
    check(ars_cmd(unzip_main, "unzip -q /big.zip low.txt high.txt") == 0 && ars_is("/dest/low.txt", "low\n") && ars_is("/dest/high.txt", "high\n"),
          "a member before 2 GB and a member after it, in the order of the directory");
    if(ars_stderr()[0]) printf("%s", ars_stderr());
    if(slow) check(ars_cmd(unzip_main, "unzip -t -q /big.zip pad.bin") == 0, "and one that runs across 2 GB, CRC and all");
    else printf("test_unzip: reading a member across 2 GB is left to tests/archive-sanitize.sh (AR_SLOW): skipped\n");
    free(a);
}

/* A central directory that alternates low and high offsets past 2 GB: each high one costs a read from lseek's limit, so without a
   bound a 4 GB file would cost about 60 TB. Both members are empty: the cost is all in getting there. */
static void test_seek_bound(void) {
    enum { PAIRS = 200 };                               /* 2.2 GB is 53 MB past lseek's limit: this many passes the bound */
    const unsigned long long hole = 2200000000ULL;
    unsigned char *a = malloc(47 * 2 * PAIRS + 4096), *p, *cd;
    size_t head, tail_n;
    unsigned high, cd_off, cd_n;
    int i;
    p = a;
    put32(&p, 0x04034b50u); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0); put16(&p, 0x21); put32(&p, 0);
    put32(&p, 0); put32(&p, 0); put16(&p, 1); put16(&p, 0);
    put8(&p, 'a');
    head = (size_t)(p - a);
    high = (unsigned)(head + hole);
    put32(&p, 0x04034b50u); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0); put16(&p, 0x21); put32(&p, 0);
    put32(&p, 0); put32(&p, 0); put16(&p, 1); put16(&p, 0);
    put8(&p, 'b');
    cd_off = high + 31;
    cd = p;
    for(i = 0; i < 2 * PAIRS; i++) {
        put32(&p, 0x02014b50u); put16(&p, 20); put16(&p, 20); put16(&p, 0); put16(&p, 0); put16(&p, 0); put16(&p, 0x21);
        put32(&p, 0); put32(&p, 0); put32(&p, 0); put16(&p, 1); put16(&p, 0);
        put16(&p, 0); put16(&p, 0); put16(&p, 0); put32(&p, 0); put32(&p, i & 1 ? high : 0);
        put8(&p, i & 1 ? 'b' : 'a');
    }
    cd_n = (unsigned)(p - cd);
    put32(&p, 0x06054b50u); put16(&p, 0); put16(&p, 0); put16(&p, 2 * PAIRS); put16(&p, 2 * PAIRS); put32(&p, cd_n); put32(&p, cd_off); put16(&p, 0);
    tail_n = (size_t)(p - a) - head;
    ars_reset();
    ars_put_hole("/big.zip", a, head, hole, tail_n);
    check(ars_cmd(unzip_main, "unzip -t -q /big.zip") == 2, "a directory that sends the reader back and forth past 2 GB: refused at the bound");
    check(has(ars_stderr(), "past 2 GB"), "and it is said");
    check(ars_bytes_read() <= 5 * (hole + 1000000), "and what was read stayed near the bound: at most five times the file");
    ars_reset();
    ars_put_hole("/big.zip", a, head, hole, tail_n);
    check(ars_cmd(unzip_main, "unzip -l /big.zip") == 0, "-l reaches nothing past the directory, and lists it");
    ars_reset();
    ars_put_hole("/big.zip", a, head, hole, tail_n);
    ars_mkdir("/d");
    ars_chdir("/d");
    check(ars_cmd(unzip_main, "unzip -n -q /big.zip") == 2 && ars_exists("/d/b"), "unpacking stops at the bound too, what came before it stands");
    free(a);
}

/**** the host's tools ****/

static void test_host(void) {
    const char *py = host_tool("python3"), *zip = host_tool("zip"), *ditto = host_tool("ditto");
    char dir[256], cmd[4096];
    if(py) {
        size_t n;
        unsigned char *a;
        static const char *const modes[] = { "zipfile.ZIP_STORED", "zipfile.ZIP_DEFLATED" };
        unsigned i;
        host_tmpdir(dir);
        for(i = 0; i < 2; i++) {
            snprintf(cmd, sizeof(cmd),
                     "'%s' -I -c \"import sys, zipfile, io\n"
                     "class Pipe(io.RawIOBase):\n"
                     "    def __init__(self, f): self.f = f\n"
                     "    def writable(self): return True\n"
                     "    def seekable(self): return False\n"
                     "    def write(self, b): return self.f.write(b)\n"
                     "out = open(sys.argv[1], 'wb')\n"
                     "z = zipfile.ZipFile(Pipe(out), 'w', %s)\n"           /* no seeking: data descriptors, bit 3 */
                     "z.writestr('plain.txt', b'plain\\n' * 1000)\n"
                     "z.writestr('sub/deep/file.bin', bytes(range(256)) * 300)\n"
                     "z.writestr('caf\\u00e9/\\u00fcber.txt', b'utf8 name\\n')\n"
                     "z.writestr('empty', b'')\n"
                     "z.writestr(zipfile.ZipInfo('folder/'), b'')\n"
                     "z.comment = b'made by the test'\n"
                     "z.close()\n\" '%s/py.zip'", py, modes[i], dir);
            check(system(cmd) == 0, "Python makes a zip");
            a = host_read(dir, "py.zip", &n);
            ars_reset();
            ars_put("/a.zip", a, n);
            ars_mkdir("/dest");
            ars_chdir("/dest");
            check(ars_cmd(unzip_main, "unzip -t /a.zip") == 0, "ours tests it");
            check(ars_cmd(unzip_main, "unzip /a.zip") == 0, "ours unpacks it");
            if(ars_stderr()[0]) printf("%s", ars_stderr());
            {
                size_t m = 0;
                const unsigned char *p = ars_get("/dest/sub/deep/file.bin", &m);
                check(p && m == 76800 && p[256] == 0 && p[76799] == 255, "a binary file, through data descriptors");
                ars_get("/dest/plain.txt", &m);
                check(m == 6000, "a text file");
                check(ars_is("/dest/caf\xc3\xa9/\xc3\xbc" "ber.txt", "utf8 name\n"), "a name under the UTF-8 flag");
                check(ars_is("/dest/empty", "") && ars_is_dir("/dest/folder"), "an empty file and a folder");
            }
            free(a);
        }
        /* a real deflate stream through every level zlib has, and a zip with Zip64 records it did not need */
        snprintf(cmd, sizeof(cmd),
                 "'%s' -I -c \"import sys, zipfile\n"
                 "z = zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED, compresslevel=9)\n"
                 "z.writestr('level9.txt', b'the quick brown fox ' * 5000)\n"
                 "with z.open('forced.txt', 'w', force_zip64=True) as f: f.write(b'zip64 extra in a local header\\n')\n"
                 "z.close()\n\" '%s/py9.zip'", py, dir);
        check(system(cmd) == 0, "Python makes another");
        a = host_read(dir, "py9.zip", &n);
        ars_reset();
        ars_put("/a.zip", a, n);
        ars_mkdir("/dest");
        ars_chdir("/dest");
        check(ars_cmd(unzip_main, "unzip -q /a.zip") == 0, "ours unpacks a zip with a Zip64 extra field it did not need");
        check(ars_is("/dest/forced.txt", "zip64 extra in a local header\n"), "... the data");
        ars_get("/dest/level9.txt", &n);
        check(n == 100000, "... and level 9");
        free(a);
        host_rmdir(dir);
    } else {
        printf("test_unzip: no python3 on this host: the zipfile comparison is skipped\n");
    }
    if(zip) {
        size_t n;
        unsigned char *a;
        host_tmpdir(dir);
        host_write(dir, "a.txt", "from Info-ZIP\n", 14);
        snprintf(cmd, sizeof(cmd), "cd '%s' && mkdir sub && cp a.txt sub/b.txt && zip -q -r out.zip a.txt sub", dir);
        check(system(cmd) == 0, "Info-ZIP's zip makes one");
        a = host_read(dir, "out.zip", &n);
        ars_reset();
        ars_put("/a.zip", a, n);
        ars_mkdir("/dest");
        ars_chdir("/dest");
        check(ars_cmd(unzip_main, "unzip -q /a.zip") == 0 && ars_is("/dest/a.txt", "from Info-ZIP\n") && ars_is("/dest/sub/b.txt", "from Info-ZIP\n"), "ours unpacks it");
        free(a);
        host_rmdir(dir);
    } else {
        printf("test_unzip: no zip on this host: the Info-ZIP comparison is skipped\n");
    }
    if(ditto) {
        size_t n;
        unsigned char *a;
        host_tmpdir(dir);
        snprintf(cmd, sizeof(cmd), "cd '%s' && mkdir t && printf 'from ditto\\n' > 't/caf\xc3\xa9.txt' && ditto -c -k --norsrc --sequesterRsrc t out.zip", dir);
        check(system(cmd) == 0, "ditto makes a zip, as Finder does");
        a = host_read(dir, "out.zip", &n);
        ars_reset();
        ars_put("/a.zip", a, n);
        ars_mkdir("/dest");
        ars_chdir("/dest");
        check(ars_cmd(unzip_main, "unzip -q /a.zip") == 0, "ours unpacks it");
        if(ars_stderr()[0]) printf("%s", ars_stderr());
        check(has(ars_list(), "caf"), "with its accented name, in one form of UTF-8 or the other");
        free(a);
        host_rmdir(dir);
    } else {
        printf("test_unzip: no ditto on this host (it is macOS's): that comparison is skipped\n");
    }
}

int main(void) {
    test_options();
    test_names();
    test_refusals();
    test_self();
    test_lies();
    test_damage();
    test_failures();
    test_past_2gb();
    test_seek_bound();
    test_host();
    return report("test_unzip");
}
