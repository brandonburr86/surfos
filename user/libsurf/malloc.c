/*
SurfOS user library: malloc, free, calloc, realloc
--------------------
File: malloc.c  Date: 10/5/26
--------------------
Every block starts with a header that holds its size and the size of the block
just below it (boundary tags), so free() merges a block with free neighbours on
both sides at once. Free blocks are on one list, searched first fit; when none
is big enough the heap grows with sbrk() by at least 64 KB, and the new space
joins a free block at the top. The heap ends in a header of size 0 that is
always in use, so the block above any block can be looked at. The first
libsurf had a bump allocator whose free() did nothing; the editor needs more.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

struct blk {
    size_t prev;                /* size of the block just below; 0 for the first block of a region */
    size_t size;                /* size of this block with its header; bit 0: in use */
};

struct links {                  /* the payload of a free block */
    struct blk *next, *prev;
};

#define USED      1u
#define ALIGN     8u
#define HDR       sizeof(struct blk)
#define MIN_BLOCK (HDR + sizeof(struct links))
#define GROW_MIN  (64u * 1024)
#define MAX_REQ   0x7ff00000u

#define BSIZE(b)  ((b)->size & ~(size_t)USED)
#define NEXT(b)   ((struct blk *)((char *)(b) + BSIZE(b)))
#define PREV(b)   ((struct blk *)((char *)(b) - (b)->prev))
#define LINKS(b)  ((struct links *)((b) + 1))

static struct blk *heap_end;    /* the size-0 header at the top of the heap */
static struct blk *free_list;

static void list_add(struct blk *b) {
    LINKS(b)->prev = NULL;
    LINKS(b)->next = free_list;
    if(free_list) LINKS(free_list)->prev = b;
    free_list = b;
}

static void list_del(struct blk *b) {
    struct links *l = LINKS(b);
    if(l->prev) LINKS(l->prev)->next = l->next;
    else free_list = l->next;
    if(l->next) LINKS(l->next)->prev = l->prev;
}

/* b is free and on no list: merge it with free neighbours, return the result (on no list) */
static struct blk *merge(struct blk *b) {
    struct blk *n = NEXT(b);
    if(!(n->size & USED)) {
        list_del(n);
        b->size += BSIZE(n);
        NEXT(b)->prev = BSIZE(b);
    }
    if(b->prev) {
        struct blk *p = PREV(b);
        if(!(p->size & USED)) {
            list_del(p);
            p->size += BSIZE(b);
            NEXT(p)->prev = BSIZE(p);
            b = p;
        }
    }
    return b;
}

/* b (in use) keeps need bytes; a big enough rest becomes a free block */
static void split(struct blk *b, size_t need) {
    size_t total = BSIZE(b);
    struct blk *r;
    if(total - need < MIN_BLOCK) return;
    r = (struct blk *)((char *)b + need);
    r->prev = need;
    r->size = total - need;
    NEXT(r)->prev = BSIZE(r);
    b->size = need | USED;
    list_add(merge(r));
}

/* more memory from the kernel: a free block of at least need bytes, on the list */
static struct blk *grow(size_t need) {
    size_t n = need + HDR < GROW_MIN ? GROW_MIN : (need + HDR + 4095) & ~4095u;
    char *p = sbrk((intptr_t)n);
    struct blk *b;
    if(p == (char *)-1) return NULL;
    if(heap_end && p == (char *)heap_end + HDR) {
        b = heap_end;                                   /* contiguous: the old end header starts the block */
        b->size = n;
    } else {                                            /* the first region, or someone else moved the break */
        size_t pad = (ALIGN - ((unsigned long)p & (ALIGN - 1))) & (ALIGN - 1);
        b = (struct blk *)(p + pad);
        b->prev = 0;
        b->size = (n - HDR - pad) & ~(size_t)(ALIGN - 1);
    }
    heap_end = NEXT(b);
    heap_end->prev = BSIZE(b);
    heap_end->size = USED;
    b = merge(b);
    list_add(b);
    return BSIZE(b) >= need ? b : NULL;
}

static size_t block_size(size_t n) {
    size_t need = (n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    return need < MIN_BLOCK ? MIN_BLOCK : need;
}

void *malloc(size_t n) {
    size_t need;
    struct blk *b;
    if(n > MAX_REQ) { errno = ENOMEM; return NULL; }
    need = block_size(n);
    for(b = free_list; b; b = LINKS(b)->next) if(BSIZE(b) >= need) break;
    if(!b && !(b = grow(need))) { errno = ENOMEM; return NULL; }
    list_del(b);
    b->size |= USED;
    split(b, need);
    return b + 1;
}

void free(void *p) {
    struct blk *b;
    if(!p) return;
    b = (struct blk *)p - 1;
    if(!(b->size & USED)) {
        static const char msg[] = "free(): not an allocated block\n";
        write(2, msg, sizeof(msg) - 1);
        abort();
    }
    b->size &= ~(size_t)USED;
    list_add(merge(b));
}

void *calloc(size_t n, size_t size) {
    void *p;
    if(size && n > MAX_REQ / size) { errno = ENOMEM; return NULL; }
    p = malloc(n * size);
    if(p) memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n) {
    struct blk *b, *nx;
    size_t need;
    void *q;
    if(!p) return malloc(n);
    if(!n) { free(p); return NULL; }
    if(n > MAX_REQ) { errno = ENOMEM; return NULL; }
    need = block_size(n);
    b = (struct blk *)p - 1;
    for(;;) {
        if(BSIZE(b) >= need) {                          /* it fits (maybe after taking the neighbour) */
            split(b, need);
            return p;
        }
        nx = NEXT(b);
        if(!(nx->size & USED) && BSIZE(b) + BSIZE(nx) >= need) {
            list_del(nx);
            b->size += BSIZE(nx);
            NEXT(b)->prev = BSIZE(b);
            continue;
        }
        if(nx == heap_end && grow(need - BSIZE(b)) && NEXT(b) != heap_end && !(NEXT(b)->size & USED))
            continue;                                   /* at the top: the heap grew right above it */
        break;
    }
    q = malloc(n);
    if(!q) return NULL;
    memcpy(q, p, BSIZE(b) - HDR);
    free(p);
    return q;
}
