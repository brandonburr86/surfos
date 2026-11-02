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
