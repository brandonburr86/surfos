/*
SurfOS user program wview: tests of its model
----------------------
File: test_wview.c  Date: 11/05/26
----------------------
user/wview/view.c and folder.c as they are, on the host:

- the views as test_guimedia's checks had them for the desktop's Image Viewer, in the window's content coordinates
  (the same room, 558 x 318): Fit enlarging by a whole number and shrinking, the actual size, each zoom step and
  their ends, the middle kept as the zoom changes, the arrows, Home, End, Page Up and Down, the wheel, a drag, the
  double-click, Fit following a new size;
- the toolbar: each button where it is drawn, what it does, when it may be used;
- view_paint(): pixel for pixel against a picture of four colours and against an average worked out here, a picture
  partly transparent over its squares, every zoom step at many places with nothing written outside the room;
- the words: the title, the status bar, the details (the date by the calendar), what a key asks of main.c;
- the folder: which names are pictures, the order (case, digits as numbers, ties), the limits, finding a name, the
  steps around the ends.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "view.h"

static int failures, checks;

static void check(int ok, const char *what, int line) {
    checks++;
    if(ok) return;
    failures++;
    printf("FAIL line %d: %s\n", line, what);
}
#define CHECK(x) check(!!(x), #x, __LINE__)

static struct view v;

static int key(int k) { return view_key(&v, k); }

/* a press of the left button and its release */
static int click(int x, int y) {
    int r = view_press(&v, x, y);
    view_release(&v);
    return r;
}

/* the middle of a toolbar button */
static int tool(int t) {
    struct view_tool b;
    view_tool(t, &b);
    return click(b.x + b.w / 2, b.y + b.h / 2);
}

static void views(void) {
    struct view_room r;
    int x, y, w, h, i;
    view_init(&v, VIEW_W, VIEW_H);
    view_room(&v, &r);
    CHECK(r.x == 0 && r.y == 31 && r.w == 558 && r.h == 318);
    CHECK(view_zoom(&v) == VIEW_ONE && v.fit);
    /* nothing to move or zoom without a picture */
    CHECK(key('+') == VIEW_NOTHING && key(VIEW_KEY_RIGHT) == VIEW_NOTHING && !view_tool_on(&v, VT_FIT));
    CHECK(key('o') == VIEW_OPEN && key(15) == VIEW_OPEN && view_tool_on(&v, VT_OPEN));
    view_picture(&v, 42, 30);
    /* Fit: enlarged by the largest whole number that fits the room: 10 times, centred */
    view_place(&v, &x, &y, &w, &h);
    CHECK(view_zoom(&v) == 10 * VIEW_ONE && w == 420 && h == 300 && x == 69 && y == 31 + 9);
    /* the actual size (1), centred; zoom steps (+ and -) about the middle */
    CHECK(key('1') == VIEW_DRAW);
    view_place(&v, &x, &y, &w, &h);
    CHECK(!v.fit && view_zoom(&v) == VIEW_ONE && w == 42 && h == 30 && x == 258 && y == 31 + 144);
    key('+'); CHECK(view_zoom(&v) == VIEW_ONE * 3 / 2);
    key('='); CHECK(view_zoom(&v) == 2 * VIEW_ONE);
    key('-'); CHECK(view_zoom(&v) == VIEW_ONE * 3 / 2);
    key('-'); key('_'); CHECK(view_zoom(&v) == VIEW_ONE * 2 / 3);
    for(i = 0; i < 30; i++) key('-');
    CHECK(view_zoom(&v) == VIEW_ONE / 64);
    view_place(&v, &x, &y, &w, &h); CHECK(w == 1 && h == 1);      /* 42 / 64: never less than a pixel */
    for(i = 0; i < 30; i++) key('+');
    CHECK(view_zoom(&v) == 16 * VIEW_ONE);
    /* 16 times: 672 x 480 in a room of 558 x 318; the middle stays where it was */
    key('f'); CHECK(v.fit && view_zoom(&v) == 10 * VIEW_ONE);
    key('+'); CHECK(view_zoom(&v) == 12 * VIEW_ONE);              /* 504 x 360: taller than the room, centred across */
    CHECK(v.pan_x == 0 && v.pan_y == 21);
    key('+'); CHECK(view_zoom(&v) == 16 * VIEW_ONE && v.pan_x == 57 && v.pan_y == 81);
    view_place(&v, &x, &y, &w, &h);
    CHECK(w == 672 && h == 480 && x == -57 && y == 31 - 81);
    /* moving it: the arrows, Home, End, Page Up and Down, the wheel, a drag */
    key(VIEW_KEY_HOME); CHECK(v.pan_x == 0 && v.pan_y == 0);
    key(VIEW_KEY_LEFT); key(VIEW_KEY_UP); CHECK(v.pan_x == 0 && v.pan_y == 0);
    key(VIEW_KEY_RIGHT); key(VIEW_KEY_DOWN); CHECK(v.pan_x == 32 && v.pan_y == 32);
    key(VIEW_KEY_END); CHECK(v.pan_x == 672 - 558 && v.pan_y == 480 - 318);
    key(VIEW_KEY_RIGHT); CHECK(v.pan_x == 114);
    key(VIEW_KEY_PGUP); CHECK(v.pan_y == 0);
    key(VIEW_KEY_PGDN); CHECK(v.pan_y == 162);
    view_wheel(&v, -3); CHECK(v.pan_y == 162 - 48);                /* a notch away from the user */
    v.now = 5000;
    CHECK(view_press(&v, 300, 200) == VIEW_NOTHING && v.dragging);
    view_drag(&v, 340, 230); CHECK(v.pan_x == 114 - 40 && v.pan_y == 114 - 30);
    view_drag(&v, 1340, 1230); CHECK(v.pan_x == 0 && v.pan_y == 0);
    view_release(&v); CHECK(!v.dragging);
    /* a double-click switches between Fit and the actual size; far apart in time or place, it is two presses */
    v.now = 7000;
    click(300, 200); v.now += 100; click(301, 200);
    CHECK(v.fit);
    v.now += 1000;
    click(300, 200); v.now += 100; click(300, 202);
    CHECK(!v.fit && view_zoom(&v) == VIEW_ONE);
    v.now += 1000;
    click(300, 200); v.now += 600; click(300, 200);
    CHECK(!v.fit);
    v.now += 1000;
    click(300, 100); v.now += 100; click(300, 110);
    CHECK(!v.fit);
    /* the wheel across, when the picture is no taller than the room */
    key('1'); key('+'); key('+'); key('+'); key('+');                /* 4 times: 168 x 120 */
    CHECK(view_zoom(&v) == 4 * VIEW_ONE);
    view_wheel(&v, 3); CHECK(v.pan_x == 0 && v.pan_y == 0);         /* 168 fits across */
    key('+'); key('+'); key('+'); key('+');                          /* 16 times */
    key(VIEW_KEY_HOME);
    view_wheel(&v, 3); CHECK(v.pan_y == 48 && v.pan_x == 0);
    /* Fit follows the window: a room of 958 x 404 */
    key('f');
    view_resize(&v, 958, 457);
    CHECK(v.fit && view_zoom(&v) == 13 * VIEW_ONE);
    view_place(&v, &x, &y, &w, &h); CHECK(w == 546 && h == 390 && x == (958 - 546) / 2 && y == 31 + 7);
    /* a picture larger than the room: shrunk to fit, by the side that is tighter */
    view_resize(&v, VIEW_W, VIEW_H);
    view_picture(&v, 4000, 1000);
    CHECK(v.fit && view_zoom(&v) == (558u << 16) / 4000);
    view_place(&v, &x, &y, &w, &h); CHECK(w == 557 && h == 139 && x == 0 && y == 31 + (318 - 139) / 2);
    view_picture(&v, 4096, 4096);
    CHECK(view_zoom(&v) == (318u << 16) / 4096);
    key('-'); CHECK(view_zoom(&v) == VIEW_ONE / 16);
    /* the largest shown: 4096 at 16 times is 65536 across. Each step keeps the picture's point in the middle of the room
       where it was, to within the rounding of a 16.16 fraction and a pixel, at the far end too (middle()'s 32 bits) */
    view_picture(&v, 4096, 4096);
    key('1');
    for(i = 0; i < 4; i++) key('+');
    CHECK(view_zoom(&v) == 4 * VIEW_ONE);
    key(VIEW_KEY_END);
    CHECK(v.pan_x == 4096 * 4 - 558 && v.pan_y == 4096 * 4 - 318);
    for(i = 0; i < 8; i++) {
        double before = (v.pan_x + 279) / (4096.0 * view_zoom(&v) / VIEW_ONE), after;
        key(i < 4 ? '+' : '-');
        after = (v.pan_x + 279) / (4096.0 * view_zoom(&v) / VIEW_ONE);
        CHECK(after - before < 2.0 / 4096 && before - after < 2.0 / 4096);
    }
    key('+');
    key('+');
    key('+');
    key('+');
    view_place(&v, &x, &y, &w, &h);
    CHECK(view_zoom(&v) == 16 * VIEW_ONE && w == 65536 && h == 65536 && x == -v.pan_x && v.pan_x > 60000 && v.pan_x <= 65536 - 558);
    key(VIEW_KEY_END);
    CHECK(v.pan_x == 65536 - 558 && v.pan_y == 65536 - 318);
    key('-');                                                       /* from the very end: still the same point */
    CHECK(view_zoom(&v) == 12 * VIEW_ONE && v.pan_x == 4096 * 12 - 558 && v.pan_y == 4096 * 12 - 318);
    view_picture(&v, 4096, 1);                                      /* so narrow a room shrinks it past 1/64: kept at that */
    view_resize(&v, 40, VIEW_H);
    CHECK(view_zoom(&v) == VIEW_ZOOM_MIN);
    view_resize(&v, VIEW_W, VIEW_H);
    /* a press outside the room and the toolbar does nothing */
    v.now += 1000;
    CHECK(click(10, VIEW_H - 5) == VIEW_NOTHING);
}

static void toolbar(void) {
    struct view_tool t, u;
    int i, x;
    /* 11/05/26 (the review): the window fits the smallest desktop, 640 x 400 (a menu bar of 28 and a taskbar of 26), as
       the desktop's Image Viewer's did (test_guimedia's check, whose window this is now): the narrowest with its frame of
       2 with room to spare, the first size whole, and the lowest under the title bar of 28 and the frame's last line */
    CHECK(VIEW_MIN_W + 2 <= 640 - 48 && VIEW_W + 2 <= 640 && VIEW_MIN_H + 28 + 1 <= 400 - 28 - 26);
    view_init(&v, VIEW_W, VIEW_H);
    /* the desktop's toolbar: 8 from the window's edge (7 in its content), 4 apart, 22 high, a character 8 wide and 14 more */
    for(i = 0, x = 7; i < VT_COUNT; i++) {
        view_tool(i, &t);
        CHECK(t.x == x && t.y == 4 && t.h == 22 && t.w >= 22);
        CHECK(view_tool_at(t.x, t.y) == i && view_tool_at(t.x + t.w - 1, t.y + t.h - 1) == i);
        CHECK(view_tool_at(t.x - 1, t.y + 5) != i && view_tool_at(t.x + 5, t.y - 1) == -1 && view_tool_at(t.x + 5, t.y + 22) == -1);
        x += t.w + 4;
    }
    CHECK(x + 3 == VIEW_MIN_W);                                     /* the least window holds them all, as the desktop's */
    view_tool(VT_COPY, &u);
    CHECK(view_tool_at(u.x + u.w + 2, 10) == -1);
    CHECK(!strcmp(view_tool_label(VT_OPEN), "Open") && !strcmp(view_tool_label(VT_PREVIOUS), "\x11") &&
          !strcmp(view_tool_label(VT_NEXT), "\x10") && !strcmp(view_tool_label(VT_COPY), "Copy path"));
    for(i = 0; i < VT_COUNT; i++) {                                 /* a label's cells are its bytes: gw_button's */
        view_tool(i, &t);
        CHECK(t.w == (int)strlen(view_tool_label(i)) * 8 + 14);
    }
    /* with nothing shown, only Open; and N and P when the folder of the file that was no picture has others */
    for(i = 0; i < VT_COUNT; i++) CHECK(view_tool_on(&v, i) == (i == VT_OPEN));
    CHECK(tool(VT_OPEN) == VIEW_OPEN && tool(VT_NEXT) == VIEW_NOTHING && tool(VT_COPY) == VIEW_NOTHING && !v.details);
    v.pictures = 2;
    CHECK(view_tool_on(&v, VT_NEXT) && tool(VT_NEXT) == VIEW_NEXT && key('p') == VIEW_PREVIOUS && key('i') == VIEW_NOTHING);
    v.pictures = 0;
    /* a picture of a file alone in its folder: no stepping */
    view_picture(&v, 42, 30);
    snprintf(v.file.path, sizeof(v.file.path), "/usb1p1/pictures/q.png");
    v.file.count = 1;
    v.file.index = 1;
    v.pictures = 1;
    CHECK(!view_tool_on(&v, VT_NEXT) && !view_tool_on(&v, VT_PREVIOUS) && view_tool_on(&v, VT_FIT) && view_tool_on(&v, VT_COPY));
    v.file.count = 3;
    v.pictures = 3;
    CHECK(tool(VT_NEXT) == VIEW_NEXT && tool(VT_PREVIOUS) == VIEW_PREVIOUS && tool(VT_COPY) == VIEW_COPY);
    CHECK(tool(VT_ACTUAL) == VIEW_DRAW && !v.fit && view_zoom(&v) == VIEW_ONE);
    CHECK(tool(VT_IN) == VIEW_DRAW && view_zoom(&v) == VIEW_ONE * 3 / 2);
    CHECK(tool(VT_OUT) == VIEW_DRAW && view_zoom(&v) == VIEW_ONE);
    CHECK(tool(VT_FIT) == VIEW_DRAW && v.fit);
    CHECK(tool(VT_DETAILS) == VIEW_DRAW && v.details);
    CHECK(click(300, 200) == VIEW_DRAW && !v.details);              /* a press in the room puts them away */
    CHECK(tool(VT_DETAILS) == VIEW_DRAW && v.details && key(27) == VIEW_DRAW && !v.details);
    CHECK(key(27) == VIEW_CLOSE && key(23) == VIEW_CLOSE);
    /* the keys of the new things */
    CHECK(key('n') == VIEW_NEXT && key(' ') == VIEW_NEXT && key('N') == VIEW_NEXT);
    CHECK(key('p') == VIEW_PREVIOUS && key(8) == VIEW_PREVIOUS && key(127) == VIEW_PREVIOUS);
    CHECK(key(3) == VIEW_COPY && key('i') == VIEW_DRAW && v.details && key('I') == VIEW_DRAW && !v.details);
    CHECK(key('x') == VIEW_NOTHING && key(VIEW_KEY_DOWN) == VIEW_DRAW);
    /* held down: moving, zooming and stepping go on; opening, details, copying and closing act once */
    CHECK(view_repeats(VIEW_KEY_LEFT) && view_repeats(VIEW_KEY_PGDN) && view_repeats('+') && view_repeats('-') &&
          view_repeats('n') && view_repeats(' ') && view_repeats('p') && view_repeats(127));
    CHECK(!view_repeats('o') && !view_repeats(15) && !view_repeats('i') && !view_repeats(3) && !view_repeats(27) &&
          !view_repeats(23) && !view_repeats('f') && !view_repeats('1') && !view_repeats(VIEW_KEY_HOME));
    /* a message stays until the next thing done */
    snprintf(v.status, sizeof(v.status), "Copied the path");
    key('f');
    CHECK(!v.status[0]);
}

/**** painting ****/

#define TL 0x00102030u
#define TR 0x00405060u
#define BL 0x00708090u
#define BR 0x00a0b0c0u
#define BACK 0x12345678u                /* what the room holds before: left alone where the picture is not */
#define STRIDE 600
#define ROWS 400

static unsigned screen[STRIDE * ROWS];
static unsigned quad[42 * 30];

static void quadrants(void) {
    int x, y;
    for(y = 0; y < 30; y++)
        for(x = 0; x < 42; x++) quad[y * 42 + x] = y < 15 ? (x < 21 ? TL : TR) : (x < 21 ? BL : BR);
}

static void clear(void) {
    int i;
    for(i = 0; i < STRIDE * ROWS; i++) screen[i] = BACK;
}

static unsigned at(int x, int y) { return screen[y * STRIDE + x]; }

/* nothing outside the room was written, nor outside the picture's place in it */
static int kept_outside(void) {
    struct view_room r;
    int x, y, px, py, pw, ph;
    view_room(&v, &r);
    view_place(&v, &px, &py, &pw, &ph);
    for(y = 0; y < ROWS; y++)
        for(x = 0; x < STRIDE; x++) {
            int in = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h && x >= px && x < px + pw && y >= py && y < py + ph;
            if(!in && at(x, y) != BACK) return 0;
            if(in && at(x, y) == BACK) return 0;
        }
    return 1;
}

static void painting(void) {
    static unsigned big[256 * 256], alpha[4];
    int x, y, pw, ph, i, k, ok;
    quadrants();
    view_init(&v, VIEW_W, VIEW_H);
    view_picture(&v, 42, 30);
    clear();
    view_paint(&v, quad, screen, STRIDE);
    view_place(&v, &x, &y, &pw, &ph);
    CHECK(kept_outside());
    CHECK(at(x + 5, y + 5) == TL && at(x + pw - 5, y + 5) == TR && at(x + 5, y + ph - 5) == BL && at(x + pw - 5, y + ph - 5) == BR);
    CHECK(at(x + 209, y + 149) == TL && at(x + 210, y + 149) == TR && at(x + 209, y + 150) == BL && at(x + 210, y + 150) == BR);
    key('1');
    clear();
    view_paint(&v, quad, screen, STRIDE);
    view_place(&v, &x, &y, &pw, &ph);
    CHECK(kept_outside());
    CHECK(at(x, y) == TL && at(x + 41, y) == TR && at(x, y + 29) == BL && at(x + 41, y + 29) == BR);
    CHECK(at(x + 20, y + 14) == TL && at(x + 21, y + 14) == TR && at(x + 21, y + 15) == BR);
    /* every step, at many places: nothing outside, and each pixel one of the four or an average of them */
    for(k = 0; k < 17 + 1; k++) {
        if(k) key('+');
        else for(i = 0; i < 30; i++) key('-');
        for(i = 0; i < 5; i++) {
            key(VIEW_KEY_RIGHT);
            key(VIEW_KEY_DOWN);
            clear();
            view_paint(&v, quad, screen, STRIDE);
            CHECK(kept_outside());
        }
    }
    /* shrunk: each pixel the average of those it stands for. 256 x 256 of x + y at a half: 128 x 128 shown */
    for(y = 0; y < 256; y++) for(x = 0; x < 256; x++) big[y * 256 + x] = (unsigned)((x + y) / 2) * 0x010101u;
    view_init(&v, VIEW_W, VIEW_H);
    view_picture(&v, 256, 256);
    key('1');
    key('-');
    key('-');
    CHECK(view_zoom(&v) == VIEW_ONE / 2);
    clear();
    view_paint(&v, big, screen, STRIDE);
    view_place(&v, &x, &y, &pw, &ph);
    CHECK(pw == 128 && ph == 128 && kept_outside());
    for(ok = 1, i = 0; i < 128; i += 7)
        for(k = 0; k < 128; k += 5) {
            unsigned sum = 0, want;
            int a, b;
            for(a = 0; a < 2; a++) for(b = 0; b < 2; b++) sum += (unsigned)((k * 2 + a + i * 2 + b) / 2);
            want = (sum + 2) / 4 * 0x010101u;
            if(at(x + k, y + i) != want) ok = 0;
        }
    CHECK(ok);
    /* transparency: solid as it is, not there the squares, half there between them */
    alpha[0] = 0x00ff0000u;
    alpha[1] = 0xff00ff00u;
    alpha[2] = 0x800000ffu;
    alpha[3] = 0x40ffffffu;
    view_init(&v, VIEW_W, VIEW_H);
    view_picture(&v, 2, 2);
    key('1');
    for(i = 0; i < 3; i++) key('+');
    CHECK(view_zoom(&v) == 3 * VIEW_ONE);
    clear();
    view_paint(&v, alpha, screen, STRIDE);
    view_place(&v, &x, &y, &pw, &ph);
    CHECK(pw == 6 && ph == 6 && kept_outside());
    CHECK(at(x, y) == 0xff0000u);
    {
        unsigned k1 = ((x + 3) >> 3 ^ y >> 3) & 1 ? 0xC9CFC4u : 0xF4F1E6u, k2 = (x >> 3 ^ (y + 3) >> 3) & 1 ? 0xC9CFC4u : 0xF4F1E6u;
        unsigned c = at(x, y + 3), want = 0;
        int s;
        CHECK(at(x + 3, y) == k1);                              /* not there: the square it is in */
        for(s = 0; s <= 16; s += 8) want |= (((0xffu >> s & 255) * 127 + (k2 >> s & 255) * 128 + 127) / 255) << s;
        CHECK(c == want);                                       /* half there: the blue over the square, by the eighth */
    }
    CHECK(at(x + 3, y + 3) != 0xffffffu && (at(x + 3, y + 3) & 0xff) >= 0xf0);
    /* no picture, no pixels: nothing painted */
    view_init(&v, VIEW_W, VIEW_H);
    clear();
    view_paint(&v, quad, screen, STRIDE);
    view_picture(&v, 42, 30);
    view_paint(&v, NULL, screen, STRIDE);
    for(ok = 1, i = 0; i < STRIDE * ROWS; i++) if(screen[i] != BACK) ok = 0;
    CHECK(ok);
}

/**** words ****/

static void words(void) {
    char t[96], left[VIEW_STATUS + 64], right[32], lines[VIEW_DETAILS][VIEW_DETAIL];
    int n;
    view_init(&v, VIEW_W, VIEW_H);
    view_title(&v, t, sizeof(t)); CHECK(!strcmp(t, "Image Viewer"));
    view_status(&v, left, sizeof(left), right, sizeof(right)); CHECK(!strcmp(left, "No picture") && !right[0]);
    CHECK(view_details(&v, lines) == 0);
    snprintf(v.file.path, sizeof(v.file.path), "/usb1p1/sea/wave.png");
    snprintf(v.file.kind, sizeof(v.file.kind), "PNG");
    v.file.size = 1234567;
    v.file.mtime = 1793895200u;         /* 2026-11-05 16:13:20 */
    v.file.w = 4000;
    v.file.h = 3000;
    v.file.index = 3;
    v.file.count = 12;
    view_picture(&v, 4000, 3000);
    view_title(&v, t, sizeof(t)); CHECK(!strcmp(t, "wave.png - Image Viewer"));
    view_status(&v, left, sizeof(left), right, sizeof(right));
    CHECK(!strcmp(left, "wave.png  4000 x 3000, PNG  (3 of 12)") && !strcmp(right, "Fit 11%"));
    key('1');
    view_status(&v, left, sizeof(left), right, sizeof(right)); CHECK(!strcmp(right, "100%"));
    n = view_details(&v, lines);
    CHECK(n == 8);
    CHECK(!strcmp(lines[0], "wave.png") && !strcmp(lines[1], "Folder: /usb1p1/sea") && !strcmp(lines[2], "Kind: PNG picture"));
    CHECK(!strcmp(lines[3], "File: 1,234,567 bytes") && !strcmp(lines[4], "Picture: 4000 x 3000 pixels"));
    CHECK(!strcmp(lines[5], "Changed: 2026-11-05 16:13") && !strcmp(lines[6], "Shown: 100%") && !strcmp(lines[7], "In its folder: 3 of 12"));
    v.file.count = 1;
    view_status(&v, left, sizeof(left), right, sizeof(right)); CHECK(!strcmp(left, "wave.png  4000 x 3000, PNG"));
    snprintf(v.file.kind, sizeof(v.file.kind), "24-bit BMP");      /* as the desktop's viewer began it: "24-bit" */
    view_status(&v, left, sizeof(left), right, sizeof(right)); CHECK(!strcmp(left, "wave.png  4000 x 3000, 24-bit BMP"));
    view_details(&v, lines); CHECK(!strcmp(lines[2], "Kind: 24-bit BMP picture"));
    snprintf(v.file.kind, sizeof(v.file.kind), "PNG");
    snprintf(v.status, sizeof(v.status), "next.png: not a picture");
    view_status(&v, left, sizeof(left), right, sizeof(right)); CHECK(!strcmp(left, "next.png: not a picture") && !strcmp(right, "100%"));
    /* the calendar: the start, a leap day, the end of a year, 2100 not a leap year, the last second of 32 bits */
    v.file.size = 7;
    v.file.mtime = 1;
    n = view_details(&v, lines); CHECK(!strcmp(lines[5], "Changed: 1970-01-01 00:00") && !strcmp(lines[3], "File: 7 bytes"));
    v.file.mtime = 951782400u;          /* 2000-02-29 */
    view_details(&v, lines); CHECK(!strcmp(lines[5], "Changed: 2000-02-29 00:00"));
    v.file.mtime = 1798761599u;         /* 2026-12-31 23:59:59 */
    view_details(&v, lines); CHECK(!strcmp(lines[5], "Changed: 2026-12-31 23:59"));
    v.file.mtime = 4107542400u;         /* 2100-03-01: no 29th of February that year */
    view_details(&v, lines); CHECK(!strcmp(lines[5], "Changed: 2100-03-01 00:00"));
    v.file.mtime = 4294967295u;
    view_details(&v, lines); CHECK(!strcmp(lines[5], "Changed: 2106-02-07 06:28"));
    v.file.mtime = 0;                   /* not known: no line */
    v.file.count = 0;
    CHECK(view_details(&v, lines) == 6);
    /* a file in the root, and names */
    snprintf(v.file.path, sizeof(v.file.path), "/a.bmp");
    view_details(&v, lines); CHECK(!strcmp(lines[1], "Folder: /"));
    CHECK(!strcmp(view_name("/x/y.gif"), "y.gif") && !strcmp(view_name("z.jpg"), "z.jpg") && !strcmp(view_name("/"), "/"));
}

/**** the folder ****/

static char names[FOLDER_BYTES];
static struct folder f;

static void folder(void) {
    static const char *const in[] = { "pic10.png", "Pic9.PNG", "notes.txt", "pic9.png", "a.jpeg", "B.JPG", "c.gif", ".png",
                                      "pic09.png", "z.bmp", "image.png.txt", "x.Bmp", "pic1.png", "pic001.png", "png",
                                      "photo 2.jpg", "photo 10.jpg", "a10b2.gif", "a10b10.gif", "a9b99.gif" };
    static const char *const order[] = { "a.jpeg", "a9b99.gif", "a10b2.gif", "a10b10.gif", "B.JPG", "c.gif", "photo 2.jpg",
                                         "photo 10.jpg", "pic001.png", "pic1.png", "Pic9.PNG", "pic09.png", "pic9.png",
                                         "pic10.png", "x.Bmp", "z.bmp" };
    char name[16];
    size_t i;
    int k, ok;
    CHECK(folder_is_picture("a.png") && folder_is_picture("A.PNG") && folder_is_picture("b.JpEg") && folder_is_picture("c.gif"));
    CHECK(!folder_is_picture(".png") && !folder_is_picture("png") && !folder_is_picture("a.png.txt") && !folder_is_picture("a.pn"));
    CHECK(!folder_is_picture("") && !folder_is_picture("a.tiff") && !folder_is_picture("jpeg"));
    folder_start(&f, names);
    for(i = 0; i < sizeof(in) / sizeof(in[0]); i++) folder_add(&f, in[i]);
    CHECK(f.count == 16 && !f.dropped);
    folder_sort(&f);
    for(ok = 1, i = 0; i < sizeof(order) / sizeof(order[0]); i++) if(strcmp(folder_name(&f, (int)i), order[i])) ok = 0;
    CHECK(ok);
    if(!ok) for(i = 0; i < (size_t)f.count; i++) printf("  %s\n", folder_name(&f, (int)i));
    CHECK(folder_find(&f, "pic9.png") == 12 && folder_find(&f, "PIC10.PNG") == 13 && folder_find(&f, "nope.png") == -1);
    CHECK(folder_find(&f, "Pic9.PNG") == 10);                       /* the name as it is before one that differs by case */
    CHECK(folder_step(&f, 0, 1) == 1 && folder_step(&f, 15, 1) == 0 && folder_step(&f, 0, -1) == 15);
    CHECK(folder_step(&f, -1, 1) == 0 && folder_step(&f, -1, -1) == 15);
    CHECK(!strcmp(folder_name(&f, 99), "") && !strcmp(folder_name(&f, -1), ""));
    /* the limits: FOLDER_MAX names, FOLDER_BYTES of them */
    folder_start(&f, names);
    for(k = 0; k < FOLDER_MAX + 10; k++) {
        snprintf(name, sizeof(name), "%05d.png", FOLDER_MAX + 10 - k);
        folder_add(&f, name);
    }
    CHECK(f.count == FOLDER_MAX && f.dropped == 10);
    folder_sort(&f);
    for(ok = 1, k = 1; k < f.count; k++) if(atoi(folder_name(&f, k - 1)) >= atoi(folder_name(&f, k))) ok = 0;
    CHECK(ok && atoi(folder_name(&f, 0)) == 11);
    {
        static char longname[300];
        folder_start(&f, names);
        memset(longname, 'a', 250);
        memcpy(longname + 250, ".png", 5);
        for(k = 0; k < 2000; k++) {
            longname[0] = (char)('a' + k % 26);
            longname[1] = (char)('a' + k / 26 % 26);
            longname[2] = (char)('a' + k / 676 % 26);
            folder_add(&f, longname);
        }
        CHECK(f.count == FOLDER_BYTES / 255 && f.dropped == 2000 - FOLDER_BYTES / 255 && f.used <= FOLDER_BYTES);
    }
    folder_start(&f, names);
    CHECK(folder_step(&f, 0, 1) == -1 && folder_find(&f, "a.png") == -1);
    folder_sort(&f);
    CHECK(f.count == 0);
}

int main(void) {
    views();
    toolbar();
    painting();
    words();
    folder();
    if(failures) {
        printf("WVIEW TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WVIEW TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
