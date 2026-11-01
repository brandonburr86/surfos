/*
SurfOS board: SurfCPU on the Arty A7 -- scan code set 1 to keys
----------------------
File: kbdset1.h Date: 10/15/26 (SurfCPU)
----------------------
kernel/keyboard.c's translation of set 1 (its tables and rules: letters follow Caps Lock,
the keypad follows Num Lock, the E0 keys become keyboard.h's codes, Ctrl+letter a control
character, F1-F4 switch consoles, Ctrl+Alt+Del reboots, raw mode's Backspace and Enter)
without the 8042 around it, as a pure function that says what to do instead of doing it,
so it can be tested on a host. Its tables are keyboard.c's, unchanged: keep them in step.
*/

#ifndef _KBDSET1_H
#define _KBDSET1_H

#include <surfos/types.h>

#define KBD_NONE    0
#define KBD_KEY     1               /* key: for tty_key() on the active tty */
#define KBD_SWITCH  2               /* console: tty_switch() (F1..F4) */
#define KBD_LEDS    3               /* leds: the lock keys' LEDs changed (NUM_LED, CAPS_LED, SCROLL_LED) */
#define KBD_REBOOT  4               /* Ctrl+Alt+Del */

struct kbd_event {
    int type;
    u8 key;
    int console;
    u8 leds;
};

struct kbd_set1 {
    bool shift_l, shift_r, ctrl, alt, e0;
    u8 leds;                        /* starts as keyboard.c's: Num Lock on */
    int consoles;                   /* F1..F<consoles> switch consoles (NUM_CONSOLES) */
};

void kbd_set1_init(struct kbd_set1 *k, int consoles);
void kbd_set1_reset(struct kbd_set1 *k);    /* an error on the line: modifiers released, no prefix */
/* one set 1 byte; raw: the active tty is in raw mode. Returns ev->type */
int kbd_set1_byte(struct kbd_set1 *k, u8 code, bool raw, struct kbd_event *ev);

#endif
