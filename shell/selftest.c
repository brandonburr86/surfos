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
#include <surfos/sysimpl.h>
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

/* 10/18/26 (round fs-fix-8): a write by ram_stuck_task does not end until the test clears it (a request that takes
   long, B56); ram_hook sees each write before it is stored (B46, B51: Ctrl+C at a chosen write); ram_flush_hook sees
   each flush, which takes ram_flush_ms; and the highest count of ram_watch_task inside a file system seen in its reads
   is kept (B52), and (round fs-fix-9) the lowest seen in any of its requests, and how many there were */
static surf_task *volatile ram_stuck_task;
static volatile u32 ram_stuck_lba;                   /* (round fs-fix-9: only its write of this block; 0: any) */
static volatile int ram_stuck_hit;
static void (*volatile ram_hook)(u32 lba);
static void (*volatile ram_flush_hook)(void);
static u_long ram_flush_ms;
static surf_task *volatile ram_watch_task;
static volatile u_int ram_watch_hold, ram_watch_min, ram_watch_n;

static void ram_watch(void) {
    if(ram_watch_task && curTask == ram_watch_task && (!ram_watch_n++ || curTask->fs_hold < ram_watch_min))
        ram_watch_min = curTask->fs_hold;
}

static volatile int ram_read_fail0;                  /* 10/18/26 (round fs-fix-10): block 0 cannot be read (B61) */

static int ram_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    if(ram_watch_task && curTask == ram_watch_task && curTask->fs_hold > ram_watch_hold) ram_watch_hold = curTask->fs_hold;
    ram_watch();
    if(ram_read_fail0 && lba == 0) return BDEV_EIO;
    memcpy(buf, (u8 *)d->priv + lba * 512, count * 512);
    return 0;
}

static bool ram_refuse;                              /* 10/16/26 (B10): the test's RAM disks refuse every write */
static u_long ram_slow_ms;                           /* 10/17/26 (B39): and take this long for each (a driver's delay) */
/* 10/17/26 (B46): the writes they have begun, and while ram_ntrace is below RAM_TRACE, which block each was, which task
   wrote it and when it began (ram_trace_on: from then on); ram_fast_task's writes are never slow */
#define RAM_TRACE 8
static volatile u_int ram_writes_begun, ram_ntrace;
static u_long ram_trace_on;
static struct { u32 lba; u_long at; char who[8]; } ram_trace[RAM_TRACE];
static surf_task *volatile ram_fast_task;

static int ram_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    ram_watch();
    if(ram_refuse) return BDEV_EIO;
    ram_writes_begun++;
    if(ram_trace_on && ram_ntrace < RAM_TRACE) {
        u_int k = ram_ntrace++;
        ram_trace[k].lba = lba;
        ram_trace[k].at = uptime_ms() - ram_trace_on;
        strlcpy(ram_trace[k].who, curTask ? curTask->name : "?", sizeof(ram_trace[k].who));
    }
    if(ram_hook) ram_hook(lba);
    if(ram_stuck_task && curTask == ram_stuck_task && (!ram_stuck_lba || (lba <= ram_stuck_lba && ram_stuck_lba < lba + count))) {
        ram_stuck_hit = 1;
        while(ram_stuck_task) sleep_ms(10);
    }
    if(ram_slow_ms && curTask != ram_fast_task) sleep_ms(ram_slow_ms);
    memcpy((u8 *)d->priv + lba * 512, buf, count * 512);
    return 0;
}

static int ram_flush(struct bdev *d) {
    (void)d;
    ram_watch();
    if(ram_flush_hook) ram_flush_hook();
    if(ram_flush_ms) sleep_ms(ram_flush_ms);
    return 0;
}

static const struct bdev_ops ram_ops = { ram_read, ram_write, ram_flush };

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

/* 10/16/26 (audit B10): a stick that refuses every write once a file is written: the file's close says
   EIO (it said nothing: cp and edit reported a save that was only in the cache), and umount gives the
   blocks up and says how many */
static void test_fat_refused(void) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    struct file *f;
    char msg[160];
    u_int lost = 0;
    int r, c = 0;
    if(!d) { check(false, "refused writes: 4 MB of memory"); kfree(mem); return; }
    strlcpy(d->name, "stick8", sizeof(d->name));
    r = fat_mkfs(d, "REFUSED", 0, msg, sizeof(msg));
    check(r == 0 && (r = vfs_mount_flags(d, "/selftest-refused", "fat", 0)) == 0, "refused writes: mkfs and mount a RAM stick");
    if(r) { kfree(d); kfree(mem); return; }
    ram_refuse = true;
    r = vfs_open("/selftest-refused/a.txt", O_WRONLY | O_CREAT, &f);
    if(r == 0) {
        r = vfs_write(f, "refused", 7) == 7 ? 0 : -EIO;
        c = vfs_close(f);
    }
    check(r == 0 && c == -EIO, "refused writes: the write goes into the cache, and its close says EIO");
    check(file_is("/selftest-refused/a.txt", "refused"), "refused writes: the file reads back from the cache");
    r = vfs_umount_lost("/selftest-refused", &lost);
    check(r == 0 && lost > 0, "refused writes: umount gives the blocks up and says how many");
    ram_refuse = false;
    binvalidate(d);
    kfree(d);
    kfree(mem);
}

/* 10/17/26 (audit B39): a task killed inside the block cache. A sync waiting for a block another task holds (B18: up
   to 10 s) is cut short by the kill and gives up at once; a sync inside a device's write that takes its time is not
   killed at once when the kill has waited KILL_WAIT_MS: it comes once the task is out of the cache. Killed at once
   there, a task left the cache's records of its copies on its dead stack and blocks marked written or copied for good
   (kernel/task.c's kill_escalate()). The syncing tasks start inside a call (TF_INCALL), as a system call is */
static struct bdev *kc_dev;
static volatile int kc_result, kc_done;

static void kc_sync(void *arg) {
    (void)arg;
    kc_result = bsync(kc_dev);
    kc_done = 1;
    task_call_leave(TC_SYSCALL);                     /* the call ends: a kill that waited ends the task here */
}

/* a sync of the test's disk in a task of its own, killed after 200 ms: its status, and the milliseconds from the kill
   to its end */
static int kc_kill_sync(const char *name, u_long *ms) {
    surf_task *t;
    u_long pid, t0;
    int status = 0;
    kc_done = 0;
    kc_result = 1;
    t = kthread_create_on(name, curTask->con, kc_sync, NULL, PL_NORMAL, TF_INCALL);
    if(!t) return 1;
    pid = t->pid;
    sleep_ms(200);
    t0 = uptime_ms();
    kill_task(t);
    if(task_wait(pid, &status) != (int)pid) status = 1;
    *ms = uptime_ms() - t0;
    return status;
}

static void test_kill_in_cache(void) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    struct buf *b;
    u_long ms;
    int status;
    bool written;
    if(!d) { check(false, "kill in the cache: 4 MB of memory"); kfree(mem); return; }
    strlcpy(d->name, "stick7", sizeof(d->name));
    kc_dev = d;
    b = bread(d, 10);                                /* this task holds a changed block */
    if(!b) { check(false, "kill in the cache: a block"); kfree(d); kfree(mem); return; }
    memset(b->data, 'A', 512);
    bdirty(b);
    status = kc_kill_sync("killsync", &ms);          /* its sync waits for the hold */
    check(status == -1 && kc_done && kc_result == -1 && ms < 1000,
          "a kill cuts short a sync's wait for a block another task holds (it gives up at once)");
    brelse(b);
    check(bsync(d) == 0 && mem[10 * 512] == 'A', "the block is written by the next sync");
    b = bread(d, 20);                                /* a changed block nobody holds, and a device that takes 3 s */
    if(b) { memset(b->data, 'B', 512); bdirty(b); brelse(b); }
    ram_slow_ms = 3000;
    status = kc_kill_sync("killsync2", &ms);         /* its sync is in the device's write when the kill comes */
    ram_slow_ms = 0;
    b = bpeek(d, 20);
    written = mem[20 * 512] == 'B' && (!b || (!b->writing && !b->dirty));
    if(b) brelse(b);
    check(status == -1 && ms >= 2500 && written && bcache_audit(d) == 0,
          "a kill that waited does not come at once inside the cache: the device's write ends first");
    binvalidate(d);
    kfree(d);
    kfree(mem);
}

/* 10/17/26 (audit B46): umount interrupted while another task's write of the volume's device is in flight (the review's
   R1: a `sync` on another console; here a sync of the test's disk, whose writes take 300 ms each). The umount's waits
   were cut short (B39) and it went on: it gave up changes the disk would take and left the volume marked as not cleanly
   unmounted. Now an umount that has begun finishes (fs_finish, surfos/task.h): Ctrl+C cuts the command after it, and
   a kill comes after it. The volume's changes wait in the cache because the disk refused them for a while (a write's
   close said EIO): every VFS call that changes a volume syncs it before it returns (mkdir, rm, mv, a written file's
   close), and a volume a file or a command is using cannot be unmounted, so changes a sync of another task can be
   writing at an umount are those a refusal (or a sync cut short) left behind. The interrupt reaches the umount task
   once its command has begun, before its syncs; its own writes are not slow, so that the other task's write is still
   under way when the umount has written everything else (as in the review's R1, where it ran inside that write) */
static struct bdev *ku_dev;
static volatile int ku_umount_r, ku_done, ku_cut, ku_go, ku_int_seen;
static volatile u_int ku_lost;
static volatile u_long ku_began, ku_ended;

static void ku_sync(void *arg) {
    (void)arg;
    bsync(ku_dev);
}

static void ku_umount(void *arg) {
    u_int lost = 99;
    bool ctrl_c = arg != NULL;
    char *argv[] = { "umount", "/selftest-um", NULL };
    if(ctrl_c) task_call_enter(TC_COMMAND);          /* as the ring-0 shell runs a command */
    while(!ku_go) sleep_ms(5);                       /* the interrupt is sent meanwhile (a driver's delay: not cut short) */
    ku_began = uptime_ms() - ram_trace_on;
    ku_int_seen = (curTask->intr & TI_INT) != 0;     /* 10/18/26: (round fs-fix-9) the Ctrl+C has come */
    if(ctrl_c) {                                     /* 10/18/26 (B54): the umount command: CMD_OK when nothing was lost */
        ku_umount_r = shell_run_command(2, argv);
        lost = 0;
    } else ku_umount_r = vfs_umount_lost("/selftest-um", &lost);
    ku_ended = uptime_ms() - ram_trace_on;
    ku_lost = lost;
    ku_done = 1;
    if(ctrl_c) ku_cut = task_call_leave(TC_COMMAND);
    else task_call_leave(TC_SYSCALL);                /* the kill that waited ends the task here */
}

/* /selftest-um mounted, a file written while the disk refused writes (its close says EIO: the file's blocks stay in
   the cache, changed), the disk taking writes again, 300 ms each but the umount's; a sync of the disk in its first write;
   an umount begun with Ctrl+C or a kill reaching it then. The umount task's status; the disk's writes and the umount's
   times said */
static int ku_run(const char *path, const char *text, bool ctrl_c) {
    surf_task *ts, *tu;
    struct file *fl;
    u_long pid_s = 0, pid_u = 0, f;
    u_int begun, waited, i;
    int status = 1, st2, r, c = 0;
    if(vfs_mount_flags(ku_dev, "/selftest-um", "fat", 0) != 0) return 1;
    ram_refuse = true;
    r = vfs_open(path, O_WRONLY | O_CREAT, &fl);
    if(r == 0) {
        r = vfs_write(fl, text, strlen(text)) == (int)strlen(text) ? 0 : -EIO;
        c = vfs_close(fl);
    }
    ram_refuse = false;
    if(r || c != -EIO || bdirty_count(ku_dev) == 0) { vfs_umount("/selftest-um"); return 1; }
    ku_done = ku_cut = ku_go = ku_int_seen = 0;
    ku_umount_r = 1;
    ku_lost = 99;
    ku_began = ku_ended = 0;
    ram_ntrace = 0;
    ram_trace_on = uptime_ms();
    ram_fast_task = NULL;
    ram_slow_ms = 300;                               /* (the kill's 2 s are not reached: the umount ends after 4 writes) */
    begun = ram_writes_begun;
    ts = kthread_create_on("umsync", curTask->con, ku_sync, NULL, PL_NORMAL, 0);
    if(ts) pid_s = ts->pid;
    for(waited = 0; ram_writes_begun == begun && waited < 1000; waited += 5) sleep_ms(5);   /* its write is in flight */
    tu = kthread_create_on("umount", curTask->con, ku_umount, ctrl_c ? (void *)1 : NULL, PL_NORMAL, ctrl_c ? 0 : TF_INCALL);
    if(tu) {
        pid_u = tu->pid;
        ram_fast_task = tu;                          /* (it does nothing before ku_go) */
        sleep_ms(20);                                /* the umount task is in its call, its command begun */
        if(ctrl_c) {
            f = irq_save();
            task_interrupt_job(tu);                  /* Ctrl+C on its terminal, as tty.c sends it */
            irq_restore(f);
        } else kill_task(tu);
        ku_go = 1;
        if(task_wait(pid_u, &status) != (int)pid_u) status = 1;
    }
    if(ts) task_wait(pid_s, &st2);
    ram_slow_ms = 0;
    ram_fast_task = NULL;
    ram_trace_on = 0;
    printf("        (the disk's writes:");
    for(i = 0; i < ram_ntrace && i < RAM_TRACE; i++) printf(" block %lu by %s at %lu ms;", (u_long)ram_trace[i].lba, ram_trace[i].who, ram_trace[i].at);
    printf(" the umount from %lu to %lu ms)\n", ku_began, ku_ended);
    return status;
}

static void test_kill_umount(void) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    char msg[160];
    int r, status;
    bool clean;
    if(!d) { check(false, "umount interrupted: 4 MB of memory"); kfree(mem); return; }
    strlcpy(d->name, "stick6", sizeof(d->name));
    ku_dev = d;
    r = fat_mkfs(d, "UMOUNT", 0, msg, sizeof(msg));
    if(r == 0) r = vfs_mount_flags(d, "/selftest-um", "fat", 0);
    if(r == 0) { r = put_file("/selftest-um/a.txt", "written before"); vfs_umount("/selftest-um"); }
    check(r == 0, "umount interrupted: mkfs, mount and a file on a RAM stick");
    if(r) { binvalidate(d); kfree(d); kfree(mem); return; }
    status = ku_run("/selftest-um/c.txt", "refused, then synced: Ctrl+C", true);
    clean = (mem[512 + 3] & 0x80) != 0;              /* FAT[1]'s clean bit */
    check(status == 0 && ku_done && ku_umount_r == CMD_OK && clean,
          "Ctrl+C in umount while another task's write is in flight: it finishes, gives nothing up, the volume clean");
    check(ku_done && ku_int_seen && !ku_cut,         /* (B54; round fs-fix-9: the Ctrl+C seen to have come first) */
          "and the umount command, finished, has its own status (0), not Ctrl+C's 130, the Ctrl+C having come");
    status = ku_run("/selftest-um/k.txt", "refused, then synced: a kill", false);
    clean = (mem[512 + 3] & 0x80) != 0;
    check(status == -1 && ku_done && ku_umount_r == 0 && ku_lost == 0 && clean,
          "a kill during umount while another task's write is in flight: it finishes, gives nothing up, then the kill");
    r = vfs_mount_flags(d, "/selftest-um", "fat", MNT_RDONLY);
    check(r == 0 && file_is("/selftest-um/a.txt", "written before") && file_is("/selftest-um/c.txt", "refused, then synced: Ctrl+C") &&
          file_is("/selftest-um/k.txt", "refused, then synced: a kill"), "and the volume holds both files the umounts wrote");
    if(r == 0) vfs_umount("/selftest-um");
    binvalidate(d);
    kfree(d);
    kfree(mem);
}

/* 10/17/26 (audit B46): eject with Ctrl+C reaching it as its command begins: it unmounts nothing and says the disk stays
   mounted (it went on to unmount, and its waits were cut short where they met another task's write). eject finds a
   disk by name, so the RAM stick is registered for this test (and unregistered after) */
static struct bdev *ke_dev;
static volatile int ke_result, ke_go, ke_cut;

static void ke_eject(void *arg) {
    char *argv[3];
    (void)arg;
    argv[0] = "eject";
    argv[1] = ke_dev->name;
    argv[2] = NULL;
    task_call_enter(TC_COMMAND);                     /* as the ring-0 shell runs a command */
    while(!ke_go) sleep_ms(5);                       /* Ctrl+C is sent meanwhile */
    ke_result = cmd_eject(2, argv);
    ke_cut = task_call_leave(TC_COMMAND);
}

static void test_eject_interrupted(void) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    struct stat st;
    surf_task *t;
    char msg[160];
    u_long pid, f;
    int r, status = 1;
    bool registered = false;
    if(!d) { check(false, "eject interrupted: 4 MB of memory"); kfree(mem); return; }
    strlcpy(d->name, "stick4", sizeof(d->name));
    r = fat_mkfs(d, "EJECT", 0, msg, sizeof(msg));
    if(r == 0) r = bdev_register(d);
    if(r == 0) registered = true;
    if(r == 0) r = vfs_mount_flags(d, "/selftest-ej", "fat", 0);
    if(r == 0) r = put_file("/selftest-ej/e.txt", "ejected later");
    check(r == 0, "eject interrupted: a RAM stick registered as stick4, mounted, a file on it");
    if(r == 0) {
        ke_dev = d;
        ke_go = ke_cut = 0;
        ke_result = -1;
        t = kthread_create_on("ejectint", curTask->con, ke_eject, NULL, PL_NORMAL, 0);
        if(t) {
            pid = t->pid;
            sleep_ms(20);                            /* in its command, waiting to go */
            f = irq_save();
            task_interrupt_job(t);                   /* Ctrl+C on its terminal, as tty.c sends it */
            irq_restore(f);
            ke_go = 1;
            if(task_wait(pid, &status) != (int)pid) status = 1;
        }
        check(t && status == 0 && ke_result == CMD_FAIL && ke_cut && file_is("/selftest-ej/e.txt", "ejected later") &&
              vfs_stat("/selftest-ej", &st) == 0, "Ctrl+C in eject before it unmounts: it unmounts nothing and says the disk stays mounted");
        vfs_umount("/selftest-ej");
    }
    if(registered) bdev_unregister(d);
    binvalidate(d);
    kfree(d);
    kfree(mem);
}

/* 10/17/26 (audit B47): a task killed inside a device request outside the cache (mkfs, fdisk, a dump of a disk). The
   kill that waited came at once after 2 s inside the driver (here its 3 s delay; in a real driver its lock and its
   transfer half done: the card hung); now it comes once the request is over. And mkfs, killed, stops before its next
   request and says how far it got */
static struct bdev *kd_dev;
static u8 *kd_buf;
static volatile int kd_result, kd_done;
static char kd_msg[160];

static void kd_write(void *arg) {
    (void)arg;
    kd_result = bdev_write(kd_dev, 30, 1, kd_buf);
    kd_done = 1;
    task_call_leave(TC_SYSCALL);
}

static void kd_mkfs(void *arg) {
    (void)arg;
    kd_result = fat_mkfs(kd_dev, "KILLED", 0, kd_msg, sizeof(kd_msg));
    kd_done = 1;
    task_call_leave(TC_SYSCALL);
}

/* fn in a task of its own inside a call, killed after `after` ms: its status, and the milliseconds from the kill to
   its end */
static int kd_kill(const char *name, kthread_fn fn, u_long after, u_long *ms) {
    surf_task *t;
    u_long pid, t0;
    int status = 0;
    kd_done = 0;
    kd_result = 1;
    t = kthread_create_on(name, curTask->con, fn, NULL, PL_NORMAL, TF_INCALL);
    if(!t) return 1;
    pid = t->pid;
    sleep_ms(after);
    t0 = uptime_ms();
    kill_task(t);
    if(task_wait(pid, &status) != (int)pid) status = 1;
    *ms = uptime_ms() - t0;
    return status;
}

static void test_kill_device(void) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    u8 *buf = (u8 *)kcalloc(1, 512);
    u_long ms;
    int status;
    if(!d || !buf) { check(false, "kill in a device request: memory"); kfree(d); kfree(mem); kfree(buf); return; }
    strlcpy(d->name, "stick5", sizeof(d->name));
    kd_dev = d;
    kd_buf = buf;
    memset(buf, 'D', 512);
    ram_slow_ms = 3000;
    status = kd_kill("killdev", kd_write, 200, &ms); /* its write takes 3 s: the kill waits 2 s, then for the request */
    ram_slow_ms = 0;
    check(status == -1 && ms >= 2500 && mem[30 * 512] == 'D',
          "a kill that waited does not come at once inside a device request outside the cache: the request ends first");
    printf("        (it ended %lu ms after its kill, sent 200 ms into a 3000 ms write)\n", ms);
    ram_slow_ms = 300;
    status = kd_kill("killmkfs", kd_mkfs, 100, &ms); /* in mkfs's first request */
    ram_slow_ms = 0;
    check(status == -1 && kd_done && kd_result == -EINTR && strstr(kd_msg, "interrupted at sector") && ms < 1500,
          "mkfs killed stops before its next request and says how far it got");
    printf("        (it ended %lu ms after its kill: %s)\n", ms, kd_done ? kd_msg : "killed before mkfs returned");
    binvalidate(d);
    kfree(d);
    kfree(mem);
    kfree(buf);
}

/* 10/17/26 (audit B48): a kernel thread killed outside any call while it is inside a file system: the kill waits until
   it leaves (looked at every 20 ms). One that enters a call meanwhile gets that call's 2 s, as any call does: it was
   killed at once in the call 20 ms later, said not to have ended within 2000 ms, holding whatever the call held. One
   that leaves without entering a call is killed within a few ticks of leaving (B39) */
static volatile int kn_flag;
static volatile u_long kn_out;

static void kn_call(void *arg) {
    (void)arg;
    fs_enter();                                      /* inside a file system, outside any call */
    sleep_ms(300);                                   /* the kill comes meanwhile: put off */
    task_call_enter(TC_COMMAND);                     /* a kernel command begins, still inside */
    fs_leave();
    sleep_ms(500);                                   /* well within a call's 2 s */
    kn_flag = 1;
    task_call_leave(TC_COMMAND);                     /* the kill comes here */
}

static void kn_leave(void *arg) {
    (void)arg;
    fs_enter();
    sleep_ms(300);
    fs_leave();
    kn_out = uptime_ms();
    sleep_ms(2000);                                  /* killed long before this ends */
    kn_flag = 1;
}

/* fn in a kernel thread of its own (no call), killed after 100 ms: its status; *killed and *end, when it was killed
   and when it ended */
static int kn_kill(const char *name, kthread_fn fn, u_long *killed, u_long *end) {
    surf_task *t = kthread_create_on(name, curTask->con, fn, NULL, PL_NORMAL, 0);
    u_long pid;
    int status = 0;
    *killed = *end = uptime_ms();
    if(!t) return 1;
    pid = t->pid;
    sleep_ms(100);
    *killed = uptime_ms();
    kill_task(t);
    if(task_wait(pid, &status) != (int)pid) status = 1;
    *end = uptime_ms();
    return status;
}

static void test_kill_outside_call(void) {
    u_long killed, end;
    int status;
    kn_flag = 0;
    status = kn_kill("killnc", kn_call, &killed, &end);
    check(status == -1 && kn_flag, "a kernel thread killed inside a file system that enters a call before it dies gets the call's 2 s");
    printf("        (it ended %lu ms after its kill, %s)\n", end - killed, kn_flag ? "its call done" : "killed in its call");
    kn_flag = 0;
    kn_out = 0;
    status = kn_kill("killout", kn_leave, &killed, &end);
    check(status == -1 && !kn_flag && kn_out && end - kn_out < 250,
          "a kill put off inside a file system comes within a few ticks of the thread leaving it");
    printf("        (killed %lu ms after it left the file system)\n", kn_out ? end - kn_out : 0UL);
}

/**** round fs-fix-8 (audit B50-B55): the review of B46-B49 ****/

/* a RAM stick of the test's own, named name, with a FAT volume mounted at path and a.txt on it; NULL when there is no
   memory or something fails (the caller says so) */
static struct bdev *fx_stick(const char *name, const char *path, u8 **memp) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    char msg[160];
    int r;
    *memp = mem;
    if(!d) { kfree(mem); return NULL; }
    strlcpy(d->name, name, sizeof(d->name));
    r = fat_mkfs(d, "ROUND8", 0, msg, sizeof(msg));
    if(r == 0) r = vfs_mount_flags(d, path, "fat", 0);
    if(r == 0) {
        char file[VFS_PATH_MAX];
        snprintf(file, sizeof(file), "%s/a.txt", path);
        r = put_file(file, "written before");
        if(r) vfs_umount(path);
    }
    if(r) { binvalidate(d); kfree(d); kfree(mem); *memp = NULL; return NULL; }
    return d;
}

/* path/name written while the stick refuses writes: its close says EIO and its blocks stay changed in the cache (every
   VFS call that changes a volume syncs it before it returns, B46's test: this leaves changes no call has written) */
static int fx_refused(const char *path, const char *name, const char *text) {
    char file[VFS_PATH_MAX];
    struct file *f;
    int r, c = 0;
    snprintf(file, sizeof(file), "%s/%s", path, name);
    ram_refuse = true;
    r = vfs_open(file, O_WRONLY | O_CREAT, &f);
    if(r == 0) {
        r = vfs_write(f, text, strlen(text)) == (int)strlen(text) ? 0 : -EIO;
        c = vfs_close(f);
    }
    ram_refuse = false;
    return r ? r : c == -EIO ? 0 : -EIO;
}

/* the test's end with a stick: unmounted (when it still is), its blocks forgotten, freed */
static void fx_done(struct bdev *d, u8 *mem, const char *path) {
    struct stat st;
    if(path && vfs_stat(path, &st) == 0) vfs_umount(path);
    binvalidate(d);
    kfree(d);
    kfree(mem);
}

/* a command run as the shell runs it (shell_run_command()), what it says kept in out; its status */
static int fx_capture(char *out, u_int size, int argc, char **argv) {
    struct kcapture cap, *outer = curTask->kcap;     /* (the self test's own output may be kept: SYS_kcmd) */
    int r;
    cap.buf = out;
    cap.len = 0;
    cap.size = size - 1;
    cap.cut = 0;
    console_capture_begin(&cap);
    r = shell_run_command(argc, argv);
    console_capture_end();
    if(outer) console_capture_begin(outer);
    out[cap.len] = 0;
    return r;
}

/* 10/18/26 (audit B56): an umount (a step that must finish, B46) waits for another task's write of a block it owes for
   as long as that write takes. Round fs-fix-8 gave such a write up after 30 s (B50), which also counted the time the
   write queued for a driver's lock or the bounce buffer: the umount then let the device go while the write went on,
   and a mkfs after it took the late write (the old root directory landed on the new volume: the review's step). Here
   the sync's write of the root directory block is held 2.5 s (ram_stuck_task) and Ctrl+C reaches the umount meanwhile;
   while it waits, mount and mkfs are refused; once the write ends the umount finishes clean, and mkfs then makes a
   volume that nothing old lands on. (The run on 3ce9600 lowers its bound to 2 s here, as the review did) */
static struct bdev *kw_dev;
static volatile int kw_go, kw_go2, kw_umount_r, kw_done;
static volatile u_int kw_lost;
static volatile u_long kw_ended;

static void kw_sync(void *arg) {
    (void)arg;
    while(!kw_go) sleep_ms(5);
    bsync(kw_dev);
}

static void kw_umount(void *arg) {
    u_int lost = 99;
    (void)arg;
    task_call_enter(TC_COMMAND);
    while(!kw_go2) sleep_ms(5);                      /* (once the sync's write is under way) */
    kw_umount_r = vfs_umount_lost("/selftest-uw", &lost);
    kw_lost = lost;
    kw_ended = uptime_ms();
    kw_done = 1;
    task_call_leave(TC_COMMAND);
}

static void test_umount_waits(void) {
    u8 *mem;
    struct bdev *d = fx_stick("stick2", "/selftest-uw", &mem);
    surf_task *ts, *tu;
    struct stat st2;
    char msg[160];
    u_long pid_s, pid_u, released, f;
    int st, waited, rm = 1, rk = 1, mk = 1, mr = 1;
    bool early, clean = false, old_gone = false;
    if(!d) { check(false, "umount and a write that takes long: a RAM stick"); return; }
    if(fx_refused("/selftest-uw", "b.txt", "refused, then slow") != 0) { check(false, "a file the stick refused"); fx_done(d, mem, "/selftest-uw"); return; }
    kw_dev = d;
    kw_go = kw_go2 = kw_done = 0;
    kw_umount_r = 1;
    kw_lost = 99;
    kw_ended = 0;
    ram_stuck_hit = 0;
    ts = kthread_create_on("uwsync", curTask->con, kw_sync, NULL, PL_NORMAL, 0);
    tu = ts ? kthread_create_on("uwumount", curTask->con, kw_umount, NULL, PL_NORMAL, 0) : NULL;
    if(!ts || !tu) {
        check(false, "umount and a write that takes long: two tasks");
        kw_go = kw_go2 = 1;
        if(ts) task_wait(ts->pid, &st);
        if(tu) task_wait(tu->pid, &st);
        fx_done(d, mem, "/selftest-uw");
        return;
    }
    pid_s = ts->pid;
    pid_u = tu->pid;
    ram_stuck_lba = (mem[14] | mem[15] << 8) + mem[16] * (mem[22] | mem[23] << 8);   /* the root directory: reserved + FATs */
    ram_stuck_task = ts;                             /* the sync's write of it (the last of b.txt's orders) is held */
    kw_go = 1;
    for(waited = 0; !ram_stuck_hit && waited < 2000; waited += 10) sleep_ms(10);
    /* (base harness: round fs-fix-8's bound lowered here) */
    kw_go2 = 1;                                      /* the umount begins */
    sleep_ms(20);
    f = irq_save();
    task_interrupt_job(tu);                          /* Ctrl+C on its terminal */
    irq_restore(f);
    sleep_ms(500);
    rm = vfs_mount_flags(d, "/selftest-uw2", "fat", 0);
    rk = fat_mkfs(d, "LATE", 0, msg, sizeof(msg));
    if(rm == 0) vfs_umount("/selftest-uw2");        /* (the code before: let in) */
    for(waited = 500; !kw_done && waited < 2500; waited += 50) sleep_ms(50);
    early = kw_done != 0;                            /* returned while the write was still held (the code before) */
    if(early) {
        clean = (mem[512 + 3] & 0x80) != 0;
        mk = fat_mkfs(d, "NEWVOL", 0, msg, sizeof(msg));   /* the review's step: mkfs, then the held write ends */
    }
    released = uptime_ms();
    ram_stuck_task = NULL;                           /* the write ends now */
    task_wait(pid_u, &st);
    task_wait(pid_s, &st);
    ram_stuck_lba = 0;
    /* (base harness: round fs-fix-8's bound put back here) */
    if(!early) {
        clean = (mem[512 + 3] & 0x80) != 0;
        mk = fat_mkfs(d, "NEWVOL", 0, msg, sizeof(msg));
    }
    mr = mk == 0 ? vfs_mount_flags(d, "/selftest-uw", "fat", MNT_RDONLY) : -1;
    old_gone = mr == 0 && vfs_stat("/selftest-uw/a.txt", &st2) != 0 && vfs_stat("/selftest-uw/b.txt", &st2) != 0;
    if(mr == 0) vfs_umount("/selftest-uw");
    printf("        (the umount ended %s the held write did, %u changed blocks given up; meanwhile mount %d, mkfs %d)\n",
           early ? "BEFORE" : "after", kw_lost, rm, rk);
    check(ram_stuck_hit && kw_done && !early && kw_ended >= released && kw_umount_r == 0 && kw_lost == 0 && clean,
          "an interrupted umount waits for another task's write of a block it owes as long as it takes (2.5 s here), gives "
          "nothing up, the volume clean");
    check(rm == -EBUSY && rk == -EACCES, "while it waits, mount and mkfs refuse the device");
    check(mk == 0 && old_gone, "and a volume mkfs makes after it takes no old write: the old files are not on it");
    fx_done(d, mem, NULL);
}

/* 10/18/26 (audit B56): a request issued on a device and not ended keeps it busy (struct bdev's requests): no mount,
   mkfs or fdisk takes a device a late write may still land on. Here a raw write of a RAM stick nothing has mounted is
   held 1 s; meanwhile mount, mkfs and fdisk's rule refuse the stick, saying why; once it has ended, they may */
static struct bdev *kq_dev;
static volatile int kq_r, kq_done;

static void kq_write(void *arg) {
    static u8 blk[512];
    (void)arg;
    memset(blk, 0x5a, sizeof(blk));
    kq_r = bdev_write(kq_dev, kq_dev->nblocks - 1, 1, blk);   /* (the last block: no file's) */
    kq_done = 1;
}

static void test_request_busy(void) {
    u8 *mem;
    struct bdev *d = fx_stick("stickq", "/selftest-rq", &mem);
    surf_task *t;
    char msg[160], why[120];
    int rm = 1, rk = 1, rf = 0, r2 = 1, st, waited;
    u_int counts = 99;
    if(!d) { check(false, "a request under way: a RAM stick"); return; }
    vfs_umount("/selftest-rq");                     /* nothing mounts it now */
    kq_dev = d;
    kq_done = 0;
    kq_r = 1;
    msg[0] = why[0] = 0;
    ram_stuck_hit = 0;
    t = kthread_create_on("rqwrite", curTask->con, kq_write, NULL, PL_NORMAL, 0);
    if(t) {
        ram_stuck_task = t;
        for(waited = 0; !ram_stuck_hit && waited < 2000; waited += 5) sleep_ms(5);
        rm = vfs_mount_flags(d, "/selftest-rq", "fat", 0);
        rk = fat_mkfs(d, "BUSY", 0, msg, sizeof(msg));
        rf = bdev_may_format(d, why, sizeof(why));
        sleep_ms(1000);
        ram_stuck_task = NULL;                       /* the request ends */
        task_wait(t->pid, &st);
    }
    if(rm == 0) vfs_umount("/selftest-rq");         /* (the code before: let in) */
    r2 = vfs_mount_flags(d, "/selftest-rq", "fat", MNT_RDONLY);
    counts = d->requests + d->mounting;             /* (round fs-fix-9: let go of on every way out) */
    printf("        (while the write was under way: mount %d, mkfs %d \"%s\", fdisk's rule \"%s\")\n", rm, rk, msg, why);
    check(t && ram_stuck_hit && rm == -EBUSY && rk == -EACCES && strstr(msg, "has not ended") && rf != 0 &&
          strstr(why, "has not ended"), "a device with a request under way is busy: mount, mkfs and fdisk refuse it, saying why");
    check(kq_done && kq_r == 0 && r2 == 0 && counts == 0,
          "and once the request has ended, it mounts (no request or mount left counted on the stick)");
    fx_done(d, mem, "/selftest-rq");
}

/* 10/18/26 (audit B51): mount, mkfs and fdisk of a device while its umount still writes (the review's M2 and M5): the
   mount leaves the list before the umount's syncs, and vfs_bdev_busy() looked at the list only, so a second mount's file
   went with the old umount's binvalidate(), the card was marked clean under a live mount, mkfs's FATs crossed the old
   volume's writes. Now the device is busy until its umount has let go of it, and they say it is being unmounted. So
   does eject, which found nothing mounted and said "safe to remove" while the umount still wrote */
static struct bdev *kb_dev;
static volatile int kb_done;

static void kb_umount(void *arg) {
    u_int lost = 0;
    (void)arg;
    vfs_umount_lost("/selftest-busy", &lost);
    kb_done = 1;
}

static void test_unmounting_busy(void) {
    u8 *mem;
    struct bdev *d = fx_stick("stick3", "/selftest-busy", &mem);
    char msg[160], why[120], said[240];
    char *ej_argv[] = { "eject", "stick3", NULL };
    surf_task *tu;
    u_int begun;
    int rm = 1, rk = 1, rf = 0, re = CMD_OK, st, waited;
    bool registered;
    if(!d) { check(false, "a device being unmounted: a RAM stick"); return; }
    if(fx_refused("/selftest-busy", "b.txt", "written while it is unmounted") != 0) { check(false, "a file the stick refused"); fx_done(d, mem, "/selftest-busy"); return; }
    registered = bdev_register(d) == 0;              /* (eject finds disks by name) */
    said[0] = 0;
    kb_dev = d;
    kb_done = 0;
    msg[0] = why[0] = 0;
    ram_slow_ms = 300;
    begun = ram_writes_begun;
    tu = kthread_create_on("bsyumount", curTask->con, kb_umount, NULL, PL_NORMAL, 0);
    if(tu) {
        for(waited = 0; ram_writes_begun == begun && waited < 2000; waited += 5) sleep_ms(5);   /* its syncs write */
        rm = vfs_mount_flags(d, "/selftest-busy2", "fat", 0);
        rk = fat_mkfs(d, "BUSY", 0, msg, sizeof(msg));
        rf = bdev_may_format(d, why, sizeof(why));
        if(registered) re = fx_capture(said, sizeof(said), 2, ej_argv);
        task_wait(tu->pid, &st);
    }
    ram_slow_ms = 0;
    if(rm == 0) vfs_umount("/selftest-busy2");      /* (the code before let it mount) */
    printf("        (while it was unmounted: mount %d, mkfs %d \"%s\", fdisk's rule \"%s\")\n", rm, rk, msg, why);
    check(tu && rm == -EBUSY && rk == -EACCES && strstr(msg, "being unmounted") && rf != 0 && strstr(why, "being unmounted"),
          "a device whose umount still writes is busy: mount, mkfs and fdisk refuse it, saying it is being unmounted");
    check(tu && registered && re == CMD_FAIL && strstr(said, "being unmounted") && !strstr(said, "safe to remove"),
          "and eject refuses it, saying so (not \"safe to remove\" while the umount still writes)");
    if(!(re == CMD_FAIL && strstr(said, "being unmounted"))) printf("        (eject said: %s)\n", said);
    rm = vfs_mount_flags(d, "/selftest-busy", "fat", MNT_RDONLY);
    check(rm == 0 && file_is("/selftest-busy/a.txt", "written before") && file_is("/selftest-busy/b.txt", "written while it is unmounted"),
          "once its umount has finished, it mounts again with every file");
    if(rm == 0) vfs_umount("/selftest-busy");
    if(registered) bdev_unregister(d);              /* (gone, off the list; nothing holds it now) */
    fx_done(d, mem, NULL);
}

/* 10/18/26 (the review's coverage note 7; audit B51, B54): eject interrupted in its unmount phase (a step that must finish,
   B46) while another task writes one of the blocks it owes. As eject's write-out ends (its flush), a file on the stick is
   written while the stick refuses (its blocks stay changed in the cache), a `sync` of the disk on another console,
   looking every 5 ms, begins to write them (each write takes 300 ms), and that write is held until eject is in its
   unmount phase, waiting for it: then Ctrl+C reaches eject. eject finishes, gives nothing up and says "safe to remove";
   its status is its own (B54: it was 130); and the disk stays busy for fdisk and mkfs meanwhile (B51) */
static struct bdev *ke2_dev;
static surf_task *volatile ke2_syncer, *volatile ke2_eject;
static volatile int ke2_go, ke2_stop, ke2_result, ke2_cut, ke2_hit, ke2_busy_said, ke2_flushing, ke2_armed, ke2_phase,
                    ke2_int_seen;

/* the syncer's first write once the file is there: held until eject is in its unmount phase, then Ctrl+C for eject */
static void ke2_hook(u32 lba) {
    char why[120];
    u_long f;
    int waited;
    (void)lba;
    if(ke2_hit || !ke2_armed || curTask != ke2_syncer || !ke2_eject) return;
    ke2_hit = 1;
    for(waited = 0; !ke2_eject->fs_finish && waited < 3000; waited += 5) sleep_ms(5);
    ke2_phase = ke2_eject->fs_finish != 0;
    sleep_ms(50);                                    /* (its umount's sync waits for this write now) */
    f = irq_save();
    task_interrupt_job(ke2_eject);                   /* Ctrl+C on eject's terminal */
    ke2_int_seen = (ke2_eject->intr & TI_INT) != 0;  /* (round fs-fix-9: it has come) */
    irq_restore(f);
    ke2_busy_said = bdev_may_format(ke2_dev, why, sizeof(why)) != 0 && strstr(why, "being unmounted") != NULL;
}

/* eject's flush at the end of its write-out: held until the syncer's write has begun */
static void ke2_flush(void) {
    int waited;
    if(curTask != ke2_eject || ke2_flushing) return;
    ke2_flushing = 1;
    for(waited = 0; !ke2_hit && waited < 3000; waited += 5) sleep_ms(5);
}

static void ke2_sync(void *arg) {
    (void)arg;
    while(!ke2_stop) { bsync(ke2_dev); sleep_ms(5); }
}

static void ke2_run(void *arg) {
    char *argv[] = { "eject", NULL, NULL };
    (void)arg;
    argv[1] = ke2_dev->name;
    task_call_enter(TC_COMMAND);
    while(!ke2_go) sleep_ms(5);
    ke2_result = cmd_eject(2, argv);
    ke2_cut = task_call_leave(TC_COMMAND);
}

static void test_eject_unmount_phase(void) {
    u8 *mem;
    struct bdev *d = fx_stick("stick1", "/selftest-ej2", &mem);
    surf_task *ts, *te;
    int st, r, dirt = -1, waited;
    bool clean, registered = false;
    if(!d) { check(false, "eject's unmount phase: a RAM stick"); return; }
    if(bdev_register(d) == 0) registered = true;     /* (eject finds disks by name) */
    ke2_dev = d;
    ke2_go = ke2_stop = ke2_hit = ke2_busy_said = ke2_flushing = ke2_armed = ke2_phase = ke2_int_seen = 0;
    ke2_result = -1;
    ke2_cut = 1;
    ke2_eject = NULL;
    ram_slow_ms = 300;
    ts = kthread_create_on("ejsync", curTask->con, ke2_sync, NULL, PL_NORMAL, 0);
    te = registered && ts ? kthread_create_on("ejectphase", curTask->con, ke2_run, NULL, PL_NORMAL, 0) : NULL;
    ke2_syncer = ts;
    ke2_eject = te;
    ram_hook = ke2_hook;
    ram_flush_hook = ke2_flush;
    if(te) {
        ke2_go = 1;
        for(waited = 0; !ke2_flushing && waited < 5000; waited += 5) sleep_ms(5);   /* eject's write-out ends */
        if(ke2_flushing) dirt = fx_refused("/selftest-ej2", "b.txt", "written as eject wrote out");
        ke2_armed = 1;
        task_wait(te->pid, &st);
    }
    ram_hook = NULL;
    ram_flush_hook = NULL;
    ke2_stop = 1;
    if(ts) task_wait(ts->pid, &st);
    ram_slow_ms = 0;
    ke2_eject = ke2_syncer = NULL;
    clean = (mem[512 + 3] & 0x80) != 0;
    r = vfs_mount_flags(d, "/selftest-ej2", "fat", MNT_RDONLY);
    printf("        (the file written as eject wrote out: %d; Ctrl+C %s)\n", dirt,
           !ke2_hit ? "not sent: no write of the syncer's" : ke2_phase ? "in eject's unmount phase" : "before eject's unmount phase");
    check(te && dirt == 0 && ke2_hit && ke2_phase && ke2_result == CMD_OK && clean && r == 0 &&
          file_is("/selftest-ej2/a.txt", "written before") && file_is("/selftest-ej2/b.txt", "written as eject wrote out"),
          "eject interrupted in its unmount phase while another task writes a block it owes: it finishes, gives nothing up");
    check(te && ke2_hit && ke2_phase && ke2_int_seen && !ke2_cut,
          "and its status is its own (0, with \"safe to remove\"), not Ctrl+C's 130, the Ctrl+C having come");
    check(te && ke2_hit && ke2_phase && ke2_busy_said, "and its disk is busy meanwhile: fdisk and mkfs say it is being unmounted");
    if(r == 0) vfs_umount("/selftest-ej2");
    if(registered) bdev_unregister(d);              /* (gone, off the list; nothing holds it now) */
    fx_done(d, mem, NULL);
}

/* 10/18/26 (audit B58): eject takes its disk from its first step (vfs_unmount_begin()): before, it counted the disk as
   being unmounted only from its unmount phase, so a mount of another partition during its write-out came in, and eject
   then synced and dropped that partition's blocks while it was mounted and said "safe to remove". Here a RAM disk with
   two FAT partitions, the first mounted; eject's write-out is held at its flush while the second is mounted */
static struct bdev *kx_disk;
static surf_task *volatile kx_eject;
static volatile int kx_go, kx_flushing, kx_tried, kx_result;
static volatile u_int kx_hold;

static void kx_flush(void) {
    int waited;
    if(curTask != kx_eject || kx_flushing) return;
    kx_flushing = 1;
    kx_hold = curTask->fs_hold;                      /* (its own count and the flush request's) */
    for(waited = 0; !kx_tried && waited < 3000; waited += 5) sleep_ms(5);
}

static void kx_run(void *arg) {
    char *argv[] = { "eject", NULL, NULL };
    (void)arg;
    argv[1] = kx_disk->name;
    task_call_enter(TC_COMMAND);
    while(!kx_go) sleep_ms(5);
    kx_result = cmd_eject(2, argv);
    task_call_leave(TC_COMMAND);
}

/* a new table on disk (MBR), then partitions of count blocks (0: the rest), committed and the disk read again: 0 */
static int kx_partition(struct bdev *disk, const u32 *counts, u_int n) {
    struct part_table *t = (struct part_table *)kalloc(sizeof(struct part_table));
    struct part_plan plan;
    struct part_io io;
    struct part_kind k;
    char err[120];
    u_int i;
    int r = -1;
    plan.buf = (u8 *)kalloc(PART_PLAN_BUF);
    if(t && plan.buf && part_parse_type("fat16", &k) == 0) {
        part_io_init(&io, disk, false);
        io.note = NULL;
        r = part_read(&io, disk->nblocks, t) || part_plan_new(&io, disk->nblocks, PART_SCHEME_MBR, &plan, err, sizeof(err)) ||
            part_commit(disk, t, &plan, err, sizeof(err)) ? -1 : 0;
        for(i = 0; i < n && r == 0; i++)
            r = part_read(&io, disk->nblocks, t) || part_plan_add(&io, t, counts[i], &k, &plan, err, sizeof(err)) ||
                part_commit(disk, t, &plan, err, sizeof(err)) ? -1 : 0;
    }
    kfree(plan.buf);
    kfree(t);
    return r;
}

#define KX_BLOCKS 16384                              /* 8 MB: two FAT16 partitions (each needs about 2 MB) */

static void test_eject_writeout_mount(void) {
    u8 *mem = (u8 *)kcalloc(1, KX_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL, *p1 = NULL, *p2 = NULL;
    static const u32 counts[2] = { 6144, 0 };
    char n1[BDEV_NAME_LEN], n2[BDEV_NAME_LEN], msg[160];
    const char *step = "register";
    struct stat st2;
    surf_task *te = NULL;
    int rm = 1, st, waited, r = -1;
    bool registered = false, still = false;
    if(!d) { check(false, "eject's write-out and a mount: an 8 MB RAM disk"); kfree(mem); return; }
    strlcpy(d->name, "stick7", sizeof(d->name));
    d->nblocks = KX_BLOCKS;
    msg[0] = 0;
    registered = bdev_register(d) == 0;
    if(registered && (step = "partition", kx_partition(d, counts, 2) == 0)) {
        bdev_part_name(d, 1, n1, sizeof(n1));
        bdev_part_name(d, 2, n2, sizeof(n2));
        p1 = bdev_find(n1);
        p2 = bdev_find(n2);
    }
    if(p1 && p2 && (step = "mkfs", fat_mkfs(p1, "PART1", 0, msg, sizeof(msg)) == 0 && fat_mkfs(p2, "PART2", 0, msg, sizeof(msg)) == 0) &&
       (step = "mount", vfs_mount_flags(p1, "/selftest-x1", "fat", 0) == 0)) {
        step = "a file";
        r = put_file("/selftest-x1/a.txt", "on the first partition");
    }
    if(r != 0) {
        printf("        (the setup failed at its step '%s'%s%s)\n", step, msg[0] ? ": " : "", msg);
        check(false, "eject's write-out and a mount: a RAM disk with two FAT partitions, the first mounted");
        if(p1 && vfs_stat("/selftest-x1", &st2) == 0) vfs_umount("/selftest-x1");
        if(registered) bdev_unregister(d);
        binvalidate(d);
        kfree(d);
        kfree(mem);
        return;
    }
    kx_disk = d;
    kx_go = kx_flushing = kx_tried = 0;
    kx_hold = 0;
    kx_result = -1;
    te = kthread_create_on("ejwrite", curTask->con, kx_run, NULL, PL_NORMAL, 0);
    kx_eject = te;
    ram_flush_hook = kx_flush;
    if(te) {
        kx_go = 1;
        for(waited = 0; !kx_flushing && waited < 5000; waited += 5) sleep_ms(5);   /* eject is writing /selftest-x1 out */
        rm = vfs_mount_flags(p2, "/selftest-x2", "fat", 0);
        kx_tried = 1;
        task_wait(te->pid, &st);
    }
    ram_flush_hook = NULL;
    kx_eject = NULL;
    still = rm == 0 && vfs_stat("/selftest-x2", &st2) == 0;
    printf("        (a mount of %s during eject's write-out: %d; eject %d%s; counted %u deep in its flush; the disk's "
           "counts after it: unmounting %u, mounting %u, requests %u)\n", n2, rm, kx_result,
           still ? ", the partition still mounted after it" : "", kx_hold, d->unmounting + p1->unmounting + p2->unmounting,
           d->mounting + p1->mounting + p2->mounting, d->requests + p1->requests + p2->requests);
    check(te && kx_flushing && rm == -EBUSY && kx_result == CMD_OK && !still,
          "eject's disk is busy from its first step: a mount of another partition during its write-out is refused");
    check(te && kx_flushing && kx_hold >= 2 && !d->unmounting && !p1->unmounting && !p2->unmounting && !p2->mounting,
          "and eject is counted inside while it holds the disk (a kill waits until it lets go), and lets go at its end");
    if(still) vfs_umount("/selftest-x2");
    if(vfs_stat("/selftest-x1", &st2) == 0) vfs_umount("/selftest-x1");
    bdev_unregister(d);                              /* (it and its partitions gone, off the list) */
    binvalidate(d);
    kfree(d);
    kfree(mem);
}

/* 10/18/26 (audit B59): kill leaves the kernel's own threads alone (TF_SYSTEM): killed at once outside any call, the USB
   hub thread or the SD card's watch left its driver's lock held, and every later request to that device waited for good,
   an umount's too. Here a thread of the test's own, marked so, holds a lock while `kill <pid>` is typed */
static mutex_t ks_lock = MUTEX_INIT("selftest-sys");
static volatile int ks_go, ks_done;

static void ks_thread(void *arg) {
    (void)arg;
    mutex_lock(&ks_lock);
    while(!ks_go) sleep_ms(5);
    mutex_unlock(&ks_lock);
    ks_done = 1;
}

static void test_kill_system(void) {
    char out[240], pidtxt[16];
    char *argv[] = { "kill", pidtxt, NULL };
    surf_task *t;
    u_long pid;
    int status = 1, r;
    ks_go = ks_done = 0;
    t = kthread_create_on("selftest-sys", curTask->con, ks_thread, NULL, PL_NORMAL, TF_SYSTEM);
    if(!t) { check(false, "kill of a system thread: a thread"); return; }
    pid = t->pid;
    sleep_ms(20);                                    /* (it holds its lock) */
    snprintf(pidtxt, sizeof(pidtxt), "%lu", pid);
    out[0] = 0;
    r = fx_capture(out, sizeof(out), 2, argv);
    sleep_ms(50);
    ks_go = 1;
    if(task_wait(pid, &status) != (int)pid) status = 1;
    if(out[0] && out[strlen(out) - 1] == '\n') out[strlen(out) - 1] = 0;
    printf("        (kill said: %s)\n", out[0] ? out + 4 : "nothing");
    check(r == CMD_FAIL && strstr(out, "not killed") && ks_done && status == 0,
          "kill refuses one of the kernel's own threads (it would leave its lock held): the thread lives on and lets go");
}

/* 10/18/26 (audit B52): fdisk killed while its commit writes, flushes and reads back (part_commit(), driver/part.c, holds
   its lock throughout): only each device request counted inside (B47), so a kill that waited 2 s could come at once
   between two of them and leave the lock held for good (every later fdisk refused, the rescan skipped). Now the commit
   counts itself inside while it holds the lock: its reads after the flush run counted twice (the commit, the request),
   and the kill comes after it. Here the stick's flush takes 3 s and the kill is sent 200 ms in. (Round fs-fix-9) Where
   the kill lands between two requests is a matter of microseconds, so whether the next fdisk was refused said little
   by itself (the code before let go in time here): the check is that every request the commit makes while it holds
   the lock (its writes, the flush, its reads back, the rescan's) runs counted twice, so the kill cannot come while the
   lock is held, whatever the timing, and then that the next fdisk is not refused */
static struct bdev *kf_dev;
static struct part_table *kf_t;
static struct part_plan kf_plan;
static volatile int kf_r, kf_done;
static char kf_msg[120];

static void kf_commit(void *arg) {
    (void)arg;
    ram_watch_task = curTask;                        /* (its every request watched, the first one too) */
    kf_r = part_commit(kf_dev, kf_t, &kf_plan, kf_msg, sizeof(kf_msg));
    kf_done = 1;
    task_call_leave(TC_SYSCALL);                     /* the kill that waited ends the task here */
}

static void test_fdisk_kill(void) {
    u8 *mem = (u8 *)kcalloc(1, UNPLUG_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    struct part_table *t2 = (struct part_table *)kalloc(sizeof(struct part_table));
    struct part_io io;
    char err[120], err2[120];
    surf_task *tk;
    u_long t0, ms = 0;
    int status = 1, r2 = 1;
    kf_t = (struct part_table *)kalloc(sizeof(struct part_table));
    kf_plan.buf = (u8 *)kalloc(PART_PLAN_BUF);
    err2[0] = 0;
    if(!d || !t2 || !kf_t || !kf_plan.buf) { check(false, "fdisk killed: memory"); goto out; }
    strlcpy(d->name, "stickf", sizeof(d->name));
    kf_dev = d;
    part_io_init(&io, d, false);
    io.note = NULL;
    if(part_read(&io, d->nblocks, kf_t) != 0 || part_plan_new(&io, d->nblocks, PART_SCHEME_MBR, &kf_plan, err, sizeof(err)) != 0) {
        check(false, "fdisk killed: a plan for a blank stick");
        goto out;
    }
    kf_done = 0;
    kf_r = 1;
    ram_watch_hold = ram_watch_min = ram_watch_n = 0;
    ram_flush_ms = 3000;
    tk = kthread_create_on("killfdisk", curTask->con, kf_commit, NULL, PL_NORMAL, TF_INCALL);
    if(tk) {
        u_long pid = tk->pid;
        ram_watch_task = tk;
        sleep_ms(200);
        t0 = uptime_ms();
        kill_task(tk);
        if(task_wait(pid, &status) != (int)pid) status = 1;
        ms = uptime_ms() - t0;
    }
    ram_flush_ms = 0;
    ram_watch_task = NULL;
    part_io_init(&io, d, false);
    io.note = NULL;
    if(part_read(&io, d->nblocks, t2) == 0) r2 = part_commit(d, t2, &kf_plan, err2, sizeof(err2));   /* the next fdisk */
    printf("        (it ended %lu ms after its kill; its reads after the flush ran counted %u deep, each of its %u requests at "
           "least %u; the next fdisk: %d%s%s)\n", ms, ram_watch_hold, ram_watch_n, ram_watch_min, r2, err2[0] ? ", " : "", err2);
    check(tk && status == -1 && mem[510] == 0x55 && mem[511] == 0xAA && ram_watch_hold >= 2,
          "fdisk killed in its commit: the commit is counted inside throughout and finishes, the table written");
    check(tk && ram_watch_n > 0 && ram_watch_min >= 2 && r2 != 1 && !strstr(err2, "another fdisk"),
          "and it holds its lock only counted inside (each of its requests counted twice): the kill comes after it lets go, "
          "and the next fdisk is not refused");
out:
    if(d) binvalidate(d);
    kfree(d);
    kfree(mem);
    kfree(t2);
    kfree(kf_t);
    kfree(kf_plan.buf);
}

/* 10/18/26 (audit B53): cp and fsbench when Ctrl+C reaches them in a write: it stops after its first piece (B39), and cp
   wrote once more to learn why and said "short write", its full-disk message; fsbench said "I/O error". cp's source is
   on the initrd, read in one piece whatever comes: only the write is cut */
static volatile int kp_go, kp_r1, kp_r2;
static char kp_out1[400], kp_out2[400];

static void kp_cmds(void *arg) {
    char *cp_argv[] = { "cp", "/initrd/bin/hello", "/selftest-cp/hello", NULL };
    char *fb_argv[] = { "fsbench", "/selftest-cp", "1", NULL };
    (void)arg;
    task_call_enter(TC_COMMAND);
    while(!kp_go) sleep_ms(5);                       /* Ctrl+C is sent meanwhile: still there for both */
    kp_r1 = fx_capture(kp_out1, sizeof(kp_out1), 3, cp_argv);
    kp_r2 = fx_capture(kp_out2, sizeof(kp_out2), 3, fb_argv);
    task_call_leave(TC_COMMAND);
}

static void test_cp_interrupted(void) {
    u8 *mem;
    struct bdev *d;
    struct stat st;
    surf_task *t;
    u_long f;
    int status;
    if(vfs_stat("/initrd/bin/hello", &st) != 0 || st.size < 4096) { printf("  skip cp interrupted (no /initrd/bin/hello of 4 KB)\n"); return; }
    d = fx_stick("stickc", "/selftest-cp", &mem);
    if(!d) { check(false, "cp interrupted: a RAM stick"); return; }
    kp_go = 0;
    kp_out1[0] = kp_out2[0] = 0;
    t = kthread_create_on("cpint", curTask->con, kp_cmds, NULL, PL_NORMAL, 0);
    if(t) {
        sleep_ms(20);
        f = irq_save();
        task_interrupt_job(t);
        irq_restore(f);
        kp_go = 1;
        task_wait(t->pid, &status);
    }
    check(t && strstr(kp_out1, "cut short after") && strstr(kp_out1, "is incomplete") && !strstr(kp_out1, "short write"),
          "cp interrupted in a write says it was cut short and the copy is incomplete (not \"short write\")");
    check(t && strstr(kp_out2, "fsbench: interrupted") && !strstr(kp_out2, "I/O error"),
          "fsbench interrupted in a write says so (not \"I/O error\")");
    if(!(t && strstr(kp_out1, "is incomplete"))) printf("        (cp said: %s)\n", kp_out1);
    if(!(t && strstr(kp_out2, "fsbench: interrupted"))) printf("        (fsbench said: %s)\n", kp_out2);
    fx_done(d, mem, "/selftest-cp");
}

/* 10/18/26 (audit B55): a kernel thread outside any call, inside a file system, when its terminal hangs up: its kill comes
   as soon as it leaves the file system (B39, B48); one that enters a call first keeps the hangup's rule there: no
   escalation, the call's end awaited. It got a kill's 2 s and was killed at once in the call. No kernel thread is on a
   pty today; here one is (TF_INCALL, on a pty this test opens and hangs up), outside its call inside a file system */
static volatile int kh_ready, kh_hung, kh_done;

static void kh_thread(void *arg) {
    (void)arg;
    fs_enter();                                      /* inside a file system ... */
    task_call_leave(TC_SYSCALL);                     /* ... outside any call */
    kh_ready = 1;
    while(!kh_hung) sleep_ms(5);
    task_call_enter(TC_COMMAND);                     /* a kernel command begins, still inside */
    fs_leave();
    sleep_ms(2500);                                  /* longer than a kill's KILL_WAIT_MS */
    kh_done = 1;
    task_call_leave(TC_COMMAND);                     /* the hangup's kill comes here */
}

static void test_hangup_nocall(void) {
    surf_console *con;
    surf_task *t;
    u_long pid, t0, ms;
    int fd, status = 1, waited;
    fd = sys_openpty();
    con = fd >= 0 ? pty_console_of(fd) : NULL;
    if(!con) { if(fd >= 0) fd_close(fd); printf("  skip a hangup outside a call (no pty)\n"); return; }
    kh_ready = kh_hung = kh_done = 0;
    t = kthread_create_on("hupnocall", con, kh_thread, NULL, PL_NORMAL, TF_INCALL);
    if(!t) { fd_close(fd); check(false, "a hangup outside a call: a task on the pty"); return; }
    pid = t->pid;
    for(waited = 0; !kh_ready && waited < 2000; waited += 5) sleep_ms(5);
    fd_close(fd);                                    /* the master closed: the hangup (task_kill_tty()) */
    kh_hung = 1;
    t0 = uptime_ms();
    if(task_wait(pid, &status) != (int)pid) status = 1;
    ms = uptime_ms() - t0;
    printf("        (it ended %lu ms after the hangup, %s)\n", ms, kh_done ? "its call done" : "killed in its call");
    check(status == -1 && kh_done, "a kernel thread hung up outside a call that enters one keeps the hangup's rule: its call's end awaited");
}

/**** round fs-fix-10 (audit B61-B65): the tenth review ****/

/* 10/18/26 (audit B61): another task's raw request of a partition when fdisk's commit reads the disk again at its end
   (the review's R1): the rescan asked whether the disk was busy, a request under way included (B56), so with another
   task's request in flight it refused after the new table was on the disk ("could not be read again (-16)"), the old
   partitions left registered and the new one never registered: a later mkfs formatted the old extent. Now the commit
   holds the disk from its check to its end (vfs_bdev_change_begin()) and another task's request to it or to a partition
   of it is refused at once meanwhile (BDEV_EBUSY), so the commit meets none, its rescan included. Here the commit adds a
   second partition, and its flush starts another task's raw write of the first partition's last block (held in the
   driver, should it get there), which first asks whether the partition is busy */
static surf_task *volatile kr_fdisk;
static struct bdev *volatile kr_p1;
static volatile int kr_go, kr_done, kr_armed, kr_kicked, kr_wr;
static const char *volatile kr_busy;

static void kr_writer(void *arg) {
    static u8 blk[512];
    (void)arg;
    while(!kr_go) sleep_ms(5);
    kr_busy = vfs_bdev_busy_why(kr_p1);
    memset(blk, 0x5a, sizeof(blk));
    kr_wr = bdev_write(kr_p1, kr_p1->nblocks - 1, 1, blk);
    kr_done = 1;
}

static void kr_flush(void) {
    int waited;
    if(!kr_armed || curTask != kr_fdisk || kr_kicked) return;
    kr_kicked = 1;
    kr_go = 1;                                       /* the other task's raw write begins now */
    for(waited = 0; !ram_stuck_hit && !kr_done && waited < 2000; waited += 5) sleep_ms(5);
}

static void test_change_rescan(void) {
    u8 *mem = (u8 *)kcalloc(1, KX_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL, *p2;
    static const u32 counts[1] = { 6144 };
    struct part_table *t = (struct part_table *)kalloc(sizeof(struct part_table));
    struct part_plan plan;
    struct part_io io;
    struct part_kind k;
    char err[256], n1[BDEV_NAME_LEN], n2[BDEV_NAME_LEN];
    surf_task *tw = NULL;
    int r = -9, st, entries = -1;
    bool registered = false, stuck;
    plan.buf = (u8 *)kalloc(PART_PLAN_BUF);
    err[0] = 0;
    if(!d || !t || !plan.buf) { check(false, "fdisk's commit and another task's write: memory"); goto out; }
    strlcpy(d->name, "stickR", sizeof(d->name));
    d->nblocks = KX_BLOCKS;
    registered = bdev_register(d) == 0;
    if(!registered || kx_partition(d, counts, 1) != 0) {
        check(false, "fdisk's commit and another task's write: a partitioned RAM disk");
        goto out;
    }
    bdev_part_name(d, 1, n1, sizeof(n1));
    bdev_part_name(d, 2, n2, sizeof(n2));
    kr_p1 = bdev_find(n1);
    if(!kr_p1) { check(false, "fdisk's commit and another task's write: the first partition"); goto out; }
    kr_go = kr_done = kr_kicked = 0;
    kr_wr = 1;
    kr_busy = NULL;
    ram_stuck_hit = 0;
    ram_stuck_lba = 0;
    tw = kthread_create_on("crwrite", curTask->con, kr_writer, NULL, PL_NORMAL, 0);
    ram_stuck_task = tw;
    kr_fdisk = curTask;
    part_io_init(&io, d, false);
    io.note = NULL;
    if(tw && part_parse_type("fat16", &k) == 0 && part_read(&io, d->nblocks, t) == 0 &&
       part_plan_add(&io, t, 0, &k, &plan, err, sizeof(err)) == 0) {
        kr_armed = 1;
        ram_flush_hook = kr_flush;
        r = part_commit(d, t, &plan, err, sizeof(err));   /* fdisk's commit: a second partition, the rest of the disk */
        ram_flush_hook = NULL;
        kr_armed = 0;
    }
    stuck = ram_stuck_hit != 0;                      /* (the other task's write reached the driver) */
    ram_stuck_task = NULL;                           /* (and ends now) */
    kr_go = 1;
    if(tw) task_wait(tw->pid, &st);
    p2 = bdev_find(n2);
    if(part_read(&io, d->nblocks, t) == 0) entries = (int)t->nparts;
    printf("        (the commit: %d%s%s; the other task's write: %d%s, %s busy: %s; %s registered: %s; the table on the "
           "disk: %d partitions)\n", r, err[0] ? ", " : "", err, kr_wr, stuck ? ", held in the driver" : "", n1,
           kr_busy ? kr_busy : "no", n2, p2 ? "yes" : "NO", entries);
    check(tw && r == 0 && p2 && entries == 2,
          "fdisk's commit while another task writes to a partition: the disk is read again, the new partition registered");
    check(tw && kr_kicked && kr_wr == BDEV_EBUSY && !stuck && kr_busy && !strcmp(kr_busy, VFS_BUSY_CHANGING),
          "and that task's write is refused at once while the commit holds the disk (BDEV_EBUSY), the partition said to "
          "be being changed");
out:
    if(registered) {
        bdev_unregister(d);                          /* (it and its partitions gone, off the list) */
        binvalidate(d);
    }
    kfree(plan.buf);
    kfree(t);
    kfree(d);
    kfree(mem);
}

/* 10/18/26 (audit B61): the claim of mkfs and fdisk is one look and claim under the mount table's lock, the look at the
   requests under way and the claim in one step no request comes between: a request under way refuses it and leaves
   nothing counted (B56's rule); held, it refuses another task's requests to the device (BDEV_EBUSY) and its mount at
   once, and lets the claiming task's own requests through; let go of, the others are let in again */
static struct bdev *volatile kv_dev;
static volatile int kv_go, kv_read, kv_mount, kv_done;
static const char *volatile kv_busy;

static void kv_other(void *arg) {
    u8 *blk = (u8 *)kalloc(512);
    (void)arg;
    while(!kv_go) sleep_ms(5);
    kv_busy = vfs_bdev_busy_why(kv_dev);
    kv_read = blk ? bdev_read(kv_dev, 0, 1, blk) : -99;
    kv_mount = vfs_mount_flags(kv_dev, "/selftest-cv", "fat", MNT_RDONLY);
    if(kv_mount == 0) vfs_umount("/selftest-cv");
    kfree(blk);
    kv_done = 1;
}

/* kv_other in a task of its own, to its end */
static void kv_run(void) {
    surf_task *o;
    int st;
    kv_go = kv_done = 0;
    kv_read = kv_mount = 1;
    kv_busy = NULL;
    o = kthread_create_on("cvother", curTask->con, kv_other, NULL, PL_NORMAL, 0);
    if(!o) return;
    kv_go = 1;
    task_wait(o->pid, &st);
}

static void test_change_claim(void) {
    u8 *mem, *blk = (u8 *)kalloc(512);
    struct bdev *d = fx_stick("stickv", "/selftest-cv", &mem);
    const char *why = NULL, *held_busy;
    surf_task *t;
    int r1 = 0, r2, own, held_read, held_mount, held_done, st, waited;
    u_int hold = curTask->fs_hold, left;
    if(!d || !blk) {
        check(false, "the claim of mkfs and fdisk: a RAM stick");
        kfree(blk);
        if(d) fx_done(d, mem, "/selftest-cv");
        return;
    }
    vfs_umount("/selftest-cv");                      /* nothing mounts it now */
    kq_dev = d;
    kq_done = 0;
    kq_r = 1;
    ram_stuck_hit = 0;
    t = kthread_create_on("cvwrite", curTask->con, kq_write, NULL, PL_NORMAL, 0);
    if(t) {
        ram_stuck_task = t;
        for(waited = 0; !ram_stuck_hit && waited < 2000; waited += 5) sleep_ms(5);
        r1 = vfs_bdev_change_begin(d, &why);         /* another task's write is under way */
        if(r1 == 0) vfs_bdev_change_end(d);          /* (the code before: taken) */
        ram_stuck_task = NULL;                       /* the write ends */
        task_wait(t->pid, &st);
    }
    left = curTask->fs_hold - hold;
    r2 = vfs_bdev_change_begin(d, NULL);             /* nothing under way: taken */
    kv_dev = d;
    kv_run();                                        /* another task, while it is held */
    held_read = kv_read;
    held_mount = kv_mount;
    held_busy = kv_busy;
    held_done = kv_done;
    own = bdev_read(d, 0, 1, blk);                   /* its own request */
    if(r2 == 0) vfs_bdev_change_end(d);
    kv_run();                                        /* and once it is let go of */
    printf("        (with a write under way: %d, %s; held: another task's read %d, mount %d, busy: %s; its own read %d; "
           "let go of: read %d, mount %d)\n", r1, why ? why : "-", held_read, held_mount, held_busy ? held_busy : "no",
           own, kv_read, kv_mount);
    check(t && ram_stuck_hit && kq_r == 0 && r1 == -EBUSY && why && !strcmp(why, VFS_BUSY_REQUEST) && left == 0,
          "mkfs's and fdisk's claim of a device with a request under way is refused, saying why, and leaves nothing counted");
    check(r2 == 0 && held_done && held_read == BDEV_EBUSY && held_mount == -EBUSY && held_busy &&
          !strcmp(held_busy, VFS_BUSY_CHANGING) && own == 0,
          "held, it refuses another task's requests and mount at once, saying why, and lets its own requests through");
    check(kv_done && kv_read == 0 && kv_mount == 0 && curTask->fs_hold == hold, "let go of, the others are let in");
    kfree(blk);
    fx_done(d, mem, NULL);
}

/* 10/18/26 (audit B61): mkfs of a whole disk reads it again at its end (its old partitions go), and passed over the
   rescan's result: when it failed, mkfs said nothing and returned 0. Now it says the disk could not be read again and
   what to do, and fails. Here the disk's block 0 cannot be read once mkfs has flushed, and the rescan reads it; the
   answer to mkfs's question is typed ahead */
static surf_task *volatile km_task;

static void km_flush(void) {
    if(km_task && curTask == km_task) ram_read_fail0 = 1;
}

static void test_mkfs_rescan(void) {
    u8 *mem = (u8 *)kcalloc(1, KX_BLOCKS * 512);
    struct bdev *d = mem ? ram_disk(mem) : NULL;
    static const u32 counts[1] = { 0 };
    static char out[800];
    char *argv[] = { "mkfs", "stickM", NULL }, n1[BDEV_NAME_LEN];
    struct tty *tty = curTask->tty ? curTask->tty : tty_active;
    const char *answer = "stickM\n", *c, *said;
    bool registered = false, before = false;
    int r = -1;
    out[0] = 0;
    if(!d) { check(false, "mkfs of a whole disk that cannot be read again: memory"); kfree(mem); return; }
    strlcpy(d->name, "stickM", sizeof(d->name));
    d->nblocks = KX_BLOCKS;
    registered = bdev_register(d) == 0;
    if(registered && kx_partition(d, counts, 1) == 0) {
        bdev_part_name(d, 1, n1, sizeof(n1));
        before = bdev_find(n1) != NULL;
    }
    if(!before || !tty) {
        check(false, "mkfs of a whole disk that cannot be read again: a partitioned RAM disk");
        goto out;
    }
    tty_flush(tty);
    for(c = answer; *c; c++) tty_input(tty, (u_char)*c);   /* the answer to its question, typed ahead */
    km_task = curTask;
    ram_flush_hook = km_flush;
    r = fx_capture(out, sizeof(out), 2, argv);
    ram_flush_hook = NULL;
    km_task = NULL;
    ram_read_fail0 = 0;
    tty_flush(tty);
    said = strstr(out, "read again");
    if(said) { while(said > out && said[-1] != '\n') said--; }
    printf("        (mkfs returned %d: %.*s)\n", r, said ? (int)(strchr(said, '\n') ? strchr(said, '\n') - said : 200) : 7,
           said ? said : "nothing");
    check(r == CMD_FAIL && strstr(out, "could not be read again") && strstr(out, "plug it in again") &&
          !strstr(out, "was read again"), "mkfs of a whole disk that cannot be read again at its end says so and what to "
          "do, and fails");
out:
    if(registered) {
        bdev_unregister(d);
        binvalidate(d);
    }
    kfree(d);
    kfree(mem);
}

/* 10/18/26 (audit B62): a task holding a mutex is not killed at once, inside a file system or not: killed at once, it
   left the lock with the dead task, and every task that wanted the lock waited for good (a driver's thread holds its
   device's lock in its probes and polls outside any request, as the USB hub thread and the SD card's watch do: every
   later request to the device). Here a kernel thread outside any call holds a lock of the test's own for 300 ms when it
   is killed: the kill comes once it lets go, and the lock is free after */
static mutex_t kl_lock = MUTEX_INIT("selftest-kill");
static volatile int kl_held, kl_let_go;
static volatile u_long kl_out;

static void kl_thread(void *arg) {
    (void)arg;
    mutex_lock(&kl_lock);
    kl_held = 1;
    sleep_ms(300);                                   /* the kill comes meanwhile: put off */
    kl_let_go = 1;
    kl_out = uptime_ms();
    mutex_unlock(&kl_lock);
    sleep_ms(2000);                                  /* killed long before this ends */
    kl_held = 2;
}

static void test_kill_holding_lock(void) {
    surf_task *t;
    u_long pid, end;
    int status = 0, waited;
    bool free_after;
    kl_held = kl_let_go = 0;
    kl_out = 0;
    t = kthread_create_on("killlock", curTask->con, kl_thread, NULL, PL_NORMAL, 0);
    if(!t) { check(false, "a kill of a task holding a lock: a thread"); return; }
    pid = t->pid;
    for(waited = 0; !kl_held && waited < 1000; waited += 5) sleep_ms(5);
    kill_task(t);
    if(task_wait(pid, &status) != (int)pid) status = 1;
    end = uptime_ms();
    free_after = mutex_trylock(&kl_lock);
    if(free_after) mutex_unlock(&kl_lock);
    printf("        (%s; it ended %lu ms after)\n", kl_let_go ? "it let go of its lock" : "killed holding its lock",
           kl_let_go ? end - kl_out : 0UL);
    check(status == -1 && kl_let_go && kl_held == 1 && free_after && end - kl_out < 250,
          "a task holding a lock is not killed at once: the kill comes once it lets go, and the lock is free after");
}

/* 10/18/26 (audit B63): a mutex goes to its waiters in the order they came. mutex_unlock() only woke the first waiter,
   and the task letting go could take the lock again before the waiter ran: a task taking a lock again and again kept a
   waiter out for as long as it went on, so "each waits for those ahead of it" (B56) was no bound. Here the test holds a
   lock of its own while another task queues for it, then lets go and takes it again 50 times, never yielding in
   between, holding it 2 ms each time: the waiter has it at the first letting go */
static mutex_t ko_lock = MUTEX_INIT("selftest-fifo");
static volatile int ko_round, ko_got;

static void ko_waiter(void *arg) {
    (void)arg;
    mutex_lock(&ko_lock);
    ko_got = ko_round;
    mutex_unlock(&ko_lock);
}

static void test_mutex_fifo(void) {
    surf_task *t;
    u_long until;
    int st, waited;
    ko_round = 0;
    ko_got = -1;
    mutex_lock(&ko_lock);
    t = kthread_create_on("fifowait", curTask->con, ko_waiter, NULL, PL_NORMAL, 0);
    if(!t) { mutex_unlock(&ko_lock); check(false, "a mutex's waiters in order: a thread"); return; }
    for(waited = 0; !wq_count(&ko_lock.waiters) && waited < 1000; waited += 5) sleep_ms(5);   /* (it waits for the lock) */
    for(ko_round = 1; ko_round <= 50; ko_round++) {
        mutex_unlock(&ko_lock);
        mutex_lock(&ko_lock);                        /* again at once */
        for(until = uptime_ms() + 2; uptime_ms() < until; );
    }
    mutex_unlock(&ko_lock);
    task_wait(t->pid, &st);
    printf("        (the waiter had the lock at letting go number %d of 50)\n", ko_got);
    check(ko_got == 1, "a mutex goes to its waiters in the order they came: a task taking it again and again keeps no "
          "waiter out");
}

/* 10/18/26 (audit B65): a close whose write-out Ctrl+C cut short said EIO, as one whose write the device refused: cp
   then said "interrupted while they were written out" when the card had refused them, and fsbench said "interrupted".
   vfs_close() says EINTR now when the write-out was cut short with nothing refused and the device still there (what is
   left is owed still: the next sync writes it), and EIO when the device refused a write or went away, Ctrl+C or not.
   Here a file the stick refused at first is closed while another task's write of the root directory, which the close
   owes, is held, and Ctrl+C reaches the close there; then a file the stick refuses is closed with Ctrl+C pending */
static struct bdev *kz_dev;
static volatile int kz_go, kz_ready, kz_go2, kz_close, kz_done;

static void kz_sync(void *arg) {
    (void)arg;
    while(!kz_go) sleep_ms(5);
    bsync(kz_dev);
}

static void kz_closer(void *arg) {
    struct file *f = NULL;
    task_call_enter(TC_COMMAND);
    kz_close = 1;
    if(vfs_open((const char *)arg, O_WRONLY | O_CREAT, &f) == 0) {
        vfs_write(f, "closed after Ctrl+C", 19); /* (its write-out refused: ram_refuse) */
        kz_ready = 1;
        while(!kz_go2) sleep_ms(5);
        kz_close = vfs_close(f);
    }
    kz_done = 1;
    task_call_leave(TC_COMMAND);
}

static surf_task *kz_start(const char *name, const char *path) {
    kz_ready = kz_go2 = kz_done = 0;
    kz_close = 1;
    return kthread_create_on(name, curTask->con, kz_closer, (void *)path, PL_NORMAL, 0);
}

static void test_close_cut_short(void) {
    u8 *mem;
    struct bdev *d = fx_stick("stickz", "/selftest-cz", &mem);
    surf_task *tc, *ts = NULL;
    u_long f;
    int st, waited, cut = 1, refused = 1;
    bool early = false;
    if(!d) { check(false, "a close cut short: a RAM stick"); return; }
    kz_dev = d;
    kz_go = 0;
    ram_stuck_hit = 0;
    ram_refuse = true;                               /* the file's blocks stay changed in the cache */
    tc = kz_start("czclose", "/selftest-cz/c.txt");
    for(waited = 0; tc && !kz_ready && !kz_done && waited < 2000; waited += 5) sleep_ms(5);
    ram_refuse = false;
    if(tc && kz_ready) {
        ts = kthread_create_on("czsync", curTask->con, kz_sync, NULL, PL_NORMAL, 0);
        ram_stuck_lba = (mem[14] | mem[15] << 8) + mem[16] * (mem[22] | mem[23] << 8);   /* the root directory */
        ram_stuck_task = ts;                         /* another task's write of it is held */
        kz_go = 1;
        for(waited = 0; ts && !ram_stuck_hit && waited < 2000; waited += 5) sleep_ms(5);
        kz_go2 = 1;                                  /* the close waits for that block */
        sleep_ms(50);
        f = irq_save();
        task_interrupt_job(tc);                      /* Ctrl+C on its terminal */
        irq_restore(f);
        for(waited = 0; !kz_done && waited < 1500; waited += 10) sleep_ms(10);
        early = kz_done != 0;                        /* (it returned while the write was still held) */
    }
    ram_stuck_task = NULL;                           /* the write ends */
    kz_go = kz_go2 = 1;
    if(tc) task_wait(tc->pid, &st);
    if(ts) task_wait(ts->pid, &st);
    ram_stuck_lba = 0;
    cut = kz_close;
    ram_refuse = true;                               /* then a close the stick refuses, Ctrl+C pending */
    tc = kz_start("czrefuse", "/selftest-cz/d.txt");
    for(waited = 0; tc && !kz_ready && !kz_done && waited < 2000; waited += 5) sleep_ms(5);
    if(tc) {
        f = irq_save();
        task_interrupt_job(tc);
        irq_restore(f);
        kz_go2 = 1;
        task_wait(tc->pid, &st);
    }
    ram_refuse = false;
    refused = kz_close;
    printf("        (cut short in its wait%s: %d; refused with Ctrl+C pending: %d)\n", early ? "" : " (NOT before the held "
           "write ended)", cut, refused);
    check(ram_stuck_hit && early && cut == -EINTR, "a close whose write-out Ctrl+C cut short says EINTR (the next sync "
          "writes the rest), not EIO");
    check(refused == -EIO, "and one the device refused says EIO, Ctrl+C or not");
    fx_done(d, mem, "/selftest-cz");                 /* (its umount writes the rest) */
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
    test_fat_utf8, test_fat_concurrent, test_fat_unplug, test_fat_refused, test_kill_in_cache, test_kill_umount, test_eject_interrupted,
    test_kill_device, test_kill_outside_call, test_umount_waits, test_request_busy, test_unmounting_busy, test_eject_unmount_phase,
    test_eject_writeout_mount, test_kill_system,
    test_fdisk_kill, test_cp_interrupted, test_hangup_nocall,
    test_change_rescan, test_change_claim, test_mkfs_rescan, test_kill_holding_lock, test_mutex_fifo, test_close_cut_short,
    test_user, test_net, test_usb, test_tasks,
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
