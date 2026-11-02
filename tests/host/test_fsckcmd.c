/* Host test of shell/fsckcmd.c (11/01/26, the command-line suite): the real cmd_fsck() over a device layer this test scripts,
   and a fat_check() that does what the script says. What it pins is the order of things, which only the command knows: the
   device held from before the first read to after the last write, a read-only mount held and not taken, no write function
   without -r, keys typed ahead dropped before the question, the plan in the question and so on the console whatever the
   output goes to, bdev_may_format() again after the answer, nothing cached kept after a write. tests/host/test_fsck.c proves
   the checker; tools/qemu-usb.py's scenario `fsck` runs the two together on a stick. fsckcmd.c is built with its printf()
   renamed (fsck_printf: the Makefile), so this test keeps the host's. Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat_check.h>
#include "../../shell/shell.h"

int fsck_printf(const char *fmt, ...);
int cmd_fsck(int argc, char **argv);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/* what happened, in order */
static char events[1024];
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
static struct tty own_tty, active_tty;
struct tty *tty_active = &active_tty;

/* the devices: usb1 a disk, usb1p1 a partition of it */
static struct bdev disk, part;

/* the script */
static int may_refuses, may_refuses_after;      /* bdev_may_format() says no: at once; from its second call */
static int may_calls;
static int mounted;                             /* 0 none, 1 read-only, 2 read-write: of the device asked for */
static const char *busy_why;                    /* vfs_bdev_change_begin() refuses */
static int check_status, check_wrote, check_asks;
static const char *answer;                      /* what is typed; NULL: Ctrl+C */
static char question_seen[512];
static struct fat_check_io io_seen;
static int flags_seen, ask_result, changing_at_check, held_at_check, holding, changing;
static struct vnode the_root;

struct bdev *bdev_find(const char *name) {
    if(!strcmp(name, "usb1")) return &disk;
    if(!strcmp(name, "usb1p1")) return &part;
    return NULL;
}
struct bdev *bdev_disk(struct bdev *d) { return d == &part ? &disk : d; }
int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf) { (void)d; (void)lba; (void)count; (void)buf; ev("read"); return 0; }
int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf) { (void)d; (void)lba; (void)count; (void)buf; ev("write"); return 0; }
int bdev_flush(struct bdev *d) { (void)d; ev("flush"); return 0; }

int bdev_may_format(struct bdev *d, char *why, size_t size) {
    (void)d;
    ev("may");
    if(may_refuses || (may_refuses_after && ++may_calls > 1)) { snprintf(why, size, "it is mounted"); return -1; }
    return 0;
}

const char *bdev_busy_words(const char *busy) { return busy; }

int vfs_hold_mount(const struct bdev *dev, char *path, size_t size, struct vnode **root, bool *readonly) {
    (void)dev;
    ev("hold");
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

void binvalidate(struct bdev *d) { ev(d == &part ? "invalidate-part" : "invalidate-disk"); }
void part_fmt_size(u32 blocks, char *buf, size_t size) { (void)blocks; snprintf(buf, size, "16M"); }

void tty_flush(struct tty *t) {
    check(t == (curTask->tty ? curTask->tty : tty_active), "the keys dropped are those of this task's terminal");
    ev("drop-keys");
}

void shell_ask(const char *question, char *answer_out, size_t size) {
    ev("question");
    strlcpy(question_seen, question, sizeof(question_seen));
    memset(answer_out, 0, size);
    if(answer) strlcpy(answer_out, answer, size);
}

/* the checker: it reads, asks when it may, and writes when told to go on */
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
    if(check_asks) {
        ask_result = io->ask(io->ctx, "end 1 chain, mark it clean");
        ev(ask_result ? "yes" : "no");
        if(!ask_result) { res->cancelled = true; return res->status = FAT_CHECK_PROBLEMS; }
    }
    if(check_wrote) {
        io->write(io->ctx, 1, 1, sector);
        io->flush(io->ctx);
        res->wrote = true;
    }
    return res->status = check_status;
}

/* one command, from a clean slate; its status */
static int run(int argc, char **argv) {
    int r;
    events[0] = 0;
    said[0] = 0;
    said_len = 0;
    question_seen[0] = 0;
    memset(&io_seen, 0, sizeof(io_seen));
    may_calls = 0;
    ask_result = -1;
    flags_seen = -1;
    r = cmd_fsck(argc, argv);
    check(task.fs_hold == 0, "every fs_enter() has its fs_leave()");
    return r;
}

static void script(void) {
    may_refuses = may_refuses_after = 0;
    mounted = 0;
    busy_why = NULL;
    check_status = 0;
    check_wrote = check_asks = 0;
    answer = NULL;
}

static void usage(void) {
    char *none[] = { "fsck", NULL }, *two[] = { "fsck", "usb1", "usb2", NULL }, *flag[] = { "fsck", "-x", "usb1", NULL }, *no[] = { "fsck", "nothing", NULL };
    script();
    check(run(1, none) == CMD_USAGE && !events[0] && strstr(said, "usage"), "no device: usage, and nothing touched");
    check(run(3, two) == CMD_USAGE && !events[0] && strstr(said, "usage"), "two devices: usage");
    check(run(3, flag) == CMD_USAGE && !events[0] && strstr(said, "usage"), "a flag it does not know: usage");
    check(run(2, no) == CMD_USAGE && !events[0] && strstr(said, "no block device"), "a device that is not there: says so");
}

static void looks(void) {
    char *look[] = { "fsck", "usb1p1", NULL }, *verbose[] = { "fsck", "-v", "usb1p1", NULL };
    script();
    check_status = 1;
    check(run(2, look) == 1, "a look: the checker's status is the command's");
    check(!strcmp(events, "hold begin check read end "), "a look at an unmounted device: taken before the first read, let go of after it, and no rule asked");
    check(changing_at_check && !held_at_check, "the checker runs with the device taken");
    check(!io_seen.write && !io_seen.ask, "a look has no write function, and no question");
    check(!(flags_seen & FAT_CHECK_REPAIR), "a look is not asked to repair");
    check(strstr(said, "usb1p1: ") && strstr(said, "16M") && strstr(said, "    a line of the checker\n"), "it says what the device is, and the checker's lines, indented");

    run(3, verbose);
    check(flags_seen == FAT_CHECK_VERBOSE, "-v goes to the checker");

    /* K2: a read-only mount is held for the run, not walked past */
    script();
    mounted = 1;
    check(run(2, look) == 0, "a look at a read-only mount runs");
    check(!strcmp(events, "hold check read release "), "a read-only mount: held before the first read, let go of after it, and not taken");
    check(held_at_check && !changing_at_check, "the checker runs with the mount held");
    check(!io_seen.write, "and it has no write function");

    script();
    mounted = 2;
    check(run(2, look) == CMD_USAGE, "a read-write mount: refused");
    check(!strcmp(events, "hold release "), "the checker never runs on it, and the mount is let go of");
    check(strstr(said, "mounted read-write on /mnt/usb") != NULL, "it says where, and what to do");

    /* its disk, or a partition of it: nothing here can hold that, and the device layer says it is busy */
    script();
    busy_why = "a partition of it is mounted";
    check(run(2, look) == CMD_USAGE, "a mount of a related device: refused");
    check(!strcmp(events, "hold begin "), "the checker never runs then");
    check(strstr(said, "a partition of it is mounted") != NULL, "with the reason");
}

static void repairs(void) {
    char *fix[] = { "fsck", "-r", "usb1p1", NULL }, *fixdisk[] = { "fsck", "-r", "usb1", NULL }, *both[] = { "fsck", "-vr", "usb1p1", NULL };

    /* the rule first, and nothing else */
    script();
    may_refuses = 1;
    check(run(3, fix) == CMD_USAGE, "-r on what may not be changed: refused");
    check(!strcmp(events, "may "), "before anything is taken or read");
    check(strstr(said, "it is mounted") != NULL, "with the reason");

    /* a mount of it: the device layer refuses, and no hold is taken for a repair */
    script();
    busy_why = "it is mounted";
    check(run(3, fix) == CMD_USAGE && !strcmp(events, "may begin "), "-r on a busy device: refused before the first read");

    /* the whole way */
    script();
    check_asks = check_wrote = 1;
    answer = "usb1p1";
    check(run(3, fix) == 0, "-r answered: the checker's status");
    check(flags_seen == FAT_CHECK_REPAIR, "the checker is asked to repair");
    check(io_seen.write && io_seen.ask && io_seen.flush, "and has a write function, a flush and a question");
    check(changing_at_check, "the device is taken through the question and the writes");
    check(strstr(events, "drop-keys question ") != NULL, "keys typed ahead are dropped before the question");
    check(strstr(events, "may begin check read drop-keys question may yes write flush invalidate-part invalidate-disk end ") != NULL,
          "the rule, the device taken, the check, the question, the rule again, the writes, nothing cached kept, the device let go of");
    check(strstr(question_seen, "end 1 chain, mark it clean") != NULL, "the plan is in the question: shell_ask() shows it on the console, whatever the output goes to");
    check(strstr(question_seen, "Type usb1p1 to repair usb1p1: ") != NULL, "and it says what to type");
    check(strstr(question_seen, "\n") != NULL, "the plan on a line of its own");
    check(strstr(said, "end 1 chain") == NULL, "the plan is not left to what may be going to a file");

    /* an answer that is not the name */
    script();
    check_asks = check_wrote = 1;
    answer = "yes";
    check(run(3, fix) == 1 && ask_result == 0, "a wrong answer: no");
    check(strstr(events, "write") == NULL && strstr(events, "invalidate") == NULL, "nothing written, nothing dropped");
    check(strstr(said, "Cancelled") != NULL, "it says so");

    /* Ctrl+C at the question leaves the answer empty */
    script();
    check_asks = check_wrote = 1;
    answer = NULL;
    check(run(3, fix) == 1 && ask_result == 0 && !strstr(events, "write"), "Ctrl+C at the question: no");

    /* the stick changed hands while the question waited */
    script();
    check_asks = check_wrote = 1;
    may_refuses_after = 1;
    answer = "usb1p1";
    check(run(3, fix) == 1 && ask_result == 0, "the rule says no after the answer: no");
    check(strstr(events, "write") == NULL && strstr(said, "it is mounted"), "nothing written, with the reason");
    check(strstr(events, "end ") != NULL, "and the device is let go of");

    /* a repair that wrote nothing keeps the cache */
    script();
    check_asks = 1;
    answer = "usb1p1";
    run(3, fix);
    check(strstr(events, "invalidate") == NULL, "nothing written, nothing dropped from the cache");

    /* a whole disk has one thing to drop */
    script();
    check_asks = check_wrote = 1;
    answer = "usb1";
    check(run(3, fixdisk) == 0, "-r on a whole disk");
    check(strstr(events, "invalidate-disk end ") != NULL && !strstr(events, "invalidate-part"), "its own cache, once");

    script();
    check_asks = 1;
    answer = "usb1p1";
    run(3, both);
    check(flags_seen == (FAT_CHECK_REPAIR | FAT_CHECK_VERBOSE), "-vr: both");
}

/* the terminal the keys are dropped from */
static void terminals(void) {
    char *fix[] = { "fsck", "-r", "usb1", NULL };
    script();
    check_asks = 1;
    answer = "usb1";
    task.tty = NULL;
    run(3, fix);
    check(strstr(events, "drop-keys") != NULL, "a task with no terminal of its own: the active one's keys");
    task.tty = &own_tty;
    run(3, fix);
    check(strstr(events, "drop-keys") != NULL, "a task with its own: those");
}

int main(void) {
    strlcpy(disk.name, "usb1", sizeof(disk.name));
    strlcpy(part.name, "usb1p1", sizeof(part.name));
    disk.nblocks = 32768;
    part.nblocks = 32000;
    part.parent = &disk;
    usage();
    looks();
    repairs();
    terminals();
    printf("FSCKCMD: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
