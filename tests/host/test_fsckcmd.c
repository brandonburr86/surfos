/* Host test of shell/fsckcmd.c (11/01/26, the command-line suite; 11/02/26: it only reads): the real cmd_fsck() over a
   device layer this test scripts, and a fat_check() that records what it was given. What it pins is what only the command
   knows: -r refused before anything is touched, no write function and no repair asked for, the device held from before
   the first read to after the last, a read-only mount held and not taken, netman's mount on /.netman never held but waited
   for (kernel/netman.c gives its unmount up when something holds it), every fs_enter() with its fs_leave(); 11/02/26 (the
   third review): an interrupted look is 130 with no word of a repair, and no memory to look at the mounts is 2.
   tests/host/test_fsck.c proves the checker; tools/qemu-usb.py's scenario fsck runs the two together on a stick.
   fsckcmd.c is built with its printf() renamed (fsck_printf: the Makefile), so this test keeps the host's. Built like the
   kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <surfos/task.h>
#include <surfos/kcmd.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/fat_check.h>
#include <mm/kalloc.h>
#include "../../shell/shell.h"

int fsck_printf(const char *fmt, ...);
int cmd_fsck(int argc, char **argv);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/* what happened, in order */
static char events[2048];
static void ev(const char *what) { strlcat(events, what, sizeof(events)); strlcat(events, " ", sizeof(events)); }

static char said[2048];
static int said_len;

int fsck_printf(const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(said + said_len, sizeof(said) - said_len, fmt, ap);
    va_end(ap);
    if(n > 0) said_len += n;
    if(said_len > (int)sizeof(said) - 1) said_len = sizeof(said) - 1;
    return n;
}

static surf_task task;
surf_task *curTask = &task;

/* the heap: one block at a time, and refused when the script says so */
static char heap[1 << 16];
static int heap_used, heap_refuses;
void *kalloc(size_t size) { if(heap_used || heap_refuses || size > sizeof(heap)) return NULL; heap_used = 1; return heap; }
void kfree(void *mem) { if(mem == heap) heap_used = 0; }

/* the devices: usb1 a disk, usb1p1 a partition of it */
static struct bdev disk, part;

/* the script */
static int mounted;                             /* of the device asked for: 0 none, 1 read-only, 2 read-write */
static int netman_for;                          /* views that list it on /.netman before it goes; -1: it stays */
static int netman_races;                        /* holds that find netman's mount made in the moment after a view */
static int netman_rw;                           /* netman's mount is read-write (a write of the settings) */
static int intr_at;                             /* the sleep that Ctrl+C cuts short (0: none) */
static const char *busy_why;                    /* vfs_bdev_change_begin() refuses */
static int check_status;
static int check_stopped;                       /* Ctrl+C cut the checker short */
static int holding, changing, held_at_check, changing_at_check, sleeps, views, flags_seen, holds_while_listed;
static struct fat_check_io io_seen;
static struct vnode the_root;

struct bdev *bdev_find(const char *name) {
    if(!strcmp(name, "usb1")) return &disk;
    if(!strcmp(name, "usb1p1")) return &part;
    return NULL;
}
struct bdev *bdev_disk(struct bdev *d) { return d == &part ? &disk : d; }
int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf) { (void)d; (void)lba; (void)count; (void)buf; ev("read"); return 0; }
const char *bdev_busy_words(const char *busy) { return busy; }
void part_fmt_size(u32 blocks, char *buf, size_t size) { (void)blocks; snprintf(buf, size, "16M"); }

/* the mount table as a copy of it shows it: nothing held */
static int listed;
int vfs_mount_views(struct vfs_mount_view *out, int max) {
    int n = 0;
    views++;
    listed = netman_for != 0;
    if(netman_for > 0) netman_for--;
    if(listed && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].path, sizeof(out[n].path), "/.netman");
        out[n].dev = &part;
        out[n].readonly = !netman_rw;
        n++;
    }
    if(n < max) {                               /* somebody else's mount, of another device */
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].path, sizeof(out[n].path), "/hda1");
        out[n].dev = NULL;
        n++;
    }
    return n;
}

int vfs_hold_mount(const struct bdev *dev, char *path, size_t size, struct vnode **root, bool *readonly) {
    (void)dev;
    ev("hold");
    if(listed) holds_while_listed++;
    if(netman_races) {                          /* netman mounted it in the moment since the view */
        netman_races--;
        holding = 1;
        *root = &the_root;
        if(readonly) *readonly = true;
        snprintf(path, size, "/.netman");
        ev("netman-held");
        return 0;
    }
    if(!mounted) return -ENOENT;
    holding = 1;
    *root = &the_root;
    if(readonly) *readonly = mounted == 1;
    snprintf(path, size, "/mnt/usb");
    return 0;
}

void vfs_release_mount(struct vnode *root) {
    check(root == &the_root && holding, "the mount let go of is the one held");
    holding = 0;
    ev("release");
}

int vfs_bdev_change_begin(struct bdev *d, const char **why) {
    (void)d;
    ev("begin");
    if(why) *why = busy_why;
    if(busy_why) return -EBUSY;
    changing = 1;
    fs_enter();
    return 0;
}

void vfs_bdev_change_end(struct bdev *d) {
    (void)d;
    changing = 0;
    ev("end");
    fs_leave();
}

int sleep_intr(u_long ms) {
    (void)ms;
    sleeps++;
    ev("sleep");
    return sleeps == intr_at ? -EINTR : 0;
}

/* the checker: it records what it was given, reads, says a line */
int fat_check(const struct fat_check_io *io, int flags, struct fat_check_result *res) {
    char sector[512];
    memset(res, 0, sizeof(*res));
    io_seen = *io;
    flags_seen = flags;
    held_at_check = holding;
    changing_at_check = changing;
    ev("check");
    io->read(io->ctx, 0, 1, sector);
    io->say(io->ctx, "a line of the checker");
    if(check_stopped) io->say(io->ctx, "interrupted");
    res->stopped = check_stopped != 0;
    return res->status = check_status;
}

/* one command, from a clean slate; its status */
static int run(int argc, char **argv) {
    int r;
    events[0] = 0;
    said[0] = 0;
    said_len = 0;
    memset(&io_seen, 0, sizeof(io_seen));
    flags_seen = -1;
    sleeps = views = holds_while_listed = 0;
    listed = 0;
    r = cmd_fsck(argc, argv);
    check(task.fs_hold == 0, "every fs_enter() has its fs_leave()");
    check(!holding && !changing && !heap_used, "nothing is held after it, and the heap is as it was");
    return r;
}

static void script(void) {
    mounted = 0;
    netman_for = 0;
    netman_races = 0;
    netman_rw = 0;
    intr_at = 0;
    busy_why = NULL;
    check_status = 0;
    check_stopped = 0;
    heap_refuses = 0;
}

static void usage(void) {
    char *none[] = { "fsck", NULL }, *two[] = { "fsck", "usb1", "usb2", NULL }, *flag[] = { "fsck", "-x", "usb1", NULL }, *no[] = { "fsck", "nothing", NULL };
    script();
    check(run(1, none) == CMD_USAGE && !events[0] && strstr(said, "usage: fsck [-v] <device>"), "no device: usage, and nothing touched");
    check(run(3, two) == CMD_USAGE && !events[0] && strstr(said, "usage"), "two devices: usage");
    check(run(3, flag) == CMD_USAGE && !events[0] && strstr(said, "usage"), "a flag it does not know: usage");
    check(run(2, no) == CMD_USAGE && !events[0] && strstr(said, "no block device"), "a device that is not there: says so");
}

/* -r, however it is written: refused before anything is looked at */
static void repairs(void) {
    char *a[] = { "fsck", "-r", "usb1p1", NULL }, *b[] = { "fsck", "-vr", "usb1p1", NULL }, *c[] = { "fsck", "usb1p1", "-r", NULL },
         *e[] = { "fsck", "-v", "-r", "usb1", NULL }, *f[] = { "fsck", "-r", NULL }, *g[] = { "fsck", "-r", "nothing", NULL };
    char **cases[] = { a, b, c, e, f, g };
    int argcs[] = { 3, 3, 3, 4, 2, 3 }, i;
    for(i = 0; i < 6; i++) {
        char m[120];
        script();
        mounted = 1;
        snprintf(m, sizeof(m), "-r (case %d): refused with status 2 and one plain line, and nothing looked at", i);
        check(run(argcs[i], cases[i]) == CMD_USAGE && !events[0] && !views && !strcmp(said,
              "    fsck: repairs are not available in this version, and the stick can be repaired on another computer\n"), m);
    }
}

static void looks(void) {
    char *look[] = { "fsck", "usb1p1", NULL }, *verbose[] = { "fsck", "-v", "usb1p1", NULL };
    script();
    check_status = FAT_CHECK_PROBLEMS;
    check(run(2, look) == FAT_CHECK_PROBLEMS, "a look: the checker's status is the command's");
    check(!strcmp(events, "hold begin check read end "), "a look at an unmounted device: taken before the first read, let go of after it");
    check(changing_at_check && !held_at_check, "the checker runs with the device taken");
    check(!io_seen.write && !io_seen.ask && !io_seen.flush, "no write function, no question, no flush: nothing reaches a repair");
    check(flags_seen == 0, "no repair asked for, and no plan");
    check(strstr(said, "usb1p1: ") && strstr(said, "16M") && strstr(said, "    a line of the checker\n"), "it says what the device is, and the checker's lines");
    check(strstr(said, "repairs are not available in this version: the stick can be repaired on another computer") != NULL,
          "problems found: where they can be repaired");

    script();
    check_status = FAT_CHECK_CLEAN;
    check(run(2, look) == 0 && !strstr(said, "repairs are not available"), "clean: nothing about repairs");

    script();
    check_status = FAT_CHECK_PROBLEMS;                  /* what the checker returns when it was stopped */
    check_stopped = 1;
    check(run(2, look) == KCMD_INTERRUPTED && strstr(said, "    interrupted\n") && !strstr(said, "repairs are not available"),
          "Ctrl+C in a look: status 130, as for any command cut short, and no word of a repair");
    check(!strcmp(events, "hold begin check read end "), "and the device is let go of as after any look");

    script();
    run(3, verbose);
    check(flags_seen == FAT_CHECK_VERBOSE, "-v goes to the checker, and only that");

    script();
    mounted = 1;
    check(run(2, look) == 0, "a look at a read-only mount runs");
    check(!strcmp(events, "hold check read release "), "a read-only mount: held before the first read, let go of after it, and not taken");
    check(held_at_check && !changing_at_check, "the checker runs with the mount held");

    script();
    mounted = 2;
    check(run(2, look) == CMD_USAGE, "a read-write mount: refused");
    check(!strcmp(events, "hold release "), "the checker never runs on it, and the mount is let go of");
    check(strstr(said, "mounted read-write on /mnt/usb") != NULL, "it says where, and what to do");

    script();
    busy_why = "a partition of it is mounted";
    check(run(2, look) == CMD_USAGE && !strcmp(events, "hold begin ") && strstr(said, "a partition of it is mounted"),
          "a mount of a related device: refused, with the reason");
}

/* netman's mount of the boot volume on /.netman: never held, waited for */
static void netman(void) {
    char *look[] = { "fsck", "usb1p1", NULL };
    script();
    netman_for = 3;
    check(run(2, look) == 0, "netman's mount, there for a moment: the look runs once it has gone");
    check(!holds_while_listed && sleeps == 3, "no hold taken while it is listed, and a wait for each look at it");
    check(!strcmp(events, "sleep sleep sleep hold begin check read end "), "then the device is taken as any unmounted one");

    script();
    netman_for = 2;
    netman_rw = 1;
    check(run(2, look) == 0 && !holds_while_listed && !strstr(said, "read-write"), "netman's mount for a write: waited for too, not refused as read-write");

    script();
    netman_for = -1;
    check(run(2, look) == CMD_USAGE, "netman's mount that stays: refused");
    check(!holds_while_listed && !strstr(events, "check") && sleeps == 25, "after 25 waits of 200 ms, nothing held, nothing read");
    check(strstr(said, "the network settings have usb1p1 mounted on /.netman for a moment: try again") != NULL, "it says what is going on");

    script();
    netman_races = 1;
    check(run(2, look) == 0, "netman's mount made in the moment between the view and the hold: the look runs once it has gone");
    check(strstr(events, "hold netman-held release sleep hold begin check") == events, "it is let go of at once, without reading");

    script();
    heap_refuses = 1;
    check(run(2, look) == FAT_CHECK_TROUBLE && strstr(said, "no memory to look at the mounts") && !strstr(events, "hold"),
          "no memory to look at the mounts: it says so, status 2, and holds nothing");

    script();
    netman_for = -1;
    intr_at = 2;
    check(run(2, look) == KCMD_INTERRUPTED && strstr(said, "interrupted") && !strstr(events, "check"), "Ctrl+C while it waits: interrupted, status 130, nothing read");
}

int main(void) {
    strlcpy(disk.name, "usb1", sizeof(disk.name));
    strlcpy(part.name, "usb1p1", sizeof(part.name));
    disk.nblocks = 32768;
    part.nblocks = 32000;
    part.parent = &disk;
    usage();
    repairs();
    looks();
    netman();
    printf("FSCKCMD: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
