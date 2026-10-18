/* Host-side test of edit's saving (10/17/26): user/bin/edit.c as it is (tests/host/edit_bridge.c) on an
   in-memory file system whose calls the test makes fail. Covered: the new file beside the file
   (<file>.tmp, or the next name past the user's own files of those names, which are never touched), what
   a failed write, fsync or rename leaves (what this save made goes, but never after a rename that may
   have half happened: the text stays in the new file, the file's data is not freed, and the status line
   names it), the file written in place where there is no rename or no room for such a name, and the
   line ends. Run by `make unittest`. */
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
    char buf[4096];
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
    start();
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1, "a save: a.txt.tmp renamed over the file");
    check(has(sim_log(), "open a.txt.tmp excl\n") && has(sim_log(), "fsync a.txt.tmp\n") &&
          strstr(sim_log(), "fsync a.txt.tmp\n") < strstr(sim_log(), "rename a.txt.tmp a.txt\n"),
          "the new file is made with O_EXCL and fsynced before the rename");
    check(!ed_modified() && !strcmp(ed_msg(), "Wrote 2 lines"), "then: not modified, and Wrote 2 lines");
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
    check(!strcmp(ed_msg(), "Not saved: a.txt.tmp: File exists (move it out of the way)"), "... and said");
}

static void failed_write(void) {
    start();
    sim_fail(SIM_WRITE, "a.txt.tmp", SIM_ENOSPC, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 1 && ed_modified(),
          "a failed write: the file as it was, the new file gone");
    check(!strcmp(ed_msg(), "Not saved: a.txt.tmp: No space left on device"), "... and said");

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
    check(!strcmp(ed_msg(), "Not saved: a.txt.tmp: Read-only file system"), "... and said");
}

static void failed_rename(void) {
    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EBUSY, SIM_NONE);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && sim_names() == 1 && ed_modified(),
          "a rename that changed nothing (EBUSY): the new file goes");
    check(!strcmp(ed_msg(), "Not saved: rename: Device or resource busy"), "... and said");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_HALF);
    check(ed_save() < 0 && is("a.txt.tmp", "Xone\ntwo\n") && !sim_corrupt("a.txt") && ed_modified(),
          "a half-done rename (EIO): the new file is kept, and the file's data is not freed under it");
    check(!has(sim_log(), "unlink"), "... nothing is deleted");
    check(!strcmp(ed_msg(), "Not saved: rename: I/O error. The text is in a.txt.tmp"), "... and the message names the new file");
    sim_clear_log();
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && has(sim_log(), "rename a.txt.1.tmp a.txt\n") && !ed_modified(),
          "the next save goes by a.txt.1.tmp");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_NONE);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && is("a.txt.tmp", "Xone\ntwo\n") && !has(sim_log(), "unlink"),
          "EIO with nothing done is kept just the same (there is no telling)");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EIO, SIM_DONE);
    check(ed_save() < 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1 && !strcmp(ed_msg(), "Not saved: rename: I/O error"),
          "a rename done all the same: no new file to name");

    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_EXDEV, SIM_NONE);
    sim_fail(SIM_UNLINK, "a.txt.tmp", SIM_EIO, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && is("a.txt.tmp", "Xone\ntwo\n") &&
          !strcmp(ed_msg(), "Not saved: rename: Invalid cross-device link. The text is in a.txt.tmp"),
          "a new file that would not go is named");

    sim_reset();
    sim_put("/stick/notes/a.txt", "one\n");
    ed_start("/stick/notes/a.txt");
    ed_type("X");
    sim_fail(SIM_RENAME, "/stick/notes/a.txt.tmp", SIM_EIO, SIM_HALF);
    check(ed_save() < 0 && !strcmp(ed_msg(), "Not saved: rename: I/O error. The text is in a.txt.tmp"),
          "the new file is named by its last part (it is beside the file)");
}

static void in_place(void) {
    char name[300];
    start();
    sim_fail(SIM_RENAME, "a.txt.tmp", SIM_ENOSYS, SIM_NONE);
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1 && has(sim_log(), "unlink a.txt.tmp\n") &&
          has(sim_log(), "open a.txt trunc\n"), "no rename here (ENOSYS): the new file goes, the file is written in place");
    sim_clear_log();
    ed_type("Y");
    check(ed_save() == 0 && is("a.txt", "XYone\ntwo\n") && !has(sim_log(), ".tmp"), "... and from then on, with no new file");

    start();
    sim_fail(SIM_OPEN, "a.txt.tmp", SIM_ENAMETOOLONG, 0);
    check(ed_save() == 0 && is("a.txt", "Xone\ntwo\n") && sim_names() == 1 && has(sim_log(), "open a.txt trunc\n"),
          "no room for the name a.txt.tmp: in place");

    start();
    sim_put("a.txt.tmp", "mine");
    sim_fail(SIM_OPEN, "a.txt.1.tmp", SIM_ENAMETOOLONG, 0);
    check(ed_save() < 0 && is("a.txt", "one\ntwo\n") && is("a.txt.tmp", "mine") && !has(sim_log(), "trunc") &&
          !strcmp(ed_msg(), "Not saved: a.txt.tmp: File exists (move it out of the way)"),
          "a.txt.tmp taken and the next name too long: not in place, and said");

    memset(name, 'n', 260);                     /* past edit's own buffer for the new file's name */
    strcpy(name + 260, ".tm");                  /* <name>.tmp cut off to fit it */
    sim_reset();
    sim_put(name, "mine");
    name[260] = 0;
    sim_put(name, "one\n");
    ed_start(name);
    ed_type("X");
    check(ed_save() == 0 && is(name, "Xone\n") && sim_names() == 2 && !has(sim_log(), "excl"),
          "a name with no room for .tmp: in place");
    strcpy(name + 260, ".tm");
    check(is(name, "mine"), "... never by a cut-off name, which may be the user's");
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
    in_place();
    line_ends();
    printf("EDIT UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
