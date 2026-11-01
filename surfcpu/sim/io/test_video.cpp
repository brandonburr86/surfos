/*
SurfCPU surf_video testbench
--------------------
File: test_video.cpp Date: 10/15/26 (SurfCPU)
--------------------
Verilator harness for surf_video (inside tb_video, which adds a text RAM stand-in) with
two unrelated clocks, a SurfBus master on the sys clock and VgaCapture on the pins.
Checks:
  * reset values and register rules (errors for unknown offsets, partial words, fetches;
    field masks; INFO and PIXCLK);
  * timing measured by the capture from the syncs alone: 800 x 525 totals, 96-pixel and
    2-line sync pulses, both negative, porches 16/48 and 10/33 from the test pattern's
    border, nothing but black in the blanking, every line and frame the same; the refresh
    rate from SURF_PIX_HZ;
  * the colour-bar test pattern, every pixel of a whole frame; ENABLE = 0 and modes 2, 3
    black with the syncs running;
  * text mode: random cells (all 256 glyphs, random colours) compared pixel by pixel with
    driver/font8x16.c and the palette, and decoded back to characters and colours with
    decode_text; a readable screen decoded row by row; hardware scroll (and SCROLL 30/31
    acting as 0); the cursor in display coordinates, out of range, and blinking with a
    16-frame half period; palette changes;
  * CTRL, CURSOR and SCROLL written in mid-frame apply from the next frame, wholly (a mode
    change too); a palette write applies as it arrives;
  * refused writes and fetches with live state (VBLANK_EV, palette, settings): no effect;
  * STATUS.VBLANK duty, VBLANK_EV once per frame with the interrupt, the FRAME counter;
  * a reset of the pixel domain alone (its settings come back over the crossing) and of the
    sys domain alone (registers back to reset values, the syncs undisturbed);
all at the board's clocks (SURF_SYS_HZ / SURF_PIX_HZ) or, by seed, at other ratios for the
clock crossings.
--gfx (10/18/26, SurfGPU): the same tests on surf_video built with HAS_GFX = 1 (mk/area-gpu.mk),
where offset 0x080 is GFX_BASE; nothing else changes: VRAM and text RAM start all zero (as the
real ones do) and GPAL[0] is black, so modes 2 and 3 still show black at the point this test
looks at them (sim/gpu/test_video_gfx.cpp tests the graphics).

    Vtb_video --font driver/font8x16.c --out DIR [--seed N] [--gfx]
*/
#include "Vtb_video.h"
#include "verilated.h"

#include "surfbus_master.h"
#include "surfcpu/soc.h"
#include "vga_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

static const uint32_t B = SURF_VIDEO_BASE;
static const uint16_t CGA[16] = {0x000, 0x00A, 0x0A0, 0x0AA, 0xA00, 0xA0A, 0xA50, 0xAAA,
                                 0x555, 0x55F, 0x5F5, 0x5FF, 0xF55, 0xF5F, 0xFF5, 0xFFF};
enum { CTRL_EN = 1, MODE_BARS = 0 << 1, MODE_TEXT = 1 << 1, MODE_2 = 2 << 1, MODE_3 = 3 << 1,
       CUR_EN = 1 << 3, CUR_BLINK = 1 << 4 };

struct Tb {
    Vtb_video *top;
    VgaCapture cap;
    uint64_t sys_half, pix_half, next_sys, next_pix;
    uint64_t sys_cycles = 0, pix_cycles = 0;

    Tb(Vtb_video *t, uint64_t sys_period_ps, uint64_t pix_period_ps, uint64_t phase_ps)
        : top(t), sys_half(sys_period_ps / 2), pix_half(pix_period_ps / 2), next_sys(sys_period_ps / 2),
          next_pix(phase_ps + pix_period_ps / 2) {}

    // run until the next rising edge of clk, pix_clk edges included
    void posedge() {
        for (;;) {
            const uint64_t tn = std::min(next_sys, next_pix);
            bool sys_rise = false, pix_rise = false;
            if (next_sys == tn) {
                top->clk = !top->clk;
                sys_rise = top->clk;
                next_sys += sys_half;
            }
            if (next_pix == tn) {
                top->pix_clk = !top->pix_clk;
                pix_rise = top->pix_clk;
                next_pix += pix_half;
            }
            top->eval();
            if (pix_rise) {
                pix_cycles++;
                cap.tick(top->vga_r, top->vga_g, top->vga_b, top->vga_hs, top->vga_vs);
            }
            if (sys_rise) {
                sys_cycles++;
                return;
            }
        }
    }
};

using Bus = SurfBusMaster<Vtb_video>;

struct Screen {                     // the harness's copy of text RAM cells 0..2399
    uint32_t cell[2400] = {};
};

static VgaFont g_font;
static std::string g_out;
static bool g_gfx = false;          // the block has HAS_GFX (offset 0x080 is a register there)

static void tram_write(Tb &tb, Bus &bus, Screen &s, int idx, uint32_t v) {
    bus.idle(0);
    tb.top->tram_we = 1;
    tb.top->tram_waddr = static_cast<uint16_t>(idx);
    tb.top->tram_wdata = v;
    tb.posedge();
    tb.top->tram_we = 0;
    if (idx < 2400) s.cell[idx] = v;
}

static void wait_frames(Tb &tb, Bus &bus, uint64_t n) {
    const uint64_t target = tb.cap.frames() + n;
    while (tb.cap.frames() < target) bus.idle(2000);
}

static uint16_t pattern(int x, int y) {
    if (x == 0 || x == 639 || y == 0 || y == 479) return 0xFFF;
    if (y < 360) {
        const int bar = x / 80;
        return static_cast<uint16_t>(((bar & 2) ? 0 : 0xF00) | ((bar & 4) ? 0 : 0x0F0) | ((bar & 1) ? 0 : 0x00F));
    }
    const int lv = x / 40;
    if (y < 390) return static_cast<uint16_t>(lv << 8 | lv << 4 | lv);
    if (y < 420) return static_cast<uint16_t>(lv << 8);
    if (y < 450) return static_cast<uint16_t>(lv << 4);
    return static_cast<uint16_t>(lv);
}

struct View {                       // what the picture should show in text mode
    const Screen *s;
    const uint16_t *pal;
    int scroll = 0;
    bool cursor = false;
    int cur_col = 0, cur_row = 0;
    uint16_t px(int x, int y) const {
        const int col = x / 8, srow = y / 16, gy = y % 16;
        const int trow = (srow + scroll) % 30;
        const uint32_t c = s->cell[trow * 80 + col];
        bool on = (g_font.rows[c & 0xFF][gy] >> (7 - x % 8)) & 1;
        if (cursor && col == cur_col && srow == cur_row && gy >= 14) on = true;
        return pal[on ? (c >> 8) & 15 : (c >> 12) & 15];
    }
};

static int compare_view(Tb &tb, const View &v, const char *what) {
    int bad = 0, first_x = -1, first_y = -1;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
            if (tb.cap.pixel(x, y) != v.px(x, y)) {
                if (!bad) { first_x = x; first_y = y; }
                bad++;
            }
    CHECK(bad == 0, "%s: %d pixels differ, first at (%d,%d): %03x, want %03x", what, bad, first_x, first_y,
          first_x >= 0 ? tb.cap.pixel(first_x, first_y) : 0, first_x >= 0 ? v.px(first_x, first_y) : 0);
    return bad;
}

// ------------------------------------------------------------------ tests

static void test_registers(Tb &tb, Bus &bus) {
    CHECK(bus.rd(B + SURF_VIDEO_CTRL) == CTRL_EN, "CTRL after reset: %08x", bus.rd(B + SURF_VIDEO_CTRL));
    CHECK((bus.rd(B + SURF_VIDEO_STATUS) & ~SURF_VIDEO_STATUS_VBLANK_MASK) == 0, "STATUS after reset");
    CHECK(bus.rd(B + SURF_VIDEO_CURSOR) == 0 && bus.rd(B + SURF_VIDEO_SCROLL) == 0 && bus.rd(B + SURF_VIDEO_IE) == 0,
          "CURSOR/SCROLL/IE after reset");
    CHECK(bus.rd(B + SURF_VIDEO_INFO) == 0x10081E50u, "INFO %08x", bus.rd(B + SURF_VIDEO_INFO));
    CHECK(bus.rd(B + SURF_VIDEO_PIXCLK) == SURF_PIX_HZ, "PIXCLK %u", bus.rd(B + SURF_VIDEO_PIXCLK));
    int pal_bad = 0;
    for (int i = 0; i < 16; i++)
        if (bus.rd(B + SURF_VIDEO_PALETTE + 4 * i) != CGA[i]) pal_bad++;
    CHECK(pal_bad == 0, "%d palette entries differ from the CGA colours after reset", pal_bad);
    const uint32_t bad[] = {0x01C, 0x020, 0x03C, 0x080, 0x042, 0x07E, 0x100, 0xFFC};
    for (uint32_t off : bad) {
        if (g_gfx && off == SURF_VIDEO_GFX_BASE) continue;
        CHECK(bus.read(B + off).err && bus.write(B + off, 0).err, "offset %03x not an error", off);
    }
    CHECK(bus.write(B + SURF_VIDEO_CTRL, 0, 0x1).err && bus.read(B + SURF_VIDEO_STATUS, 0xE).err, "partial access");
    CHECK(bus.fetch(B + SURF_VIDEO_CTRL).err, "fetch not an error");
    CHECK(bus.rd(B + SURF_VIDEO_CTRL) == CTRL_EN, "a refused write changed CTRL");
    // field masks
    bus.wr(B + SURF_VIDEO_CURSOR, 0xFFFFFFFF);
    CHECK(bus.rd(B + SURF_VIDEO_CURSOR) == 0x1F7F, "CURSOR mask: %08x", bus.rd(B + SURF_VIDEO_CURSOR));
    bus.wr(B + SURF_VIDEO_SCROLL, 0xFFFFFFFF);
    CHECK(bus.rd(B + SURF_VIDEO_SCROLL) == 0x1F, "SCROLL mask");
    bus.wr(B + SURF_VIDEO_IE, 0xFFFFFFFF);
    CHECK(bus.rd(B + SURF_VIDEO_IE) == 1, "IE mask");
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 7, 0xFFFFF123);
    CHECK(bus.rd(B + SURF_VIDEO_PALETTE + 4 * 7) == 0x123, "PALETTE mask");
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 7, CGA[7]);
    bus.wr(B + SURF_VIDEO_INFO, 0);                       // read-only: ignored
    CHECK(bus.rd(B + SURF_VIDEO_INFO) == 0x10081E50u, "INFO changed by a write");
    bus.wr(B + SURF_VIDEO_CURSOR, 0);
    bus.wr(B + SURF_VIDEO_SCROLL, 0);
    bus.wr(B + SURF_VIDEO_IE, 0);
    bus.wr(B + SURF_VIDEO_STATUS, 0xFFFFFFFF);
}

static void test_timing_and_pattern(Tb &tb, Bus &bus) {
    wait_frames(tb, bus, 3);
    const VgaMeasured &m = tb.cap.measured();
    CHECK(m.polarity_known && m.hsync_negative && m.vsync_negative, "sync polarity: h %d v %d", m.hsync_negative,
          m.vsync_negative);
    CHECK(m.h_total == 800 && m.h_sync == 96, "horizontal: total %d, sync %d", m.h_total, m.h_sync);
    CHECK(m.v_total == 525 && m.v_sync == 2, "vertical: total %d, sync %d", m.v_total, m.v_sync);
    CHECK(m.h_back == 48 && m.h_front == 16 && m.v_back == 33 && m.v_front == 10,
          "porches from the border: h %d/%d v %d/%d", m.h_front, m.h_back, m.v_front, m.v_back);
    CHECK(m.x0 == 144 && m.x1 == 783 && m.y0 == 35 && m.y1 == 514, "picture box %d..%d x %d..%d", m.x0, m.x1, m.y0, m.y1);
    int bad = 0, fx = -1, fy = -1;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
            if (tb.cap.pixel(x, y) != pattern(x, y)) {
                if (!bad) { fx = x; fy = y; }
                bad++;
            }
    CHECK(bad == 0, "test pattern: %d pixels differ, first (%d,%d) %03x want %03x", bad, fx, fy,
          fx >= 0 ? tb.cap.pixel(fx, fy) : 0, fx >= 0 ? pattern(fx, fy) : 0);
    tb.cap.write_ppm(g_out + "/testpattern.ppm");
    std::printf("timing: %d x %d total, hsync %d px, vsync %d lines, both %s; porches h %d/%d v %d/%d; "
                "%.4f kHz line, %.4f Hz frame at %u Hz; test pattern: 307200 pixels exact\n",
                m.h_total, m.v_total, m.h_sync, m.v_sync, m.hsync_negative && m.vsync_negative ? "negative" : "?",
                m.h_front, m.h_back, m.v_front, m.v_back, m.line_hz(SURF_PIX_HZ) / 1000.0, m.frame_hz(SURF_PIX_HZ),
                SURF_PIX_HZ);
}

static bool all_black(const VgaCapture &cap) {
    for (uint16_t p : cap.frame())
        if (p) return false;
    return true;
}

static void test_black(Tb &tb, Bus &bus) {
    const uint32_t modes[] = {MODE_BARS, MODE_2 | CTRL_EN, MODE_3 | CTRL_EN, MODE_TEXT};
    for (uint32_t m : modes) {
        bus.wr(B + SURF_VIDEO_CTRL, m);
        wait_frames(tb, bus, 2);
        CHECK(all_black(tb.cap), "picture not black with CTRL %02x", m);
        CHECK(tb.cap.measured().h_total == 800 && tb.cap.measured().v_total == 525, "syncs stopped with CTRL %02x", m);
    }
}

static void fill_random(Tb &tb, Bus &bus, Screen &s, std::mt19937 &rng) {
    for (int i = 0; i < 2400; i++) {
        const uint32_t fg = rng() % 16;
        uint32_t bg = rng() % 16;
        if (bg == fg) bg = (bg + 1 + rng() % 15) % 16;
        // glyphs 0..255 each appear at least nine times; bits 31:16 are reserved (stored, not shown)
        tram_write(tb, bus, s, i, (rng() & 0xFFFF0000u) | bg << 12 | fg << 8 | static_cast<uint32_t>(i % 256));
    }
}

static void test_text(Tb &tb, Bus &bus, Screen &s, std::mt19937 &rng) {
    fill_random(tb, bus, s, rng);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    wait_frames(tb, bus, 2);
    View v{&s, CGA};
    compare_view(tb, v, "random text, pixel by pixel");
    const VgaText t = tb.cap.decode_text(g_font);
    int wrong = 0, checked = 0, inverse = 0;
    for (int i = 0; i < 2400; i++) {
        const int col = i % 80, row = i / 80;
        const int code = s.cell[i] & 0xFF;
        const int dec = t.at(col, row);
        const uint16_t fg = CGA[(s.cell[i] >> 8) & 15], bg = CGA[(s.cell[i] >> 12) & 15];
        const uint8_t *g = g_font.rows[code], *d = g_font.rows[dec];
        if (std::all_of(g, g + 16, [](uint8_t b) { return b == 0 || b == 0xFF; }) &&
            std::all_of(g, g + 16, [&](uint8_t b) { return b == g[0]; }))
            continue;                 // a blank or solid glyph is one colour: it reads as a space
        checked++;
        // identical glyphs: the decoder names the first, so compare bitmaps, not codes; a glyph
        // whose inverse is also a glyph (half blocks...) may read as that one with the colours swapped
        bool direct = std::memcmp(d, g, 16) == 0 && t.fg[i] == fg && t.bg[i] == bg;
        bool inv = t.fg[i] == bg && t.bg[i] == fg;
        for (int r = 0; r < 16 && inv; r++) inv = d[r] == static_cast<uint8_t>(~g[r]);
        if (inv) inverse++;
        if (!direct && !inv) {
            if (wrong < 3) std::printf("  cell %d,%d: code %02x decoded %02x\n", col, row, code, dec);
            wrong++;
        }
    }
    CHECK(wrong == 0 && t.unmatched == 0, "decode_text: %d of %d cells wrong, %d unmatched", wrong, checked, t.unmatched);
    tb.cap.write_ppm(g_out + "/text-random.ppm");
    std::printf("text: 2400 random cells (all 256 glyphs) exact pixel by pixel; decode_text read %d of %d two-colour "
                "cells right (%d as the inverse glyph with the colours swapped)\n", checked - wrong, checked, inverse);
}

static const char *const kBanner[] = {
    "SurfOS 1.0 on SurfCPU (RV32IM), Arty A7 -- VGA text console, 80 x 30",
    "",
    "  ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789",
    "  !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
    "",
    "~ $ ls /BOOT",
    "SURFOS.BIN  BACKUP.BIN",
    "~ $ _",
};

static void write_lines(Tb &tb, Bus &bus, Screen &s, const std::vector<std::string> &lines, uint32_t attr) {
    for (int row = 0; row < 30; row++)
        for (int col = 0; col < 80; col++) {
            const std::string &l = row < static_cast<int>(lines.size()) ? lines[row] : std::string();
            const uint8_t ch = col < static_cast<int>(l.size()) ? static_cast<uint8_t>(l[col]) : ' ';
            tram_write(tb, bus, s, row * 80 + col, attr | ch);
        }
}

static void test_banner_scroll_cursor(Tb &tb, Bus &bus, Screen &s, std::mt19937 &rng) {
    std::vector<std::string> lines(kBanner, kBanner + sizeof kBanner / sizeof kBanner[0]);
    for (int r = static_cast<int>(lines.size()); r < 30; r++) {
        char b[96];
        std::snprintf(b, sizeof b, "line %02d: the quick brown fox jumps over the lazy dog %08x", r,
                      static_cast<unsigned>(rng()));
        lines.push_back(b);
    }
    const uint32_t attr = 1u << 12 | 15u << 8;            // white on blue
    write_lines(tb, bus, s, lines, attr);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    wait_frames(tb, bus, 2);
    VgaText t = tb.cap.decode_text(g_font);
    int bad_rows = 0;
    for (int r = 0; r < 30; r++) {
        std::string want = lines[r];
        want.resize(80, ' ');
        if (t.lines[r] != want) {
            if (bad_rows < 3) std::printf("  row %d: '%s'\n", r, t.printable(r).c_str());
            bad_rows++;
        }
    }
    CHECK(bad_rows == 0 && t.cursor_col < 0, "banner: %d rows decoded wrong, cursor %d,%d", bad_rows, t.cursor_col,
          t.cursor_row);
    tb.cap.write_ppm(g_out + "/text-banner.ppm");
    std::printf("banner: '%s'\n", t.printable(0).substr(0, 60).c_str());

    // hardware scroll: display row r shows text row (r + SCROLL) mod 30; 30 and 31 act as 0
    const int scrolls[] = {static_cast<int>(1 + rng() % 29), 29, 30, 31, 0};
    for (int sc : scrolls) {
        bus.wr(B + SURF_VIDEO_SCROLL, static_cast<uint32_t>(sc));
        wait_frames(tb, bus, 2);
        t = tb.cap.decode_text(g_font);
        const int eff = sc >= 30 ? 0 : sc;
        int bad = 0;
        for (int r = 0; r < 30; r++) {
            std::string want = lines[(r + eff) % 30];
            want.resize(80, ' ');
            if (t.lines[r] != want) bad++;
        }
        CHECK(bad == 0, "SCROLL %d: %d display rows wrong", sc, bad);
    }

    // the cursor, in display coordinates (after SCROLL)
    const int sc = static_cast<int>(3 + rng() % 20);
    const int ccol = static_cast<int>(rng() % 80), crow = static_cast<int>(rng() % 30);
    bus.wr(B + SURF_VIDEO_SCROLL, static_cast<uint32_t>(sc));
    bus.wr(B + SURF_VIDEO_CURSOR, static_cast<uint32_t>(crow << 8 | ccol));
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT | CUR_EN);
    wait_frames(tb, bus, 2);
    View v{&s, CGA};
    v.scroll = sc;
    v.cursor = true;
    v.cur_col = ccol;
    v.cur_row = crow;
    compare_view(tb, v, "cursor and scroll, pixel by pixel");
    t = tb.cap.decode_text(g_font);
    CHECK(t.cursor_col == ccol && t.cursor_row == crow, "cursor decoded at %d,%d, want %d,%d", t.cursor_col,
          t.cursor_row, ccol, crow);
    std::string want = lines[(crow + sc) % 30];
    want.resize(80, ' ');
    CHECK(t.lines[crow] == want, "the row under the cursor decoded wrong: '%s'", t.printable(crow).c_str());
    // out of range: no cursor anywhere
    bus.wr(B + SURF_VIDEO_CURSOR, 30u << 8 | 80u);
    wait_frames(tb, bus, 2);
    t = tb.cap.decode_text(g_font);
    CHECK(t.cursor_col < 0, "cursor shown at %d,%d while out of range", t.cursor_col, t.cursor_row);
    bus.wr(B + SURF_VIDEO_SCROLL, 0);

    // palette: blue becomes dark green, white becomes yellow
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 1, 0x040);
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 15, 0xFF0);
    wait_frames(tb, bus, 2);
    uint16_t pal2[16];
    std::memcpy(pal2, CGA, sizeof pal2);
    pal2[1] = 0x040;
    pal2[15] = 0xFF0;
    View vp{&s, pal2};
    compare_view(tb, vp, "after palette writes");
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 1, CGA[1]);
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 15, CGA[15]);
}

static void test_blink(Tb &tb, Bus &bus) {
    const int ccol = 5, crow = 7;
    bus.wr(B + SURF_VIDEO_CURSOR, static_cast<uint32_t>(crow << 8 | ccol));
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT | CUR_EN | CUR_BLINK);
    wait_frames(tb, bus, 2);
    std::vector<int> vis;
    uint64_t last = tb.cap.frames();
    while (vis.size() < 44) {
        bus.idle(3000);
        if (tb.cap.frames() == last) continue;
        last = tb.cap.frames();
        // rows 14 and 15 of the cursor cell in the foreground colour (white on blue: 0xFFF)
        bool on = true;
        for (int x = 0; x < 8; x++)
            for (int y = 14; y < 16; y++) on = on && tb.cap.pixel(ccol * 8 + x, crow * 16 + y) == 0xFFF;
        vis.push_back(on ? 1 : 0);
    }
    // runs of equal visibility: all whole runs are 16 frames long
    std::vector<int> runs;
    int len = 1;
    for (size_t i = 1; i < vis.size(); i++) {
        if (vis[i] == vis[i - 1]) len++;
        else { runs.push_back(len); len = 1; }
    }
    int ons = 0;
    for (int x : vis) ons += x;
    const bool whole_ok = runs.size() >= 2 && std::all_of(runs.begin() + 1, runs.end(), [](int r) { return r == 16; });
    CHECK(whole_ok && ons > 0 && ons < 44, "cursor blink: runs %s, %d of 44 frames on",
          [&] { std::string s; for (int r : runs) s += std::to_string(r) + " "; return s; }().c_str(), ons);
    std::printf("blink: whole on/off runs of 16 frames (period 32 frames = %.2f Hz)\n",
                tb.cap.measured().frame_hz(SURF_PIX_HZ) / 32.0);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
}

// Refused writes (partial words) and fetches with live state: VBLANK_EV set with its
// interrupt enabled, a palette entry, CTRL, CURSOR and SCROLL away from their reset values.
static void test_refused_writes(Tb &tb, Bus &bus) {
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 3, 0x123);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT | CUR_EN);
    bus.wr(B + SURF_VIDEO_CURSOR, 4u << 8 | 6u);
    bus.wr(B + SURF_VIDEO_SCROLL, 5);
    bus.wr(B + SURF_VIDEO_IE, 1);
    bus.wr(B + SURF_VIDEO_STATUS, SURF_VIDEO_STATUS_VBLANK_EV_MASK);
    uint64_t n = 0;
    while (!(bus.rd(B + SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK) && n++ < 2000000) bus.idle(50);
    const uint32_t regs[] = {SURF_VIDEO_CTRL, SURF_VIDEO_CURSOR, SURF_VIDEO_SCROLL, SURF_VIDEO_IE,
                             SURF_VIDEO_PALETTE + 4 * 3};
    uint32_t before[5];
    for (int i = 0; i < 5; i++) before[i] = bus.rd(B + regs[i]);
    const uint8_t bes[] = {0x1, 0x3, 0x7, 0xE, 0x8, 0x0};
    for (uint8_t be : bes) {
        CHECK(bus.write(B + SURF_VIDEO_STATUS, 0xFFFFFFFF, be).err, "partial STATUS write (be %x) not an error", be);
        for (uint32_t r : regs) CHECK(bus.write(B + r, 0, be).err, "partial write (be %x) to %03x not an error", be, r);
    }
    CHECK(bus.fetch(B + SURF_VIDEO_STATUS).err && bus.fetch(B + SURF_VIDEO_PALETTE + 4 * 3).err, "fetches not errors");
    bus.idle(2);
    CHECK((bus.rd(B + SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK) && tb.top->irq,
          "a refused STATUS write cleared VBLANK_EV");
    int changed = 0;
    for (int i = 0; i < 5; i++) changed += bus.rd(B + regs[i]) != before[i];
    CHECK(changed == 0, "refused writes changed %d registers (palette entry 3 now %03x)", changed,
          bus.rd(B + SURF_VIDEO_PALETTE + 4 * 3));
    bus.wr(B + SURF_VIDEO_IE, 0);
    bus.wr(B + SURF_VIDEO_STATUS, SURF_VIDEO_STATUS_VBLANK_EV_MASK);
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 3, CGA[3]);
    bus.wr(B + SURF_VIDEO_CURSOR, 0);
    bus.wr(B + SURF_VIDEO_SCROLL, 0);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
}

// wait for the capture's next frame edge, then until it reaches this line of the frame
static void sync_to_line(Tb &tb, Bus &bus, int64_t line) {
    const uint64_t f = tb.cap.frames();
    while (tb.cap.frames() == f) bus.idle(16);
    while (tb.cap.line() < line) bus.idle(16);
}

static void wait_frame_edge(Tb &tb, Bus &bus, uint64_t frames) {
    while (tb.cap.frames() < frames) bus.idle(64);
}

// CTRL, CURSOR and SCROLL written in the middle of a frame (at picture line 240): that frame
// is drawn wholly with the old values and the very next one wholly with the new ones. A palette
// write applies as it arrives: the frame it lands in changes partway, the next is all new.
static void test_frame_latch(Tb &tb, Bus &bus, Screen &s) {
    const int64_t mid = 35 + 240;                     // capture line of picture line 240
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    bus.wr(B + SURF_VIDEO_SCROLL, 0);
    wait_frames(tb, bus, 2);
    View old_v{&s, CGA};
    View new_v{&s, CGA};
    new_v.scroll = 9;
    new_v.cursor = true;
    new_v.cur_col = 12;
    new_v.cur_row = 3;
    sync_to_line(tb, bus, mid);
    bus.wr(B + SURF_VIDEO_SCROLL, 9);
    bus.wr(B + SURF_VIDEO_CURSOR, 3u << 8 | 12u);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT | CUR_EN);
    uint64_t f0 = tb.cap.frames();
    CHECK(tb.cap.line() < 35 + 480, "the writes did not land inside the picture (line %lld)",
          static_cast<long long>(tb.cap.line()));
    wait_frame_edge(tb, bus, f0 + 1);
    compare_view(tb, old_v, "the frame SCROLL, CURSOR and CTRL were written in");
    wait_frame_edge(tb, bus, f0 + 2);
    compare_view(tb, new_v, "the next frame, with the new SCROLL, CURSOR and CTRL");
    // a mode change in mid-frame: this frame stays text, the next is the test pattern
    sync_to_line(tb, bus, mid);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_BARS);
    f0 = tb.cap.frames();
    wait_frame_edge(tb, bus, f0 + 1);
    compare_view(tb, new_v, "the frame MODE was changed in");
    wait_frame_edge(tb, bus, f0 + 2);
    int bad = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) bad += tb.cap.pixel(x, y) != pattern(x, y);
    CHECK(bad == 0, "the frame after the MODE change: %d pixels differ from the test pattern", bad);
    // a palette write applies as it arrives
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    bus.wr(B + SURF_VIDEO_SCROLL, 0);
    wait_frames(tb, bus, 2);
    sync_to_line(tb, bus, mid);
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 1, 0x0C0);
    f0 = tb.cap.frames();
    wait_frame_edge(tb, bus, f0 + 1);
    CHECK(tb.cap.pixel(639, 0) == CGA[1] && tb.cap.pixel(639, 479) == 0x0C0,
          "palette written in mid-frame: top %03x (want the old %03x), bottom %03x (want 0c0)", tb.cap.pixel(639, 0),
          CGA[1], tb.cap.pixel(639, 479));
    wait_frame_edge(tb, bus, f0 + 2);
    uint16_t pal2[16];
    std::memcpy(pal2, CGA, sizeof pal2);
    pal2[1] = 0x0C0;
    View vp{&s, pal2};
    compare_view(tb, vp, "the frame after a mid-frame palette write");
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 1, CGA[1]);
    bus.wr(B + SURF_VIDEO_CURSOR, 0);
    std::printf("frame latch: CTRL, CURSOR and SCROLL written at picture line 240 change the next frame, never "
                "the current one; a palette write applies at once\n");
}

// one clock domain reset alone: the pixel clock's MMCM losing lock, or a sys reset
static uint64_t g_odd_lines_allowed = 0, g_odd_frames_allowed = 0, g_blank_allowed = 0;

static void test_domain_resets(Tb &tb, Bus &bus, Screen &s) {
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    bus.wr(B + SURF_VIDEO_PALETTE + 4 * 1, 0x123);       // a setting the pixel side must get back
    wait_frames(tb, bus, 2);
    const uint64_t lines0 = tb.cap.measured().lines_odd, frames0 = tb.cap.measured().frames_odd;
    const uint64_t blank0 = tb.cap.measured().blank_pixels;
    tb.top->pix_rst = 1;
    bus.idle(300);
    tb.top->pix_rst = 0;
    wait_frames(tb, bus, 3);
    uint16_t pal[16];
    std::memcpy(pal, CGA, sizeof pal);
    pal[1] = 0x123;
    View v{&s, pal};
    compare_view(tb, v, "after a reset of the pixel domain alone");
    // the restart breaks the line and frame lengths once, then they are steady again
    const uint64_t lines1 = tb.cap.measured().lines_odd, frames1 = tb.cap.measured().frames_odd;
    CHECK(lines1 - lines0 <= 4 && frames1 - frames0 <= 2, "pixel reset: %llu odd lines, %llu odd frames",
          (unsigned long long)(lines1 - lines0), (unsigned long long)(frames1 - frames0));
    g_odd_lines_allowed = lines1 - lines0;
    g_odd_frames_allowed = frames1 - frames0;
    // the picture restarted in the middle of the capture's frame, so that one frame's picture
    // lies outside the window the old syncs give: counted once, never again
    const uint64_t blank1 = tb.cap.measured().blank_pixels;
    g_blank_allowed = blank1 - blank0;
    wait_frames(tb, bus, 2);
    CHECK(tb.cap.measured().lines_odd == lines1 && tb.cap.measured().frames_odd == frames1, "timing not steady again");
    CHECK(tb.cap.measured().blank_pixels == blank1, "pixels in the blanking after the pixel reset");
    // a sys reset alone: the registers go back to their reset values (bars, CGA), the syncs run on
    bus.idle(1);
    tb.top->rst = 1;
    bus.idle(10);
    tb.top->rst = 0;
    bus.idle(2);
    CHECK(bus.rd(B + SURF_VIDEO_CTRL) == CTRL_EN && bus.rd(B + SURF_VIDEO_PALETTE + 4) == CGA[1],
          "registers after a sys reset");
    wait_frames(tb, bus, 2);
    int bad = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) bad += tb.cap.pixel(x, y) != pattern(x, y);
    CHECK(bad == 0, "after a sys reset: %d pixels differ from the test pattern", bad);
    CHECK(tb.cap.measured().lines_odd == lines1 && tb.cap.measured().frames_odd == frames1,
          "a sys reset disturbed the syncs");
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    std::printf("resets: the pixel domain alone recovers its settings (%llu odd lines at the restart); "
                "a sys reset alone leaves the syncs untouched\n", (unsigned long long)g_odd_lines_allowed);
}

static void test_vblank(Tb &tb, Bus &bus) {
    bus.wr(B + SURF_VIDEO_STATUS, SURF_VIDEO_STATUS_VBLANK_EV_MASK);
    // VBLANK duty over exactly two frames from a frame edge, sampled from the bus
    uint64_t samples = 0, high = 0, rises = 0;
    bool prev = false;
    const uint64_t fs = tb.cap.frames();
    while (tb.cap.frames() == fs) bus.idle(1);
    const uint64_t f0 = tb.cap.frames();
    while (tb.cap.frames() < f0 + 2) {
        const bool vb = bus.rd(B + SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_MASK;
        samples++;
        high += vb;
        if (vb && !prev && samples > 1) rises++;
        prev = vb;
        bus.idle(97);
    }
    const double duty = double(high) / samples;
    CHECK(rises >= 1 && rises <= 3 && duty > 0.080 && duty < 0.092, "VBLANK: %llu rises, duty %.4f (want 45/525 = 0.0857)",
          (unsigned long long)rises, duty);
    // VBLANK_EV and the interrupt, FRAME counting
    bus.wr(B + SURF_VIDEO_IE, 1);
    bus.wr(B + SURF_VIDEO_STATUS, SURF_VIDEO_STATUS_VBLANK_EV_MASK);
    bus.idle(2);
    std::vector<uint64_t> at;
    uint32_t frame0 = 0, frame1 = 0;
    const uint64_t cap0 = tb.cap.frames();
    while (at.size() < 6) {
        bus.idle(1);
        if (!tb.top->irq) continue;
        const uint32_t st = bus.rd(B + SURF_VIDEO_STATUS);
        CHECK(st & SURF_VIDEO_STATUS_VBLANK_EV_MASK, "irq without VBLANK_EV: %08x", st);
        if (at.empty()) frame0 = st >> 16;
        frame1 = st >> 16;
        at.push_back(tb.sys_cycles);
        bus.wr(B + SURF_VIDEO_STATUS, SURF_VIDEO_STATUS_VBLANK_EV_MASK);
        bus.idle(1);
        CHECK(!tb.top->irq, "irq still set after clearing VBLANK_EV");
    }
    const uint64_t capn = tb.cap.frames() - cap0;
    // one event per frame: 420000 pix clocks apart, in sys cycles
    const double per = 420000.0 * (2.0 * tb.pix_half) / (2.0 * tb.sys_half);
    int bad = 0;
    for (size_t i = 1; i < at.size(); i++)
        if (std::abs(double(at[i] - at[i - 1]) - per) > 8) bad++;
    CHECK(bad == 0, "VBLANK_EV spacing differs from one frame (%.0f sys cycles) %d times", per, bad);
    CHECK(frame1 - frame0 == at.size() - 1, "FRAME counted %u for %zu events", frame1 - frame0, at.size() - 1);
    CHECK(capn + 1 >= at.size() - 1 && capn <= at.size() + 1, "capture saw %llu frames for %zu events",
          (unsigned long long)capn, at.size() - 1);
    bus.wr(B + SURF_VIDEO_IE, 0);
    bus.idle(1);
    CHECK(!tb.top->irq, "irq with IE clear");
    std::printf("vblank: duty %.4f, VBLANK_EV every %.0f sys cycles with the interrupt, FRAME +%u\n", duty, per,
                frame1 - frame0);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    unsigned seed = 1;
    std::string font_path;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) g_out = argv[++i];
        else if (!std::strcmp(argv[i], "--gfx")) g_gfx = true;
    }
    if (font_path.empty() || g_out.empty() || !g_font.load_c(font_path)) {
        std::printf("usage: %s --font driver/font8x16.c --out DIR [--seed N] (font: %s)\n", argv[0], font_path.c_str());
        return 2;
    }
    std::mt19937 rng(seed);
    // the board's clocks, or by seed a faster or slower sys clock against the pixel clock
    const uint64_t pix_ps = (1000000000000ull + SURF_PIX_HZ / 2) / SURF_PIX_HZ;   // 39724 ps
    const uint64_t sys_choices[] = {40000, 40000, 20000, 100000, 33333};
    const uint64_t sys_ps = sys_choices[seed % 5];
    auto top = std::make_unique<Vtb_video>();
    Tb tb(top.get(), sys_ps, pix_ps, rng() % pix_ps);
    top->clk = 0;
    top->pix_clk = 0;
    top->rst = 1;
    top->pix_rst = 1;
    top->s_req_valid = 0;
    top->tram_we = 0;
    top->vram_we = 0;                       // tb_video's VRAM stand-in stays all zero
    top->eval();
    for (int i = 0; i < 8; i++) tb.posedge();
    top->rst = 0;
    top->pix_rst = 0;
    tb.posedge();
    Bus bus(top.get(), [&] { tb.posedge(); }, seed);
    bus.max_gap = static_cast<int>(seed % 3);
    Screen screen;

    std::printf("clocks: sys %.3f MHz, pix %.4f MHz (simulated periods %llu / %llu ps)\n", 1e6 / sys_ps, 1e6 / pix_ps,
                (unsigned long long)sys_ps, (unsigned long long)pix_ps);
    test_registers(tb, bus);
    test_timing_and_pattern(tb, bus);
    test_black(tb, bus);
    test_text(tb, bus, screen, rng);
    test_banner_scroll_cursor(tb, bus, screen, rng);
    test_blink(tb, bus);
    test_frame_latch(tb, bus, screen);
    test_vblank(tb, bus);
    test_refused_writes(tb, bus);
    test_domain_resets(tb, bus, screen);

    const VgaMeasured &m = tb.cap.measured();
    CHECK(m.blank_pixels == g_blank_allowed, "%llu non-black pixels in the blanking (%llu in the frame of the pixel reset)",
          (unsigned long long)m.blank_pixels, (unsigned long long)g_blank_allowed);
    CHECK(m.lines_odd == g_odd_lines_allowed && m.frames_odd == g_odd_frames_allowed,
          "unstable timing: %llu odd lines, %llu odd frames (the pixel reset accounts for %llu, %llu)",
          (unsigned long long)m.lines_odd, (unsigned long long)m.frames_odd, (unsigned long long)g_odd_lines_allowed,
          (unsigned long long)g_odd_frames_allowed);
    CHECK(bus.problems.empty(), "SurfBus protocol: %s", bus.problems.empty() ? "" : bus.problems[0].c_str());
    std::printf("capture: %llu frames, %llu lines, %d x %d; no pixel in the blanking outside the frame of the pixel "
                "reset\n", (unsigned long long)m.frames, (unsigned long long)m.lines_seen, m.h_total, m.v_total);
    top->final();
    if (failures) {
        std::printf("test_video: FAIL (%d of %d checks failed, seed %u, %llu pix cycles)\n", failures, checks, seed,
                    (unsigned long long)tb.pix_cycles);
        return 1;
    }
    std::printf("test_video: PASS (%d checks, seed %u, %llu frames, %llu pix cycles)\n", checks, seed,
                (unsigned long long)m.frames, (unsigned long long)tb.pix_cycles);
    return 0;
}
