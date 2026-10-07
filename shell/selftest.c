/*
SurfOS kernel self tests
(C)2004 Brandon Burr, written 10/2026 (roadmap T1/U1)

`selftest` in the shell; `make test` runs it over the serial console and looks for
SELFTEST PASS. Every test prints one line; the summary prints the verdict.

10/6/26: the user shell runs selftest and heaptest in its own task (SYS_kcmd), on any
console and from any program, so two could run at once. They share this file's state
(the counters, the ring and its semaphores, test_event, ansi_con, the files of fixed
names on /hda1, /selftest-stick, the scratch ramdisk rd1), a second run would
re-initialise a semaphore or an event the first one waits on, and two heaptests freed
each other's blocks. So one runs at a time, and a second is refused (status 1).

10/6/26: a kill of the task running it waits for the self test to end (surfos/task.h),
and so does Ctrl+C: the test under way finishes, waiting for its helper tasks, its
waits cut short (its checks may fail then), no other starts, and SELFTEST STOPPED
says so; the self test lets go of its claim on the way out as always.
*/

#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/sync.h>
#include <surfos/ktimer.h>
#include <surfos/timer.h>
#include <surfos/klog.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/fat.h>
#include <fs/bcache.h>
#include <surfos/process.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <net/tcp.h>
#include <surfos/multiboot.h>
#include <mm/kalloc.h>
#include <sys/usb.h>
#include <surfos/tty.h>
#include <surfos/keyboard.h>
#include <surfos/irq.h>
#include "shell.h"

static int fails;

static void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok) fails++;
}

/**** one at a time (10/6/26) ****/

/* The task running a self test or heaptest, and which one. Not a mutex: the pager lets a
   task that holds one print on without stopping, and nobody waits for this one, a second
   run is refused at once. A heaptest whose task was killed leaves nothing behind but
   memory and gives way; a killed self test may leave helper tasks waiting on its
   semaphores, so the self test stays refused until a reboot then. 10/6/26: a kill waits
   for the end of the self test, which lets go of its claim then; only a self test the
   kill could not wait for (2 s: task.h) still keeps it. */
static u_long busy_pid;
static bool busy_selftest;

static bool claim(const char *what, bool selftest) {
    u_long flags = irq_save(), pid = busy_pid;
    surf_task *t = pid ? task_find(pid) : NULL;
    bool killed = pid && (!t || t->state == TS_DEAD);
    if(!pid || (killed && !busy_selftest)) {
        busy_pid = curTask->pid;
        busy_selftest = selftest;
        irq_restore(flags);
        return true;
    }
    irq_restore(flags);
    if(killed) printf("    %s: a self test (pid %lu) was killed before it ended: no other until a reboot\n", what, pid);
    else printf("    %s: the %s of pid %lu is running: one at a time\n", what, busy_selftest ? "self test" : "heaptest", pid);
    return false;
}

static void unclaim(void) {
    busy_pid = 0;
}

/**** heap ****/

/* Random allocate/free with a byte pattern per block, then everything freed and the
   heap walked and compared with the starting point. 10/6/26: the tables are the run's
   own, from the heap, allocated before the starting point is taken. */
static int heap_round(void) {
    enum { SLOTS = 192, ROUNDS = 20000 };
    struct { void *ptrs[SLOTS]; u_int sizes[SLOTS]; } *s = kcalloc(1, sizeof(*s));
    struct heap_stats before, after;
    u_int i, r, allocs = 0, frees = 0, maxlive = 0, live = 0, bad = 0;

    if(!s) {
        printf("heaptest: no memory for its tables: HEAPTEST FAIL\n");
        return 1;
    }
    kheap_stats(&before);
    srand(getticks() + 7);
    for(r = 0; r < ROUNDS && !task_interrupted(); r++) {      /* 10/6/26: a kill or Ctrl+C: fewer rounds */
        i = rand() % SLOTS;
        if(s->ptrs[i]) {
            u_char *p = s->ptrs[i];
            u_int k;
            for(k = 0; k < s->sizes[i]; k++) if(p[k] != (u_char)(i + k)) { bad++; break; }
            kfree(s->ptrs[i]);
            s->ptrs[i] = NULL;
            frees++; live--;
        } else {
            u_int sz = (rand() % 16 == 0) ? 1 + rand() % 20000 : 1 + rand() % 512;
            u_char *p = kalloc(sz);
            u_int k;
            if(!p) { bad++; continue; }
            for(k = 0; k < sz; k++) p[k] = (u_char)(i + k);
            s->ptrs[i] = p; s->sizes[i] = sz;
            allocs++; live++;
            if(live > maxlive) maxlive = live;
        }
    }
    for(i = 0; i < SLOTS; i++) if(s->ptrs[i]) { kfree(s->ptrs[i]); s->ptrs[i] = NULL; frees++; }
    kheap_stats(&after);
    if(!kheap_check()) bad++;
    if(after.bytes_used != before.bytes_used || after.blocks_used != before.blocks_used) bad++;
    kfree(s);
    printf("heaptest: %u allocs, %u frees, %u live at peak, used %lu -> %lu bytes, %lu free blocks: %s\n",
           allocs, frees, maxlive, before.bytes_used, after.bytes_used, after.blocks_free, bad ? "HEAPTEST FAIL" : "HEAPTEST PASS");
    return bad ? 1 : 0;
}

int heaptest() {
    int r;
    if(!claim("heaptest", false)) return 1;
    r = heap_round();
    unclaim();
    return r;
}

/**** tasks ****/

static void exit_with_arg(void *arg) {
    task_exit((int)(u_long)arg);
}

static void test_tasks(void) {
    u_int before = task_count(), i, n, collected = 0, others = task_child_count();
    u_long pids[64];
    int status = 0;
    surf_task *t;

    /* 10/6/26: the user shell runs this in its own task, which may have programs of its own in
       the background: wait for these children by pid, never for any */
    for(n = 0; n < 64; n++) {
        t = kthread_create("churn", exit_with_arg, (void *)(u_long)n, PL_NORMAL, 0);
        if(!t) break;
        pids[n] = t->pid;
    }
    check(n == 64, "64 kthreads created");
    for(i = 0; i < n; i++) {
        if(task_wait(pids[i], &status) < 0) break;
        collected++;
    }
    check(collected == 64, "all 64 collected by task_wait");
    check(task_count() == before, "task count back to the start");

    t = kthread_create("exit42", exit_with_arg, (void *)42, PL_HIGH, 0);
    if(t) {
        u_long pid = t->pid; /* t is freed by task_wait: do not touch it afterwards */
        check(task_wait(pid, &status) == (int)pid && status == 42, "exit code 42 reaches task_wait");
    } else {
        check(false, "exit code 42 reaches task_wait");
    }
    if(!others) check(task_wait(WAIT_ANY, &status) == -1, "task_wait with no children returns -1");
    else check(task_child_count() == others, "the caller's own children are left alone");
}

/**** sleep and timers ****/

static void test_sleep(void) {
    u_long t0 = getticks(), dt;
    sleep_ms(100);
    dt = getticks() - t0;
    check(dt >= 10 && dt <= 13, "sleep_ms(100) takes 10-13 ticks");
}

static volatile u_int timer_fires;
static void count_fire(void *arg) { timer_fires++; }

static void test_timers(void) {
    struct ktimer t;
    u_int after_cancel;
    timer_fires = 0;
    ktimer_init(&t, count_fire, NULL);
    ktimer_start(&t, 10, true);
    sleep_ms(205);
    ktimer_cancel(&t);
    check(timer_fires >= 18 && timer_fires <= 22, "periodic 10 ms timer fires about 20 times in 205 ms");
    after_cancel = timer_fires;
    sleep_ms(50);
    check(timer_fires == after_cancel, "cancelled timer stays quiet");
}

/**** mutex ****/

static mutex_t test_mutex = MUTEX_INIT("selftest");
static volatile u_long shared_counter;

static void mutex_worker(void *arg) {
    u_int i;
    for(i = 0; i < 2000; i++) {
        u_long v;
        mutex_lock(&test_mutex);
        v = shared_counter;
        if((i % 7) == 0) yield();       /* invite the other worker in while we hold the lock */
        shared_counter = v + 1;
        mutex_unlock(&test_mutex);
    }
}

static void test_mutex_fn(void) {
    surf_task *a, *b;
    int status;
    u_int held = curTask->mutexes;      /* 10/6/26: the count the pager asks (kernel/sync.c) */
    bool ok;
    mutex_lock(&test_mutex);
    ok = curTask->mutexes == held + 1;
    ok = ok && !mutex_trylock(&test_mutex) && curTask->mutexes == held + 1;
    mutex_unlock(&test_mutex);
    ok = ok && curTask->mutexes == held && mutex_trylock(&test_mutex) && curTask->mutexes == held + 1;
    mutex_unlock(&test_mutex);
    check(ok && curTask->mutexes == held, "a task counts its mutexes: lock, a failed trylock, trylock, unlock");
    shared_counter = 0;
    a = kthread_create("mutexA", mutex_worker, NULL, PL_NORMAL, 0);
    b = kthread_create("mutexB", mutex_worker, NULL, PL_NORMAL, 0);
    if(a) task_wait(a->pid, &status);
    if(b) task_wait(b->pid, &status);
    check(a && b && shared_counter == 4000, "two tasks under a mutex count to exactly 4000");
}

/**** semaphore: producer/consumer ****/

#define RING 8
static u_int ring[RING], ring_head, ring_tail;
static semaphore_t items, slots;

static void producer(void *arg) {
    u_int i;
    for(i = 1; i <= 500; i++) {
        sem_wait(&slots);
        ring[ring_head++ % RING] = i;
        sem_post(&items);
    }
}

static void test_semaphore(void) {
    surf_task *p;
    u_long sum = 0;
    u_int i;
    int status;
    ring_head = ring_tail = 0;
    sem_init(&items, 0);
    sem_init(&slots, RING);
    p = kthread_create("producer", producer, NULL, PL_NORMAL, 0);
    for(i = 0; i < 500; i++) {
        sem_wait(&items);
        sum += ring[ring_tail++ % RING];
        sem_post(&slots);
    }
    if(p) task_wait(p->pid, &status);
    check(sum == 500UL * 501 / 2, "500 items through an 8-slot ring arrive in order");
}

/**** event with timeout ****/

static event_t test_event;
static void setter(void *arg) {
    sleep_ms(20);
    event_set(&test_event);
}

static void test_event_fn(void) {
    surf_task *s;
    u_long t0;
    int rc, status;
    event_init(&test_event);
    t0 = getticks();
    rc = event_wait_timeout(&test_event, 50);
    check(rc == -1 && getticks() - t0 >= 5 && getticks() - t0 <= 8, "event_wait_timeout(50 ms) times out on time");
    s = kthread_create("setter", setter, NULL, PL_NORMAL, 0);
    rc = event_wait_timeout(&test_event, 500);
    check(rc == 0 && test_event.set, "event set by another task wakes the waiter");
    if(s) task_wait(s->pid, &status);
}

/**** libc ****/

static void test_libc(void) {
    char buf[64];
    snprintf(buf, sizeof buf, "%5d|%-5d|%05d|%x|%s", 42, 42, 42, 0xbeef, "ok");
    check(!strcmp(buf, "   42|42   |00042|beef|ok"), "snprintf widths and flags");
    snprintf(buf, sizeof buf, "%s", (char *)NULL);
    check(!strcmp(buf, "(null)"), "snprintf NULL string");
    check(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && !strcmp("x", "x"), "strcmp");
    check(strtol("-0x1F", NULL, 0) == -31 && atoi("123") == 123, "strtol/atoi");
    check(klog_length() > 0, "kernel log has the boot messages");
}

static void test_bdev(void) {
    struct bdev *rd = bdev_find("rd0"), *hd = bdev_find("hda"), *p;
    u8 *a, *b;
    int rc;
    if(!bdev_count()) { printf("  skip block devices (none attached)\n"); return; }
    a = (u8 *)kalloc(512);
    b = (u8 *)kalloc(512);
    if(!a || !b) { check(false, "block test buffers"); kfree(a); kfree(b); return; }
    if(rd && rd->nblocks >= 2) {
        u32 last = rd->nblocks - 1;
        u_int i;
        rc = bdev_read(rd, last, 1, a);                     /* keep the real contents */
        for(i = 0; i < 512; i++) b[i] = (u8)(i * 7 + 3);
        rc |= bdev_write(rd, last, 1, b);
        memset(b, 0, 512);
        rc |= bdev_read(rd, last, 1, b);
        for(i = 0; i < 512 && b[i] == (u8)(i * 7 + 3); i++);
        check(rc == 0 && i == 512, "ramdisk block write/read round trip");
        bdev_write(rd, last, 1, a);
    }
    check(bdev_read(rd ? rd : bdev_first(), 0xFFFFFFF0UL, 1, a) == -1, "read past the end is refused");
    if(hd) {
        rc = bdev_read(hd, 0, 1, a);
        check(rc == 0 && a[510] == 0x55 && a[511] == 0xAA, "hda block 0 carries an MBR signature");
        p = bdev_find("hda1");
        if(p) {
            rc = bdev_read(p, 0, 1, a);
            rc |= bdev_read(hd, p->start, 1, b);
            check(rc == 0 && !memcmp(a, b, 512) && a[510] == 0x55 && a[511] == 0xAA, "hda1 block 0 is hda's partition start and a boot sector");
        }
    }
    if(rd && vfs_bdev_busy(rd)) check(bdev_rescan(rd) == -EBUSY, "a rescan of a mounted disk is refused");
    kfree(a);
    kfree(b);
}

/* 10/5/26: the boot drive check on the scratch ramdisk rd1. An MBR partition holding a FAT32
   volume labelled SURFOSUSB makes the disk "the SurfOS boot drive", which fdisk and mkfs refuse;
   the two blocks written are put back and the table read again afterwards. */
static void test_boot_drive(void) {
    struct bdev *rd = bdev_find("rd1"), *p;
    u8 *save, *b;
    int r1, r2, rc;
    if(!rd || strcmp(rd->model, "scratch.img") || rd->nblocks < 4096 || bdev_may_format(rd, NULL, 0) != 0) {
        printf("  skip boot drive check (no free scratch ramdisk rd1)\n");
        return;
    }
    save = (u8 *)kalloc(1024);
    b = (u8 *)kcalloc(1, 512);
    if(!save || !b) { check(false, "boot drive test buffers"); kfree(save); kfree(b); return; }
    rc = bdev_read(rd, 0, 1, save) | bdev_read(rd, 2048, 1, save + 512);
    b[446 + 4] = 0x0C;                                  /* partition 1: FAT32 LBA, blocks 2048-4095 */
    b[446 + 9] = 0x08;
    b[446 + 13] = 0x08;
    b[510] = 0x55; b[511] = 0xAA;
    rc |= bdev_write(rd, 0, 1, b);
    memset(b, 0, 512);                                  /* its boot sector: just enough BPB for the label */
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    b[12] = 2; b[13] = 1; b[14] = 32; b[16] = 2;
    b[66] = 0x29;
    memcpy(b + 71, "SURFOSUSB  ", 11);
    b[510] = 0x55; b[511] = 0xAA;
    rc |= bdev_write(rd, 2048, 1, b);
    r1 = bdev_rescan(rd);
    p = bdev_find("rd1p1");
    check(rc == 0 && r1 == 1 && p && p->parent == rd && rd->protect == bdev_boot_drive && p->protect == bdev_boot_drive,
          "a SURFOSUSB volume marks its disk and partition as the boot drive");
    check(bdev_may_format(rd, NULL, 0) != 0 && (!p || bdev_may_format(p, NULL, 0) != 0), "the boot drive may not be formatted");
    rc = bdev_write(rd, 0, 1, save) | bdev_write(rd, 2048, 1, save + 512);
    r2 = bdev_rescan(rd);
    check(rc == 0 && r2 >= 0 && !rd->protect && (!p || p->gone), "put back: not the boot drive; the old rd1p1 is gone");
    kfree(save);
    kfree(b);
}

/* 10/5/26: fdisk's writer asks the safety rule again, and writes nothing when the disk no longer
   reads as the table the change was planned from (plans are made, never carried out here) */
static void test_part_commit(void) {
    struct bdev *rd = bdev_find("rd1"), *hd = bdev_find("hda");
    struct part_table *t = (struct part_table *)kalloc(sizeof(struct part_table));
    u8 *b0 = (u8 *)kalloc(512), *b1 = (u8 *)kalloc(512);
    struct part_plan plan;
    struct part_io io;
    char err[120];
    plan.buf = (u8 *)kalloc(PART_PLAN_BUF);
    if(!t || !b0 || !b1 || !plan.buf) { check(false, "part_commit test buffers"); goto out; }
    if(hd) {
        part_io_init(&io, hd, false);
        io.note = NULL;
        check(part_read(&io, hd->nblocks, t) == 0 && part_plan_new(&io, hd->nblocks, PART_SCHEME_GPT, &plan, err, sizeof(err)) == 0 &&
              bdev_read(hd, 0, 1, b0) == 0 && part_commit(hd, t, &plan, err, sizeof(err)) == -1 && strstr(err, "not a USB drive") &&
              bdev_read(hd, 0, 1, b1) == 0 && !memcmp(b0, b1, 512), "fdisk's writer refuses an internal disk and leaves it alone");
    }
    if(rd && !strcmp(rd->model, "scratch.img") && bdev_may_format(rd, NULL, 0) == 0) {
        part_io_init(&io, rd, false);
        io.note = NULL;
        check(part_read(&io, rd->nblocks, t) == 0 && part_plan_new(&io, rd->nblocks, PART_SCHEME_MBR, &plan, err, sizeof(err)) == 0,
              "a plan for the scratch disk");
        t->nblocks--;                                   /* as if the disk read differently now */
        check(bdev_read(rd, 0, 1, b0) == 0 && part_commit(rd, t, &plan, err, sizeof(err)) == -1 && strstr(err, "changed since") &&
              bdev_read(rd, 0, 1, b1) == 0 && !memcmp(b0, b1, 512), "fdisk's writer writes nothing when the table changed");
    } else printf("  skip fdisk's writer on rd1 (no free scratch ramdisk)\n");
out:
    kfree(plan.buf);
    kfree(b0);
    kfree(b1);
    kfree(t);
}

static void test_fs(void) {
    struct file *f;
    struct stat st;
    struct dirent de;
    char buf[64], path[VFS_PATH_MAX];
    int r, n = 0;

    if(vfs_stat("/initrd", &st) != 0) { printf("  skip file systems (no initrd mounted)\n"); return; }
    r = vfs_open("/initrd/motd", O_RDONLY, &f);
    if(r == 0) {
        r = vfs_read(f, buf, 17);
        vfs_close(f);
    }
    check(r == 17 && !memcmp(buf, "Welcome to SurfOS", 17), "read /initrd/motd through the VFS");
    check(vfs_stat("/initrd/etc/version", &st) == 0 && st.type == VN_FILE && st.size == 20, "stat /initrd/etc/version: 20-byte file");
    check(vfs_stat("/initrd/etc", &st) == 0 && st.type == VN_DIR, "/initrd/etc is a directory");
    check(vfs_open("/initrd/nothing", O_RDONLY, &f) == -ENOENT, "missing file is ENOENT");
    check(vfs_open("/initrd/motd", O_WRONLY, &f) == -EROFS, "writing the initrd is EROFS");
    r = vfs_open("/initrd", O_RDONLY | O_DIRECTORY, &f);
    if(r == 0) { while(vfs_readdir(f, &de) == 0) n++; vfs_close(f); }
    check(r == 0 && n >= 3, "readdir /initrd lists at least 3 entries");
    check(vfs_normalize("/initrd/etc", "../motd", path, sizeof(path)) == 0 && !strcmp(path, "/initrd/motd"), "path normalization with ..");
    check(vfs_normalize("/", "a//b/./c/../d/", path, sizeof(path)) == 0 && !strcmp(path, "/a/b/d"), "path normalization: slashes and dots");
}

static void test_fat(void) {
    struct file *f;
    struct stat st;
    struct dirent de;
    u8 *w, *rb;
    u_int i;
    int r, n = 0;

    if(vfs_stat("/hda1", &st) != 0) { printf("  skip FAT (no /hda1 mounted)\n"); return; }
    w = (u8 *)kalloc(10240);
    rb = (u8 *)kalloc(10240);
    if(!w || !rb) { check(false, "FAT test buffers"); kfree(w); kfree(rb); return; }
    for(i = 0; i < 10240; i++) w[i] = (u8)(i * 13 + (i >> 8));

    r = vfs_open("/hda1/selftest.bin", O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r == 0) { r = vfs_write(f, w, 10240); vfs_close(f); }
    check(r == 10240, "write 10 KB to /hda1/selftest.bin (ten clusters)");
    r = vfs_open("/hda1/selftest.bin", O_RDONLY, &f);
    if(r == 0) { memset(rb, 0, 10240); r = vfs_read(f, rb, 10240); vfs_close(f); }
    check(r == 10240 && !memcmp(w, rb, 10240), "read it back identically");
    r = vfs_open("/hda1/selftest.bin", O_WRONLY | O_APPEND, &f);
    if(r == 0) { r = vfs_write(f, "tail", 4); vfs_close(f); }
    check(r == 4 && vfs_stat("/hda1/selftest.bin", &st) == 0 && st.size == 10244, "O_APPEND grows it to 10244 bytes");
    r = vfs_open("/hda1/selftest.bin", O_RDONLY, &f);
    if(r == 0) { vfs_lseek(f, 10240, SEEK_SET); r = vfs_read(f, rb, 64); vfs_close(f); }
    check(r == 4 && !memcmp(rb, "tail", 4), "lseek to the end and read the appended tail");
    r = vfs_open("/hda1/selftest.bin", O_WRONLY | O_TRUNC, &f);
    if(r == 0) vfs_close(f);
    check(r == 0 && vfs_stat("/hda1/selftest.bin", &st) == 0 && st.size == 0, "O_TRUNC empties it");
    check(vfs_unlink("/hda1/selftest.bin") == 0 && vfs_stat("/hda1/selftest.bin", &st) == -ENOENT, "unlink removes it");

    vfs_unlink("/hda1/tdir/a long file name.txt");             /* leftovers of an interrupted run */
    vfs_rmdir("/hda1/tdir");
    check(vfs_mkdir("/hda1/tdir") == 0, "mkdir /hda1/tdir");
    check(vfs_mkdir("/hda1/tdir") == -EEXIST, "mkdir again is EEXIST");
    r = vfs_open("/hda1/tdir/a long file name.txt", O_WRONLY | O_CREAT, &f);
    if(r == 0) { vfs_write(f, "x", 1); vfs_close(f); }
    check(r == 0 && vfs_stat("/hda1/tdir/A LONG FILE NAME.TXT", &st) == 0 && st.size == 1, "long name created, lookup is case-insensitive");
    check(vfs_rmdir("/hda1/tdir") == -ENOTEMPTY, "rmdir of a non-empty directory is ENOTEMPTY");
    r = vfs_open("/hda1/tdir", O_RDONLY | O_DIRECTORY, &f);
    if(r == 0) { while(vfs_readdir(f, &de) == 0) n++; vfs_close(f); }
    check(r == 0 && n == 1 && !strcmp(de.name, "a long file name.txt"), "readdir of tdir shows the one file");
    check(vfs_unlink("/hda1/tdir/a long file name.txt") == 0 && vfs_rmdir("/hda1/tdir") == 0, "unlink and rmdir clean up");

    r = vfs_open("/hda1/SELFTEST.TXT", O_WRONLY | O_CREAT | O_TRUNC, &f);   /* the host checks this after the run */
    if(r == 0) { r = vfs_write(f, "SurfOS wrote this file.\n", 24); vfs_close(f); }
    check(r == 24, "SELFTEST.TXT written for the host to read with mtools");
    kfree(w);
    kfree(rb);
}

/* 10/5/26: a small file with the given contents (0 or -errno) */
static int put_file(const char *path, const char *text) {
    struct file *f;
    int r = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r) return r;
    r = vfs_write(f, text, strlen(text));
    vfs_close(f);
    return r == (int)strlen(text) ? 0 : (r < 0 ? r : -EIO);
}

/* does the file hold exactly text? */
static bool file_is(const char *path, const char *text) {
    struct file *f;
    char buf[64];
    int r = vfs_open(path, O_RDONLY, &f);
    if(r) return false;
    r = vfs_read(f, buf, sizeof(buf));
    vfs_close(f);
    return r == (int)strlen(text) && !memcmp(buf, text, r);
}

/* 10/5/26 (USB storage): rename on FAT. The tree it leaves (/hda1/RENAMED/moved/...) is what
   the host's fsck.fat checks after the run, the ".." of a moved directory included. */
/* 10/5/26 (USB storage): statfs counts clusters as they are used and freed; a read-write
   mount has cleared FAT[1]'s clean-shutdown bit (umount sets it again: the host's fsck.fat
   checks that after the smoke test) */
static void test_fat_statfs(void) {
    struct statfs a, b;
    struct stat st;
    struct bdev *d = bdev_find("hda1");
    u8 *blk;
    int r;

    if(vfs_stat("/hda1", &st) != 0 || !d) return;
    r = vfs_statfs("/hda1/DOCS", &a);
    check(r == 0 && a.unit == 1024 && a.units > 15000 && a.free_units > 0 && a.free_units < a.units && !strcmp(a.fs, "fat16") &&
          !strcmp(a.label, "SURFOS") && !strcmp(a.dev, "hda1") && !strcmp(a.path, "/hda1"), "statfs /hda1: FAT16, 1 KB clusters, label SURFOS");
    blk = (u8 *)kcalloc(1, 10240);
    if(!blk) { check(false, "statfs test buffer"); return; }
    r = put_file("/hda1/statfs.bin", "");
    if(r == 0) {
        struct file *f;
        r = vfs_open("/hda1/statfs.bin", O_WRONLY, &f);
        if(r == 0) { r = vfs_write(f, blk, 10240) == 10240 ? 0 : -EIO; vfs_close(f); }
    }
    check(r == 0 && vfs_statfs("/hda1", &b) == 0 && b.free_units + 10 == a.free_units, "a 10 KB file takes 10 clusters");
    check(vfs_unlink("/hda1/statfs.bin") == 0 && vfs_statfs("/hda1", &b) == 0 && b.free_units == a.free_units, "unlink gives them back");
    r = put_file("/hda1/grow.txt", "abc");
    if(r == 0) {                                     /* ftruncate past the end: zeros, as POSIX says */
        struct file *f;
        r = vfs_open("/hda1/grow.txt", O_RDWR, &f);
        if(r == 0) {
            u_int i;
            r = vfs_ftruncate(f, 5000);
            memset(blk, 0xEE, 5000);
            if(r == 0 && vfs_read(f, blk, 5000) != 5000) r = -EIO;
            for(i = 3; r == 0 && i < 5000; i++) if(blk[i]) r = -EIO;
            if(r == 0 && memcmp(blk, "abc", 3)) r = -EIO;
            if(r == 0) r = vfs_ftruncate(f, 2);
            vfs_close(f);
        }
    }
    check(r == 0 && vfs_stat("/hda1/grow.txt", &st) == 0 && st.size == 2 && vfs_unlink("/hda1/grow.txt") == 0,
          "ftruncate grows a file with zeros and shrinks it again");
    r = bdev_read(d, 0, 1, blk);
    if(r == 0) r = bdev_read(d, blk[14] | (blk[15] << 8), 1, blk);        /* the first sector of the FAT */
    check(r == 0 && !(blk[3] & 0x80), "mounted read-write: FAT[1] says not cleanly unmounted");
    kfree(blk);
}

/* is name an entry of the directory dir? */
static bool dir_lists(const char *dir, const char *name) {
    struct file *f;
    struct dirent de;
    bool found = false;
    if(vfs_open(dir, O_RDONLY | O_DIRECTORY, &f) != 0) return false;
    while(!found && vfs_readdir(f, &de) == 0) found = !strcmp(de.name, name);
    vfs_close(f);
    return found;
}

/* 10/5/26 (USB storage): UTF-8 names. /hda1/café.txt stays for the host to list with mtools;
   the macOS names (AppleDouble files, .DS_Store) are ordinary entries. */
static void test_fat_utf8(void) {
    static const char cafe[] = "/hda1/caf\xc3\xa9.txt";               /* café.txt */
    static const char smile[] = "/hda1/\xf0\x9f\x98\x80 smile.txt";  /* U+1F600: a surrogate pair on the disk */
    struct stat st;
    struct file *f;

    if(vfs_stat("/hda1", &st) != 0) return;
    vfs_unlink(cafe);
    check(put_file(cafe, "caf\xc3\xa9\n") == 0 && file_is("/hda1/CAF\xc3\xa9.TXT", "caf\xc3\xa9\n") &&
          vfs_stat("/hda1/caf\xc3\x89.txt", &st) == -ENOENT && dir_lists("/hda1", "caf\xc3\xa9.txt"),
          "a UTF-8 name: listed as written, ASCII letters folded only");
    check(put_file(smile, ":)") == 0 && dir_lists("/hda1", smile + 6) && vfs_unlink(smile) == 0, "a character beyond U+FFFF");
    check(vfs_open("/hda1/bad\xc3(.txt", O_WRONLY | O_CREAT, &f) == -EINVAL && vfs_open("/hda1/bad\xc0\xaf.txt", O_WRONLY | O_CREAT, &f) == -EINVAL,
          "malformed UTF-8 is refused");
    check(put_file("/hda1/._caf\xc3\xa9.txt", "AppleDouble") == 0 && put_file("/hda1/.DS_Store", "Bud1") == 0 &&
          vfs_mkdir("/hda1/.fseventsd") == 0 && dir_lists("/hda1", "._caf\xc3\xa9.txt") && dir_lists("/hda1", ".DS_Store") &&
          file_is("/hda1/.ds_store", "Bud1") && vfs_unlink("/hda1/._caf\xc3\xa9.txt") == 0 && vfs_unlink("/hda1/.DS_Store") == 0 &&
          vfs_rmdir("/hda1/.fseventsd") == 0, "macOS's ._ files, .DS_Store and .fseventsd");
}

/* 10/5/26 (USB storage): the block cache with several users at once. Three tasks write and
   check files of their own on /hda1 (their closes sync outside the volume's lock, so blocks
   are written back while others hold them) while a fourth keeps reading the initrd. */
static volatile int conc_bad;

static void conc_pattern(u8 *p, u32 n, u32 seed) {
    u32 i;
    for(i = 0; i < n; i++) p[i] = (u8)(seed * 31 + i * 7 + (i >> 9));
}

static void conc_fat(void *arg) {
    u32 id = (u32)(u_long)arg, round, k;
    char path[32];
    u8 *w = (u8 *)kalloc(8192), *rb = (u8 *)kalloc(8192);
    if(!w || !rb) { conc_bad++; kfree(w); kfree(rb); return; }
    snprintf(path, sizeof(path), "/hda1/conc%u.bin", (u_int)id);
    for(round = 0; round < 6; round++) {
        struct file *f;
        int bad = 0;
        if(vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f) != 0) { conc_bad++; continue; }
        for(k = 0; k < 5; k++) {
            u32 n = k == 4 ? 1000 : 8192;
            conc_pattern(w, 8192, id * 100 + round * 10 + k);
            if(vfs_write(f, w, n) != (int)n) bad = 1;
        }
        vfs_close(f);
        if(vfs_open(path, O_RDONLY, &f) != 0) { conc_bad++; continue; }
        for(k = 0; k < 5; k++) {
            u32 n = k == 4 ? 1000 : 8192;
            conc_pattern(w, 8192, id * 100 + round * 10 + k);
            if(vfs_read(f, rb, n) != (int)n || memcmp(w, rb, n)) bad = 1;
        }
        vfs_close(f);
        if(bad) conc_bad++;
    }
    vfs_unlink(path);
    kfree(w);
    kfree(rb);
}

static void conc_initrd(void *arg) {
    u8 *a = (u8 *)kalloc(4096), *b = (u8 *)kalloc(4096);
    int i, n0 = -1;
    if(!a || !b) { conc_bad++; kfree(a); kfree(b); return; }
    for(i = 0; i < 40; i++) {
        struct file *f;
        int n = vfs_open("/initrd/bin/hello", O_RDONLY, &f);
        if(n == 0) { n = vfs_read(f, i ? b : a, 4096); vfs_close(f); }
        if(!i) n0 = n;
        else if(n != n0 || memcmp(a, b, n)) conc_bad++;
        yield();
    }
    kfree(a);
    kfree(b);
}

static void test_fat_concurrent(void) {
    surf_task *t[4];
    struct stat st;
    int i, status;
    if(vfs_stat("/hda1", &st) != 0 || vfs_stat("/initrd/bin/hello", &st) != 0) return;
    conc_bad = 0;
    for(i = 0; i < 3; i++) t[i] = kthread_create("fatconc", conc_fat, (void *)(u_long)i, PL_NORMAL, 0);
    t[3] = kthread_create("rdconc", conc_initrd, NULL, PL_NORMAL, 0);
    for(i = 0; i < 4; i++) if(t[i]) task_wait(t[i]->pid, &status);
    check(t[0] && t[1] && t[2] && t[3] && !conc_bad, "three tasks write and check FAT files while a fourth reads the initrd");
}

/* 10/5/26 (USB storage): a FAT volume whose device goes away while it is mounted and a file
   is open, as when a USB stick is pulled. The device is a RAM disk of the test's own (never
   registered, so nothing else can see it). */
#define UNPLUG_BLOCKS 8192                           /* 4 MB: FAT16 with 512-byte clusters */

static int ram_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    memcpy(buf, (u8 *)d->priv + lba * 512, count * 512);
    return 0;
}

static int ram_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    memcpy((u8 *)d->priv + lba * 512, buf, count * 512);
    return 0;
}

static const struct bdev_ops ram_ops = { ram_read, ram_write, NULL };

static struct bdev *ram_disk(u8 *mem) {
    struct bdev *d = (struct bdev *)kcalloc(1, sizeof(struct bdev));
    if(!d) return NULL;
    strlcpy(d->name, "stick9", sizeof(d->name));
    strlcpy(d->model, "self test RAM", sizeof(d->model));
    d->block_size = 512;
    d->nblocks = UNPLUG_BLOCKS;
    d->bus = BDEV_BUS_RAM;
    d->removable = true;
    d->ops = &ram_ops;
    d->priv = mem;
    return d;
}

static void test_fat_unplug(void) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL, *d2 = NULL;
    struct statfs sf;
    struct stat st;
    struct file *f = NULL;
    struct vnode *v;
    char msg[160];
    int r;

    if(!d) { check(false, "unplug test: 4 MB of memory"); kfree(mem); return; }
    r = fat_mkfs(d, "UNPLUG", 0, msg, sizeof(msg));
    check(r == 0 && vfs_mount_flags(d, "/selftest-stick", "fat", 0) == 0, "mkfs and mount a 4 MB RAM stick");
    if(r || vfs_stat("/selftest-stick", &st) != 0) { kfree(d); kfree(mem); return; }
    r = put_file("/selftest-stick/kept.txt", "written before");
    if(r == 0) r = vfs_open("/selftest-stick/open.txt", O_WRONLY | O_CREAT, &f);
    if(r == 0) r = vfs_write(f, "x", 1) == 1 ? 0 : -EIO;
    check(r == 0 && (mem[512 + 3] & 0x80) == 0, "files written, FAT[1] says in use");
    bdev_unregister(d);                              /* pulled out */
    check(f && vfs_write(f, "y", 1) == -EIO && vfs_stat("/selftest-stick/kept.txt", &st) == -EIO &&
          vfs_lookup("/selftest-stick/kept.txt", &v) == -EIO && vfs_mkdir("/selftest-stick/d") == -EIO &&
          vfs_rename("/selftest-stick/kept.txt", "/selftest-stick/k2.txt") == -EIO && vfs_statfs("/selftest-stick", &sf) == -EIO,
          "unplugged: write, stat, open, mkdir, rename and statfs fail with EIO");
    check(vfs_umount("/selftest-stick") == -EBUSY, "umount waits for the open file");
    if(f) vfs_close(f);
    check(vfs_umount("/selftest-stick") == 0, "then it unmounts, writing nothing");
    d2 = ram_disk(mem);                              /* plugged in again */
    r = d2 ? vfs_mount_flags(d2, "/selftest-stick", "fat", MNT_RDONLY) : -ENOMEM;
    check(r == 0 && file_is("/selftest-stick/kept.txt", "written before") && (mem[512 + 3] & 0x80) == 0,
          "plugged in again: the file written before is there, the volume says not cleanly unmounted");
    if(r == 0) vfs_umount("/selftest-stick");
    if(d2) { binvalidate(d2); kfree(d2); }
    binvalidate(d);
    kfree(d);
    kfree(mem);
}

static void test_fat_rename(void) {
    struct stat st;
    struct file *f;
    int r;

    if(vfs_stat("/hda1", &st) != 0) return;
    vfs_unlink("/hda1/RENAMED/moved/a much longer name.txt");      /* leftovers of an earlier run */
    vfs_unlink("/hda1/RENAMED/moved/opened.txt");
    vfs_rmdir("/hda1/RENAMED/moved");
    vfs_rmdir("/hda1/RENAMED");
    check(put_file("/hda1/rn_a.txt", "alpha") == 0 && vfs_rename("/hda1/rn_a.txt", "/hda1/rn_b.txt") == 0 &&
          vfs_stat("/hda1/rn_a.txt", &st) == -ENOENT && file_is("/hda1/rn_b.txt", "alpha"), "rename a file in its directory");
    check(vfs_mkdir("/hda1/rn_dir") == 0 && vfs_rename("/hda1/rn_b.txt", "/hda1/rn_dir/a much longer name.txt") == 0 &&
          file_is("/hda1/rn_dir/A MUCH LONGER NAME.TXT", "alpha"), "move it into a directory under a long name");
    check(put_file("/hda1/rn_c.txt", "gamma") == 0 && vfs_rename("/hda1/rn_c.txt", "/hda1/rn_dir/a much longer name.txt") == 0 &&
          file_is("/hda1/rn_dir/a much longer name.txt", "gamma") && vfs_stat("/hda1/rn_c.txt", &st) == -ENOENT,
          "rename replaces an existing file");
    check(vfs_rename("/hda1/rn_dir", "/hda1/rn_dir/sub") == -EINVAL, "a directory cannot move into itself");
    check(vfs_mkdir("/hda1/rn_full") == 0 && put_file("/hda1/rn_full/x", "x") == 0 &&
          vfs_rename("/hda1/rn_dir", "/hda1/rn_full") == -ENOTEMPTY && put_file("/hda1/rn_d.txt", "d") == 0 &&
          vfs_rename("/hda1/rn_d.txt", "/hda1/rn_full") == -EISDIR, "no rename onto a non-empty directory or a file onto a directory");
    vfs_unlink("/hda1/rn_full/x");
    vfs_unlink("/hda1/rn_d.txt");
    r = vfs_open("/hda1/rn_dir/opened.txt", O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r == 0) {                                     /* an open file follows its entry */
        vfs_write(f, "12345", 5);
        r = vfs_rename("/hda1/rn_dir", "/hda1/rn_full");             /* an empty directory is replaced */
        if(r == 0) r = vfs_write(f, "678", 3) == 3 ? 0 : -EIO;
        vfs_close(f);
    }
    check(r == 0 && vfs_stat("/hda1/rn_full/opened.txt", &st) == 0 && st.size == 8 && vfs_stat("/hda1/rn_dir", &st) == -ENOENT,
          "move a directory over an empty one while a file in it is open");
    check(vfs_mkdir("/hda1/RENAMED") == 0 && vfs_rename("/hda1/rn_full", "/hda1/RENAMED/moved") == 0 &&
          file_is("/hda1/RENAMED/moved/a much longer name.txt", "gamma"), "move a directory into another one");
}

static int run_and_wait(const char *path, int argc, char **argv) {
    int pid = process_spawn(path, argc, argv, curTask->con, 0), status = -99;
    if(pid < 0) return pid;
    if(task_wait(pid, &status) != pid) return -98;
    return status;
}

static void test_user(void) {
    char *hello[] = { "hello", "one", "two" };
    char *crash_null[] = { "crash", "null" };
    char *crash_kernel[] = { "crash", "kernel" };
    char *crash_cli[] = { "crash", "cli" };
    char *crash_int40[] = { "crash", "int40" };
    char *crash_efault[] = { "crash", "efault" };
    char *crash_stack[] = { "crash", "stack" };
    char *motd[] = { "motd" };
    struct stat st;
    u_int before = task_count();

    if(vfs_stat("/initrd/bin/hello", &st) != 0) { printf("  skip user programs (no /initrd/bin/hello)\n"); return; }
    check(run_and_wait("/initrd/bin/hello", 3, hello) == 42, "hello runs in ring 3 and exits with 42");
    check(run_and_wait("/initrd/bin/crash", 2, crash_null) == -1, "a NULL write kills only that process");
    check(run_and_wait("/initrd/bin/crash", 2, crash_kernel) == -1, "a write to kernel memory kills the process");
    check(run_and_wait("/initrd/bin/crash", 2, crash_cli) == -1, "cli in user mode kills the process");
    check(run_and_wait("/initrd/bin/crash", 2, crash_int40) == -1, "int 0x40 from user mode kills the process");
    check(run_and_wait("/initrd/bin/crash", 2, crash_efault) == 0, "a kernel pointer in a system call is EFAULT, not a crash");
    check(run_and_wait("/initrd/bin/crash", 2, crash_stack) == 0, "the user stack grows on demand");
    check(run_and_wait("/initrd/motd", 1, motd) == 127, "a file that is not an ELF exits 127");
    check(run_and_wait("/initrd/bin/nothing", 1, motd) == -ENOENT, "a missing program is ENOENT");
    check(task_count() == before, "every process was reaped");
}

static void test_tcp(void) {
    const char *p = strstr(bootinfo.cmdline, "httpport=");
    struct tcp_socket *s;
    char buf[1024];
    int n, total = 0, got_body = 0, got_status = 0;
    u_long t0 = getticks();

    if(!netdev_default() || !netdev_default()->ip) { printf("  skip TCP (no address)\n"); return; }
    s = tcp_connect(IP4(10, 0, 2, 2), 1, 3000);
    check(s == NULL && getticks() - t0 < 2 * HZ, "connecting to a closed port is refused promptly");
    if(s) tcp_close(s);
    if(!p) { printf("  skip HTTP (no httpport= on the command line)\n"); return; }
    s = tcp_connect(IP4(10, 0, 2, 2), (u16)strtoul(p + 9, NULL, 10), 5000);
    check(s != NULL, "TCP connect to the host's HTTP server through QEMU");
    if(!s) return;
    n = tcp_send(s, "GET /selftest HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n", 42);
    check(n == 42, "the request is sent");
    while((n = tcp_recv(s, buf, sizeof(buf) - 1, 5000)) > 0) {
        buf[n] = 0;
        if(strstr(buf, "200 OK")) got_status = 1;
        if(strstr(buf, "SurfOS HTTP test OK")) got_body = 1;
        total += n;
    }
    check(n == 0, "the server closes the connection (recv returns 0)");
    check(got_status && got_body, "HTTP status line and body received");
    tcp_close(s);
}

/* 10/5/26 (USB networking): any other network, a USB adapter on the laptop for example,
   gets only the checks that hold everywhere: a lease with a gateway, and the gateway
   answering a ping or at least ARP */
static void test_net_other(struct netdev *dev) {
    const struct arp_entry *e;
    char a[16], g[16];
    int i, ok = 0;
    check(dev->ip && dev->netmask && dev->gateway, "an address, netmask and gateway (DHCP or ifconfig)");
    for(i = 1; i <= 3 && !ok && dev->gateway; i++) if(icmp_ping(dev->gateway, 0x5f61, (u16)i, 56, 2000, NULL) >= 0) ok = 1;
    for(e = arp_table(), i = 0; !ok && dev->gateway && i < ARP_TABLE_SIZE; i++)
        if(e[i].valid && e[i].resolved && e[i].ip == dev->gateway) ok = 1;
    check(ok, "the gateway answers (a ping, or at least ARP)");
    printf("        (%s: %s, gateway %s, %lu frames in, %lu out; QEMU-only checks skipped)\n", dev->name,
           ipfmt(dev->ip, a), ipfmt(dev->gateway, g), dev->rx_packets, dev->tx_packets);
}

static void test_net(void) {
    struct netdev *dev = netdev_default();
    const struct arp_entry *e;
    ipaddr_t was_ip, was_mask, was_gw, was_dns;
    int i, rtt = -1, ok = 0;
    char a[16];

    if(!dev) { printf("  skip network (no interface)\n"); return; }
    if(!dev->link_up) { printf("  skip network (%s: no link)\n", dev->name); return; }
    was_ip = dev->ip;                                /* 10/6/26: given back if the lease below fails */
    was_mask = dev->netmask;
    was_gw = dev->gateway;
    was_dns = dev->dns;
    if(!dev->ip) {
        /* which network is this? A lease from QEMU's comes with gateway 10.0.2.2; then the
           configuration is dropped again so the checks below start from nothing as always */
        if(dhcp_configure(dev, 8000) == 0 && dev->gateway != IP4(10, 0, 2, 2)) { test_net_other(dev); return; }
        mutex_lock(&net_lock);
        dev->ip = dev->netmask = dev->gateway = dev->dns = 0;
        mutex_unlock(&net_lock);
    } else if(dev->gateway != IP4(10, 0, 2, 2)) {
        test_net_other(dev);
        return;
    }
    if(!dev->ip) {                                   /* QEMU's user network: 10.0.2.0/24, gateway 10.0.2.2 */
        mutex_lock(&net_lock);
        dev->ip = IP4(10, 0, 2, 15);
        dev->netmask = IP4(255, 255, 255, 0);
        dev->gateway = IP4(10, 0, 2, 2);
        dev->dns = IP4(10, 0, 2, 3);
        arp_announce(dev);
        mutex_unlock(&net_lock);
    }
    for(i = 1; i <= 3; i++) {
        rtt = icmp_ping(IP4(10, 0, 2, 2), 0x5f5f, (u16)i, 56, 2000, NULL);
        if(rtt >= 0) ok++;
    }
    check(ok >= 1, "ping 10.0.2.2 (QEMU's gateway) is answered");
    for(e = arp_table(), i = 0; i < ARP_TABLE_SIZE; i++) if(e[i].valid && e[i].resolved && e[i].ip == IP4(10, 0, 2, 2)) break;
    check(i < ARP_TABLE_SIZE, "the gateway's MAC is in the ARP table");
    mutex_lock(&net_lock);
    dev->ip = dev->netmask = dev->gateway = dev->dns = 0;    /* start over through DHCP */
    mutex_unlock(&net_lock);
    rtt = dhcp_configure(dev, 8000);
    if(rtt != 0 && was_ip) {
        /* 10/6/26: no lease, because a kill or Ctrl+C cut the wait short or none came: the
           interface gets back the configuration it had, so a self test stopped here never
           leaves it without an address (and the laptop unreachable, telnetd included) */
        mutex_lock(&net_lock);
        dev->ip = was_ip;
        dev->netmask = was_mask;
        dev->gateway = was_gw;
        dev->dns = was_dns;
        arp_announce(dev);
        mutex_unlock(&net_lock);
    }
    check(rtt == 0 && dev->ip == IP4(10, 0, 2, 15) && dev->gateway == IP4(10, 0, 2, 2) && dev->dns == IP4(10, 0, 2, 3),
          "DHCP lease: 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3");
    check(icmp_ping(IP4(10, 0, 2, 2), 0x5f60, 1, 56, 2000, NULL) >= 0, "ping works on the leased address");
    test_tcp();
    printf("        (%s: %lu frames in, %lu out)\n", ipfmt(dev->ip, a), dev->rx_packets, dev->tx_packets);
}

/**** terminal (10/5/26) ****/

static volatile int raw_was_set;

static void set_raw_and_exit(void *arg) {     /* and never turns it off, nor the inverse or the cursor */
    struct tty *t = (struct tty *)arg;
    const char *s = "\033[7m\033[?25l";
    raw_was_set = tty_ctl(t, TTY_RAW, 1) == 0 && t->raw;
    while(*s) kputch(t->con, t->con->txtColor, *s++);
    raw_was_set = raw_was_set && t->con->inverse && t->con->cursor_off;
}

/* escape sequences on a console that is never shown (no screen, no serial port), with
   cells for the screen's grid */
static surf_console *ansi_con;

static void ansi_put(const char *s) {
    while(*s) kputch(ansi_con, WHITE_TXT, *s++);
}

/* the text at x,y, and its attribute unless attr is -1 */
static bool ansi_at(u_int x, u_int y, const char *s, int attr) {
    for(; *s; s++, x++) {
        u_char *cell = ansi_con->vmem + (y * COLUMNS + x) * 2;
        if(cell[0] != (u_char)*s || (attr >= 0 && cell[1] != attr)) return false;
    }
    return true;
}

static bool ansi_cursor(u_int x, u_int y) {
    return ansi_con->loc.x == x && ansi_con->loc.y == y;
}

static void test_ansi(void) {
    u_int i;
    ansi_con = (surf_console *)kcalloc(1, sizeof(surf_console));
    if(ansi_con) ansi_con->vmem = (u_char *)kalloc(COLUMNS * LINES * 2);
    if(!ansi_con || !ansi_con->vmem) {
        check(false, "console: memory for the escape sequence tests");
        if(ansi_con) kfree(ansi_con);
        return;
    }
    ansi_con->txtColor = WHITE_TXT;
    clearConsole(ansi_con);
    ansi_put("first\033[3;5Hxy");
    check(ansi_at(0, 0, "first", 0x07) && ansi_at(4, 2, "xy", 0x07) && ansi_cursor(6, 2), "console: ESC[row;colH");
    ansi_put("\033[7mI\033[0mN\033[1;31mR\033[44mB\033[m.");
    check(ansi_at(6, 2, "I", 0x70) && ansi_at(7, 2, "N", 0x07) && ansi_at(8, 2, "R", 0x0C) && ansi_at(9, 2, "B", 0x1C) &&
          ansi_at(10, 2, ".", 0x07), "console: SGR 7, 1, 31, 44 and 0 in VGA colours");
    ansi_put("\033[2A\033[3C\033[B\033[2D*");
    check(ansi_at(12, 1, "*", -1), "console: ESC[nA ESC[nB ESC[nC ESC[nD");
    ansi_put("\033[1;3H\033[K");
    check(ansi_at(0, 0, "fi   ", 0x07) && ansi_cursor(2, 0), "console: ESC[K erases to the end of the line");
    ansi_put("\033[99;99HZ");
    check(ansi_at(COLUMNS - 1, LINES - 1, "Z", -1) && ansi_at(0, 0, "fi", -1) && ansi_cursor(COLUMNS - 1, LINES - 1),
          "console: the bottom right corner is written without scrolling");
    ansi_put("\033[2J");
    check(ansi_at(4, 2, "  ", 0x07) && ansi_at(COLUMNS - 1, LINES - 1, " ", -1), "console: ESC[2J clears");
    ansi_put("\033[H\033[5n\033[?1049h\033(B\033]0;a title\007\033[38;5;196m\033[ q\033[?7;9rok");
    check(ansi_at(0, 0, "ok ", 0x07), "console: other sequences are dropped whole");
    ansi_put("\033[?25l");
    check(ansi_con->cursor_off && ansi_con->ansi_owner == curTask, "console: ESC[?25l hides the cursor");
    ansi_put("\033[?25h");
    check(!ansi_con->cursor_off && !ansi_con->ansi_owner, "console: ESC[?25h shows it");
    /* a full line: the cursor waits in the last column; "\r\n" is one line, "\n" alone wraps first as it always did */
    ansi_put("\033[H");
    for(i = 0; i < COLUMNS; i++) ansi_put("a");
    check(ansi_cursor(COLUMNS - 1, 0) && ansi_con->wrap, "console: the cursor stays in the last column");
    ansi_put("\r\n");
    for(i = 0; i < COLUMNS; i++) ansi_put("b");
    ansi_put("\n");
    check(ansi_cursor(0, 3), "console: \\r\\n after a full line is one line, \\n alone wraps first");
    /* the line editor's backspace goes back across a wrapped line */
    ansi_put("\033[2J\033[H");
    for(i = 0; i < COLUMNS; i++) ansi_put("c");
    ansi_put("d\b\b");
    check(ansi_at(COLUMNS - 1, 0, " ", -1) && ansi_at(0, 1, " ", -1) && ansi_cursor(COLUMNS - 1, 0),
          "console: backspace erases back across the end of a line");
    kfree(ansi_con->vmem);
    kfree(ansi_con);
    ansi_con = NULL;
}

/* raw mode on this shell's own terminal: nothing is typed, the keys are made up here */
static void test_tty(void) {
    struct tty *t = curTask->tty;
    char buf[16];
    int size = tty_ctl(t, TTY_GETSIZE, 0), n, status;
    u_int cols = t->con->stream ? t->con->cols : COLUMNS, rows = t->con->stream ? t->con->rows : LINES;
    surf_task *k;

    /* 10/6/26: run in a pseudo-terminal's session (telnet), the size is the pty's (PTY_SETSIZE) */
    check((size >> 16) == (int)cols && (size & 0xFFFF) == (int)rows, "tty: TTY_GETSIZE is the console's text grid, or the pty's size");
    tty_flush(t);
    check(tty_ctl(t, TTY_RAW, 1) == 0 && t->raw, "tty: raw mode on");
    tty_key(t, UP); tty_key(t, DEL); tty_key(t, 0x03); tty_key(t, F5); tty_key(t, 'x');
    n = tty_read(t, buf, sizeof(buf));
    check(n == 9 && !memcmp(buf, "\033[A\033[3~\003x", 9), "tty: raw keys at once, special keys as escape sequences");
    check(tty_ctl(t, TTY_RAW, 0) == 0 && !t->raw, "tty: raw mode off");
    check(tty_ctl(t, TTY_RAW, 2) < 0 && tty_ctl(t, 99, 0) < 0, "tty: unknown requests are refused");
    raw_was_set = 0;
    k = kthread_create("rawexit", set_raw_and_exit, t, PL_NORMAL, 0);
    if(k) task_wait(k->pid, &status);
    check(k && raw_was_set && !t->raw, "tty: raw mode ends when the task that set it exits");
    check(k && raw_was_set && !t->con->inverse && !t->con->cursor_off, "console: so do its colours and its hidden cursor");
    tty_flush(t);
    test_ansi();
}

/* 10/5/26: endpoint 0 of a USB device must work again after a request it stalled */
static void test_usb(void) {
    int r = usb_stall_test();
    if(r == USB_ENODEV) { printf("  skip USB (no device that stalls a bad request)\n"); return; }
    check(r == 0, "USB: endpoint 0 works again after a stalled request");
    if(r) printf("        (%s)\n", usb_strerror(r));
}

/* 10/6/26: in this order; a kill or Ctrl+C starts no other (each one waits for its helpers) */
static void (*const tests[])(void) = {
    test_bdev, test_boot_drive, test_part_commit, test_fs, test_fat, test_fat_rename, test_fat_statfs,
    test_fat_utf8, test_fat_concurrent, test_fat_unplug, test_user, test_net, test_usb, test_tasks,
    test_sleep, test_timers, test_mutex_fn, test_semaphore, test_event_fn, test_libc, test_tty
};

int run_selftest(void) {
    u_int i;
    if(!claim("selftest", true)) return 1;
    fails = 0;
    printf("\nSurfOS self test\n----------------\n");
    check(heap_round() == 0, "heap");
    check(heaptest() == 1, "a heaptest while the self test runs is refused");
    for(i = 0; i < sizeof(tests) / sizeof(tests[0]) && !task_interrupted(); i++) tests[i]();
    if(task_interrupted()) printf("SELFTEST STOPPED (cut short by a kill or Ctrl+C after %u of %u tests)\n\n", i, (u_int)(sizeof(tests) / sizeof(tests[0])));
    else printf("%s (%s)\n\n", fails ? "SELFTEST FAIL" : "SELFTEST PASS", fails ? "see above" : "all checks passed");
    unclaim();
    return fails || task_interrupted();
}
