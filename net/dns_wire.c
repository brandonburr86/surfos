/*
SurfOS Network Stack: bounded DNS packet decoding
--------------------
File: dns_wire.c Date: 10/10/26 (Undertow)
--------------------
No I/O or shared state. Names expand into caller-owned, bounded buffers;
compression pointer walks and CNAME chains have independent bounds.
*/
#include <net/dns_wire.h>
#include <fs/vfs.h>
#include <blibc_common.h>

static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

int dns_name_valid(const char *name) {
    u_int i, label = 0;
    if(!name || !*name) return 0;
    for(i = 0; name[i]; i++) {
        char c = name[i];
        if(i >= DNS_NAME_MAX) return 0;
        if(c == '.') {
            if(!label || name[i - 1] == '-') return 0;
            label = 0;
        } else {
            if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-')) return 0;
            if((!label && c == '-') || ++label > 63) return 0;
        }
    }
    return name[i - 1] != '-';        /* a single final dot is allowed */
}

static void canonical(const char *src, char *dst) {
    u_int n = 0;
    while(*src) dst[n++] = lower(*src++);
    if(n && dst[n - 1] == '.') n--;
    dst[n] = 0;
}

/* *next advances over the encoded name, not over the data reached by pointers.
   At most 128 labels/pointers are followed, including forward pointers. */
static int expand(const u8 *p, u_int len, u_int off, char *out, u_int *next) {
    u_int n = 0, steps = 0, after = 0;
    while(off < len && steps++ < 128) {
        u_int l = p[off++], j;
        if((l & 0xc0) == 0xc0) {
            if(off >= len) return -EIO;
            if(!after) after = off + 1;
            off = ((l & 0x3f) << 8) | p[off];
            continue;
        }
        if(l & 0xc0) return -EIO;
        if(!l) {
            out[n] = 0;
            *next = after ? after : off;
            return 0;
        }
        if(l > len - off || n + (n != 0) + l > DNS_NAME_MAX) return -EIO;
        if(n) out[n++] = '.';
        for(j = 0; j < l; j++) {
            /* Embedded NULs and dots cannot be represented as a host label. */
            if(!p[off + j] || p[off + j] == '.') return -EIO;
            out[n++] = lower(p[off + j]);
        }
        off += l;
    }
    return -EIO;
}

struct answer {
    char owner[DNS_NAME_MAX + 1];
    u16 type, cls, size;
    u_int data, next;
};
static int answer_at(const u8 *p, u_int len, u_int off, struct answer *a) {
    if(expand(p, len, off, a->owner, &off) || off > len || len - off < 10) return -EIO;
    a->type = be16(p + off); a->cls = be16(p + off + 2);
    a->size = be16(p + off + 8); a->data = off + 10;
    if(a->size > len - a->data) return -EIO;
    a->next = a->data + a->size;
    return 0;
}

int dns_parse_a(const u8 *p, u_int len, u16 id, const char *name, ipaddr_t *out) {
    char current[DNS_NAME_MAX + 1], question[DNS_NAME_MAX + 1], target[DNS_NAME_MAX + 1];
    struct answer a;
    u_int off, start, i, hop, next, records;
    u16 flags, count;
    if(!p || !out || !dns_name_valid(name)) return -EINVAL;
    if(len < 2 || be16(p) != id) return 1;
    if(len < 12) return -EIO;
    flags = be16(p + 2);
    if(!(flags & 0x8000) || (flags & 0x7800) || be16(p + 4) != 1) return 1;
    if(expand(p, len, 12, question, &off) || len - off < 4) return -EIO;
    canonical(name, current);
    if(strcmp(current, question) || be16(p + off) != 1 || be16(p + off + 2) != 1) return 1;
    if(flags & 0x0200) return -EIO;             /* TC: never trust a partial answer */
    if((flags & 15) == 3) return -ENOENT;
    if(flags & 15) return -EIO;
    start = off + 4; count = be16(p + 6);
    /* A truncated authority/additional section also makes this an incomplete reply. */
    records = (u_int)count + be16(p + 8) + be16(p + 10);
    for(i = 0, off = start; i < records; i++, off = a.next) {
        if(answer_at(p, len, off, &a)) return -EIO;
        if(i < count && a.cls == 1 && a.type == 1 && a.size != 4) return -EIO;
        if(i < count && a.cls == 1 && a.type == 5 &&
           (expand(p, len, a.data, target, &next) || next != a.next)) return -EIO;
    }
    for(hop = 0; hop <= 8; hop++) {
        int found = 0, cname = 0;
        ipaddr_t address = 0;
        target[0] = 0;
        for(i = 0, off = start; i < count; i++, off = a.next) {
            if(answer_at(p, len, off, &a)) return -EIO;
            if(a.cls != 1 || strcmp(a.owner, current)) continue;
            if(a.type == 1) { memcpy(&address, p + a.data, 4); found = 1; }
            if(a.type == 5) {
                char value[DNS_NAME_MAX + 1];
                if(expand(p, len, a.data, value, &next)) return -EIO;
                if(cname && strcmp(target, value)) return -EIO;
                strcpy(target, value); cname = 1;
            }
        }
        if(found && cname) return -EIO;
        if(found) { *out = address; return 0; }
        if(!cname) return -ENOENT;
        if(hop == 8 || !target[0] || !strcmp(current, target)) return -EIO;
        strcpy(current, target);
    }
    return -ENOENT;
}
