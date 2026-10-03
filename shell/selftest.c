/*
SurfOS kernel self tests
(C)2004 Brandon Burr, written 10/2026 (roadmap T1/U1)

`selftest` in the shell; `make test` runs it over the serial console and looks for
SELFTEST PASS. Every test prints one line; the summary prints the verdict.
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
#include <fs/vfs.h>
#include <surfos/process.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <net/tcp.h>
#include <surfos/multiboot.h>
#include <mm/kalloc.h>
#include "shell.h"

static int fails;

static void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok) fails++;
}

/**** heap ****/

/* Random allocate/free with a byte pattern per block, then everything freed and the
   heap walked and compared with the starting point. */
int heaptest() {
    enum { SLOTS = 192, ROUNDS = 20000 };
    static void *ptrs[SLOTS];
    static u_int sizes[SLOTS];
    struct heap_stats before, after;
    u_int i, r, allocs = 0, frees = 0, maxlive = 0, live = 0, bad = 0;

    memset(ptrs, 0, sizeof(ptrs));
    kheap_stats(&before);
    srand(getticks() + 7);
    for(r = 0; r < ROUNDS; r++) {
        i = rand() % SLOTS;
        if(ptrs[i]) {
            u_char *p = ptrs[i];
            u_int k;
            for(k = 0; k < sizes[i]; k++) if(p[k] != (u_char)(i + k)) { bad++; break; }
            kfree(ptrs[i]);
            ptrs[i] = NULL;
            frees++; live--;
        } else {
            u_int sz = (rand() % 16 == 0) ? 1 + rand() % 20000 : 1 + rand() % 512;
            u_char *p = kalloc(sz);
            u_int k;
            if(!p) { bad++; continue; }
            for(k = 0; k < sz; k++) p[k] = (u_char)(i + k);
            ptrs[i] = p; sizes[i] = sz;
            allocs++; live++;
            if(live > maxlive) maxlive = live;
        }
    }
    for(i = 0; i < SLOTS; i++) if(ptrs[i]) { kfree(ptrs[i]); ptrs[i] = NULL; frees++; }
    kheap_stats(&after);
    if(!kheap_check()) bad++;
    if(after.bytes_used != before.bytes_used || after.blocks_used != before.blocks_used) bad++;
    printf("heaptest: %u allocs, %u frees, %u live at peak, used %lu -> %lu bytes, %lu free blocks: %s\n",
           allocs, frees, maxlive, before.bytes_used, after.bytes_used, after.blocks_free, bad ? "HEAPTEST FAIL" : "HEAPTEST PASS");
    return bad ? 1 : 0;
}

/**** tasks ****/

static void exit_with_arg(void *arg) {
    task_exit((int)(u_long)arg);
}

static void test_tasks(void) {
    u_int before = task_count(), i, collected = 0;
    int status = 0;
    surf_task *t;

    for(i = 0; i < 64; i++) {
        t = kthread_create("churn", exit_with_arg, (void *)(u_long)i, PL_NORMAL, 0);
        if(!t) break;
    }
    check(i == 64, "64 kthreads created");
    for(i = 0; i < 64; i++) {
        if(task_wait(WAIT_ANY, &status) < 0) break;
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
    check(task_wait(WAIT_ANY, &status) == -1, "task_wait with no children returns -1");
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
    kfree(a);
    kfree(b);
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
    check(vfs_stat("/initrd/etc/version", &st) == 0 && st.type == VN_FILE && st.size == 22, "stat /initrd/etc/version: 22-byte file");
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

static void test_net(void) {
    struct netdev *dev = netdev_default();
    const struct arp_entry *e;
    int i, rtt = -1, ok = 0;
    char a[16];

    if(!dev) { printf("  skip network (no interface)\n"); return; }
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
    check(rtt == 0 && dev->ip == IP4(10, 0, 2, 15) && dev->gateway == IP4(10, 0, 2, 2) && dev->dns == IP4(10, 0, 2, 3),
          "DHCP lease: 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3");
    check(icmp_ping(IP4(10, 0, 2, 2), 0x5f60, 1, 56, 2000, NULL) >= 0, "ping works on the leased address");
    test_tcp();
    printf("        (%s: %lu frames in, %lu out)\n", ipfmt(dev->ip, a), dev->rx_packets, dev->tx_packets);
}

int run_selftest(void) {
    fails = 0;
    printf("\nSurfOS self test\n----------------\n");
    check(heaptest() == 0, "heap");
    test_bdev();
    test_fs();
    test_fat();
    test_user();
    test_net();
    test_tasks();
    test_sleep();
    test_timers();
    test_mutex_fn();
    test_semaphore();
    test_event_fn();
    test_libc();
    printf("%s (%s)\n\n", fails ? "SELFTEST FAIL" : "SELFTEST PASS", fails ? "see above" : "all checks passed");
    return fails;
}
