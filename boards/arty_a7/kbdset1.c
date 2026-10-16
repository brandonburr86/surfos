/*
SurfOS board: SurfCPU on the Arty A7 -- scan code set 1 to keys
--------------------
File: kbdset1.c Date: 10/15/26 (SurfCPU), after kernel/keyboard.c (C)2004 Brandon Burr
--------------------
See kbdset1.h. keyboard_byte(), handle_e0() and deliver() of kernel/keyboard.c, the same
rules in the same order, returning an event instead of calling the tty.
*/

#include "kbdset1.h"
#include <surfos/keyboard.h>

/* kernel/keyboard.c's tables */
static const u_char normmap[0x59] = {
/* 00 */ 0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
/* 10 */ 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
/* 20 */ 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
/* 30 */ 'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  0,    ' ',  0,    F1,   F2,   F3,   F4,   F5,
/* 40 */ F6,   F7,   F8,   F9,   F10,  0,    0,    HOME, UP,   PGUP, '-',  LEFT, '5',  RT,   '+',  END,
/* 50 */ DOWN, PGDN, INS,  DEL,  0,    0,    '\\', F11,  F12
};

static const u_char shiftmap[0x59] = {
/* 00 */ 0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
/* 10 */ 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
/* 20 */ 'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
/* 30 */ 'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  0,    ' ',  0,    F1,   F2,   F3,   F4,   F5,
/* 40 */ F6,   F7,   F8,   F9,   F10,  0,    0,    HOME, UP,   PGUP, '-',  LEFT, '5',  RT,   '+',  END,
/* 50 */ DOWN, PGDN, INS,  DEL,  0,    0,    '|',  F11,  F12
};

static const u_char numpad[13] = { '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.' };

void kbd_set1_init(struct kbd_set1 *k, int consoles) {
    k->shift_l = k->shift_r = k->ctrl = k->alt = k->e0 = false;
    k->leds = NUM_LED;
    k->consoles = consoles;
}

void kbd_set1_reset(struct kbd_set1 *k) {
    k->shift_l = k->shift_r = k->ctrl = k->alt = k->e0 = false;
}

static int deliver(struct kbd_set1 *k, u8 c, struct kbd_event *ev) {
    if(c >= F1 && c < F1 + k->consoles) {   /* F1..Fn: switch console */
        ev->type = KBD_SWITCH;
        ev->console = c - F1;
        return ev->type;
    }
    ev->type = KBD_KEY;
    ev->key = c;
    return ev->type;
}

static int leds(struct kbd_set1 *k, u8 which, struct kbd_event *ev) {
    k->leds ^= which;
    ev->type = KBD_LEDS;
    ev->leds = k->leds;
    return ev->type;
}

static int handle_e0(struct kbd_set1 *k, u8 code, bool raw, struct kbd_event *ev) {
    bool brk = code & 0x80;
    code &= 0x7F;
    switch(code) {
    case 0x1D: k->ctrl = !brk; return KBD_NONE;           /* right control */
    case 0x38: k->alt = !brk; return KBD_NONE;            /* right alt */
    case 0x1C: return brk ? KBD_NONE : deliver(k, raw ? '\r' : '\n', ev);   /* keypad enter */
    case 0x35: return brk ? KBD_NONE : deliver(k, '/', ev);
    case 0x47: return brk ? KBD_NONE : deliver(k, HOME, ev);
    case 0x48: return brk ? KBD_NONE : deliver(k, UP, ev);
    case 0x49: return brk ? KBD_NONE : deliver(k, PGUP, ev);
    case 0x4B: return brk ? KBD_NONE : deliver(k, LEFT, ev);
    case 0x4D: return brk ? KBD_NONE : deliver(k, RT, ev);
    case 0x4F: return brk ? KBD_NONE : deliver(k, END, ev);
    case 0x50: return brk ? KBD_NONE : deliver(k, DOWN, ev);
    case 0x51: return brk ? KBD_NONE : deliver(k, PGDN, ev);
    case 0x52: return brk ? KBD_NONE : deliver(k, INS, ev);
    case 0x53:
        if(brk) return KBD_NONE;
        if(k->ctrl && k->alt) {
            ev->type = KBD_REBOOT;
            return ev->type;
        }
        return deliver(k, DEL, ev);
    default: return KBD_NONE;                             /* print screen, the Windows keys, media keys */
    }
}

int kbd_set1_byte(struct kbd_set1 *k, u8 code, bool raw, struct kbd_event *ev) {
    bool brk, shift;
    u8 c;

    ev->type = KBD_NONE;
    if(code == 0xE0) { k->e0 = true; return KBD_NONE; }
    if(code == 0xE1) return KBD_NONE;
    if(k->e0) { k->e0 = false; return handle_e0(k, code, raw, ev); }

    brk = code & 0x80;
    code &= 0x7F;

    switch(code) {                                          /* modifiers and locks */
    case 0x2A: k->shift_l = !brk; return KBD_NONE;
    case 0x36: k->shift_r = !brk; return KBD_NONE;
    case 0x1D: k->ctrl = !brk; return KBD_NONE;
    case 0x38: k->alt = !brk; return KBD_NONE;
    case 0x3A: return brk ? KBD_NONE : leds(k, CAPS_LED, ev);
    case 0x45: return brk ? KBD_NONE : leds(k, NUM_LED, ev);
    case 0x46: return brk ? KBD_NONE : leds(k, SCROLL_LED, ev);
    }
    if(brk || code >= sizeof(normmap)) return KBD_NONE;

    shift = k->shift_l || k->shift_r;
    if(code >= 0x47 && code <= 0x53 && (k->leds & NUM_LED) && !shift) {
        c = numpad[code - 0x47];
    } else {
        c = shift ? shiftmap[code] : normmap[code];
        if((k->leds & CAPS_LED) && c >= 'a' && c <= 'z') c -= 32;
        else if((k->leds & CAPS_LED) && c >= 'A' && c <= 'Z' && shift) c += 32;
    }
    if(!c) return KBD_NONE;
    if(k->ctrl && c < 0x80) {
        if(c >= 'a' && c <= 'z') c -= 32;
        if(c >= '@' && c <= '_') c &= 0x1F;                 /* Ctrl-C = 3, Ctrl-U = 21, ... */
        else return KBD_NONE;
    }
    if(raw && (code == 0x0E || code == 0x1C))
        c = code == 0x0E ? 0x7F : '\r';                     /* raw mode: Backspace and Enter as a terminal sends them */
    return deliver(k, c, ev);
}
