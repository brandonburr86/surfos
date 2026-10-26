/* Host-side tests of the kernel heap (10/25/26, robustness B1): mm/kalloc.c as it is, its heap in a static array of
   this test (tests/host/kalloc_host.h), and the frame allocator it promises frames from replaced by pmm_commit()
   below, which refuses on demand. Out of memory must be a NULL from kalloc() with the heap left as it was, never
   a heap that grew without its frames; heap_check() (kheap_check()) walks every block after each step. Also the
   rule of the frame classes and the reserve (mm/pmm.h, pure). Built and run by `make unittest`. */
#include "kalloc_host.h"
#include <surfos/types.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

int vprintf(const char *fmt, va_list ap);
void exit(int code);

int sim_if = 1;                                 /* tests/host/pager_shim's interrupt flag */
unsigned char kalloc_test_kheap[KALLOC_TEST_KHEAP] __attribute__((aligned(4096)));
static unsigned char dma_arena[PAGE_SIZE] __attribute__((aligned(4096)));
u_long arena_page_tables, arena_frame_stack, arena_frame_stack_bytes, arena_dma_heap;

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/**** the frame allocator, as the heap sees it ****/

static u_long promised, promises, refuse_over = ~0UL, refusals;
static int last_cls;

int pmm_commit(u_long n, int cls) {
    promises++;
    last_cls = cls;
    if(n > refuse_over) {
        refusals++;
        return -1;
    }
    promised += n;
    return 0;
}

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

static struct heap_stats stats(void) {
    struct heap_stats s;
    kheap_stats(&s);
    return s;
}

/* every page of the heap was promised before the heap reached it */
static int all_promised(void) {
    return promised * PAGE_SIZE == kheap_top() - KHEAP_START;
}

/**** the frame classes (mm/pmm.h) ****/

static void classes(void) {
    check(pmm_kernel_reserve(1000) == 512 && pmm_kernel_reserve(32 * 1000) == 1000 && pmm_kernel_reserve(1u << 20) == 4096,
          "the reserve: a 32nd of the frames, 2 MB at least, 16 MB at most");
    check(pmm_class_may(PMM_USER, 1000, 488, 512) && !pmm_class_may(PMM_USER, 1000, 489, 512),
          "a program leaves the whole reserve");
    check(pmm_class_may(PMM_BULK, 1000, 744, 512) && !pmm_class_may(PMM_BULK, 1000, 745, 512),
          "a large kernel buffer leaves half of it");
    check(pmm_class_may(PMM_KERNEL, 1000, 1000, 512) && !pmm_class_may(PMM_KERNEL, 1000, 1001, 512),
          "the kernel may take the last frame, and not one more");
    check(!pmm_class_may(PMM_USER, 100, 1, 512) && !pmm_class_may(PMM_BULK, 200, 1, 512) &&
          pmm_class_may(PMM_KERNEL, 100, 1, 512), "inside the reserve only the kernel, inside half of it only the kernel");
}

/**** the heap ****/

/* the first allocation grows the heap by 64 KB, promised as the kernel's, before it touches a byte of it */
static void first_growth(void) {
    void *p = kalloc(100);
    check(p && (u_long)p > KHEAP_START && (u_long)p < KHEAP_START + 65536, "the first kalloc() is in the heap");
    check(promises == 1 && promised == 16 && last_cls == PMM_KERNEL, "it promised 16 frames (64 KB) as the kernel's");
    check(kheap_top() == KHEAP_START + 65536 && all_promised(), "the heap's top moved by those 64 KB");
    kfree(p);
    check(kheap_check(), "the heap checks out");
}

/* a request over 64 KB grows the heap by what it needs, promised as a large buffer's (PMM_BULK) */
static void bulk_growth(void) {
    u_long top = kheap_top();
    void *p = kalloc(1 << 20);
    check(p != NULL && last_cls == PMM_BULK && kheap_top() > top && all_promised(),
          "a megabyte: the growth promised as PMM_BULK");
    kfree(p);
    check(kheap_check(), "the heap checks out");
}

/* refused: kalloc() says NULL, and the heap is as it was (no growth without frames) */
static void refused(void) {
    struct heap_stats a = stats(), b;
    void *p;
    refuse_over = 0;
    p = kalloc(6 << 20);
    b = stats();
    check(p == NULL, "kalloc() of 6 MB with no frames to promise: NULL");
    check(b.brk == a.brk && b.bytes_used == a.bytes_used && b.grows == a.grows && all_promised() && kheap_check(),
          "and the heap as it was");
    refuse_over = ~0UL;
}

/* 64 KB cannot be promised but one page can: the heap grows by what the request needs */
static void least_growth(void) {
    static void *keep[4096];
    u_long top, sz = 1 << 22;
    void *p;
    int n = 0;
    refuse_over = 0;                            /* every free byte taken, and the heap not grown meanwhile */
    while(sz >= 16 && n < 4096) {
        if((keep[n] = kalloc(sz)) != NULL) n++;
        else sz /= 2;
    }
    top = kheap_top();
    refuse_over = 1;
    p = kalloc(100);
    check(p != NULL && kheap_top() == top + PAGE_SIZE && all_promised(),
          "64 KB refused, one page promised: the heap grows by one page and kalloc() succeeds");
    refuse_over = 0;
    check(kalloc(PAGE_SIZE) == NULL && kheap_top() == top + PAGE_SIZE, "and nothing promised: NULL, the top stays");
    refuse_over = ~0UL;
    kfree(p);
    while(n) kfree(keep[--n]);
    check(kheap_check(), "the heap checks out");
}

/* 10/25/26 (the review of B1): a free block at the top is merged with the growth, so only what it lacks is promised.
   Everything freed, the heap is that one block; a request 40 KB larger promises GROW_CHUNK (16 pages), or 10 when 16
   are refused, never the request's own size */
static void free_tail_growth(void) {
    struct heap_stats s = stats();
    u_long before, top = kheap_top();
    void *p;
    check(s.blocks_used == 0 && s.blocks_free == 1, "everything freed: the heap is one free block");
    before = promised;
    p = kalloc(s.bytes_free + 40960);
    check(p != NULL && promised - before == 16 && kheap_top() == top + 65536 && all_promised(),
          "a request 40 KB over the free top: 64 KB grown and promised, not the request's size");
    kfree(p);
    top = kheap_top();
    s = stats();
    before = promised;
    refuse_over = 12;
    p = kalloc(s.bytes_free + 40960);
    refuse_over = ~0UL;
    check(p != NULL && promised - before == 10 && kheap_top() == top + 40960 && all_promised(),
          "with 12 pages to promise: the 10 it lacks are enough, and kalloc() succeeds");
    kfree(p);
    check(kheap_check(), "the heap checks out");
}

/* a random workload whose promises are refused now and then: each block keeps its bytes, no two overlap, every
   failure is a NULL with the heap whole, and the heap checks out after every step */
static unsigned rnd_state = 20261025;
static unsigned rnd(void) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return rnd_state >> 8;
}

#define SLOTS 256
static void workload(void) {
    static unsigned char *block[SLOTS];
    static u_long size[SLOTS];
    u_long nulls = 0, i, j, k;
    int whole = 1, kept = 1, apart = 1;
    for(i = 0; i < 20000 && whole; i++) {
        j = rnd() % SLOTS;
        refuse_over = rnd() % 8 == 0 ? rnd() % 20 : ~0UL;
        if(block[j]) {
            for(k = 0; k < size[j]; k++) if(block[j][k] != (unsigned char)(j + k)) kept = 0;
            kfree(block[j]);
            block[j] = NULL;
        } else {
            size[j] = rnd() % 4 == 0 ? rnd() % 200000 + 1 : rnd() % 3000 + 1;
            block[j] = (unsigned char *)kalloc(size[j]);
            if(!block[j]) nulls++;
            else for(k = 0; k < size[j]; k++) block[j][k] = (unsigned char)(j + k);
        }
        whole = kheap_check() && all_promised();
    }
    refuse_over = ~0UL;
    for(i = 0; i < SLOTS; i++) {
        for(j = i + 1; j < SLOTS; j++) {
            if(block[i] && block[j] && block[i] < block[j] + size[j] && block[j] < block[i] + size[i]) apart = 0;
        }
    }
    for(i = 0; i < SLOTS; i++) {
        if(block[i]) kfree(block[i]);
        block[i] = NULL;
    }
    check(whole, "20000 steps with promises refused now and then: the heap checks out after each, every page promised");
    check(kept && apart, "every block kept its bytes, and no two overlapped");
    check(nulls > 0 && refusals > 0, "some allocations were refused (and said NULL)");
    check(stats().bytes_used == 0 && kheap_check(), "all freed: nothing in use");
}

int main(void) {
    arena_dma_heap = (u_long)dma_arena;
    heap_init();
    classes();
    first_growth();
    bulk_growth();
    refused();
    least_growth();
    free_tail_growth();
    workload();
    printf("KALLOC UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
