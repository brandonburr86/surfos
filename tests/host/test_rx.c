/* Host-side test of rx's start (10/17/26): user/bin/rx.c as it is (tests/host/rx_bridge.c) on the
   in-memory file system of tests/host/sim_fs.c. Covered: when an earlier rx's <path>.part keeps a later
   rx from starting (it is <path>'s twin, of its size and time, as a rename that may have half happened
   leaves it: cutting it would free <path>'s clusters) and when it is cut and written again (none, an
   empty one, one a killed rx left, with <path> or without), and a .part that cannot be looked at; then rx
   itself as far as it gets with no network (its exit status, what it cut and deleted). Run by
   `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "sim_fs.h"

int rx_part_check(const char *path);                   /* tests/host/rx_bridge.c */
int rx_run(const char *path);

static int has(const char *s, const char *part) {
    return strstr(s, part) != NULL;
}

static int is(const char *name, const char *text) {
    char buf[256];
    return sim_get(name, buf, sizeof(buf)) == (int)strlen(text) && !strcmp(buf, text);
}

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

int main(void) {
    sim_reset();
    check(rx_part_check("/st/x.new") == 0, "no .part: rx starts");
    sim_put("/st/x.new.part", "");
    check(rx_part_check("/st/x.new") == 0, "an empty .part (an rx stopped before any data came): rx starts");
    sim_put("/st/x.new.part", "the first half");
    check(rx_part_check("/st/x.new") == 0, "a .part with data a killed rx left, no file: rx starts and cuts it");
    sim_put("/st/x.new", "the last build");
    check(rx_part_check("/st/x.new") == 0, "... and with the last build beside it, newer: the same");

    sim_reset();
    sim_put("/st/x.new", "the whole file");
    sim_put("/st/x.new.part", "the whole file");
    sim_same_time("/st/x.new", "/st/x.new.part");
    check(rx_part_check("/st/x.new") == 1, "a .part that is the file's twin, of its size and time: rx does not start");
    check(rx_part_check("/ST/X.NEW") == 1, "... whatever the case of the name (FAT ignores it)");

    sim_reset();
    sim_put("/st/x.new", "same size");
    sim_put("/st/x.new.part", "same size");             /* a second later */
    check(rx_part_check("/st/x.new") == 0, "the same size at another time is not a twin");
    sim_put("/st/x.new.part", "longer than the file");
    sim_same_time("/st/x.new", "/st/x.new.part");
    check(rx_part_check("/st/x.new") == 0, "nor is the same time with another size");

    sim_reset();
    sim_put("/st/x.new.part", "data");
    sim_fail(SIM_STAT, "/st/x.new.part", SIM_EIO, 0);
    check(rx_part_check("/st/x.new") == -1, "a .part that cannot be looked at: no telling, rx does not start");
    sim_fail(SIM_STAT, "/st/x.new", SIM_EIO, 0);
    check(rx_part_check("/st/x.new") == -2, "... nor a file beside it that cannot be (said of the file)");
    sim_fail(SIM_STAT, "/st/x.new.part", SIM_ENOTDIR, 0);
    check(rx_part_check("/st/x.new") == 0, "a folder in the path that is not one: rx goes on, and its open says so");

    sim_reset();                                /* rx itself: over a twin it stops before it touches anything */
    sim_put("/st/x.new", "the whole file");
    sim_put("/st/x.new.part", "the whole file");
    sim_same_time("/st/x.new", "/st/x.new.part");
    sim_clear_log();
    check(rx_run("/st/x.new") == 1 && !has(sim_log(), "open") && !has(sim_log(), "unlink") &&
          is("/st/x.new.part", "the whole file") && is("/st/x.new", "the whole file"),
          "rx over a twin .part: exit status 1, nothing cut or deleted");
    sim_reset();                                /* over a killed rx's: made again, and its own deleted on failing */
    sim_put("/st/x.new.part", "the first half");
    sim_clear_log();
    check(rx_run("/st/x.new") == 1 && has(sim_log(), "open /st/x.new.part trunc\n") && has(sim_log(), "unlink /st/x.new.part\n") &&
          sim_names() == 0, "rx over a killed rx's .part cuts it, and deletes what it made when it cannot listen (no network here)");

    printf("RX UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
