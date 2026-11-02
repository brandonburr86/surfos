/* Host test of how far a check makes the kernel heap grow (11/02/26, the third review of track fsck): fs/fat_check.c over
   mm/kalloc.c as it is, its heap in a static array of this test (tests/host/kalloc_host.h, as tests/host/test_kalloc.c
   has it). The heap keeps what it once grew to, and under first fit a table that doubled could not take the copies it
   had left behind: with 32769 to 65536 names in the root of the owner's 118 GB stick, a look grew the heap by 4548 KB,
   past the 4 MB it was to take. A look now takes all it takes at its start, the table of names once at the size the
   volume can need (fs/fat_check.c, names_size()), so the cap bounds the growth. The volumes are computed on each read,
   so that 118 GB cost nothing to hold: FAT32, its root a chain of as many 32 KB clusters as its short names need.
   Each look runs in a child process of this test, on a heap new to it as the kernel's is at boot (heap_init() is for
   once: its counts would not start again). Made from the third review's own probe (its heaphost.c and glue.c). Built
   and run by `make unittest`. */
#include "kalloc_host.h"
#include <surfos/types.h>
#include <mm/kalloc.h>
#include <fs/fat_check.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

int vprintf(const char *fmt, va_list ap);
void exit(int code);
int fork(void);
int waitpid(int pid, int *status, int options);
int pipe(int fd[2]);
long read(int fd, void *buf, unsigned long n);
long write(int fd, const void *buf, unsigned long n);
int close(int fd);
void _exit(int code);

#define CAP (4ul << 20)                         /* CHECK_MEM_MOST in fs/fat_check.c */

int sim_if = 1;                                 /* tests/host/pager_shim's interrupt flag */
unsigned char kalloc_test_kheap[KALLOC_TEST_KHEAP] __attribute__((aligned(4096)));
static unsigned char dma_arena[PAGE_SIZE] __attribute__((aligned(4096)));
u_long arena_page_tables, arena_frame_stack, arena_frame_stack_bytes, arena_dma_heap;

int pmm_commit(u_long n, int cls) { (void)n; (void)cls; return 0; }
void get_time(time_t *t) { memset(t, 0, sizeof(*t)); }

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("PANIC: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(2);
}

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/**** the volume, computed ****/

static u32 v_blocks, v_spc, v_rsv = 32, v_spf, v_n, v_names, v_rootc, v_data;
static int v_probe, v_in_fat;                   /* stop the look at its first read of the FAT: what it took is all that counts */
static u8 v_boot[512], v_fsinfo[512];
static char said[2048];
static u32 said_n;

static void w16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void w32(u8 *p, u32 v) { w16(p, v & 0xFFFF); w16(p + 2, v >> 16); }

static u32 fatv(u32 c) {
    if(c == 0) return 0x0FFFFFF8;
    if(c == 1) return 0x0FFFFFFF;
    if(c >= 2 && c < 2 + v_rootc) return c == 1 + v_rootc ? 0x0FFFFFFF : c + 1;
    return 0;
}

static int v_read(void *ctx, u32 lba, u32 count, void *buf) {
    u32 i, k;
    (void)ctx;
    if(lba + count > v_blocks) return -1;
    memset(buf, 0, (size_t)count * 512);
    for(i = 0; i < count; i++) {
        u32 at = lba + i;
        u8 *o = (u8 *)buf + (size_t)i * 512;
        if(at == 0 || at == 6) memcpy(o, v_boot, 512);
        else if(at == 1) memcpy(o, v_fsinfo, 512);
        else if(at >= v_rsv && at < v_rsv + 2 * v_spf) {
            u32 s = (at - v_rsv) % v_spf;
            v_in_fat = 1;
            for(k = 0; k < 128; k++) if(s * 128 + k < v_n + 2) w32(o + k * 4, fatv(s * 128 + k));
        } else if(at >= v_data && at < v_data + v_rootc * v_spc) {
            for(k = 0; k < 16; k++) {
                u32 slot = (at - v_data) * 16 + k;
                if(slot >= v_names) break;
                snprintf((char *)o + k * 32, 12, "N%07uTXT", slot);
                o[k * 32 + 11] = 0x20;
            }
        }
    }
    return 0;
}

static bool v_stop(void *ctx) { (void)ctx; return v_probe && v_in_fat; }

static void v_say(void *ctx, const char *line) {
    u32 n = strlen(line);
    (void)ctx;
    if(said_n + n + 2 < sizeof(said)) {
        memcpy(said + said_n, line, n);
        said_n += n;
        said[said_n++] = '\n';
        said[said_n] = 0;
    }
}

/* FAT32 of `blocks` blocks of `spc` sectors a cluster, `names` short names in its root */
static void volume(u32 blocks, u32 spc, u32 names) {
    u32 n, need;
    v_blocks = blocks;
    v_spc = spc;
    v_names = names;
    v_rootc = names ? (names * 32 + spc * 512 - 1) / (spc * 512) : 1;
    for(v_spf = 1; ; v_spf = need) {                /* the FAT grows until it covers what is left: it never comes back */
        n = (blocks - v_rsv - 2 * v_spf) / spc;
        need = ((n + 2) * 4 + 511) / 512;
        if(need <= v_spf) break;
    }
    v_n = n;
    v_data = v_rsv + 2 * v_spf;
    memset(v_boot, 0, sizeof(v_boot));
    v_boot[0] = 0xEB; v_boot[1] = 0x58; v_boot[2] = 0x90;
    w16(v_boot + 11, 512);
    v_boot[13] = (u8)spc;
    w16(v_boot + 14, v_rsv);
    v_boot[16] = 2;
    v_boot[21] = 0xF8;
    w32(v_boot + 32, blocks);
    w32(v_boot + 36, v_spf);
    w32(v_boot + 44, 2);
    w16(v_boot + 48, 1);
    w16(v_boot + 50, 6);
    v_boot[510] = 0x55; v_boot[511] = 0xAA;
    memset(v_fsinfo, 0, sizeof(v_fsinfo));
    w32(v_fsinfo, 0x41615252);
    w32(v_fsinfo + 484, 0x61417272);
    w32(v_fsinfo + 488, 0xFFFFFFFF);
    w32(v_fsinfo + 492, 0xFFFFFFFF);
    w32(v_fsinfo + 508, 0xAA550000);
}

/**** a look, and the heap around it ****/

struct heapseen {
    int status, stopped;
    unsigned long grew, left;                   /* the heap's top moved by; bytes still in use afterwards */
    int whole;                                  /* kheap_check() */
    char said[1600];
};

/* a look here, on the heap as it is */
static void look_here(struct heapseen *h) {
    struct fat_check_io io;
    struct fat_check_result r;
    struct heap_stats a, b;
    memset(&io, 0, sizeof(io));
    io.blocks = v_blocks;
    io.read = v_read;
    io.stop = v_stop;
    io.say = v_say;
    said_n = 0;
    said[0] = 0;
    v_in_fat = 0;
    kheap_stats(&a);
    h->status = fat_check(&io, 0, &r);
    kheap_stats(&b);
    h->stopped = r.stopped;
    h->grew = b.brk - a.brk;
    h->left = b.bytes_used - a.bytes_used;
    h->whole = kheap_check();
    memcpy(h->said, said, sizeof(h->said) - 1);
    h->said[sizeof(h->said) - 1] = 0;
}

static int read_all(int fd, void *buf, unsigned long n) {
    unsigned long got = 0;
    while(got < n) {
        long k = read(fd, (char *)buf + got, n - got);
        if(k <= 0) return -1;
        got += (unsigned long)k;
    }
    return 0;
}

/* a look in a child: a new heap, a hole of `hole` bytes below a block in use first, as a heap in use has; with two, a
   second look on the same heap after the first */
static void look(u32 hole, struct heapseen *h, struct heapseen *h2) {
    int fd[2], pid, st = -1;
    if(pipe(fd)) { printf("FAIL: no pipe\n"); exit(2); }
    pid = fork();
    if(pid < 0) { printf("FAIL: no fork\n"); exit(2); }
    if(!pid) {
        struct heapseen x, y;
        void *p;
        close(fd[0]);
        arena_dma_heap = (u_long)dma_arena;
        heap_init();
        p = hole ? kalloc(hole) : NULL;
        (void)kalloc(16);
        if(p) kfree(p);
        look_here(&x);
        if(write(fd[1], &x, sizeof(x)) != (long)sizeof(x)) _exit(3);
        if(h2) {
            look_here(&y);
            if(write(fd[1], &y, sizeof(y)) != (long)sizeof(y)) _exit(3);
        }
        _exit(0);
    }
    close(fd[1]);
    memset(h, 0, sizeof(*h));
    if(read_all(fd[0], h, sizeof(*h)) || (h2 && read_all(fd[0], h2, sizeof(*h2)))) {
        h->status = -1;
        snprintf(h->said, sizeof(h->said), "(the child that looked ended without a word)");
    }
    close(fd[0]);
    waitpid(pid, &st, 0);
    if(st) {
        h->status = -1;
        snprintf(h->said, sizeof(h->said), "(the child that looked ended with status %d)", st);
    }
}

/* a whole look at a volume: its status, within the cap, nothing left, the heap whole, and all its names compared */
static void whole_look(const char *what, u32 blocks, u32 spc, u32 names, u32 hole, int status) {
    struct heapseen h;
    char m[2000];
    volume(blocks, spc, names);
    look(hole, &h, NULL);
    printf("    %s: the heap grew by %lu KB\n", what, h.grew >> 10);
    snprintf(m, sizeof(m), "%s: status %d (to be %d), the heap grew by %lu KB (at most %lu), %lu bytes left in use, the heap %s:\n%s",
             what, h.status, status, h.grew >> 10, CAP >> 10, h.left, h.whole ? "whole" : "CORRUPT", h.said);
    check(h.status == status && h.grew <= CAP && !h.left && h.whole && !strstr(h.said, "not checked whole"), m);
}

/* does a volume of n clusters of 32 KB fit? What the heap grew by, the look stopped once all was taken */
static int fits(u32 n, struct heapseen *h) {
    v_probe = 1;
    volume(32 + 2 * (((n + 2) * 4 + 511) / 512) + n * 64, 64, 10);
    look(0, h, NULL);
    v_probe = 0;
    return h->stopped;
}

int main(void) {
    struct heapseen h, h2;
    char m[2000];
    u32 lo, hi, mid;
    printf("FSCKHEAP: how far a look makes the kernel heap grow (mm/kalloc.c), %lu KB at most\n", CAP >> 10);

    /* the third review's volumes: the owner's 118 GB stick with 32 KB clusters, and 275 million blocks */
    whole_look("118 GB, 32 KB clusters, 10 names", 247463936, 64, 10, 0, FAT_CHECK_CLEAN);
    whole_look("118 GB, 32 KB clusters, 32769 names", 247463936, 64, 32769, 0, FAT_CHECK_CLEAN);
    whole_look("118 GB, 32 KB clusters, 65534 names", 247463936, 64, 65534, 0, FAT_CHECK_CLEAN);
    whole_look("275 million blocks, 32 KB clusters, 40000 names", 275000000, 64, 40000, 0, FAT_CHECK_CLEAN);
    whole_look("the same, after a hole of 1 MB in the heap", 275000000, 64, 40000, 1u << 20, FAT_CHECK_CLEAN);
    whole_look("64 MB, 512-byte clusters, 1000 names", 131072, 1, 1000, 0, FAT_CHECK_CLEAN);

    /* a second look on the same heap takes what the first left free, and grows it no further */
    volume(247463936, 64, 65534);
    look(0, &h, &h2);
    snprintf(m, sizeof(m), "a second look at the 118 GB stick on the same heap: status %d, the heap grew by %lu bytes more", h2.status, h2.grew);
    check(h.status == FAT_CHECK_CLEAN && h2.status == FAT_CHECK_CLEAN && !h2.grew && !h2.left && h2.whole, m);

    /* refused before anything is taken: the same 118 GB in 512-byte clusters */
    volume(247463936, 1, 10);
    look(0, &h, NULL);
    snprintf(m, sizeof(m), "118 GB in 512-byte clusters: refused (status %d), the heap grew by %lu KB:\n%s", h.status, h.grew >> 10, h.said);
    check(h.status == FAT_CHECK_TROUBLE && strstr(h.said, "more than the 4096 KB a check may have") && h.grew <= (64ul << 10) && !h.left, m);

    /* the edge of the cap, found by search: the largest volume of 32 KB clusters that fits takes no more than the cap,
       and one a cluster bigger is refused */
    check(fits(1000000, &h), "a million clusters fit");
    for(lo = 1000000, hi = 1u << 26; hi - lo > 1; ) {
        mid = lo + (hi - lo) / 2;
        if(fits(mid, &h)) lo = mid;
        else hi = mid;
    }
    (void)fits(lo, &h);
    printf("    the edge: %u clusters of 32 KB fit, and grew the heap by %lu KB\n", lo, h.grew >> 10);
    snprintf(m, sizeof(m), "%u clusters, the most that fit: the heap grew by %lu KB, at most %lu", lo, h.grew >> 10, CAP >> 10);
    check(h.grew <= CAP && !h.left && h.whole, m);
    check(!fits(lo + 1, &h) && h.grew <= (64ul << 10), "and a cluster more is refused, having taken next to nothing");
    whole_look("the most that fit, looked at whole", 32 + 2 * (((lo + 2) * 4 + 511) / 512) + lo * 64, 64, 10, 0, FAT_CHECK_CLEAN);

    printf("FSCKHEAP: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
