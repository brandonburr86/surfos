/*
SurfOS user programs: the zip format
--------------------
File: zipfile.c Date: 11/01/26
--------------------
What zipfile.h says. Sums of file offsets are 64 bits wide here so that two 32-bit values from a
hostile file cannot wrap round into something that looks small.
*/

#include <string.h>
#include <surfos/cp437.h>
#include "zipfile.h"
#include "../tar/arpath.h"

#define SIG_END     0x06054b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_LOCAL   0x04034b50u
#define SIG_SIGNATURE 0x05054b50u       /* a digital signature record, at the central directory's end */
#define CENTRAL_SIZE 46
#define HOST_UNIX   3
#define HOST_DARWIN 19
#define MODE_TYPE   0170000u
#define MODE_FILE   0100000u
#define MODE_DIR    0040000u

static uint32_t le16(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const unsigned char *p) {
    return le16(p) | le16(p + 2) << 16;
}

const char *zip_find_end(const unsigned char *tail, uint32_t n, uint32_t size, struct zip_end *e) {
    uint32_t i;
    if(n < ZIP_END_SIZE || n > size) return "not a zip file";
    for(i = n - ZIP_END_SIZE;; i--) {
        if(le32(tail + i) == SIG_END && i + ZIP_END_SIZE + le16(tail + i + 20) == n) break;
        if(!i) return "not a zip file, or one cut short";
    }
    {
        const unsigned char *p = tail + i;
        uint32_t here = size - n + i;
        if(le16(p + 8) == 0xffff || le16(p + 10) == 0xffff || le32(p + 12) == 0xffffffffu || le32(p + 16) == 0xffffffffu)
            return "a Zip64 archive: its files would not fit on FAT";
        if(le16(p + 4) || le16(p + 6) || le16(p + 8) != le16(p + 10)) return "an archive on several disks";
        e->entries = le16(p + 10);
        e->cd_size = le32(p + 12);
        e->cd_offset = le32(p + 16);
        if((uint64_t)e->cd_offset + e->cd_size > here) return "the central directory is not where the end record says";
        if(e->cd_size > ZIP_CENTRAL_MAX) return "a central directory of more than 16 MB";
    }
    return NULL;
}

const char *zip_entry_at(const unsigned char *cd, uint32_t size, uint32_t *pos, struct zip_entry *e) {
    const unsigned char *p;
    uint64_t total;
    if((uint64_t)*pos + CENTRAL_SIZE > size) return "the central directory ends in the middle of an entry";
    p = cd + *pos;
    if(le32(p) != SIG_CENTRAL) return "the central directory is damaged";
    total = (uint64_t)CENTRAL_SIZE + le16(p + 28) + le16(p + 30) + le16(p + 32);
    if(*pos + total > size) return "the central directory ends in the middle of an entry";
    e->made_by = le16(p + 4);
    e->flags = le16(p + 8);
    e->method = le16(p + 10);
    e->time = le16(p + 12);
    e->date = le16(p + 14);
    e->crc = le32(p + 16);
    e->csize = le32(p + 20);
    e->usize = le32(p + 24);
    e->name_len = le16(p + 28);
    e->attrs = le32(p + 38);
    e->offset = le32(p + 42);
    e->name = p + CENTRAL_SIZE;
    if(e->csize == 0xffffffffu || e->usize == 0xffffffffu || e->offset == 0xffffffffu || le16(p + 34) == 0xffff)
        return "an entry that needs Zip64: its file would not fit on FAT";
    if(le16(p + 34)) return "an entry on another disk";
    *pos += (uint32_t)total;
    return NULL;
}

const char *zip_check_directory(const unsigned char *cd, const struct zip_end *end) {
    uint64_t all = 0;
    uint32_t pos = 0, i;
    for(i = 0; i < end->entries; i++) {
        struct zip_entry e;
        const char *why = zip_entry_at(cd, end->cd_size, &pos, &e);
        uint64_t extent;
        if(why) return why;
        extent = (uint64_t)ZIP_LOCAL_SIZE + e.name_len + e.csize;
        if(e.offset + extent > end->cd_offset) return "an entry lies outside the archive";
        all += extent;
    }
    if(all > end->cd_offset) return "the entries overlap, or claim more than the archive holds";
    if(pos != end->cd_size && !(pos + 4 <= end->cd_size && le32(cd + pos) == SIG_SIGNATURE))
        return "the central directory holds more than the end record says";
    return NULL;
}

const char *zip_local(const unsigned char *h, const struct zip_entry *e, uint32_t *name_len, uint32_t *extra_len) {
    if(le32(h) != SIG_LOCAL) return "its local header is not there";
    if(le16(h + 8) != e->method) return "its two headers disagree on the method";
    if(le16(h + 6) & ZIP_FLAG_ENCRYPTED) return "encrypted";
    *name_len = le16(h + 26);
    *extra_len = le16(h + 28);
    return NULL;
}

static int unix_made(const struct zip_entry *e) {
    unsigned int host = e->made_by >> 8;
    return host == HOST_UNIX || host == HOST_DARWIN;
}

const char *zip_name(const struct zip_entry *e, char *out, size_t size) {
    static const char too_long[] = "the name is too long";
    size_t n = 0;
    uint32_t i;
    const char *why = memchr(e->name, 0, e->name_len) ? "the name has a zero byte in it" : NULL;
    if((e->flags & ZIP_FLAG_UTF8) || (unix_made(e) && ar_utf8_valid((const char *)e->name, e->name_len))) {
        n = e->name_len < size - 1 ? e->name_len : size - 1;
        memcpy(out, e->name, n);
        out[n] = 0;
        return why ? why : n < e->name_len ? too_long : NULL;
    }
    for(i = 0; i < e->name_len; i++) {
        unsigned char c = e->name[i];
        char u[CP437_UTF8_MAX];
        int k = 1;
        if(c < 0x80) u[0] = (char)c;
        else k = cp437_utf8(c, u);
        if(n + (size_t)k >= size) {
            why = why ? why : too_long;
            break;
        }
        memcpy(out + n, u, (size_t)k);
        n += (size_t)k;
    }
    out[n] = 0;
    return why;
}

int zip_is_special(const struct zip_entry *e) {
    unsigned int mode = (e->attrs >> 16) & MODE_TYPE;
    return unix_made(e) && mode && mode != MODE_FILE && mode != MODE_DIR;
}
