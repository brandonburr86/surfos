/*
SurfCPU VGA capture tests
--------------------
File: test_vga_capture.cpp Date: 10/15/26 (SurfCPU)
--------------------
VgaCapture on its own, fed by a VGA signal generated here: the standard 640x480 timing
with negative syncs, other porches and sync widths with positive syncs, and a vsync that
changes with the hsync edge instead of at the start of a line; it must measure each one.
Frames rendered here from driver/font8x16.c (random glyphs and colours, inverse glyphs,
a cursor) must decode back with decode_text; a pixel in the blanking must be counted;
write_ppm must write the frame.

    test_vga_capture <font8x16.c> <work directory> [seed]
*/
#include "../models/vga_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>

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

struct Gen {                    // a VGA source: picture(x, y) inside the active area
    VgaTiming t;
    bool hneg = true, vneg = true;
    bool vsync_at_hsync = false;  // vsync changes at the hsync leading edge, not at x = 0
    std::function<uint16_t(int, int)> picture;
    uint16_t stray_x = 0xFFFF;    // one non-black pixel in the blanking (x in h_front), on line stray_y
    int stray_y = 0;

    void run(VgaCapture &cap, int frames) {
        const int ht = t.h_active + t.h_front + t.h_sync + t.h_back;
        const int vt = t.v_active + t.v_front + t.v_sync + t.v_back;
        for (int f = 0; f < frames; f++)
            for (int y = 0; y < vt; y++)
                for (int x = 0; x < ht; x++) {
                    const bool hs_on = x >= t.h_active + t.h_front && x < t.h_active + t.h_front + t.h_sync;
                    int vy = y;          // the line the vsync decision uses
                    if (vsync_at_hsync && x >= t.h_active + t.h_front) vy = (y + 1) % vt;
                    const bool vs_on = vy >= t.v_active + t.v_front && vy < t.v_active + t.v_front + t.v_sync;
                    uint16_t p = 0;
                    if (x < t.h_active && y < t.v_active) p = picture(x, y);
                    if (x == t.h_active + stray_x && y == stray_y) p = 0x00F;
                    cap.tick(p >> 8 & 15, p >> 4 & 15, p & 15, hs_on ? !hneg : hneg, vs_on ? !vneg : vneg);
                }
    }
};

static VgaFont g_font;

static void test_timings() {
    struct Case { VgaTiming t; bool hneg, vneg, at_hsync; const char *name; };
    VgaTiming std_t;
    VgaTiming odd;
    odd.h_front = 24; odd.h_sync = 64; odd.h_back = 72;
    odd.v_front = 3; odd.v_sync = 4; odd.v_back = 38;
    const Case cases[] = {
        {std_t, true, true, false, "640x480 negative"},
        {std_t, true, true, true, "640x480, vsync with the hsync edge"},
        {odd, false, false, false, "other porches, positive syncs"},
        {odd, true, false, true, "other porches, mixed polarity, vsync with hsync"},
    };
    for (const Case &c : cases) {
        VgaCapture cap(c.t);
        Gen g;
        g.t = c.t;
        g.hneg = c.hneg;
        g.vneg = c.vneg;
        g.vsync_at_hsync = c.at_hsync;
        g.picture = [&](int x, int y) -> uint16_t {
            return (x == 0 || y == 0 || x == c.t.h_active - 1 || y == c.t.v_active - 1) ? 0xFFF : ((x ^ y) & 0x0F0);
        };
        g.run(cap, 4);
        const VgaMeasured &m = cap.measured();
        const int ht = c.t.h_active + c.t.h_front + c.t.h_sync + c.t.h_back;
        const int vt = c.t.v_active + c.t.v_front + c.t.v_sync + c.t.v_back;
        CHECK(m.frames >= 2, "%s: %llu frames", c.name, (unsigned long long)m.frames);
        CHECK(m.h_total == ht && m.v_total == vt, "%s: totals %d x %d, want %d x %d", c.name, m.h_total, m.v_total, ht, vt);
        CHECK(m.h_sync == c.t.h_sync && m.v_sync == c.t.v_sync, "%s: sync %d px, %d lines", c.name, m.h_sync, m.v_sync);
        CHECK(m.hsync_negative == c.hneg && m.vsync_negative == c.vneg, "%s: polarity %d %d", c.name, m.hsync_negative,
              m.vsync_negative);
        // the same porches whether vsync changes at the start of a line or with the hsync edge
        CHECK(m.h_front == c.t.h_front && m.h_back == c.t.h_back && m.v_front == c.t.v_front && m.v_back == c.t.v_back,
              "%s: porches h %d/%d v %d/%d", c.name, m.h_front, m.h_back, m.v_front, m.v_back);
        CHECK(m.lines_odd == 0 && m.frames_odd == 0 && m.blank_pixels == 0, "%s: odd %llu/%llu blank %llu", c.name,
              (unsigned long long)m.lines_odd, (unsigned long long)m.frames_odd, (unsigned long long)m.blank_pixels);
        int bad = 0;
        for (int y = 0; y < c.t.v_active; y++)
            for (int x = 0; x < c.t.h_active; x++)
                if (cap.pixel(x, y) != g.picture(x, y)) bad++;
        CHECK(bad == 0, "%s: %d pixels differ in the captured frame", c.name, bad);
    }
    // a pixel in the front porch is counted
    VgaCapture cap;
    Gen g;
    g.picture = [](int, int) -> uint16_t { return 0x111; };
    g.stray_x = 5;
    g.stray_y = 100;
    g.run(cap, 4);                         // frame 0 finds the polarity; 2 and 3 count the stray pixel
    CHECK(cap.measured().blank_pixels >= 2, "blank pixels %llu", (unsigned long long)cap.measured().blank_pixels);
    // the refresh rate of the board's timing
    CHECK(std::abs(cap.measured().frame_hz(25177305) - 59.94596) < 0.0001, "frame rate %.5f",
          cap.measured().frame_hz(25177305));
}

static void test_decode(unsigned seed, const std::string &dir) {
    std::mt19937 rng(seed);
    static const uint16_t pal[16] = {0x000, 0x00A, 0x0A0, 0x0AA, 0xA00, 0xA0A, 0xA50, 0xAAA,
                                     0x555, 0x55F, 0x5F5, 0x5FF, 0xF55, 0xF5F, 0xFF5, 0xFFF};
    uint8_t code[30][80], fg[30][80], bg[30][80];
    const std::string msg = "decode_text reads this line back";
    for (int r = 0; r < 30; r++)
        for (int c = 0; c < 80; c++) {
            code[r][c] = static_cast<uint8_t>(rng());
            fg[r][c] = static_cast<uint8_t>(rng() % 16);
            bg[r][c] = static_cast<uint8_t>((fg[r][c] + 1 + rng() % 15) % 16);
            if (r == 3) {                  // a plain line: light grey on black
                code[r][c] = c < static_cast<int>(msg.size()) ? static_cast<uint8_t>(msg[c]) : ' ';
                fg[r][c] = 7;
                bg[r][c] = 0;
            }
        }
    const int cc = 9, cr = 3;              // the cursor, on 'e' of "decode"
    VgaCapture cap;
    Gen g;
    g.picture = [&](int x, int y) -> uint16_t {
        const int c = x / 8, r = y / 16, gy = y % 16;
        bool on = (g_font.rows[code[r][c]][gy] >> (7 - x % 8)) & 1;
        if (c == cc && r == cr && gy >= 14) on = true;
        return pal[on ? fg[r][c] : bg[r][c]];
    };
    g.run(cap, 3);                         // the first frame finds the syncs, the next is whole
    CHECK(cap.frames() >= 1, "no whole frame captured");
    const VgaText t = cap.decode_text(g_font);
    CHECK(t.cursor_col == cc && t.cursor_row == cr, "cursor at %d,%d", t.cursor_col, t.cursor_row);
    CHECK(t.lines[3].substr(0, msg.size()) == msg, "plain line: '%s'", t.printable(3).c_str());
    int wrong = 0, ok = 0;
    for (int r = 0; r < 30; r++)
        for (int c = 0; c < 80; c++) {
            if (r == cr && c == cc) continue;
            const uint8_t *gl = g_font.rows[code[r][c]];
            bool uniform = true;
            for (int i = 0; i < 16; i++) uniform = uniform && (gl[i] == 0 || gl[i] == 0xFF) && gl[i] == gl[0];
            if (uniform) continue;
            const uint8_t *d = g_font.rows[t.at(c, r)];
            const size_t i = static_cast<size_t>(r) * 80 + c;
            const bool direct = !std::memcmp(d, gl, 16) && t.fg[i] == pal[fg[r][c]] && t.bg[i] == pal[bg[r][c]];
            bool inv = t.fg[i] == pal[bg[r][c]] && t.bg[i] == pal[fg[r][c]];
            for (int k = 0; k < 16 && inv; k++) inv = d[k] == static_cast<uint8_t>(~gl[k]);
            if (direct || inv) ok++;
            else wrong++;
        }
    CHECK(wrong == 0 && t.unmatched == 0, "decode: %d wrong, %d unmatched, %d right", wrong, t.unmatched, ok);
    const std::string ppm = dir + "/capture-selftest.ppm";
    CHECK(cap.write_ppm(ppm), "write_ppm");
    std::FILE *f = std::fopen(ppm.c_str(), "rb");
    char head[16] = {};
    const size_t n = f ? std::fread(head, 1, 15, f) : 0;
    if (f) std::fclose(f);
    CHECK(n == 15 && std::strncmp(head, "P6\n640 480\n255\n", 15) == 0, "PPM header");
    std::printf("decode: %d cells right, cursor at %d,%d\n", ok, t.cursor_col, t.cursor_row);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::printf("usage: %s <font8x16.c> <work directory> [seed]\n", argv[0]);
        return 2;
    }
    if (!g_font.load_c(argv[1])) {
        std::printf("cannot read the font %s\n", argv[1]);
        return 2;
    }
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 0)) : 1;
    test_timings();
    test_decode(seed, argv[2]);
    if (failures) {
        std::printf("test_vga_capture: FAIL (%d of %d checks failed, seed %u)\n", failures, checks, seed);
        return 1;
    }
    std::printf("test_vga_capture: PASS (%d checks, seed %u)\n", checks, seed);
    return 0;
}
