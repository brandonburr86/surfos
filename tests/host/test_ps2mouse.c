/*
SurfOS PS/2 Mouse Tests
--------------------
File: test_ps2mouse.c Date: 10/10/26
--------------------
Literal packets exercise signed motion, buttons, resynchronization and loss.
*/
#include <sys/ps2mouse.h>

int printf(const char *, ...);
static unsigned checks, failures;
static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static bool packet(struct ps2mouse *m, u8 a, u8 b, u8 c, u32 now,
                   struct ps2mouse_event *event) {
    check(!ps2mouse_byte(m, a, now, event), "header alone produces no event");
    check(!ps2mouse_byte(m, b, now + 1, event), "two bytes produce no event");
    return ps2mouse_byte(m, c, now + 2, event);
}

int main(void) {
    struct ps2mouse m;
    struct ps2mouse_event e;
    unsigned i;
    ps2mouse_reset(&m);
    check(packet(&m, 0x09, 10, 20, 0, &e) && e.dx == 10 && e.dy == -20 && e.buttons == 1,
          "positive motion uses screen coordinates and left button");
    check(packet(&m, 0x3e, 0xf6, 0xec, 10, &e) && e.dx == -10 && e.dy == 20 && e.buttons == 6,
          "negative motion, right and middle buttons");
    check(packet(&m, 0x08, 0, 0, 20, &e) && e.dx == 0 && e.dy == 0 && e.buttons == 0,
          "button release with no motion is reported");
    check(packet(&m, 0x08, 0xff, 0xff, 30, &e) && e.dx == 255 && e.dy == -255,
          "ninth sign bit belongs to the header, not the coordinate byte");
    check(packet(&m, 0x38, 1, 1, 40, &e) && e.dx == -255 && e.dy == 255,
          "negative coordinates span more than a signed byte");
    check(packet(&m, 0x38, 0, 0, 45, &e) && e.dx == 0 && e.dy == 0,
          "stale sign bits on zero movement do not jump the cursor");
    check(packet(&m, 0x08, 0xfa, 0xaa, 50, &e) && e.dx == 250 && e.dy == -170,
          "ACK and BAT values inside a packet remain ordinary coordinate bytes");
    check(packet(&m, 0xc9, 200, 200, 60, &e) && e.dx == 0 && e.dy == 0 && e.buttons == 1,
          "overflow suppresses unreliable motion but keeps button changes");
    check(!ps2mouse_byte(&m, 0x00, 70, &e) && !ps2mouse_byte(&m, 0x07, 71, &e),
          "bytes without synchronization bit cannot start a packet");
    check(!ps2mouse_byte(&m, 0xfa, 72, &e) && !ps2mouse_byte(&m, 0xfe, 73, &e) &&
          !ps2mouse_byte(&m, 0xaa, 74, &e), "stray device replies are not packet headers");
    check(packet(&m, 0x08, 8, 8, 80, &e) && e.dx == 8 && e.dy == -8,
          "coordinate bytes with the sync bit do not restart packets");
    check(!ps2mouse_byte(&m, 0x09, 90, &e) && !ps2mouse_byte(&m, 44, 91, &e),
          "truncated packet waits for the last byte");
    check(packet(&m, 0x08, 3, 4, 300, &e) && e.dx == 3 && e.dy == -4 && !e.buttons,
          "a partial packet expires and the next header resynchronizes");
    check(!ps2mouse_byte(&m, 0x09, 310, &e), "begin a packet before a controller error");
    ps2mouse_reset(&m);
    check(packet(&m, 0x08, 7, 9, 320, &e) && e.dx == 7 && e.dy == -9 && !e.buttons,
          "controller parity or timeout reset drops partial state");
    check(!ps2mouse_byte(&m, 0x09, 0xfffffff0U, &e), "begin a packet near clock wrap");
    check(packet(&m, 0x08, 2, 6, 200, &e) && e.dx == 2 && e.dy == -6,
          "partial packet expiry survives clock wrap");
    for(i = 0; i < 256; i++) {
        check(packet(&m, 0x08, i, i, 400 + i * 4, &e) && e.dx == (int)i && e.dy == -(int)i,
              "every positive coordinate byte decodes");
    }
    for(i = 1; i < 256; i++) {
        check(packet(&m, 0x38, i, i, 1500 + i * 4, &e) && e.dx == (int)i - 256 && e.dy == 256 - (int)i,
              "every negative coordinate byte decodes");
    }
    printf("PS2 MOUSE UNIT TESTS %s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
