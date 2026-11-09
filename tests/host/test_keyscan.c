/* Host test of the keyboard's scan codes before their meaning (include/surfos/keyscan.h, kernel/keyboard.c's): the E0
   prefix, Pause's E1, and the keyboard's repeat of a key held down, in the byte sequences a PS/2 keyboard sends (scan
   code set 1). 11/05/26 (docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk; the review of stage 2, whose
   repeat harness these sequences come from). Built and run by `make unittest` (unittest-desk-keys). */
#include <stdio.h>
#include <string.h>
#include "../../include/surfos/keyscan.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/* the bytes fed one by one: what came out, a word a key ("1E" a make, "1E+" its repeat, "9E" a release, "148" a make
   after E0), the bytes that are no key left out */
static const char *feed(struct keyscan *k, const unsigned char *b, int n) {
    static char out[256];
    size_t at = 0;
    int i, key;
    out[0] = 0;
    for(i = 0; i < n; i++) {
        key = keyscan_byte(k, b[i]);
        if(key < 0) continue;
        at += (size_t)snprintf(out + at, sizeof(out) - at, "%s%X%s", at ? " " : "", (unsigned)key,
                               !(key & KEYSCAN_BREAK) && k->repeat ? "+" : "");
    }
    return out;
}

static void test_repeat(void) {
    static const unsigned char hold_a[] = { 0x1E, 0x1E, 0x1E, 0x9E, 0x1E };
    static const unsigned char two[] = { 0x1E, 0x30, 0x30, 0xB0, 0x9E };          /* a held, b pressed over it */
    static const unsigned char resume[] = { 0x1E, 0x1E, 0x30, 0xB0, 0x1E, 0x1E }; /* a's repeat resumes after b */
    static const unsigned char other_break[] = { 0x1E, 0x1E, 0xB0, 0x1E };         /* a release of another key */
    struct keyscan k;
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, hold_a, sizeof(hold_a)), "1E 1E+ 1E+ 9E 1E"));   /* held, let go, pressed again */
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, two, sizeof(two)), "1E 30 30+ B0 9E"));         /* the key pressed last is the one repeated */
    keyscan_reset(&k);
    /* a keyboard that goes back to repeating a held key after another was tapped: its first make again reads as a new
       press (keyscan.h says so), its repeats after it as repeats */
    CHECK(!strcmp(feed(&k, resume, sizeof(resume)), "1E 1E+ 30 B0 1E 1E+"));
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, other_break, sizeof(other_break)), "1E 1E+ B0 1E+"));    /* still held: still its repeat */
}

static void test_e0(void) {
    /* the grey Up with Num Lock on: Shift's fake prefixes around it change nothing; held, let go, pressed again */
    static const unsigned char up[] = { 0xE0, 0x2A, 0xE0, 0x48, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0xAA, 0xE0, 0x2A, 0xE0, 0x48 };
    /* Print Screen held: E0 2A E0 37, then E0 37 again and again, E0 B7 E0 AA */
    static const unsigned char prtsc[] = { 0xE0, 0x2A, 0xE0, 0x37, 0xE0, 0x37, 0xE0, 0x37, 0xE0, 0xB7, 0xE0, 0xAA };
    static const unsigned char keypad_8[] = { 0x48, 0x48, 0xE0, 0x48 };           /* the keypad's 8 is no grey Up */
    struct keyscan k;
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, up, sizeof(up)), "12A 148 148+ 1C8 1AA 12A 148"));
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, prtsc, sizeof(prtsc)), "12A 137 137+ 137+ 1B7 1AA"));     /* one press, then repeats */
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, keypad_8, sizeof(keypad_8)), "48 48+ 148"));
    /* E0 is a prefix for the next byte alone */
    keyscan_reset(&k);
    CHECK(keyscan_byte(&k, 0xE0) == -1 && k.e0 && keyscan_byte(&k, 0x1D) == 0x11D && !k.e0 &&
          keyscan_byte(&k, 0x1D) == 0x1D);
}

static void test_pause_and_errors(void) {
    /* Pause: E1 1D 45 E1 9D C5; the E1 bytes are dropped, the rest pass as keys, as kernel/keyboard.c always had them */
    static const unsigned char pause_key[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    static const unsigned char before[] = { 0x1E, 0x1E };
    struct keyscan k;
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, pause_key, sizeof(pause_key)), "1D 45 9D C5"));
    /* a byte lost to the controller's error (a's release, say): kernel/keyboard.c resets, so a's next press is new */
    keyscan_reset(&k);
    CHECK(!strcmp(feed(&k, before, sizeof(before)), "1E 1E+"));
    keyscan_reset(&k);
    CHECK(keyscan_byte(&k, 0x1E) == 0x1E && !k.repeat);
    /* without the reset, the next press would have been called a repeat: why keyboard.c forgets on an error */
    CHECK(keyscan_byte(&k, 0x1E) == 0x1E && k.repeat);
    /* an E0 cut by the error goes too: the byte after it is no grey key */
    keyscan_reset(&k);
    CHECK(keyscan_byte(&k, 0xE0) == -1);
    keyscan_reset(&k);
    CHECK(keyscan_byte(&k, 0x48) == 0x48);
}

int main(void) {
    test_repeat();
    test_e0();
    test_pause_and_errors();
    printf("test_keyscan: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
