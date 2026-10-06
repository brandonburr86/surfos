/* Host-side unit tests for the framebuffer renderer (driver/fbcore.c): mode checks,
   RGB and BGR channel layouts, 15/16/24/32 bits per pixel, padded pitch, glyphs,
   scaling, the text grid and its margins (10/5/26: the grid fills the screen), the
   cursor, and that nothing is written outside the visible lines. 10/6/26: a cell is a
   code page 437 character (driver/cp437.c), so the whole font is drawn, not only ASCII.
   The "framebuffer" is a buffer in memory with guard bytes around it. Run by
   `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <sys/framebuffer.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

#define GUARD 4096
#define GUARD_BYTE 0xA5
static u8 mem[GUARD + 1920 * 4 * 1080 + 256 * 1024 + GUARD];

static struct fb_mode mode(u32 w, u32 h, u32 pitch, u32 bpp, int bgr) {
    struct fb_mode m;
    memset(&m, 0, sizeof(m));
    m.addr = 0x80000000ULL;
    m.width = w; m.height = h; m.pitch = pitch; m.bpp = bpp; m.type = MB_FB_RGB;
    if(bpp == 32 || bpp == 24) {
        m.red_pos = bgr ? 16 : 0; m.green_pos = 8; m.blue_pos = bgr ? 0 : 16;
        m.red_size = m.green_size = m.blue_size = 8;
    } else if(bpp == 16) {
        m.red_pos = 11; m.red_size = 5; m.green_pos = 5; m.green_size = 6; m.blue_pos = 0; m.blue_size = 5;
    } else {
        m.red_pos = 10; m.red_size = 5; m.green_pos = 5; m.green_size = 5; m.blue_pos = 0; m.blue_size = 5;
    }
    return m;
}

/* a fresh surface in mem[], everything (guards and pixels) set to the guard byte */
static void surface(struct fb_surface *s, const struct fb_mode *m) {
    memset(mem, GUARD_BYTE, sizeof(mem));
    fb_setup(s, mem + GUARD, m);
}

static u32 pixel(const struct fb_surface *s, u32 x, u32 y) {
    const u8 *p = s->base + (u_long)y * s->mode.pitch + (u_long)x * s->bytespp;
    u32 v = 0, i;
    for(i = 0; i < s->bytespp; i++) v |= (u32)p[i] << (8 * i);
    return v;
}

static int guards_intact(const struct fb_surface *s) {
    u_long i, y, used = (u_long)s->mode.pitch * s->mode.height;
    for(i = 0; i < GUARD; i++) if(mem[i] != GUARD_BYTE) return 0;
    for(i = GUARD + used; i < sizeof(mem); i++) if(mem[i] != GUARD_BYTE) return 0;
    for(y = 0; y < s->mode.height; y++)                        /* the padding at the end of each line */
        for(i = (u_long)s->mode.width * s->bytespp; i < s->mode.pitch; i++)
            if(s->base[y * s->mode.pitch + i] != GUARD_BYTE) return 0;
    return 1;
}

static void test_modes(void) {
    struct fb_mode m;
    u64 bytes = 0;

    m = mode(1920, 1080, 7680, 32, 1);
    check(fb_check_mode(&m, &bytes) == NULL && bytes == 7680ULL * 1080, "1920x1080x32 is accepted");
    m.addr = 0x4000000000ULL;
    check(fb_check_mode(&m, NULL) == NULL, "a framebuffer above 4 GB is a valid mode");
    m = mode(1920, 1080, 7680, 32, 1); m.type = MB_FB_INDEXED;
    check(fb_check_mode(&m, NULL) != NULL, "indexed colour is refused");
    m = mode(1920, 1080, 7680, 32, 1); m.type = MB_FB_EGA;
    check(fb_check_mode(&m, NULL) != NULL, "EGA text is refused");
    m = mode(1024, 768, 1024, 8, 1);
    check(fb_check_mode(&m, NULL) != NULL, "8 bits per pixel is refused");
    m = mode(600, 400, 2400, 32, 1);
    check(fb_check_mode(&m, NULL) != NULL, "narrower than 640 is refused");
    m = mode(640, 399, 2560, 32, 1);
    check(fb_check_mode(&m, NULL) != NULL, "lower than 400 is refused");
    m = mode(1024, 768, 4000, 32, 1);
    check(fb_check_mode(&m, NULL) != NULL, "a pitch shorter than a line is refused");
    m = mode(1024, 768, 4096, 32, 1); m.red_pos = 30;
    check(fb_check_mode(&m, NULL) != NULL, "a colour field outside the pixel is refused");
    m = mode(1024, 768, 4096, 32, 1); m.green_size = 0;
    check(fb_check_mode(&m, NULL) != NULL, "an empty colour field is refused");
    m = mode(16384, 16384, 65536, 32, 1);
    check(fb_check_mode(&m, NULL) != NULL, "more than 256 MB is refused");
    m = mode(20000, 1080, 80000, 32, 1);
    check(fb_check_mode(&m, NULL) != NULL, "an implausible width is refused");
    m = mode(1024, 768, 4096, 32, 1); m.addr = 0;
    check(fb_check_mode(&m, NULL) != NULL, "address 0 is refused");
    m = mode(1024, 768, 4096, 32, 1); m.addr = 0xFFFFFFFFFFFFF000ULL;
    check(fb_check_mode(&m, NULL) != NULL, "an address that wraps is refused");
}

static void test_colours(void) {
    struct fb_surface s;
    struct fb_mode m;

    m = mode(640, 400, 2560, 32, 1);                           /* BGRX, what UEFI GOP usually gives */
    fb_setup(&s, mem, &m);
    check(s.palette[15] == 0xFFFFFF && s.palette[0] == 0, "white and black, BGRX");
    check(s.palette[4] == 0xAA0000 && s.palette[1] == 0x0000AA, "red in bits 16-23, blue in 0-7 (BGRX)");
    m = mode(640, 400, 2560, 32, 0);                           /* RGBX */
    fb_setup(&s, mem, &m);
    check(s.palette[4] == 0x0000AA && s.palette[1] == 0xAA0000, "red in bits 0-7, blue in 16-23 (RGBX)");
    check(s.palette[6] == 0x0055AA, "brown keeps its green half (RGBX)");
    m = mode(640, 400, 1280, 16, 1);
    fb_setup(&s, mem, &m);
    check(s.palette[15] == 0xFFFF && s.palette[4] == (21u << 11), "16 bpp 5:6:5");
    m = mode(640, 400, 1280, 15, 1);
    fb_setup(&s, mem, &m);
    check(s.palette[15] == 0x7FFF && s.bytespp == 2, "15 bpp 5:5:5 in two bytes");
}

static void test_drawing(void) {
    struct fb_surface s;
    struct fb_mode m;
    u32 x, ok, col, row;
    static const u32 bpps[4] = { 32, 24, 16, 15 };
    u32 i;

    for(i = 0; i < 4; i++) {
        u32 b = bpps[i], bpp_bytes = (b + 7) / 8;
        char what[96];
        m = mode(640, 400, 640 * bpp_bytes + 52, b, 1);        /* a padded pitch */
        surface(&s, &m);
        for(row = 0; row < s.rows; row++)
            for(col = 0; col < s.cols; col++) fb_draw_glyph(&s, col, row, 'A' + (col + row) % 26, 0x07, 0);
        snprintf(what, sizeof(what), "%u bpp: all 80x25 cells drawn, padding and guards untouched", b);
        check(guards_intact(&s), what);
        /* 'A' at (0,0): row 2 is 0x7c, so x 1-5 are on and x 0 is off */
        snprintf(what, sizeof(what), "%u bpp: glyph pixels in foreground and background", b);
        check(pixel(&s, 1, 2) == s.palette[7] && pixel(&s, 5, 2) == s.palette[7] && pixel(&s, 0, 2) == s.palette[0] &&
              pixel(&s, 6, 2) == s.palette[0], what);
    }

    m = mode(640, 400, 2560, 32, 1);
    surface(&s, &m);
    fb_draw_glyph(&s, 80, 0, 'X', 0x0F, 0);
    fb_draw_glyph(&s, 0, 25, 'X', 0x0F, 0);
    fb_draw_glyph(&s, 1000, 1000, 'X', 0x0F, 0);
    for(ok = 1, x = 0; x < sizeof(mem); x++) if(mem[x] != GUARD_BYTE) ok = 0;
    check(ok, "cells outside 80x25 draw nothing");

    fb_fill(&s, 630, 390, 100, 100, 0x123456);
    check(pixel(&s, 639, 399) == 0x123456 && pixel(&s, 629, 399) != 0x123456 && guards_intact(&s), "fill is clipped");

    fb_draw_glyph(&s, 0, 0, ' ', 0x1E, 1);                     /* yellow on blue with the cursor */
    check(pixel(&s, 0, 0) == s.palette[1] && pixel(&s, 0, 14) == s.palette[14] && pixel(&s, 7, 15) == s.palette[14] &&
          pixel(&s, 0, 13) == s.palette[1], "background colour and an underline cursor on rows 14-15");
    fb_draw_glyph(&s, 1, 0, ' ', 0x9E, 0);                     /* bit 7 is blink, not a bright background */
    check(pixel(&s, 8, 0) == s.palette[1], "attribute bit 7 does not brighten the background");
}

/* 10/6/26: cells above 0x7e and below 0x20 are characters too (code page 437) */
static void test_cp437_cells(void) {
    struct fb_surface s;
    struct fb_mode m;
    u32 x, y, ok;

    m = mode(640, 400, 2560, 32, 1);
    surface(&s, &m);
    fb_draw_glyph(&s, 0, 0, 0xDB, 0x0F, 0);                    /* █ */
    for(ok = 1, y = 0; y < 16; y++) for(x = 0; x < 8; x++) if(pixel(&s, x, y) != s.palette[15]) ok = 0;
    check(ok, "0xDB (full block) fills its cell with the foreground");
    fb_draw_glyph(&s, 1, 0, 0x00, 0x1F, 0);                    /* the empty cell */
    fb_draw_glyph(&s, 2, 0, 0xFF, 0x1F, 0);                    /* no-break space */
    for(ok = 1, y = 0; y < 16; y++) for(x = 8; x < 24; x++) if(pixel(&s, x, y) != s.palette[1]) ok = 0;
    check(ok, "0x00 (the empty cell) and 0xFF (no-break space) draw only the background");
    fb_draw_glyph(&s, 3, 0, 0xC4, 0x07, 0);                    /* ─ */
    check(pixel(&s, 24, 7) == s.palette[7] && pixel(&s, 31, 7) == s.palette[7] && pixel(&s, 24, 6) == s.palette[0],
          "0xC4 (box drawing) runs across the whole cell, so lines join");
    fb_draw_glyph(&s, 4, 0, 0x10, 0x07, 0);                    /* SurfOS's breadcrumb ▸ */
    check(pixel(&s, 33, 8) == s.palette[7] && pixel(&s, 37, 8) == s.palette[7] && pixel(&s, 38, 8) == s.palette[0] &&
          pixel(&s, 33, 5) == s.palette[0] && pixel(&s, 33, 11) == s.palette[0], "0x10 is the small triangle of the prompt");
    check(guards_intact(&s), "code page 437 cells: padding and guards untouched");
}

static void test_scaling(void) {
    struct fb_surface s;
    struct fb_mode m;
    u32 ok, x, y;

    m = mode(1920, 1080, 7680, 32, 1);
    surface(&s, &m);
    check(s.scale == 2 && s.cols == 120 && s.rows == 33 && s.ox == 0 && s.oy == 12,
          "1920x1080: 120x33 cells at scale 2, a 12-pixel margin above and below");
    fb_draw_glyph(&s, 0, 0, 'A', 0x0F, 0);
    /* row 2, x 1 of the glyph is on: pixels (2..3, 16..17) after scaling and the margin */
    check(pixel(&s, 2, 16) == s.palette[15] && pixel(&s, 3, 17) == s.palette[15] &&
          pixel(&s, 0, 16) == s.palette[0] && pixel(&s, 1, 17) == s.palette[0], "each glyph pixel is 2x2");
    for(ok = 1, y = 0; y < 12; y++) for(x = 0; x < 1920; x++) if(pixel(&s, x, y) != 0xA5A5A5A5) ok = 0;
    check(ok && guards_intact(&s), "nothing drawn in the margin above the text");
    fb_draw_glyph(&s, 119, 32, 'Z', 0x0F, 0);
    /* 'Z' row 2 is 0xfe, x 0-6 on; the cell starts at 1904,1036 (119 * 16, 12 + 32 * 32) */
    check(pixel(&s, 1904, 1040) == s.palette[15] && pixel(&s, 1919, 1041) == s.palette[0] && guards_intact(&s),
          "the last cell (119,32) is drawn inside the screen");
    for(ok = 1, y = 1068; y < 1080; y++) for(x = 0; x < 1920; x++) if(pixel(&s, x, y) != 0xA5A5A5A5) ok = 0;
    check(ok, "nothing drawn in the margin below the text");
    surface(&s, &m);
    fb_draw_glyph(&s, 120, 0, 'X', 0x0F, 0);
    fb_draw_glyph(&s, 0, 33, 'X', 0x0F, 0);
    for(ok = 1, x = 0; x < sizeof(mem); x++) if(mem[x] != GUARD_BYTE) ok = 0;
    check(ok, "cells outside 120x33 draw nothing");

    m = mode(1366, 768, 5464, 32, 1);
    surface(&s, &m);
    check(s.scale == 1 && s.cols == 170 && s.rows == 48 && s.ox == 3 && s.oy == 0, "1366x768: 170x48 at scale 1");
    fb_draw_glyph(&s, 0, 0, 'A', 0x0F, 0);
    check(pixel(&s, 4, 2) == s.palette[15] && pixel(&s, 3, 2) == s.palette[0] && pixel(&s, 2, 2) == 0xA5A5A5A5,
          "1366x768: the first cell starts after the 3-pixel margin");
    fb_draw_glyph(&s, 169, 47, 'Z', 0x0F, 0);
    check(pixel(&s, 3 + 169 * 8, 47 * 16 + 2) == s.palette[15] && pixel(&s, 1362, 767) == s.palette[0] &&
          pixel(&s, 1363, 767) == 0xA5A5A5A5 && guards_intact(&s), "1366x768: the last cell ends before the right margin");

    m = mode(1280, 800, 5120, 32, 1);
    fb_setup(&s, mem, &m);
    check(s.scale == 2 && s.cols == 80 && s.rows == 25 && s.ox == 0 && s.oy == 0, "1280x800 (OVMF): 80x25 at scale 2, as before");
    m = mode(1024, 768, 4096, 32, 1);
    fb_setup(&s, mem, &m);
    check(s.scale == 1 && s.cols == 128 && s.rows == 48 && s.ox == 0 && s.oy == 0, "1024x768: 128x48 at scale 1");
    m = mode(800, 600, 3200, 32, 1);
    surface(&s, &m);
    check(s.scale == 1 && s.cols == 100 && s.rows == 37 && s.ox == 0 && s.oy == 4, "800x600: 100x37, 4-pixel margins");
    fb_draw_glyph(&s, 99, 36, 'A', 0x0F, 0);
    check(pixel(&s, 99 * 8 + 1, 4 + 36 * 16 + 2) == s.palette[15] && pixel(&s, 99 * 8, 4 + 36 * 16 + 2) == s.palette[0] &&
          pixel(&s, 799, 597) == 0xA5A5A5A5 && guards_intact(&s), "800x600: the last cell at its offset, the margin untouched");
    m = mode(640, 480, 2560, 32, 1);
    fb_setup(&s, mem, &m);
    check(s.scale == 1 && s.cols == 80 && s.rows == 30 && s.ox == 0 && s.oy == 0, "640x480: 80x30");
    m = mode(640, 400, 2560, 32, 1);
    fb_setup(&s, mem, &m);
    check(s.scale == 1 && s.cols == 80 && s.rows == 25 && s.ox == 0 && s.oy == 0, "640x400: exactly 80x25");
    m = mode(1280, 1024, 5120, 32, 1);
    fb_setup(&s, mem, &m);
    check(s.scale == 2 && s.cols == 80 && s.rows == 32 && s.ox == 0 && s.oy == 0, "1280x1024: 80x32 at scale 2");
    m = mode(3840, 2160, 15360, 32, 1);
    fb_setup(&s, mem, &m);
    check(s.scale == 5 && s.cols == 96 && s.rows == 27 && s.ox == 0 && s.oy == 0, "3840x2160: 96x27 at scale 5");
    m = mode(3000, 400, 12000, 32, 1);                          /* very wide: capped at 240 columns */
    fb_setup(&s, mem, &m);
    check(s.scale == 1 && s.cols == FB_MAX_COLS && s.rows == 25 && s.ox == (3000 - 240 * 8) / 2 && s.oy == 0,
          "3000x400: capped at 240 columns, centred");
    m = mode(640, 2000, 2560, 32, 1);                           /* very tall: capped at 100 rows */
    fb_setup(&s, mem, &m);
    check(s.scale == 1 && s.cols == 80 && s.rows == FB_MAX_ROWS && s.ox == 0 && s.oy == (2000 - 100 * 16) / 2,
          "640x2000: capped at 100 rows, centred");
}

int main(void) {
    test_modes();
    test_colours();
    test_drawing();
    test_cp437_cells();
    test_scaling();
    printf("%s: %d checks, %d failures\n", failures ? "FRAMEBUFFER UNIT TESTS FAILED" : "FRAMEBUFFER UNIT TESTS PASSED",
           checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
