/*
SurfOS user programs: the zip format
----------------------
File: zipfile.h Date: 11/01/26
----------------------
Reading a zip's end record, central directory and local headers out of memory, with no system
calls, so that tests/host/ can run them on anything. Each function returns NULL, or why the bytes
cannot be used. Nothing in the archive is believed: each offset and length is checked against the
file's real size and the buffer in hand before it is used.

zip_check_directory() is the bound on the whole archive: entries must lie inside the file, before
the central directory, and all of them together must fit there. So the zip bomb that lays many
entries over one stretch of compressed data is refused before anything is written.

Zip64 is refused where it is needed (a field that says 0xffffffff or 0xffff): FAT files stop at
4 GB, so nothing past those limits could be held here.
*/

#ifndef _ZIPFILE_H
#define _ZIPFILE_H

#include <stddef.h>
#include <stdint.h>

#define ZIP_END_SIZE    22
#define ZIP_TAIL_MAX    (ZIP_END_SIZE + 65535)  /* the end record and the longest comment */
#define ZIP_CENTRAL_MAX (16u << 20)             /* 65535 entries of 200 bytes */
#define ZIP_LOCAL_SIZE  30

#define ZIP_STORED   0
#define ZIP_DEFLATED 8
#define ZIP_FLAG_ENCRYPTED 1
#define ZIP_FLAG_UTF8      0x800

struct zip_end {
    uint32_t entries, cd_size, cd_offset;
};

struct zip_entry {
    const unsigned char *name;          /* in the central directory, name_len bytes, not terminated */
    uint32_t name_len;
    unsigned int made_by, flags, method, time, date;
    uint32_t crc, csize, usize, offset, attrs;
};

/* the end record in the last n bytes of a file of size bytes */
const char *zip_find_end(const unsigned char *tail, uint32_t n, uint32_t size, struct zip_end *e);

/* the entry at *pos of a central directory of size bytes; *pos moves past it */
const char *zip_entry_at(const unsigned char *cd, uint32_t size, uint32_t *pos, struct zip_entry *e);

/* every entry of the central directory, against the end record */
const char *zip_check_directory(const unsigned char *cd, const struct zip_end *end);

/* a local header: the lengths of the name and the extra field after it */
const char *zip_local(const unsigned char *h, const struct zip_entry *e, uint32_t *name_len, uint32_t *extra_len);

/* The name as UTF-8 into out, terminated, cut if it does not fit (and then a reason is returned). A name without the
   UTF-8 flag is code page 437, except that one made on Unix or a Mac and valid as UTF-8 is that: both write UTF-8
   without saying so. */
const char *zip_name(const struct zip_entry *e, char *out, size_t size);

/* is it a symbolic link, a device or the like by its Unix mode? Those cannot be held on FAT. */
int zip_is_special(const struct zip_entry *e);

#endif
