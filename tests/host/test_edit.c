/* Host-side test of edit's saving (10/17/26): user/bin/edit.c as it is (tests/host/edit_bridge.c) on an
   in-memory file system whose calls the test makes fail. Covered: the new file beside the file
   (<file>.tmp, or the next name past the user's own files of those names, which are never touched), what
   a failed write, fsync or rename leaves (what this save made goes, but never after a rename that may
   have half happened: the text stays in the new file, and from then on nothing is renamed or written
   over the file, so neither name's data is freed under the other), the same care when the file is opened
   with such a name beside it, the file written in place where there is no rename or no room for such a
   name, the kernel's limit on a path's length, and the line ends. 10/25/26 (track A, A9): ^Z and ^Y (runs of
   typing, Backspace, Delete and ^K as one change, an Enter and a paste alone, the cursor put back, the saved text
   unmodified again, redo forgotten by a new change, 1,000 changes kept), and UTF-8 as characters (the arrows,
   Backspace, Delete, columns up and down, typed byte by byte, drawn as it is or as '?', a long line cut at its
   cells). Run by `make unittest`. */
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

/**** 10/25/26 (track A, A9): ^Z and ^Y, UTF-8 ****/

#define CTRL(c) ((c) & 0x1f)

static void keys(const char *s) {
    for(; *s; s++) ed_key((unsigned char)*s);
}

static int at(int y, int x) {
    int cy, cx;
    ed_cursor(&cy, &cx);
    return cy == y && cx == x;
}

/* u.txt with "one" and "two", open, the cursor at the start */
static void fresh(const char *text) {
    sim_reset();
    sim_put("u.txt", text);
    ed_start("u.txt");
}

static void undo_redo(void) {
    int i;
    fresh("one\ntwo\n");
    keys("abc");
    check(!strcmp(ed_line(0), "abcone") && at(0, 3), "typing: abc at the start");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "one") && at(0, 0) && !ed_modified(), "^Z: the run of typing undone at once, the cursor "
          "back, the text as read again (not modified)");
    ed_key(CTRL('y'));
    check(!strcmp(ed_line(0), "abcone") && at(0, 3) && ed_modified(), "^Y: done again, the cursor after it");
    ed_key(CTRL('y'));
    check(!strncmp(ed_msg(), "Nothing to redo", 15), "^Y with nothing undone says so");

    fresh("one\ntwo\n");
    keys("ab");
    ed_key(ed_right);
    keys("c");
    check(!strcmp(ed_line(0), "abocne"), "typing, Right, typing");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "abone") && at(0, 3), "^Z: only the run after the cursor moved");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "one") && at(0, 0), "^Z: then the one before");
    ed_key(CTRL('z'));
    check(!strcmp(ed_msg(), "Nothing to undo"), "and then nothing, said");

    fresh("one\ntwo\n");
    keys("x\ry");
    check(ed_nlines() == 4 && !strcmp(ed_line(0), "x") && !strcmp(ed_line(1), "yone"), "x, Enter, y");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(1), "one") && at(1, 0), "^Z: y");
    ed_key(CTRL('z'));
    check(ed_nlines() == 3 && !strcmp(ed_line(0), "xone") && at(0, 1), "^Z: the Enter, a change of its own");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "one") && !ed_modified(), "^Z: x");
    ed_key(CTRL('y'));
    ed_key(CTRL('y'));
    ed_key(CTRL('y'));
    check(ed_nlines() == 4 && !strcmp(ed_line(0), "x") && !strcmp(ed_line(1), "yone") && at(1, 1),
          "^Y three times: all of it again");

    fresh("one\ntwo\n");
    ed_key(ed_end);
    ed_key(0x7f);
    ed_key(0x7f);
    check(!strcmp(ed_line(0), "o") && at(0, 1), "Backspace twice at the end");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "one") && at(0, 3), "^Z: the run of Backspace at once");
    ed_key(ed_home);
    ed_key(ed_del);
    ed_key(ed_del);
    check(!strcmp(ed_line(0), "e"), "Delete twice at the start");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "one") && at(0, 0), "^Z: the run of Delete at once");
    ed_key(ed_down);
    ed_key(ed_home);
    ed_key(0x7f);
    check(ed_nlines() == 2 && !strcmp(ed_line(0), "onetwo") && at(0, 3), "Backspace at a line's start joins the lines");
    ed_key(CTRL('z'));
    check(ed_nlines() == 3 && !strcmp(ed_line(0), "one") && !strcmp(ed_line(1), "two") && at(1, 0),
          "^Z: apart again, the cursor where the Backspace was");

    fresh("one\ntwo\nthree\n");
    ed_key(CTRL('k'));
    ed_key(CTRL('k'));
    check(ed_nlines() == 2 && !strcmp(ed_line(0), "three"), "^K twice");
    ed_key(CTRL('z'));
    check(ed_nlines() == 4 && !strcmp(ed_line(0), "one") && !strcmp(ed_line(1), "two") && !strcmp(ed_line(2), "three"),
          "^Z: both lines back, in their order");
    ed_key(CTRL('y'));
    ed_key(ed_down);
    ed_key(CTRL('u'));
    check(ed_nlines() == 4 && !strcmp(ed_line(0), "three") && !strcmp(ed_line(1), "one") && !strcmp(ed_line(2), "two"),
          "^Y, then ^U pastes them");
    ed_key(CTRL('z'));
    check(ed_nlines() == 2 && !strcmp(ed_line(0), "three") && !strcmp(ed_line(1), ""), "^Z: the paste undone");

    fresh("only\n");
    ed_key(ed_down);
    ed_key(CTRL('k'));
    ed_key(ed_home);
    ed_key(ed_up);
    ed_key(CTRL('k'));
    check(ed_nlines() == 1 && !strcmp(ed_line(0), ""), "^K of the last lines: one empty line left");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "only"), "^Z: the only line's text back");

    fresh("one\n");
    keys("a");
    ed_key(CTRL('z'));
    keys("b");
    ed_key(CTRL('y'));
    check(!strcmp(ed_line(0), "bone") && !strncmp(ed_msg(), "Nothing to redo", 15),
          "a change after ^Z: what was undone cannot be done again");

    fresh("one\n");
    keys("a");
    check(ed_save() == 0 && !ed_modified(), "typed and saved");
    keys("b");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "aone") && !ed_modified(), "typing after a save is a change of its own: ^Z gives the "
          "saved text, not modified");
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "one") && ed_modified(), "^Z before the save: modified");
    ed_key(CTRL('y'));
    check(!strcmp(ed_line(0), "aone") && !ed_modified(), "^Y back to the saved text: not modified");

    fresh("\n");
    for(i = 0; i < 1001; i++) {                 /* 1,001 changes: each letter, then Home to end the run */
        ed_key('a' + i % 26);
        ed_key(ed_home);
    }
    for(i = 0; i < 1000; i++) ed_key(CTRL('z'));
    check((int)strlen(ed_line(0)) == 1, "1,000 changes undone: the first is kept no more");
    check(ed_modified() && ed_quit() == 0, "... so the text is still modified, and ^Q asks before it quits (the "
          "programs' review, U2)");
    ed_key(CTRL('z'));
    check(!strcmp(ed_msg(), "Nothing to undo"), "... and then nothing to undo");
}

static void utf8_text(void) {
    const char *r;
    int cells, i;
    char line[512];
    fresh("caf\xc3\xa9 \xc2\xb1\nab\n");
    ed_utf8(1);
    ed_key(ed_end);
    check(at(0, 8) && ed_col() == 6, "End: after the line's 8 bytes, at column 6");
    ed_key(ed_left);
    ed_key(ed_left);
    ed_key(ed_left);
    check(at(0, 3) && ed_col() == 3, "Left three times: before the e with its accent (one character, two bytes)");
    ed_key(ed_right);
    check(at(0, 5) && ed_col() == 4, "Right over it: two bytes");
    ed_key(0x7f);
    check(!strcmp(ed_line(0), "caf \xc2\xb1") && at(0, 3), "Backspace takes the whole character");
    ed_key(ed_end);
    ed_key(ed_left);
    ed_key(ed_del);
    check(!strcmp(ed_line(0), "caf "), "Delete too");
    ed_key(CTRL('z'));
    ed_key(CTRL('z'));
    check(!strcmp(ed_line(0), "caf\xc3\xa9 \xc2\xb1"), "^Z gives them back");
    ed_key(ed_end);
    ed_key(ed_down);
    check(at(1, 2), "Down from column 6 to a shorter line: its end");
    ed_key(ed_up);
    check(at(0, 8), "Up again: the column the cursor had (6), the end, byte 8");
    ed_key(ed_home);
    ed_key(0xc3);
    ed_key(0xbc);
    check(!strncmp(ed_line(0), "\xc3\xbc" "caf", 5) && at(0, 2) && ed_col() == 1,
          "UTF-8 typed a byte at a time (a terminal's): one character, one column");
    ed_key(CTRL('z'));
    check(!strncmp(ed_line(0), "caf", 3), "^Z: the character at once");

    r = ed_render(0, 0);
    check(!strcmp(r, "caf\xc3\xa9 \xc2\xb1"), "drawn as it is where the console draws UTF-8");
    ed_utf8(0);
    r = ed_render(0, 0);
    check(!strcmp(r, "caf? ?"), "and as ? where it does not");
    ed_utf8(1);

    fresh("a\xff" "b\xc2\x85" "c\xe2\x82\n");
    ed_utf8(1);
    check(!strcmp(ed_render(0, 0), "a?b?c??"), "a byte that is not UTF-8, a C1 control and a sequence cut short: a ? each");
    ed_key(ed_end);
    check(ed_col() == 7, "... a column each");

    for(i = 0; i < 100; i++) memcpy(line + 2 * i, "\xc3\xa9", 2);
    memcpy(line + 200, "\n", 2);
    fresh(line);
    ed_utf8(1);
    r = ed_render(0, 0);
    check((int)strlen(r) == 79 * 2 + 1 && r[158] == '>' && ed_cells(r, 1000, &cells) == 159 && cells == 80,
          "100 such characters on 80 columns: 79 of them and '>'");
    check(ed_cells("a\xc3\xa9 b", 2, &cells) == 3 && cells == 2, "a row's cells counted in characters, not bytes");
}

int main(void) {
    plain();
    users_files();
    failed_write();
    failed_rename();
    twins();
    in_place();
    line_ends();
    undo_redo();
    utf8_text();
    printf("EDIT UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
