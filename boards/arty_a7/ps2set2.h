/*
SurfOS board: SurfCPU on the Arty A7 -- PS/2 scan code set 2 to set 1
----------------------
File: ps2set2.h Date: 10/15/26 (SurfCPU)
----------------------
A PS/2 keyboard sends scan code set 2; on a PC the 8042 controller translates it to set 1,
which is what SurfOS's keyboard code was written for (kernel/keyboard.c). surf_ps2 hands
the CPU set 2 untranslated, so this does the 8042's job in software: each key byte through
the translation table, F0 (break) folded into the next byte's top bit, E0 passed on. Two
things it does better than an 8042: the Pause key's eight bytes are swallowed whole (an
8042 turns them into Ctrl and Num Lock presses), and the keyboard's own replies (ACK,
resend, the self-test result, errors) are told apart from keys and handed back instead.
Pure code, host-tested (boards/arty_a7/tests/test_arty.c).
*/

#ifndef _PS2SET2_H
#define _PS2SET2_H

#include <surfos/types.h>

#define PS2_ACK      0xFA
#define PS2_RESEND   0xFE
#define PS2_BAT_OK   0xAA           /* self test passed: after power-up, a reset or plugging in */
#define PS2_BAT_FAIL1 0xFC
#define PS2_BAT_FAIL2 0xFD
#define PS2_ECHO     0xEE
#define PS2_ERROR0   0x00           /* key detection error or buffer overrun */
#define PS2_ERROR1   0xFF

struct ps2_set2 {
    bool e0, f0;                    /* prefixes seen */
    u8 pause_left;                  /* bytes of the Pause sequence still to swallow */
    u_long keys, replies, unknown;  /* counts */
};

void ps2_set2_reset(struct ps2_set2 *s);    /* a framing error or a lost byte: forget the prefixes */
/* One byte from the keyboard: 0, 1 or 2 bytes of set 1 in out (an E0 prefix, then the code,
   bit 7 set for a release), the number returned. A reply of the keyboard's gives 0 and is
   put in *reply (-1 otherwise). */
int ps2_set2_byte(struct ps2_set2 *s, u8 b, u8 out[2], int *reply);
u8 ps2_set2_translate(u8 code);             /* the table alone: set 1 for a set 2 key code, 0 for none */

#endif
