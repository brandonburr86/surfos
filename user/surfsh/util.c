/*
SurfOS shell: surfsh
--------------------
File: util.c    Date: 10/24/26
--------------------
What the other files share: memory that never comes back NULL, a string that grows (struct
sbuf), and an arena, memory taken in blocks and given back all at once (the tree of a
command, which lives until the command has run). surfsh without memory cannot go on with
half a command: it says so and ends with status 2, and init starts a new one.
*/

#include "surfsh.h"
#include <stdarg.h>

#define ARENA_BLOCK 4096

struct ablock {
    struct ablock *next;
    size_t used, size, pad;         /* pad: the memory that follows is 8-aligned */
};

static void no_memory(void) {
    static const char msg[] = "surfsh: out of memory\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(STATUS_USAGE);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if(!p) no_memory();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if(!p) no_memory();
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

/**** strings that grow ****/

static void sb_room(struct sbuf *b, int more) {
    if(more < 0 || more > INT_MAX / 2 - 1 - b->len) no_memory();    /* 10/25/26: more than an int counts */
    if(b->len + more + 1 <= b->cap) return;
    while(b->len + more + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 64;
    b->s = xrealloc(b->s, (size_t)b->cap);
}

void sb_put(struct sbuf *b, const char *s, int n) {
    sb_room(b, n);
    memcpy(b->s + b->len, s, (size_t)n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_puts(struct sbuf *b, const char *s) {
    sb_put(b, s, (int)strlen(s));
}

void sb_putc(struct sbuf *b, char c) {
    sb_put(b, &c, 1);
}

void sb_printf(struct sbuf *b, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if(n <= 0) return;
    sb_room(b, n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += n;
}

/* s as one word of a command: itself when nothing in it is special, otherwise in single quotes,
   a ' in it as '\'' (the parser reads that back as s) */
void sb_quote(struct sbuf *b, const char *s) {
    if(!needs_quotes(s)) {
        sb_puts(b, s);
        return;
    }
    sb_putc(b, '\'');
    for(; *s; s++) {
        if(*s == '\'') sb_puts(b, "'\\''");
        else sb_putc(b, *s);
    }
    sb_putc(b, '\'');
}

const char *sb_str(struct sbuf *b) {
    return b->s ? b->s : "";
}

void sb_free(struct sbuf *b) {
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

/**** arenas ****/

void *arena_alloc(struct arena *a, size_t n) {
    struct ablock *b = a->blocks;
    char *p;
    n = (n + 7) & ~(size_t)7;
    if(!b || b->used + n > b->size) {
        size_t size = n > ARENA_BLOCK ? n : ARENA_BLOCK;
        b = xmalloc(sizeof(struct ablock) + size);
        b->size = size;
        b->used = 0;
        b->next = a->blocks;
        a->blocks = b;
    }
    p = (char *)(b + 1) + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strndup(struct arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void arena_free(struct arena *a) {
    while(a->blocks) {
        struct ablock *next = a->blocks->next;
        free(a->blocks);
        a->blocks = next;
    }
}
