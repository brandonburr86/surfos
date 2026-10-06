/*
SurfOS Code Page 437
----------------------
File: cp437.h   Date: 10/6/26
----------------------
The console's character set (driver/cp437.c). A console cell holds one byte, and since
10/6/26 that byte is a code page 437 character: VGA text mode draws it with the card's
own font, the framebuffer with driver/font8x16.c, which holds the same 256 characters in
the same order. SurfOS's symbols take three places whose VGA glyph is close:

    0x10  U+25B8 ▸  (VGA ►; ► U+25BA and ▶ U+25B6 are drawn there too)
    0xF7  U+224B ≋  (VGA ≈; ≈ U+2248 too)
    0xF9  U+2026 …  (VGA ∙; ∙ U+2219 too)

Programs print UTF-8. kputch() hands every byte that is not plain ASCII to utf8_feed():
a complete character becomes its code page 437 index, a character code page 437 does
not have becomes '?', and bytes that are not UTF-8 (a stray byte above 0x7f, a sequence
cut short or not allowed) are drawn as the code page 437 characters they are, which is
what programs of 2004 meant by them.
*/

#ifndef _SYS_CP437_H
#define _SYS_CP437_H

#include <surfos/types.h>

/* index -> Unicode: the graphic set (0x01-0x1F are ☺..▼, 0x7F is ⌂) with SurfOS's three
   places; 0x00 is the empty cell (U+0000) */
extern const u16 cp437_unicode[256];

/* Unicode -> index, -1 when code page 437 has no place for it. The control characters
   (U+0001-U+001F, U+007F-U+009F) have none: they are never drawn. */
int cp437_from_unicode(u32 u);

/* index -> the UTF-8 bytes of its character (1 to 3, not terminated); the empty cell 0x00
   is a space. out must hold CP437_UTF8_MAX bytes. */
#define CP437_UTF8_MAX 3
int cp437_utf8(u8 index, char *out);

/* the UTF-8 decoder of one console: all zero is "nothing held" */
struct utf8_dec {
    u8 have;            /* bytes of an unfinished sequence held in b[] */
    u8 need;            /* that sequence's length, 2 to 4 */
    u8 b[4];
};

/* Feed one byte. Returns how many cells to draw, their indexes in out[] (UTF8_OUT_MAX
   at most): nothing while a sequence is unfinished; one cell for a finished one; a byte
   below 0x80 comes out last as itself, after the bytes of any sequence it cut short (the
   caller carries out a control character itself); a byte that is not UTF-8 comes out as
   itself. */
#define UTF8_OUT_MAX 4
int utf8_feed(struct utf8_dec *d, u8 c, u8 *out);

#endif
