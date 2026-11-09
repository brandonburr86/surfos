/* Reading and saving the Code Editor's files (user/wcode/fileio.c, 11/05/26, the desktop applications, track code) on the
   simulated file system of tests/host/sim_fs.c, where opens, writes, fsyncs and renames fail on request, through the
   bridge tests/host/wcode_save_bridge.c. These are edit's rules (tests/host/test_edit.c) and saveutil.h's, kept by a program
   with a window: what matters is what is on the disk after each failure. A file is as it was or complete; nothing is ever
   truncated or written in place; a new file the save made goes again when it fails; after a rename that may have half
   happened nothing is renamed or written over the file again, each save putting a copy beside it, and neither of the two
   is deleted. Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "wcode_sim.h"

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n  the message: %s\n  the calls: %s\n", what, wcs_msg(), sim_log());
    }
}

/* the file's bytes are want's, all of them and no more (11/09/26: by count, not as strings) */
static int is(const char *name, const char *want) {
    static char buf[1 << 16];
    int n = sim_get(name, buf, sizeof(buf));
    return n >= 0 && (size_t)n == strlen(want) && !memcmp(buf, want, (size_t)n);
}

static int gone(const char *name) {
    char buf[8];
    return sim_get(name, buf, sizeof(buf)) < 0;
}

static int has(const char *s, const char *part) {
    return strstr(s, part) != NULL;
}

/* a.c read, an X typed at its start, the log clear */
static void start(void) {
    sim_reset();
    sim_put("a.c", "one\ntwo\n");
    wcs_open("a.c");
    wcs_type("X");
    sim_clear_log();
}

static void saving(void) {
    char name[32];
    int k;
    start();
    check(wcs_modified() && !wcs_safe(), "an edit: unsaved");
    check(wcs_save(NULL) == 0 && is("a.c", "Xone\ntwo\n") && sim_names() == 1 && !wcs_modified() && wcs_safe(),
          "saved: through a new file beside it, which is gone");
    check(has(sim_log(), "open a.c.tmp excl\n") && has(sim_log(), "fsync a.c.tmp\n") &&
          has(sim_log(), "rename a.c.tmp a.c\n") && !has(sim_log(), "trunc"), "... O_EXCL, fsynced, renamed, never truncated");
    check(!strcmp(wcs_msg(), "Saved a.c"), "... and said");
    wcs_undo();
    check(wcs_modified(), "undo after a save: unsaved again");

    start();
    sim_put("a.c.tmp", "mine");
    check(wcs_save(NULL) == 0 && is("a.c", "Xone\ntwo\n") && is("a.c.tmp", "mine") && has(sim_log(), "rename a.c.1.tmp a.c\n"),
          "the user's a.c.tmp is not touched: a.c.1.tmp is the new file");

    start();
    sim_put("a.c.tmp", "mine");
    for(k = 1; k < 10; k++) {
        snprintf(name, sizeof(name), "a.c.%d.tmp", k);
        sim_put(name, "mine");
    }
    check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && sim_names() == 11 && wcs_modified() &&
          !has(sim_log(), "unlink") && !has(sim_log(), "trunc"), "ten names taken: not saved, nothing touched");
    check(!strcmp(wcs_msg(), "Not saved: no name is free beside it: move a.c.tmp out of the way"), "... and said");

    start();
    sim_fail(SIM_WRITE, "a.c.tmp", SIM_ENOSPC, 0);
    check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && sim_names() == 1 && wcs_modified(),
          "a failed write: the file as it was, the new file gone");
    check(!strcmp(wcs_msg(), "Not saved: No space left on device"), "... and said");

    start();
    sim_fail(SIM_WRITE, "a.c.tmp", 0, 0);
    check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && sim_names() == 1 && has(wcs_msg(), "No space"),
          "a short write is a full disk");

    start();
    sim_fail(SIM_FSYNC, "a.c.tmp", SIM_EIO, 0);
    check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && sim_names() == 1 && !has(sim_log(), "rename"),
          "a failed fsync: no rename, the new file gone");

    start();
    sim_fail(SIM_OPEN, "a.c.tmp", SIM_EROFS, 0);
    check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && sim_names() == 1 && !has(sim_log(), "unlink") &&
          !strcmp(wcs_msg(), "Not saved: the disk is read-only (Save as elsewhere)"), "a read-only disk: said, nothing done");
}

static void renames(void) {
    static const int none[] = { SIM_EXDEV, SIM_EROFS, SIM_EBUSY, SIM_EINVAL, SIM_ENOENT, SIM_EFAULT, SIM_ENOTDIR,
                                SIM_EISDIR, SIM_ENOTEMPTY, SIM_ENAMETOOLONG, SIM_ENOSPC, SIM_ENOMEM };
    char what[96];
    unsigned i;
    for(i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        start();
        sim_fail(SIM_RENAME, "a.c.tmp", none[i], SIM_NONE);
        snprintf(what, sizeof(what), "a rename that failed with %d changed nothing: the new file goes", none[i]);
        check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && sim_names() == 1 && wcs_modified() && !wcs_unsure(), what);
    }
    start();
    sim_fail(SIM_RENAME, "a.c.tmp", SIM_EIO, SIM_NONE);
    check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && is("a.c.tmp", "Xone\ntwo\n") && !has(sim_log(), "unlink") &&
          wcs_unsure() && wcs_safe(), "a rename that may have changed something (EIO): the new file is kept");

    start();
    sim_fail(SIM_RENAME, "a.c.tmp", SIM_EIO, SIM_HALF);
    check(wcs_save(NULL) < 0 && is("a.c.tmp", "Xone\ntwo\n") && !sim_corrupt("a.c") && wcs_modified() && wcs_unsure(),
          "a half-done rename: the new file is kept, the file's data not freed under it");
    check(!has(sim_log(), "unlink") && !strcmp(wcs_copied(), "a.c.tmp"), "... nothing deleted, and the copy named");
    check(!strcmp(wcs_msg(), "Not saved: I/O error. The text is in a.c.tmp; check the disk before changing either"),
          "... and said");
    check(wcs_safe(), "... the text is safe there: closing need not ask");
    sim_clear_log();
    wcs_type("Y");
    check(!wcs_safe(), "an edit since: not safe");
    check(wcs_save(NULL) == 1 && is("a.c.1.tmp", "XYone\ntwo\n") && !has(sim_log(), "rename") && !has(sim_log(), "unlink") &&
          !has(sim_log(), "trunc") && wcs_modified() && wcs_safe(), "the next save makes a copy beside it, renaming nothing");
    check(!sim_corrupt("a.c") && !sim_corrupt("a.c.tmp") && is("a.c.tmp", "Xone\ntwo\n"), "... neither name's data freed");
    check(!strcmp(wcs_msg(), "Not saved over the file; a copy is in a.c.1.tmp: check the disk"), "... and says where");
    sim_clear_log();
    check(wcs_save(NULL) == 1 && !has(sim_log(), "open") && !strcmp(wcs_msg(), "Not saved over the file; the text is in a.c.1.tmp"),
          "no edit since the last copy: no other copy");
    wcs_type("Z");
    sim_fail(SIM_WRITE, "a.c.2.tmp", SIM_EIO, 0);
    check(wcs_save(NULL) < 0 && gone("a.c.2.tmp") && is("a.c.1.tmp", "XYone\ntwo\n"),
          "a copy that fails goes again, and only it");
    check(wcs_save(NULL) == 1 && is("a.c.2.tmp", "XYZone\ntwo\n") && !strcmp(wcs_copied(), "a.c.2.tmp"), "then another copy");

    start();
    sim_fail(SIM_RENAME, "a.c.tmp", SIM_EIO, SIM_DONE);
    check(wcs_save(NULL) < 0 && is("a.c", "Xone\ntwo\n") && sim_names() == 1 && wcs_unsure() &&
          !strcmp(wcs_msg(), "Not saved: I/O error: check the disk"), "a rename done all the same: no new file, the same care");

    start();
    sim_fail(SIM_RENAME, "a.c.tmp", SIM_EXDEV, SIM_NONE);
    sim_fail(SIM_UNLINK, "a.c.tmp", SIM_EIO, 0);
    check(wcs_save(NULL) < 0 && is("a.c", "one\ntwo\n") && is("a.c.tmp", "Xone\ntwo\n") && !wcs_unsure() &&
          !strcmp(wcs_msg(), "Not saved: Invalid cross-device link. The text is in a.c.tmp"),
          "a rename that changed nothing and a new file that would not go: named, no care needed");
}

static void twins(void) {
    sim_reset();
    sim_put("a.c", "one\ntwo\n");
    sim_put("a.c.tmp", "one\ntwo\n");
    sim_same_time("a.c", "a.c.tmp");
    check(wcs_open("a.c") == 0 && wcs_unsure() && !strcmp(wcs_msg(), "a.c.tmp may share this file's data: check the disk (fsck) first"),
          "a name beside the file of its size and time: the care from the start");
    wcs_type("X");
    check(wcs_save(NULL) == 1 && is("a.c.1.tmp", "Xone\ntwo\n") && is("a.c", "one\ntwo\n") && is("a.c.tmp", "one\ntwo\n") &&
          !has(sim_log(), "rename"), "... a save puts a copy beside, and the two stay as they are");
    check(has(wcs_msg(), "a.c.tmp may share its data"), "... and says why");

    sim_reset();
    sim_put("b.c", "x\n");
    wcs_open("b.c");
    sim_put("b.c.part", "x\n");
    sim_same_time("b.c", "b.c.part");
    wcs_type("Y");
    sim_clear_log();
    check(wcs_save(NULL) == 1 && is("b.c", "x\n") && is("b.c.tmp", "Yx\n") && wcs_unsure(),
          "one found at a save (a .part): from then on the care");

    sim_reset();
    sim_put("e.c", "");
    sim_put("e.c.tmp", "");
    sim_same_time("e.c", "e.c.tmp");
    check(wcs_open("e.c") == 0 && !wcs_unsure(), "two empty files hold no data: no pair");
}

static void save_as(void) {
    sim_reset();
    wcs_new();
    wcs_type("int x;\n");
    check(wcs_save(NULL) < 0 && !strcmp(wcs_msg(), "Not saved: it has no name yet (Save as)"), "a new file needs a name");
    sim_fail(SIM_WRITE, "new.c", SIM_ENOSPC, 0);
    check(wcs_save("new.c") < 0 && sim_names() == 0 && wcs_modified() && !wcs_path()[0],
          "Save as of a new file that fails: the file it made goes again");
    sim_clear_log();
    check(wcs_save("new.c") == 0 && is("new.c", "int x;\n") && has(sim_log(), "open new.c excl\n") &&
          has(sim_log(), "fsync new.c\n") && !has(sim_log(), "rename") && !wcs_modified(), "Save as: a new file, O_EXCL");
    check(!strcmp(wcs_path(), "new.c") && wcs_lang() == 1, "... the document is that file now, in C");
    sim_put("other.c", "old\n");
    wcs_type("y");
    sim_clear_log();
    check(wcs_save("other.c") == 0 && is("other.c", "int x;\ny") && is("new.c", "int x;\n") &&
          has(sim_log(), "rename other.c.tmp other.c\n"), "Save as over a file there: replaced as a save replaces");
    sim_mkdir("adir");
    check(wcs_save("adir") < 0 && has(wcs_msg(), "folder"), "never over a folder");
}

static void reading(void) {
    char where[64];
    sim_reset();
    sim_put("w.txt", "a\r\nb\nc\r\n");
    check(wcs_open("w.txt") == 0 && wcs_crlf(), "most lines end in CR LF: new ones do");
    wcs_type("Z\n");
    check(wcs_save(NULL) == 0 && is("w.txt", "Z\r\na\r\nb\nc\r\n"), "the line ends kept as they were, a new one the file's");
    check(wcs_open("missing.c") < 0 && has(wcs_msg(), "Cannot open missing.c"), "no such file: said");
    sim_mkdir("d");
    check(wcs_open("d") < 0 && has(wcs_msg(), "folder"), "a folder: said");
    check(wcs_open("/dev/kmsg") < 0 && has(wcs_msg(), "device"), "a device: never read");
    /* the window gone with changes unsaved: the text beside the file */
    sim_reset();
    sim_put("r.c", "r\n");
    wcs_open("r.c");
    check(wcs_rescue(where, sizeof(where)) < 0, "nothing unsaved: nothing to put beside it");
    wcs_type("Q");
    check(wcs_rescue(where, sizeof(where)) == 0 && !strcmp(where, "r.c.tmp") && is("r.c.tmp", "Qr\n") && is("r.c", "r\n"),
          "unsaved: the text beside the file, the file as it was");
    wcs_new();
    wcs_type("q");
    check(wcs_rescue(where, sizeof(where)) < 0, "a new file has nowhere to go");
}


/* 11/09/26 (the review): a file saved by another window since this one read it (H1); a device under another name
   (H2); a lone CR kept through Enter and Backspace (L3); the bytes compared as bytes, their count and all */
static void review(void) {
    static const char lone[] = "a\rb\r\rc\n\r";
    sim_reset();
    sim_put("h.c", "int x;\n");
    wcs_use(0);
    check(wcs_open("h.c") == 0, "window A reads h.c");
    wcs_use(1);
    check(wcs_open("h.c") == 0, "window B reads it too");
    wcs_caret(wcs_len());
    wcs_type("// B");
    check(wcs_save(NULL) == 0 && is("h.c", "int x;\n// B"), "B saves");
    wcs_use(0);
    wcs_caret(wcs_len());
    wcs_type("// A");
    check(wcs_save(NULL) < 0 && wcs_disk_changed() && is("h.c", "int x;\n// B") &&
          has(wcs_msg(), "changed on the disk since it was read here"), "A's save over it is refused: B's work stays");
    wcs_overwrite();
    check(wcs_save(NULL) == 0 && is("h.c", "int x;\n// A") && !wcs_disk_changed(), "A says save over it: saved");
    wcs_type("!");
    check(wcs_save(NULL) == 0 && is("h.c", "int x;\n// A!"), "... and saves again without asking: what it saved it knows");
    wcs_use(1);
    wcs_type("?");
    check(wcs_save(NULL) < 0 && wcs_disk_changed() && is("h.c", "int x;\n// A!"), "now B is asked");
    wcs_use(0);
    check(wcs_open("/usb1p1/../dev/tty") < 0 && has(wcs_msg(), "is a device"), "/usb1p1/../dev/tty is /dev/tty: refused");
    sim_reset();
    sim_put("r.c", lone);
    wcs_open("r.c");
    wcs_caret(wcs_len());
    wcs_type("\n");
    wcs_back();
    check(wcs_save(NULL) == 0 && is("r.c", lone), "Enter after the last lone CR, then Backspace: the same bytes");
    wcs_open("r.c");
    wcs_caret(2);
    wcs_type("\n");
    check(wcs_save(NULL) == 0 && is("r.c", "a\r\r\nb\r\rc\n\r"), "Enter after a lone CR: it stays lone, a CR LF after it");
    check(!is("r.c", "a\r\r\nb\r\rc\n\rX") && !is("r.c", "a\r\r\nb"), "(is() compares every byte, and the count)");
}

int main(void) {
    saving();
    renames();
    twins();
    save_as();
    reading();
    review();
    printf("test_wcode_save: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
