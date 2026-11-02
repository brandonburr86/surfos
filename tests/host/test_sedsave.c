/* sed -i and w on a simulated file system (11/01/26, the command-line suite, tests/host/sim_fs.c): user/bin/sed.c as it is, its files
   going through user/bin/saveutil.h. What matters is what is on the disk after each failure: a file is as it was or complete,
   and nothing is left behind but what a half-done rename must leave. tests/test_sed.py has the rest of sed. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "sim_fs.h"

int sed_main(int argc, char **argv);
void sed_fail_read(int n);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
        printf("  the calls: %s\n", sim_log());
    }
}

/* sed with these words: its status */
static int sed(const char *first, ...) {
    char *argv[16];
    int argc = 0;
    const char *a = first;
    va_list ap;
    argv[argc++] = "sed";
    va_start(ap, first);
    while(a) {
        argv[argc++] = (char *)a;
        a = va_arg(ap, const char *);
    }
    va_end(ap);
    argv[argc] = NULL;
    return sed_main(argc, argv);
}

static int file_is(const char *name, const char *want) {
    static char buf[1 << 20];
    return sim_get(name, buf, sizeof(buf)) >= 0 && !strcmp(buf, want);
}

static int gone(const char *name) {
    char buf[8];
    return sim_get(name, buf, sizeof(buf)) < 0;
}

/* a text longer than sed's buffers */
static const char *long_text(void) {
    static char text[40000];
    int i;
    if(!text[0]) {
        for(i = 0; i + 1 < (int)sizeof(text); i += 8) memcpy(text + i, "abcdefg\n", 8);
        text[sizeof(text) - 1 - (sizeof(text) - 1) % 8] = 0;
    }
    return text;
}

static void in_place(void) {
    sim_reset();
    sim_put("a.txt", "one\ntwo\n");
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 0, "-i: status");
    check(file_is("a.txt", "1\ntwo\n"), "-i: the file is the new one");
    check(gone("a.txt.tmp"), "-i: the new file's name is gone");
    check(strstr(sim_log(), "open a.txt.tmp excl") && strstr(sim_log(), "fsync a.txt.tmp") && strstr(sim_log(), "rename a.txt.tmp a.txt"), "-i: excl, fsync, rename");
    check(strstr(sim_log(), "fsync") < strstr(sim_log(), "rename"), "-i: fsync before the rename");
    check(!strstr(sim_log(), "open a.txt trunc") && !strstr(sim_log(), "open a.txt excl"), "-i: the file itself is only read");

    /* a user's a.txt.tmp stays */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.tmp", "the user's\n");
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 0, "-i beside a .tmp: status");
    check(file_is("a.txt", "1\n") && file_is("a.txt.tmp", "the user's\n") && gone("a.txt.1.tmp"), "-i beside a .tmp: the other name is used, and gone");

    /* what looks like the file's other half: left alone */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.tmp", "one\n");
    sim_same_time("a.txt", "a.txt.tmp");
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 2, "-i beside a twin: status");
    check(file_is("a.txt", "one\n") && file_is("a.txt.tmp", "one\n"), "-i beside a twin: both as they were");

    /* an empty file: nothing to do, nothing done */
    sim_reset();
    sim_put("e.txt", "");
    check(sed("-i", "p", "e.txt", NULL) == 0 && file_is("e.txt", "") && gone("e.txt.tmp"), "-i on an empty file");
    check(!strstr(sim_log(), "rename"), "-i on an empty file: no rename");

    /* q: what came out is the file */
    sim_reset();
    sim_put("a.txt", "1\n2\n3\n");
    check(sed("-i", "2q", "a.txt", NULL) == 0 && file_is("a.txt", "1\n2\n"), "-i with q");

    /* a mistake in the script: nothing happens */
    sim_reset();
    sim_put("a.txt", "1\n");
    check(sed("-i", "k", "a.txt", NULL) == 2 && file_is("a.txt", "1\n") && sim_names() == 1, "-i with a bad script");
    /* the runtime's mistake on the first line */
    sim_reset();
    sim_put("a.txt", "1\n");
    check(sed("-i", "s//x/", "a.txt", NULL) == 2 && file_is("a.txt", "1\n") && gone("a.txt.tmp"), "-i stopped by a mistake at run time");

    /* #n counts alone on its line: a comment that starts so must not empty the file */
    sim_reset();
    sim_put("a.txt", "axb\n");
    sim_put("s.sed", "#normalise line ends\ns/x/y/\n");
    check(sed("-i", "-f", "s.sed", "a.txt", NULL) == 0 && file_is("a.txt", "ayb\n"), "-i and a comment that starts with #n");
    sim_reset();
    sim_put("a.txt", "axb\n");
    check(sed("-i", "#n: not the option\ns/x/y/", "a.txt", NULL) == 0 && file_is("a.txt", "ayb\n"), "-i and #n with more on its line");
    sim_reset();
    sim_put("a.txt", "axb\n");
    sim_put("s.sed", "#n\n/a/p\n");
    check(sed("-i", "-f", "s.sed", "a.txt", NULL) == 0 && file_is("a.txt", "axb\n"), "-i and #n alone on its line");

    /* no case conversion here: refused, not a letter in the file */
    {
        static const char *const asked[] = { "s/b/\\U&/", "s/b/\\L&/", "s/b/\\u&/", "s/b/\\l&/", "s/b/&\\E/" };
        int i;
        for(i = 0; i < 5; i++) {
            sim_reset();
            sim_put("a.txt", "abc\n");
            check(sed("-i", asked[i], "a.txt", NULL) == 2 && file_is("a.txt", "abc\n") && sim_names() == 1, "-i with a case conversion");
        }
    }

    /* several files: each on its own */
    sim_reset();
    sim_put("a.txt", "1\n");
    sim_put("c.txt", "1\n");
    check(sed("-i", "s/1/2/", "a.txt", "nosuch.txt", "c.txt", NULL) == 2, "-i with one missing: status");
    check(file_is("a.txt", "2\n") && file_is("c.txt", "2\n") && gone("nosuch.txt.tmp"), "-i with one missing: the others are done");
    sim_reset();
    sim_put("a.txt", "1\n");
    sim_mkdir("d");
    check(sed("-i", "s/1/2/", "d", "a.txt", NULL) == 2 && file_is("a.txt", "2\n"), "-i on a directory: said, and the next one done");
}

static void in_place_failures(void) {
    int i;
    /* a write that fails, at each of the writes of a long file */
    for(i = 0; i < 5; i++) {
        sim_reset();
        sim_put("a.txt", long_text());
        sim_fail(SIM_WRITE, "a.txt.tmp", SIM_ENOSPC, i);
        check(sed("-i", "s/a/b/", "a.txt", NULL) == 2, "a full disk: status");
        check(file_is("a.txt", long_text()), "a full disk: the file is as it was");
        check(gone("a.txt.tmp") && sim_names() == 1, "a full disk: nothing left beside it");
    }
    /* a short write: the rest goes after it */
    sim_reset();
    sim_put("a.txt", "one\ntwo\n");
    sim_fail(SIM_WRITE, "a.txt.tmp", 0, 0);
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 0 && file_is("a.txt", "1\ntwo\n"), "a short write");
    /* fsync and close */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_FSYNC, "a.txt.tmp", SIM_EIO, 0);
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && gone("a.txt.tmp"), "fsync fails");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_CLOSE, "a.txt.tmp", SIM_EIO, 0);
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && gone("a.txt.tmp"), "close fails");
    /* a rename that changed nothing */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_ENOSPC, SIM_NONE);
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && gone("a.txt.tmp"), "a rename refused");
    /* one that may have half happened: the new name stays, and nothing is deleted */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_NONE);
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && file_is("a.txt.tmp", "1\n"), "a rename's I/O error: both kept");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_HALF);
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 2, "a half-done rename: status");
    check(file_is("a.txt", "1\n") && !sim_corrupt("a.txt") && !sim_corrupt("a.txt.tmp"), "a half-done rename: nothing freed under anything");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_DONE);
    check(sed("-i", "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "1\n") && gone("a.txt.tmp"), "a rename done with an error");
    /* a rename that may have half happened: the disk needs checking, and nothing more is touched */
    {
        static const int how[] = { SIM_NONE, SIM_HALF, SIM_DONE };
        for(i = 0; i < 3; i++) {
            sim_reset();
            sim_put("a.txt", "one\n");
            sim_put("c.txt", "one\n");
            sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, how[i]);
            check(sed("-i", "s/one/1/", "a.txt", "c.txt", NULL) == 2, "a half-done rename, two files: status");
            check(file_is("c.txt", "one\n") && !strstr(sim_log(), "open c.txt"), "a half-done rename: the next file is left alone");
            /* and so is a w file not yet in its place */
            sim_reset();
            sim_put("a.txt", "one\n");
            sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, how[i]);
            check(sed("-i", "w out", "a.txt", NULL) == 2 && gone("out") && gone("out.tmp"), "a half-done rename: a w file does not appear");
        }
    }
    /* the same for a backup */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("c.txt", "one\n");
    sim_fail(SIM_RENAME, "a.txt.bak.tmp", SIM_EIO, SIM_HALF);
    check(sed("-i.bak", "s/one/1/", "a.txt", "c.txt", NULL) == 2, "a backup's half-done rename: status");
    check(file_is("a.txt", "one\n") && gone("a.txt.tmp"), "a backup's half-done rename: the file as it was");
    check(file_is("c.txt", "one\n") && !strstr(sim_log(), "open c.txt"), "a backup's half-done rename: the next file is left alone");
    /* and for w's files, the last one named goes first */
    sim_reset();
    sim_put("a.txt", "1\n");
    sim_fail(SIM_RENAME, "second.tmp", SIM_EIO, SIM_HALF);
    check(sed("w first\nw second", "a.txt", NULL) == 2, "a w file's half-done rename: status");
    check(gone("first") && gone("first.tmp") && !sim_corrupt("second"), "a w file's half-done rename: the other is left alone");
    /* a rename refused changed nothing: the next file goes on */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("c.txt", "one\n");
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_ENOSPC, SIM_NONE);
    check(sed("-i", "s/one/1/", "a.txt", "c.txt", NULL) == 2 && file_is("a.txt", "one\n") && file_is("c.txt", "1\n"), "a rename refused: the next file is done");
    /* r: a file that is not there is nothing, and anything else stops it all: the output would be short with no sign of it */
    sim_reset();
    sim_put("a.txt", "one\n");
    check(sed("-i", "r nosuch.txt", "a.txt", NULL) == 0 && file_is("a.txt", "one\n"), "-i and r of a missing file");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("r.txt", "R\n");
    sim_fail(SIM_OPEN, "r.txt", SIM_EIO, 0);
    check(sed("-i", "r r.txt", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && gone("a.txt.tmp"), "-i and r that cannot open its file");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("r.txt", "R\n");
    sed_fail_read(1);
    check(sed("-i", "r r.txt", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && gone("a.txt.tmp"), "-i and r that cannot read its file");
    sed_fail_read(-1);
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("r.txt", "R\n");
    check(sed("-n", "w o1\nw o2\nw o3\nw o4\nw o5\nw o6\nw o7\nr r.txt", "a.txt", NULL) == 2 && sim_names() == 2, "r out of descriptors");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("r.txt", "R\n");
    sim_fail(SIM_OPEN, "r.txt", SIM_EIO, 0);
    check(sed("-n", "r r.txt\nq7", "a.txt", NULL) == 2, "q's number does not hide what stopped the run");
    /* the input fails part of the way: what was read is no replacement */
    for(i = 0; i < 3; i++) {
        sim_reset();
        sim_put("a.txt", long_text());
        sim_put("c.txt", "1\n");
        sed_fail_read(i);
        check(sed("-i", "s/a/b/", "a.txt", NULL) == 2, "a read fails: status");
        check(file_is("a.txt", long_text()) && gone("a.txt.tmp"), "a read fails: the file is as it was");
    }
    sed_fail_read(-1);
}

static void backups(void) {
    sim_reset();
    sim_put("a.txt", "one\n");
    check(sed("-i.bak", "s/one/1/", "a.txt", NULL) == 0, "-i.bak: status");
    check(file_is("a.txt", "1\n") && file_is("a.txt.bak", "one\n") && sim_names() == 2, "-i.bak: both");
    /* the backup is whole before the file changes */
    check(strstr(sim_log(), "rename a.txt.bak.tmp a.txt.bak") && strstr(sim_log(), "rename a.txt.bak.tmp a.txt.bak") < strstr(sim_log(), "rename a.txt.tmp a.txt"), "-i.bak: the backup first");
    /* an old backup goes through a save too */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.bak", "older\n");
    check(sed("-i.bak", "s/one/1/", "a.txt", NULL) == 0 && file_is("a.txt.bak", "one\n"), "-i.bak over an old one");
    /* * in the suffix */
    sim_reset();
    sim_put("a.txt", "one\n");
    check(sed("-iold_*", "s/one/1/", "a.txt", NULL) == 0 && file_is("old_a.txt", "one\n"), "-iold_*");
    sim_reset();
    sim_mkdir("d");
    sim_put("d/a.txt", "one\n");
    check(sed("-iold_*", "s/one/1/", "d/a.txt", NULL) == 0 && file_is("d/old_a.txt", "one\n") && file_is("d/a.txt", "1\n"), "-iold_* in a folder");
    /* no backup, no change */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_WRITE, "a.txt.bak.tmp", SIM_ENOSPC, 0);
    check(sed("-i.bak", "s/one/1/", "a.txt", NULL) == 2, "a backup that fails: status");
    check(file_is("a.txt", "one\n") && sim_names() == 1, "a backup that fails: the file as it was, nothing beside it");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_fail(SIM_RENAME, "a.txt.bak.tmp", SIM_ENOSPC, SIM_NONE);
    check(sed("-i.bak", "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && sim_names() == 1, "a backup's rename refused");
    /* a backup named as the new contents' working name would be renamed over them, and the file would stay as it was, status 0 */
    {
        static const char *const taken[] = { "-i.tmp", "-i.TMP", "-i*.tmp", "-i*" };
        int i;
        for(i = 0; i < 4; i++) {
            sim_reset();
            sim_put("a.txt", "one\n");
            check(sed(taken[i], "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && sim_names() == 1, "a backup with the working name");
        }
    }
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.tmp", "the user's\n");
    check(sed("-i.1.tmp", "s/one/1/", "a.txt", NULL) == 2 && file_is("a.txt", "one\n") && file_is("a.txt.tmp", "the user's\n") && sim_names() == 2, "-i.1.tmp when .tmp is taken");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("c.txt", "one\n");
    sim_put("c.txt.tmp", "the user's\n");
    check(sed("-i.tmp", "s/one/1/", "a.txt", "c.txt", NULL) == 2, "-i.tmp, one refused: status");
    check(file_is("a.txt", "one\n") && file_is("c.txt", "1\n") && file_is("c.txt.tmp", "one\n") && sim_names() == 3, "-i.tmp, one refused: the next is done");
    /* the same file twice with a backup: the second pass's backup would hold the first's output, and the old file nowhere */
    sim_reset();
    sim_put("g.txt", "one\n");
    check(sed("-i.bak", "s/$/x/", "g.txt", "g.txt", NULL) == 2 && file_is("g.txt", "one\n") && sim_names() == 1, "one file twice with a backup");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("h.txt", "one\n");
    check(sed("-i.bak", "s/$/x/", "a.txt", "h.txt", "H.TXT", NULL) == 2, "one file in two cases with a backup: status");
    check(file_is("a.txt", "one\n") && file_is("h.txt", "one\n") && sim_names() == 2, "one file in two cases with a backup: nothing has changed");
    /* a name that is not there is said as ever, twice or not */
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("c.txt", "one\n");
    check(sed("-i.bak", "s/$/x/", "a.txt", "nosuch", "c.txt", NULL) == 2 && file_is("a.txt", "onex\n") && file_is("c.txt", "onex\n"), "a missing file with a backup");
    /* without a backup nothing is lost: it goes twice, as GNU's does */
    sim_reset();
    sim_put("g.txt", "one\n");
    check(sed("-i", "s/$/x/", "g.txt", "g.txt", NULL) == 0 && file_is("g.txt", "onexx\n"), "one file twice");
}

static void w_files(void) {
    /* a new file appears at the end */
    sim_reset();
    sim_put("a.txt", "1\n2\n");
    check(sed("-n", "w out.txt", "a.txt", NULL) == 0 && file_is("out.txt", "1\n2\n") && gone("out.txt.tmp"), "w: a new file");
    check(strstr(sim_log(), "open out.txt.tmp excl") != NULL, "w: excl");
    /* one that is there: not truncated, replaced */
    sim_reset();
    sim_put("a.txt", "1\n");
    sim_put("out.txt", "old\n");
    check(sed("-n", "w out.txt", "a.txt", NULL) == 0 && file_is("out.txt", "1\n"), "w: over a file");
    check(!strstr(sim_log(), "open out.txt trunc"), "w: nothing truncated");
    /* a run that fails leaves it */
    sim_reset();
    sim_put("a.txt", "1\n");
    sim_put("out.txt", "old\n");
    check(sed("w out.txt\nk", "a.txt", NULL) == 2 && file_is("out.txt", "old\n") && gone("out.txt.tmp"), "w and a mistake in the script");
    sim_reset();
    sim_put("a.txt", "1\n");
    check(sed("w new.txt\nk", "a.txt", NULL) == 2 && gone("new.txt") && gone("new.txt.tmp"), "w of a new file and a mistake");
    sim_reset();
    sim_put("a.txt", "1\n");
    sim_put("out.txt", "old\n");
    check(sed("w out.txt\ns//x/", "a.txt", NULL) == 2 && file_is("out.txt", "old\n") && gone("out.txt.tmp"), "w and a mistake at run time");
    /* a full disk */
    sim_reset();
    sim_put("a.txt", long_text());
    sim_put("out.txt", "old\n");
    sim_fail(SIM_WRITE, "out.txt.tmp", SIM_ENOSPC, 1);
    check(sed("-n", "w out.txt", "a.txt", NULL) == 2 && file_is("out.txt", "old\n") && gone("out.txt.tmp"), "w on a full disk");
    /* -i and w fail together: a w that fails stops it all */
    sim_reset();
    sim_put("a.txt", long_text());
    sim_fail(SIM_WRITE, "out.txt.tmp", SIM_ENOSPC, 0);
    check(sed("-i", "w out.txt", "a.txt", NULL) == 2 && file_is("a.txt", long_text()) && sim_names() == 1, "-i and w on a full disk");
    /* a w file that cannot be begun */
    sim_reset();
    sim_put("a.txt", "1\n");
    sim_fail(SIM_OPEN, "out.txt.tmp", SIM_EROFS, 0);
    check(sed("w out.txt", "a.txt", NULL) == 2 && sim_names() == 1, "w on a read-only disk");
    /* the same file twice is one file */
    sim_reset();
    sim_put("a.txt", "1\n2\n");
    check(sed("-n", "1w out.txt\n2w out.txt", "a.txt", NULL) == 0 && file_is("out.txt", "1\n2\n"), "w twice to one file");
    /* FAT ignores case, as this file system does: two names, one file, one output */
    sim_reset();
    sim_put("a.txt", "1\n2\n");
    check(sed("-n", "1w OUT.TXT\n2w out.txt", "a.txt", NULL) == 0 && file_is("out.txt", "1\n2\n") && sim_names() == 2, "w in two cases, a new file");
    sim_reset();
    sim_put("a.txt", "1\n2\n");
    sim_put("out.txt", "old\n");
    check(sed("-n", "1w out.txt\n2w OUT.TXT", "a.txt", NULL) == 0 && file_is("out.txt", "1\n2\n") && sim_names() == 2, "w in two cases, a file that is there");
    sim_reset();
    sim_put("a.txt", "1\n2\n");
    check(sed("-n", "1s/1/x/w Out.Txt\n2w out.txt\n2w OUT.TXT", "a.txt", NULL) == 0 && file_is("out.txt", "x\n2\n2\n") && sim_names() == 2, "s's w and w");
    /* one name that is another's working name would lose the other's lines at the end: refused */
    sim_reset();
    sim_put("a.txt", "1\n2\n");
    check(sed("-n", "1w out\n2w out.tmp", "a.txt", NULL) == 2 && sim_names() == 1, "w to the name the other goes through");
    sim_reset();
    sim_put("a.txt", "1\n2\n");
    check(sed("-n", "1w out.tmp\n2w out", "a.txt", NULL) == 0 && file_is("out.tmp", "1\n") && file_is("out", "2\n"), "the other way round is safe");
}

/* the memory comes back: a run frees all it took, in the simulated heap of 16 MB */
static void many_runs(void) {
    int i;
    for(i = 0; i < 3000; i++) {
        sim_reset();
        sim_put("a.txt", "one\ntwo\n");
        if(sed("-n", "-e", "/one/{s//1/p;h}", "-e", "y/two/TWO/;a text", "-e", "w out.txt", "a.txt", NULL)) {
            check(0, "run after run");
            break;
        }
    }
    check(file_is("out.txt", "1\nTWO\n"), "run after run: the last one's file");
    /* D keeps its place in the allocation: it goes back whole, and the room at the front is used */
    sim_reset();
    sim_put("a.txt", "one\ntwo\nthree\nfour\nfive\n");
    for(i = 0; i < 3000; i++) {
        if(sed("-i", "$!N\ns/$/ and more and more and more and more and more and more and more/\nP\nD", "a.txt", NULL)) {
            check(0, "run after run with D");
            break;
        }
        sim_reset();
        sim_put("a.txt", "one\ntwo\nthree\nfour\nfive\n");
    }
    check(sed("$!N\nP\nD", "a.txt", NULL) == 0, "run after run with D: the last one");
}

int main(void) {
    in_place();
    in_place_failures();
    backups();
    w_files();
    many_runs();
    printf("test_sedsave: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
