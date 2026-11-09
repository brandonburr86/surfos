/*
SurfOS Keyboard: the Scan Codes Before Their Meaning
--------------------
File: keyscan.h Date: 11/05/26
--------------------
(docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk; the review of stage 2)
kernel/keyboard.c's bytes from the keyboard (scan code set 1) as keys, before what the keys mean:
the E0 prefix that comes before the grey keys, Pause's E1 sequence, which is dropped byte by byte as
before, and whether a key's make code is the keyboard's repeat of the key held down (typematic: its
make again with no break between). Shift's fake prefixes around the grey keys with Num Lock on (E0
2A, E0 36 and their breaks) are no keys of their own and change nothing. A lost byte (the
controller's error) forgets the key held: keyscan_reset(). Pure, so that tests/host/test_keyscan.c
runs it on the host.

A keyboard that goes back to repeating a key still held after another was tapped (some do) sends
that key's make again: it reads as a new press, then its repeats as repeats.
*/
#ifndef _SURFOS_KEYSCAN_H
#define _SURFOS_KEYSCAN_H

#define KEYSCAN_E0    0x100             /* a key after the E0 prefix */
#define KEYSCAN_BREAK 0x80              /* its release */

struct keyscan {
    int e0;                             /* the byte before was E0 */
    unsigned held;                      /* the key pressed last (KEYSCAN_E0 | code), until it is let go; 0 */
    int repeat;                         /* the key keyscan_byte() gave last is a make the keyboard repeated */
};

static inline void keyscan_reset(struct keyscan *k) {
    k->e0 = 0;
    k->held = 0;
    k->repeat = 0;
}

/* one byte from the keyboard: -1 for one that is no key (E0, E1), else the key: its code with KEYSCAN_BREAK for a
   release, and KEYSCAN_E0 after the prefix; k->repeat says whether a make is the keyboard's repeat */
static inline int keyscan_byte(struct keyscan *k, unsigned char b) {
    unsigned key;
    if(b == 0xE0) {
        k->e0 = 1;
        return -1;
    }
    if(b == 0xE1) return -1;                    /* Pause: its bytes after this pass as keys, as they always did */
    key = (k->e0 ? KEYSCAN_E0 : 0) | (b & 0x7F);
    k->e0 = 0;
    if(key == (KEYSCAN_E0 | 0x2A) || key == (KEYSCAN_E0 | 0x36)) {    /* Shift's fake prefixes */
        k->repeat = 0;
        return (int)(key | (b & KEYSCAN_BREAK));
    }
    if(b & KEYSCAN_BREAK) {
        if(k->held == key) k->held = 0;
        k->repeat = 0;
        return (int)(key | KEYSCAN_BREAK);
    }
    k->repeat = k->held == key;
    k->held = key;
    return (int)key;
}

#endif
