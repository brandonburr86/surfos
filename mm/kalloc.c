/*
SurfOS kernel memory allocation
Copyright (C)2004 Brandon Burr, heap v2 10/2026 (roadmap M3)

Two heaps built on the same code: the kernel heap at KHEAP_START (pages appear on first
touch) and the DMA heap in the 1:1 mapped 10-15 MB area. Each block has a 16 byte header
with a magic word, its payload size and the previous block's size, so neighbours can be
merged in both directions; free blocks keep their list links in the payload. First fit,
splitting, 16 byte alignment, poisoning on free, and a bad pointer panics instead of
corrupting the lists. The 2004 allocator never merged, leaked its descriptors and read
address 4 when its free list was empty (audit M1-M4, M11).
*/

#include <surfos/types.h>
#include <surfos/panic.h>
#include <surfos/irq.h>
#include <surfos/console.h>
#include <mm/memory.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define HEAP_ALIGN 16
#define HEAP_HDR   16
#define HEAP_MIN   16
#define MAGIC_USED 0x53554642 /* "SUFB": SurfOS used block */
#define MAGIC_FREE 0x53554646 /* "SUFF" */
#define GROW_CHUNK (64 * 1024)
#define POISON 0xDD

struct hblock {
    u32 magic;
    u32 size;       /* payload bytes, a multiple of HEAP_ALIGN */
    u32 prev_size;  /* payload bytes of the block before this one, 0 for the first */
    u32 heapid;
};

struct hfree {      /* lives in the payload of a free block */
    struct hfree *next, *prev;
};

struct heap {
    const char *name;
    u32 id;
    u_long start, limit, brk;
    struct hfree *free_list;
    struct hblock *last;
    u_long bytes_used, bytes_free, blocks_used, blocks_free, grows;
};

static struct heap kheap = { "kernel", 1, KHEAP_START, KHEAP_END, KHEAP_START, NULL, NULL, 0, 0, 0, 0, 0 };
static struct heap dmaheap = { "dma", 2, PHEAP_START, PHEAP_END, PHEAP_START, NULL, NULL, 0, 0, 0, 0, 0 };

static inline struct hblock *hdr_of(void *payload) { return (struct hblock *)((u_long)payload - HEAP_HDR); }
static inline void *payload_of(struct hblock *b) { return (void *)((u_long)b + HEAP_HDR); }

static inline struct hblock *next_block(struct heap *h, struct hblock *b) {
    u_long n = (u_long)b + HEAP_HDR + b->size;
    return n < h->brk ? (struct hblock *)n : NULL;
}

static inline struct hblock *prev_block(struct heap *h, struct hblock *b) {
    if((u_long)b == h->start) return NULL;
    return (struct hblock *)((u_long)b - HEAP_HDR - b->prev_size);
}

static void fl_insert(struct heap *h, struct hblock *b) {
    struct hfree *n = (struct hfree *)payload_of(b);
    n->prev = NULL;
    n->next = h->free_list;
    if(h->free_list) h->free_list->prev = n;
    h->free_list = n;
    h->blocks_free++;
    h->bytes_free += b->size;
}

static void fl_remove(struct heap *h, struct hblock *b) {
    struct hfree *n = (struct hfree *)payload_of(b);
    if(n->prev) n->prev->next = n->next;
    else h->free_list = n->next;
    if(n->next) n->next->prev = n->prev;
    h->blocks_free--;
    h->bytes_free -= b->size;
}

/* Add room at the top of the heap as one free block (merged with the last block if that
   is free). Touching the new header maps the page in the kernel heap. */
static struct hblock *heap_grow(struct heap *h, u_long need) {
    u_long chunk = need + HEAP_HDR;
    struct hblock *b;

    if(chunk < GROW_CHUNK) chunk = GROW_CHUNK;
    chunk = (chunk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if(h->brk + chunk > h->limit) {
        chunk = (need + HEAP_HDR + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if(h->brk + chunk > h->limit) return NULL;
    }

    b = (struct hblock *)h->brk;
    b->magic = MAGIC_FREE;
    b->size = chunk - HEAP_HDR;
    b->prev_size = h->last ? h->last->size : 0;
    b->heapid = h->id;
    h->brk += chunk;
    h->grows++;

    if(h->last && h->last->magic == MAGIC_FREE) { /* merge into the free block below */
        struct hblock *p = h->last;
        fl_remove(h, p);
        p->size += HEAP_HDR + b->size;
        b = p;
    }
    h->last = b;
    fl_insert(h, b);
    return b;
}

static void *heap_alloc(struct heap *h, size_t size) {
    u_long flags;
    struct hfree *n;
    struct hblock *b = NULL;

    if(size < HEAP_MIN) size = HEAP_MIN;
    size = (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);

    flags = irq_save();
    for(n = h->free_list; n; n = n->next) { /* first fit */
        if(hdr_of(n)->size >= size) { b = hdr_of(n); break; }
    }
    if(!b) b = heap_grow(h, size);
    if(!b || b->size < size) {
        irq_restore(flags);
        return NULL;
    }
    fl_remove(h, b);

    if(b->size >= size + HEAP_HDR + HEAP_MIN) { /* split off the tail */
        struct hblock *rest = (struct hblock *)((u_long)payload_of(b) + size);
        struct hblock *after;
        rest->magic = MAGIC_FREE;
        rest->size = b->size - size - HEAP_HDR;
        rest->prev_size = size;
        rest->heapid = h->id;
        b->size = size;
        after = next_block(h, rest);
        if(after) after->prev_size = rest->size;
        if(h->last == b) h->last = rest;
        fl_insert(h, rest);
    }

    b->magic = MAGIC_USED;
    h->blocks_used++;
    h->bytes_used += b->size;
    irq_restore(flags);
    return payload_of(b);
}

static void heap_free(struct heap *h, void *mem) {
    u_long flags;
    struct hblock *b, *n, *p;

    if(!mem) return;
    if((u_long)mem < h->start + HEAP_HDR || (u_long)mem >= h->brk || ((u_long)mem & (HEAP_ALIGN - 1)))
        panic("kfree: %p is not in the %s heap", mem, h->name);
    b = hdr_of(mem);
    if(b->magic == MAGIC_FREE) panic("kfree: double free of %p (%s heap)", mem, h->name);
    if(b->magic != MAGIC_USED || b->heapid != h->id) panic("kfree: corrupt block at %p (%s heap)", mem, h->name);

    flags = irq_save();
    h->blocks_used--;
    h->bytes_used -= b->size;
    memset(mem, POISON, b->size);
    b->magic = MAGIC_FREE;

    n = next_block(h, b);
    if(n && n->magic == MAGIC_FREE) { /* merge the block above into this one */
        fl_remove(h, n);
        b->size += HEAP_HDR + n->size;
        if(h->last == n) h->last = b;
    }
    p = prev_block(h, b);
    if(p && p->magic == MAGIC_FREE) { /* merge this one into the block below */
        fl_remove(h, p);
        p->size += HEAP_HDR + b->size;
        if(h->last == b) h->last = p;
        b = p;
    }
    n = next_block(h, b);
    if(n) n->prev_size = b->size;

    fl_insert(h, b);
    irq_restore(flags);
}

static void *heap_realloc(struct heap *h, void *mem, size_t size) {
    void *fresh;
    struct hblock *b;
    if(!mem) return heap_alloc(h, size);
    if(!size) { heap_free(h, mem); return NULL; }
    b = hdr_of(mem);
    if(b->magic != MAGIC_USED) panic("krealloc: corrupt block at %p", mem);
    if(b->size >= size) return mem;
    fresh = heap_alloc(h, size);
    if(!fresh) return NULL;
    memcpy(fresh, mem, b->size);
    heap_free(h, mem);
    return fresh;
}

static bool heap_check(struct heap *h) {
    struct hblock *b = (struct hblock *)h->start, *prev = NULL;
    u_long flags = irq_save();
    u_long used = 0, freed = 0;

    while((u_long)b < h->brk) {
        if(b->magic != MAGIC_USED && b->magic != MAGIC_FREE) {
            irq_restore(flags);
            kprintf("%s heap: bad magic 0x%08x at %p\n", h->name, b->magic, b);
            return false;
        }
        if(b->prev_size != (prev ? prev->size : 0)) {
            irq_restore(flags);
            kprintf("%s heap: block %p has prev_size %u, previous block is %u\n", h->name, b, b->prev_size, prev ? prev->size : 0);
            return false;
        }
        if(b->magic == MAGIC_USED) used += b->size; else freed += b->size;
        prev = b;
        b = (struct hblock *)((u_long)b + HEAP_HDR + b->size);
    }
    irq_restore(flags);
    if((u_long)b != h->brk || used != h->bytes_used || freed != h->bytes_free) {
        kprintf("%s heap: walk ends at %p (brk %p), used %lu/%lu free %lu/%lu\n", h->name, b, (void *)h->brk,
                used, h->bytes_used, freed, h->bytes_free);
        return false;
    }
    return true;
}

static void heap_fill_stats(struct heap *h, struct heap_stats *st) {
    st->name = h->name;
    st->start = h->start;
    st->brk = h->brk;
    st->limit = h->limit;
    st->bytes_used = h->bytes_used;
    st->bytes_free = h->bytes_free;
    st->blocks_used = h->blocks_used;
    st->blocks_free = h->blocks_free;
    st->grows = h->grows;
}

/**** the public faces ****/

void *kalloc(size_t size) { return heap_alloc(&kheap, size); }
void kfree(void *mem) { heap_free(&kheap, mem); }
void *krealloc(void *mem, size_t size) { return heap_realloc(&kheap, mem, size); }

void *kcalloc(size_t count, size_t size) {
    void *p = heap_alloc(&kheap, count * size);
    if(p) memset(p, 0, count * size);
    return p;
}

void *kalloc_dma(size_t size) { return heap_alloc(&dmaheap, size); }
void kfree_dma(void *mem) { heap_free(&dmaheap, mem); }

void kheap_stats(struct heap_stats *st) { heap_fill_stats(&kheap, st); }
void dmaheap_stats(struct heap_stats *st) { heap_fill_stats(&dmaheap, st); }
bool kheap_check(void) { return heap_check(&kheap) && heap_check(&dmaheap); }

void heap_init(void) {
    kheap.brk = kheap.start; kheap.free_list = NULL; kheap.last = NULL;
    dmaheap.brk = dmaheap.start; dmaheap.free_list = NULL; dmaheap.last = NULL;
}
