/* Host-side test of edit's saving (10/17/26): user/bin/edit.c as it is (tests/host/edit_bridge.c) on an
   in-memory file system whose calls the test makes fail. Covered: the new file beside the file
   (<file>.tmp, or the next name past the user's own files of those names, which are never touched), what
   a failed write, fsync or rename leaves (what this save made goes, but never after a rename that may
   have half happened: the text stays in the new file, and from then on nothing is renamed or written
   over the file, so neither name's data is freed under the other), the same care when the file is opened
   with such a name beside it, the file written in place where there is no rename or no room for such a
   name, the kernel's limit on a path's length, and the line ends. Run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "edit_sim.h"

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static int has(const char *s, const char *part) {
    return strstr(s, part) != NULL;
}

/* name holds text */
static int is(const char *name, const char *text) {
    static char buf[16384];
    return sim_get(name, buf, sizeof(buf)) == (int)strlen(text) && !strcmp(buf, text);
}

/* a.txt with two lines, open in the editor, an X typed at its start */
static void start(void) {
    sim_reset();
    sim_put("a.txt", "one\ntwo\n");
    ed_start("a.txt");
    ed_type("X");
}

static void plain(void) {
    const char *log;
    start();
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1, "a save: a.txt.tmp renamed over the file");
    log = sim_log();
    check(has(log, "open a.txt.tmp excl\n") && has(log, "fsync a.txt.tmp\n") && has(log, "rename a.txt.tmp a.txt\n") &&
          strstr(log, "fsync a.txt.tmp\n") < strstr(log, "rename a.txt.tmp a.txt\n"),
          "the new file is made with O_EXCL and fsynced before the rename");
    check(!ed_modified() && !ed_unsure() && !strcmp(ed_msg(), "Wrote 2 lines"), "then: not modified, and Wrote 2 lines");
}

static void users_files(void) {
    char name[32];
    int k;
    start();
    sim_put("a.txt.tmp", "mine");
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && is("a.txt.tmp", "mine") && sim_names() == 2,
          "the user's a.txt.tmp is not touched");
    check(has(sim_log(), "rename a.txt.1.tmp a.txt\n"), "a.txt.1.tmp is the new file then");

    start();
    sim_put("A.TXT.TMP", "mine");
    check(ed_save() == 0 && is("A.TXT.TMP", "mine") && has(sim_log(), "rename a.txt.1.tmp a.txt\n"),
          "nor is A.TXT.TMP: FAT's names ignore case");

    start();
    sim_mkdir("a.txt.tmp");
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 2 && has(sim_log(), "rename a.txt.1.tmp a.txt\n"),
          "a folder named a.txt.tmp is passed by too");

    start();
    sim_put("a.txt.tmp", "mine");
    for(k = 1; k < 10; k++) {
        snprintf(name, sizeof(name), "a.txt.%d.tmp", k);
        sim_put(name, "mine");
    }
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 11 && ed_modified(), "ten names taken: not saved");
    check(is("a.txt.tmp", "mine") && is("a.txt.9.tmp", "mine") && !has(sim_log(), "unlink") && !has(sim_log(), "trunc"),
          "... the user's files as they were, nothing written in place");
    check(!strcmp(ed_msg(), "Not saved: File exists (a.txt.tmp): move it out of the way"), "... and said");
}

static void failed_write(void) {
    char big[12000];
    start();
    sim_fail(SIM_WRITE, "a.txt.tmp", SIM_ENOSPC, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 1 && ed_modified(),
          "a failed write: the file as it was, the new file gone");
    check(!strcmp(ed_msg(), "Not saved: No space left on device (a.txt.tmp)"), "... and said, the reason first");

    start();
    sim_fail(SIM_WRITE, "a.txt.tmp", 0, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 1 &&
          !strcmp(ed_msg(), "Not saved: No space left on device (a.txt.tmp)"), "a short write is a full disk");

    sim_reset();                                /* more than one 8 KB piece */
    memset(big, 'x', sizeof(big) - 2);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = 0;
    sim_put("big.txt", big);
    ed_start("big.txt");
    ed_type("X");
    sim_fail(SIM_WRITE, "big.txt.tmp", SIM_ENOSPC, 1);
    check(ed_save() < 0 && is("big.txt", big) && sim_names() == 1 && has(sim_log(), "write big.txt.tmp 8192\n") &&
          has(sim_log(), "unlink big.txt.tmp\n"), "a write that fails on the second 8 KB piece: the file as it was, the new file gone");

    start();
    sim_fail(SIM_FSYNC, "a.txt.tmp", SIM_EIO, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 1 && !has(sim_log(), "rename"),
          "a failed fsync: no rename, the new file gone");

    start();
    sim_put("a.txt.tmp", "mine");
    sim_fail(SIM_WRITE, "a.txt.1.tmp", SIM_ENOSPC, 0);
    check(ed_save() < 0 && is("a.txt.tmp", "mine") && sim_names() == 2 && has(sim_log(), "unlink a.txt.1.tmp\n") &&
          !has(sim_log(), "unlink a.txt.tmp\n"), "a failed write deletes only the file this save made");

    start();
    sim_fail(SIM_OPEN, "a.txt.tmp", SIM_EROFS, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 1 && !has(sim_log(), "unlink") && !has(sim_log(), "trunc"),
          "no new file (read-only): nothing deleted, nothing written in place");
    check(!strcmp(ed_msg(), "Not saved: Read-only file system (a.txt.tmp)"), "... and said");
}

static void failed_rename(void) {
    static const int none[] = { SIM_EXDEV, SIM_EROFS, SIM_EBUSY, SIM_EINVAL, SIM_ENOENT, SIM_EFAULT, SIM_ENOTDIR,
                                SIM_EISDIR, SIM_ENOTEMPTY, SIM_ENAMETOOLONG, SIM_ENOSPC, SIM_ENOMEM };
    static const int maybe[] = { SIM_EIO, SIM_EPERM };     /* EPERM: none in this kernel; one edit does not list */
    char what[80];
    unsigned i;
    for(i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        start();
        sim_fail(SIM_RENAME, "a.txt.tmp", none[i], SIM_NONE);
        snprintf(what, sizeof(what), "a rename that failed with %d changed nothing: the new file goes", none[i]);
        check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 1 && ed_modified() && !ed_unsure(), what);
    }
    for(i = 0; i < sizeof(maybe) / sizeof(maybe[0]); i++) {
        start();
        sim_fail(SIM_RENAME, "a.txt.tmp", maybe[i], SIM_NONE);
        snprintf(what, sizeof(what), "after a rename that failed with %d the new file is kept", maybe[i]);
        check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && is("a.txt.tmp", "Xone\ntwo\n") && !has(sim_log(), "unlink") &&
              ed_unsure(), what);
    }
    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EBUSY, SIM_NONE);
    check(ed_save() < 0 && !strcmp(ed_msg(), "Not saved: Device or resource busy (rename)"), "a rename that changed nothing is said");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_HALF);
    check(ed_save() < 0 && is("a.txt.tmp", "Xone\ntwo\n") && !sim_corrupt("a.txt") && ed_modified() && ed_unsure(),
          "a half-done rename (EIO): the new file is kept, and the file's data is not freed under it");
    check(!has(sim_log(), "unlink"), "... nothing is deleted");
    check(!strcmp(ed_msg(), "Not saved: I/O error (rename). The text is in a.txt.tmp"), "... and the message names the new file");
    check(!strcmp(ed_copied(), "a.txt.tmp") && ed_quit() == 1,
          "... and with no edit since, ^Q ends edit without asking (it then says the text is in a.txt.tmp)");
    sim_clear_log();
    ed_type("Y");
    check(ed_save() == 1 && is("a.txt.1.tmp", "XYone\ntwo\n") && !has(sim_log(), "rename") && !has(sim_log(), "unlink") &&
          !has(sim_log(), "trunc") && ed_modified(), "the next save makes a copy beside the file, and renames nothing");
    check(!sim_corrupt("a.txt") && !sim_corrupt("a.txt.tmp") && is("a.txt.tmp", "Xone\ntwo\n"),
          "... so neither name's data is freed under the other");
    check(!strcmp(ed_msg(), "Not saved over the file; a copy is in a.txt.1.tmp") && !strcmp(ed_copied(), "a.txt.1.tmp"),
          "... and says where the copy is");
    sim_clear_log();
    check(ed_save() == 1 && !has(sim_log(), "open") && !strcmp(ed_msg(), "Not saved over the file; the text is in a.txt.1.tmp"),
          "with no edit since the last copy, no other copy is made");
    ed_type("Z");
    check(ed_quit() == 0 && ed_modified(), "an edit since the last copy: ^Q asks first (the input ends here: cancelled)");
    check(ed_save() == 1 && is("a.txt.2.tmp", "XYZone\ntwo\n") && is("a.txt.1.tmp", "XYone\ntwo\n") && !sim_corrupt("a.txt.tmp") &&
          !strcmp(ed_copied(), "a.txt.2.tmp"), "the one after that makes another copy, and leaves the first");
    ed_type("W");
    sim_clear_log();
    sim_fail(SIM_OPEN, "a.txt.tmp", SIM_ENAMETOOLONG, 0);
    check(ed_save() < 0 && !has(sim_log(), "trunc") && !sim_corrupt("a.txt.tmp"), "... and none is ever written in place");
    sim_put("a.txt.3.tmp", "mine");
    sim_put("a.txt.4.tmp", "mine");
    sim_put("a.txt.5.tmp", "mine");
    sim_put("a.txt.6.tmp", "mine");
    sim_put("a.txt.7.tmp", "mine");
    sim_put("a.txt.8.tmp", "mine");
    sim_put("a.txt.9.tmp", "mine");
    check(ed_save() < 0 && !strcmp(ed_msg(), "Not saved: no name is left for a copy beside the file"),
          "with no name left for a copy: said, without asking to move anything");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_DONE);
    check(ed_save() < 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1 && !strcmp(ed_msg(), "Not saved: I/O error (rename)") &&
          ed_unsure(), "a rename done all the same: no new file to name, the same care");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EXDEV, SIM_NONE);
    sim_fail(SIM_UNLINK, "a.txt.tmp", SIM_EIO, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && is("a.txt.tmp", "Xone\ntwo\n") && !ed_unsure() &&
          !strcmp(ed_msg(), "Not saved: Invalid cross-device link (rename). The text is in a.txt.tmp"),
          "a new file that would not go is named");

    sim_reset();
    sim_put("/stick/notes/a.txt", "one\n");
    ed_start("/stick/notes/a.txt");
    ed_type("X");
    sim_fail(SIM_RENAME, "/stick/notes/a.txt.tmp", SIM_EIO, SIM_HALF);
    check(ed_save() < 0 && !strcmp(ed_msg(), "Not saved: I/O error (rename). The text is in a.txt.tmp"),
          "the new file is named by its last part (it is beside the file)");
}

/* a twin beside the file when it is opened: a rename that may have half happened, in an earlier edit */
static void twins(void) {
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.tmp", "one\n");
    sim_same_time("a.txt", "a.txt.tmp");
    ed_start("a.txt");
    check(ed_unsure() && !strcmp(ed_msg(), "a.txt.tmp may share this file's data: check the disk"),
          "a .tmp beside the file of its size and time: said, and the same care");
    ed_type("X");
    check(ed_save() == 1 && is("a.txt.1.tmp", "Xone\n") && is("a.txt", "one\n") && !has(sim_log(), "rename"),
          "... so saving makes a copy");

    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.2.tmp", "one\n");
    sim_same_time("a.txt", "a.txt.2.tmp");
    ed_start("a.txt.2.tmp");
    check(ed_unsure() && has(ed_msg(), "a.txt may share"), "opening the .2.tmp itself: the file beside it is found");

    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.3.tmp", "one\n");
    sim_same_time("a.txt", "a.txt.3.tmp");
    ed_start("a.txt");
    check(ed_unsure() && has(ed_msg(), "a.txt.3.tmp may share"), "... a.txt.3.tmp too");

    sim_reset();
    sim_put("x.new", "data\n");
    sim_put("x.new.part", "data\n");
    sim_same_time("x.new", "x.new.part");
    ed_start("x.new");
    check(ed_unsure() && has(ed_msg(), "x.new.part may share"), "... and rx's x.new.part");

    sim_reset();
    sim_put("log.1", "entry\n");
    sim_put("log.1.tmp", "entry\n");
    sim_same_time("log.1", "log.1.tmp");
    ed_start("log.1.tmp");
    check(ed_unsure() && has(ed_msg(), "log.1 may share"), "opening log.1.tmp: log.1 beside it is found, not only log");

    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.tmp", "one\n");               /* a later time: not a twin */
    ed_start("a.txt");
    check(!ed_unsure(), "a .tmp of the same size but another time is not taken for one");
    sim_reset();
    sim_put("a.txt", "one\n");
    sim_put("a.txt.tmp", "longer\n");
    sim_same_time("a.txt", "a.txt.tmp");
    ed_start("a.txt");
    check(!ed_unsure(), "nor one of the same time but another size");

    sim_reset();                                /* one made after the file was opened: found when saving */
    sim_put("a.txt", "one\n");
    ed_start("a.txt");
    sim_put("a.txt.tmp", "one\n");
    sim_same_time("a.txt", "a.txt.tmp");
    ed_type("X");
    check(ed_save() == 1 && ed_unsure() && is("a.txt", "one\n") && is("a.txt.1.tmp", "Xone\n") && is("a.txt.tmp", "one\n") &&
          !strcmp(ed_msg(), "Not saved over the file; a copy is in a.txt.1.tmp (a.txt.tmp may share its data)"),
          "a twin that came after the file was opened is found when saving: a copy beside it, and said why");

    sim_reset();
    sim_put("notes.txt", "one\n");
    sim_put("notes.txt.tmp", "one\n");
    sim_same_time("notes.txt", "notes.txt.tmp");
    ed_start("NOTES.TXT.TMP");
    check(ed_unsure() && has(ed_msg(), "NOTES.TXT may share"), "the name typed in capitals: the pair is found all the same");

    sim_reset();
    sim_put("e.txt", "");
    sim_put("e.txt.tmp", "");
    sim_same_time("e.txt", "e.txt.tmp");
    ed_start("e.txt");
    check(!ed_unsure(), "two empty files are no pair: they hold no clusters");
}

static void in_place(void) {
    char name[300];
    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_ENOSYS, SIM_NONE);
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1 && has(sim_log(), "unlink a.txt.tmp\n") &&
          has(sim_log(), "open a.txt trunc\n"), "no rename here (ENOSYS): the new file goes, the file is written in place");
    sim_clear_log();
    ed_type("Y");
    check(ed_save() == 0 && is("a.txt", "XYone\ntwo\n") && !has(sim_log(), "open a.txt.tmp"), "... and from then on, with no new file");
    sim_put("a.txt.tmp", "XYone\ntwo\n");
    sim_same_time("a.txt", "a.txt.tmp");
    ed_type("Z");
    sim_clear_log();
    check(ed_save() == 1 && !has(sim_log(), "trunc") && is("a.txt", "XYone\ntwo\n") && is("a.txt.1.tmp", "XYZone\ntwo\n") &&
          !strcmp(ed_msg(), "Not saved over the file; a copy is in a.txt.1.tmp (a.txt.tmp may share its data)"),
          "where there is no rename, a twin beside the file: a copy beside it, never in place");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_ENOSYS, SIM_NONE);
    sim_fail(SIM_UNLINK, "a.txt.tmp", SIM_EIO, 0);
    sim_fail(SIM_WRITE, "a.txt", SIM_ENOSPC, 0);
    check(ed_save() < 0 && is("a.txt.tmp", "Xone\ntwo\n") &&
          !strcmp(ed_msg(), "Not saved: No space left on device (a.txt). The text is in a.txt.tmp"),
          "in place failing too, with a new file that would not go: the message names it");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_ENOSYS, SIM_NONE);
    sim_fail(SIM_WRITE, "a.txt", SIM_ENOSPC, 0);
    check(ed_save() < 0 && sim_get("a.txt.tmp", name, sizeof(name)) < 0 &&
          !strcmp(ed_msg(), "Not saved: No space left on device (a.txt)"), "... and names none that went");

    start();
    sim_fail(SIM_OPEN, "a.txt.tmp", SIM_ENAMETOOLONG, 0);
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1 && has(sim_log(), "open a.txt trunc\n"),
          "no room for the name a.txt.tmp: in place");

    start();
    sim_put("a.txt.tmp", "mine");
    sim_fail(SIM_OPEN, "a.txt.1.tmp", SIM_ENAMETOOLONG, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && is("a.txt.tmp", "mine") && !has(sim_log(), "trunc") &&
          !strcmp(ed_msg(), "Not saved: File exists (a.txt.tmp): move it out of the way"),
          "a.txt.tmp taken and the next name too long: not in place, and said");

    /* the kernel takes paths of 255 bytes at most */
    name[0] = '/';
    memset(name + 1, 'n', 250);
    name[251] = 0;
    sim_reset();
    sim_put(name, "one\n");
    ed_start(name);
    ed_type("X");
    check(ed_save() == 0 && is(name, "Xone\n") && sim_names() == 1 && has(sim_log(), "excl") && !sim_long_paths(),
          "a path of 251 bytes: its .tmp, of 255, fits");

    memset(name, 'n', 252);
    strcpy(name + 252, ".tm");                  /* <name>.tmp cut off to fit */
    sim_reset();
    sim_put(name, "mine");
    name[252] = 0;
    sim_put(name, "one\n");
    ed_start(name);
    ed_type("X");
    check(ed_save() == 0 && is(name, "Xone\n") && sim_names() == 2 && !has(sim_log(), "excl") && !sim_long_paths(),
          "a 252-byte name has no room for .tmp: in place, and no path too long is ever tried");
    strcpy(name + 252, ".tm");
    check(is(name, "mine"), "... never by a cut-off name, which may be the user's");

    memset(name, 'n', 250);
    name[250] = 0;
    sim_reset();
    sim_put(name, "one\n");
    strcpy(name + 250, ".tmp");
    sim_put(name, "mine");
    name[250] = 0;
    ed_start(name);
    ed_type("X");
    check(ed_save() < 0 && is(name, "one\n") && !has(sim_log(), "trunc") && !sim_long_paths() &&
          !strncmp(ed_msg(), "Not saved: File exists (", 24),
          "a 250-byte name with its .tmp taken has no room for .1.tmp: not in place, and the reason shows");
}

static void line_ends(void) {
    sim_reset();
    sim_put("w.txt", "one\r\ntwo\r\n");
    ed_start("w.txt");
    ed_type("X");
    check(ed_save() == 0 && is("w.txt", "Xone\r\ntwo\r\n"), "CRLF line ends are kept");

    sim_reset();
    ed_start("new.txt");
    ed_type("hello\nworld");
    check(ed_save() == 0 && is("new.txt", "hello\nworld\n") && sim_names() == 1, "a new file, with a line end at its end");
}

int main(void) {
    plain();
    users_files();
    failed_write();
    failed_rename();
    twins();
    in_place();
    line_ends();
    printf("EDIT UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
