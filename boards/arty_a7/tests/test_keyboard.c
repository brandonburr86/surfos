/*
SurfOS host test: the keyboard
--------------------
File: test_keyboard.c   Date: 10/15/26 (SurfCPU)
--------------------
boards/arty_a7/ps2set2.c and kbdset1.c: PS/2 scan code set 2 to the keys the tty gets.
The set 2 codes typed are written out here character by character (IBM's set 2),
independently of ps2set2.c's table, so a wrong translation shows as wrong text.
*/

#include <string.h>
#include "harness.h"
#include <surfos/keyboard.h>
#include "ps2set2.h"
#include "kbdset1.h"

/**** the keyboard: set 2 to tty keys ****/

/* the set 2 make code of each character a test types (IBM's set 2, written out independently of ps2set2.c) */
struct key2 { char c; u8 code; int shift; };
static const struct key2 keys2[] = {
    {'a',0x1C,0},{'b',0x32,0},{'c',0x21,0},{'d',0x23,0},{'e',0x24,0},{'f',0x2B,0},{'g',0x34,0},{'h',0x33,0},
    {'i',0x43,0},{'j',0x3B,0},{'k',0x42,0},{'l',0x4B,0},{'m',0x3A,0},{'n',0x31,0},{'o',0x44,0},{'p',0x4D,0},
    {'q',0x15,0},{'r',0x2D,0},{'s',0x1B,0},{'t',0x2C,0},{'u',0x3C,0},{'v',0x2A,0},{'w',0x1D,0},{'x',0x22,0},
    {'y',0x35,0},{'z',0x1A,0},{'1',0x16,0},{'2',0x1E,0},{'3',0x26,0},{'4',0x25,0},{'5',0x2E,0},{'6',0x36,0},
    {'7',0x3D,0},{'8',0x3E,0},{'9',0x46,0},{'0',0x45,0},{'-',0x4E,0},{'=',0x55,0},{'[',0x54,0},{']',0x5B,0},
    {'\\',0x5D,0},{';',0x4C,0},{'\'',0x52,0},{'`',0x0E,0},{',',0x41,0},{'.',0x49,0},{'/',0x4A,0},{' ',0x29,0},
    {'\n',0x5A,0},{'\t',0x0D,0},{'\b',0x66,0},
    {'!',0x16,1},{'@',0x1E,1},{'#',0x26,1},{'$',0x25,1},{'%',0x2E,1},{'^',0x36,1},{'&',0x3D,1},{'*',0x3E,1},
    {'(',0x46,1},{')',0x45,1},{'_',0x4E,1},{'+',0x55,1},{'{',0x54,1},{'}',0x5B,1},{'|',0x5D,1},{':',0x4C,1},
    {'"',0x52,1},{'~',0x0E,1},{'<',0x41,1},{'>',0x49,1},{'?',0x4A,1},
};

struct kbd {
    struct ps2_set2 s2;
    struct kbd_set1 s1;
    int raw;
    char out[256];
    int n, switches, last_console, leds_events, reboots, replies;
    u8 leds;
};

static void kbd_init(struct kbd *k) {
    memset(k, 0, sizeof(*k));
    ps2_set2_reset(&k->s2);
    kbd_set1_init(&k->s1, 4);
}

static void feed(struct kbd *k, u8 b) {
    u8 out[2];
    int reply, n, i;
    struct kbd_event ev;
    n = ps2_set2_byte(&k->s2, b, out, &reply);
    if(reply >= 0) k->replies++;
    for(i = 0; i < n; i++) {
        switch(kbd_set1_byte(&k->s1, out[i], k->raw, &ev)) {
        case KBD_KEY: if(k->n < (int)sizeof(k->out) - 1) k->out[k->n++] = (char)ev.key; break;
        case KBD_SWITCH: k->switches++; k->last_console = ev.console; break;
        case KBD_LEDS: k->leds_events++; k->leds = ev.leds; break;
        case KBD_REBOOT: k->reboots++; break;
        }
    }
}

static void press(struct kbd *k, u8 code) { feed(k, code); }
static void release(struct kbd *k, u8 code) { feed(k, 0xF0); feed(k, code); }
static void tap(struct kbd *k, u8 code) { press(k, code); release(k, code); }
static void etap(struct kbd *k, u8 code) { feed(k, 0xE0); feed(k, code); feed(k, 0xE0); feed(k, 0xF0); feed(k, code); }

/* a capital is Shift and the letter's key; a character with no key in the table is a test error */
static void type(struct kbd *k, const char *s) {
    for(; *s; s++) {
        u_int i;
        char c = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
        int shift;
        for(i = 0; i < sizeof(keys2) / sizeof(keys2[0]) && keys2[i].c != c; i++) ;
        if(i == sizeof(keys2) / sizeof(keys2[0])) { check(0, "a character the test cannot type"); continue; }
        shift = keys2[i].shift || c != *s;
        if(shift) press(k, 0x12);
        tap(k, keys2[i].code);
        if(shift) release(k, 0x12);
    }
}

static void test_keyboard(unsigned seed) {
    (void)seed;
    struct kbd k;
    const char *text = "the quick brown fox jumps over the lazy dog 0123456789 -=[]\\;',./`\n"
                       "THE QUICK BROWN FOX! @#$%^&*() _+{}|:\"<>?~\t";
    const char *ctrl = "\x03\x15";
    kbd_init(&k);
    type(&k, text);
    k.out[k.n] = 0;
    check(!strcmp(k.out, text), "typing every character, shifted and not, through set 2 -> set 1 -> keys");
    check(ps2_set2_translate(0x83) == 0x41 && ps2_set2_translate(0x76) == 0x01 && ps2_set2_translate(0x80) == 0,
          "F7's set 2 code 0x83 and Esc translate; a code no key has does not");

    kbd_init(&k);
    press(&k, 0x14); tap(&k, 0x21); tap(&k, 0x3C); release(&k, 0x14);      /* Ctrl+C, Ctrl+U */
    k.out[k.n] = 0;
    check(!strcmp(k.out, ctrl), "Ctrl+C and Ctrl+U are control characters");

    kbd_init(&k);
    tap(&k, 0x58);                                                          /* Caps Lock */
    type(&k, "ab");
    press(&k, 0x12); tap(&k, 0x1C); release(&k, 0x12);                      /* Shift+a under Caps Lock */
    k.out[k.n] = 0;
    check(!strcmp(k.out, "ABa") && k.leds_events == 1 && (k.leds & CAPS_LED) && (k.leds & NUM_LED),
          "Caps Lock: letters upper, Shift lowers them, the LEDs event carries Caps and Num");

    kbd_init(&k);
    etap(&k, 0x75); etap(&k, 0x72); etap(&k, 0x6B); etap(&k, 0x74);         /* arrows */
    etap(&k, 0x6C); etap(&k, 0x69); etap(&k, 0x7D); etap(&k, 0x7A); etap(&k, 0x70); etap(&k, 0x71);
    check(k.n == 10 && (u8)k.out[0] == UP && (u8)k.out[1] == DOWN && (u8)k.out[2] == LEFT && (u8)k.out[3] == RT &&
          (u8)k.out[4] == HOME && (u8)k.out[5] == END && (u8)k.out[6] == PGUP && (u8)k.out[7] == PGDN &&
          (u8)k.out[8] == INS && (u8)k.out[9] == DEL, "the E0 keys: arrows, Home, End, PgUp, PgDn, Insert, Delete");

    kbd_init(&k);
    tap(&k, 0x6C); tap(&k, 0x75); tap(&k, 0x71);                            /* keypad 7 8 . with Num Lock (on at start) */
    tap(&k, 0x77);                                                          /* Num Lock off */
    tap(&k, 0x75);                                                          /* keypad 8 is Up now */
    check(k.n == 4 && !memcmp(k.out, "78.", 3) && (u8)k.out[3] == UP && !(k.leds & NUM_LED), "the keypad follows Num Lock");

    kbd_init(&k);
    tap(&k, 0x05); tap(&k, 0x04);                                           /* F1, F3 */
    check(k.switches == 2 && k.last_console == 2 && k.n == 0, "F1 and F3 switch consoles");
    tap(&k, 0x03);                                                          /* F5: a key code */
    check(k.n == 1 && (u8)k.out[0] == F5, "F5 is a key code for the tty");

    kbd_init(&k);
    press(&k, 0x14); press(&k, 0x11); etap(&k, 0x71);                       /* Ctrl+Alt+Del */
    check(k.reboots == 1 && k.n == 0, "Ctrl+Alt+Del asks for a reboot");

    kbd_init(&k);
    { static const u8 pause[] = { 0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77 };
      u_int i; for(i = 0; i < sizeof(pause); i++) feed(&k, pause[i]); }
    feed(&k, 0xE0); feed(&k, 0x12); feed(&k, 0xE0); feed(&k, 0x7C);         /* Print Screen */
    feed(&k, 0xE0); feed(&k, 0xF0); feed(&k, 0x7C); feed(&k, 0xE0); feed(&k, 0xF0); feed(&k, 0x12);
    type(&k, "a");
    check(k.n == 1 && k.out[0] == 'a' && k.leds_events == 0 && !k.s1.shift_l,
          "Pause is swallowed whole (no Num Lock toggle), Print Screen does nothing, typing goes on");

    kbd_init(&k);
    feed(&k, PS2_BAT_OK); feed(&k, PS2_ACK); feed(&k, PS2_RESEND);
    type(&k, "z");
    check(k.replies == 3 && k.n == 1 && k.out[0] == 'z', "the keyboard's replies (BAT, ACK, resend) are not keys");

    kbd_init(&k);
    press(&k, 0x12); feed(&k, 0xE0);                                        /* Shift held, a prefix half received */
    ps2_set2_reset(&k.s2); kbd_set1_reset(&k.s1);                           /* a line error */
    type(&k, "q");
    check(k.n == 1 && k.out[0] == 'q', "after a line error the prefix and the held Shift are forgotten");

    kbd_init(&k);
    k.raw = 1;
    tap(&k, 0x66); tap(&k, 0x5A); etap(&k, 0x5A);                           /* Backspace, Enter, keypad Enter */
    check(k.n == 3 && (u8)k.out[0] == 0x7F && k.out[1] == '\r' && k.out[2] == '\r', "raw mode: Backspace 0x7f, Enter CR");
}

TEST(test_keyboard)
