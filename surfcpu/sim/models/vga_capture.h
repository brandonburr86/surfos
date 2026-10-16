/*
SurfCPU VGA capture
--------------------
File: vga_capture.h Date: 10/15/26 (SurfCPU)
--------------------
A monitor at the VGA pins for the simulations (MODULES.md section 7): tick(r4, g4, b4, hs,
vs) once per pix_clk. It measures the timing from the syncs alone (line and frame totals,
sync widths, polarity: the shorter phase of each sync signal is its pulse), finds the
picture as the bounding box of the non-black pixels (so a test pattern with a border gives
the porches), counts non-black pixels in the blanking intervals, and keeps the last whole
frame for pixel(), write_ppm() and decode_text().

Coordinates follow the syncs: a line starts at its hsync leading edge, and the line that
contains the vsync leading edge is line 0 of the frame. With the expected timing the
picture is pixels h_sync + h_back .. + h_active of lines v_sync + v_back .. + v_active
(144..783 and 35..514 for 640x480), which is what the frame buffer holds.

decode_text(font) cuts the picture into 8x16 cells and names each cell's character by
matching its two colours against the font's glyphs (either colour may be the foreground;
the frame's commonest colour breaks ties as the background); a cell whose rows 14-15 are
solid foreground over a glyph's rows 0-13 is reported with the cursor on it. One-colour
cells read as a space. No Verilator dependency: plain C++17.
*/
#ifndef SURFCPU_VGA_CAPTURE_H
#define SURFCPU_VGA_CAPTURE_H

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct VgaFont {
    uint8_t rows[256][16] = {};
    bool loaded = false;
    bool load_c(const std::string &path);        // driver/font8x16.c: [0xNN] = { 16 bytes }
    bool load_verilog(const std::string &path);  // rtl/peripherals/surf_video_font.v: rom[..] = 8'h..;
};

struct VgaTiming {                               // what the capture expects (frame layout)
    int h_active = 640, h_front = 16, h_sync = 96, h_back = 48;
    int v_active = 480, v_front = 10, v_sync = 2, v_back = 33;
};

struct VgaMeasured {
    uint64_t frames = 0;              // whole frames seen
    int h_total = -1, h_sync = -1;    // pixels, of the last frame
    int v_total = -1, v_sync = -1;    // lines
    bool hsync_negative = false, vsync_negative = false, polarity_known = false;
    uint64_t lines_seen = 0;
    uint64_t lines_odd = 0;           // lines whose total differed from the frame's first line
    uint64_t frames_odd = 0;          // frames whose line count differed from the one before
    int x0 = -1, x1 = -1, y0 = -1, y1 = -1;  // non-black bounding box in sync coordinates (last frame)
    int h_back = -1, h_front = -1, v_back = -1, v_front = -1;   // from that box and the syncs
    uint64_t blank_pixels = 0;        // non-black pixels outside the picture area, all frames
    double line_hz(double pix_hz) const { return h_total > 0 ? pix_hz / h_total : 0; }
    double frame_hz(double pix_hz) const { return h_total > 0 && v_total > 0 ? pix_hz / (double(h_total) * v_total) : 0; }
};

struct VgaText {
    int cols = 0, rows = 0;
    std::vector<std::string> lines;   // rows strings of cols code page 437 bytes
    std::vector<uint16_t> fg, bg;     // RGB444 per cell (row-major)
    int unmatched = 0;                // cells that match no glyph ('?')
    int uniform = 0;                  // one-colour cells (read as spaces)
    int cursor_col = -1, cursor_row = -1;
    uint8_t at(int col, int row) const { return static_cast<uint8_t>(lines[row][col]); }
    std::string printable(int row) const;    // the row with non-ASCII shown as '.'
};

class VgaCapture {
public:
    explicit VgaCapture(const VgaTiming &t = VgaTiming());

    void tick(uint8_t r4, uint8_t g4, uint8_t b4, bool hs, bool vs);

    const VgaMeasured &measured() const { return m_; }
    uint64_t frames() const { return m_.frames; }
    uint64_t ticks() const { return n_; }
    int width() const { return t_.h_active; }
    int height() const { return t_.v_active; }
    uint16_t pixel(int x, int y) const { return last_[static_cast<size_t>(y) * t_.h_active + x]; }   // RGB444
    const std::vector<uint16_t> &frame() const { return last_; }
    bool write_ppm(const std::string &path) const;
    VgaText decode_text(const VgaFont &font, int glyph_w = 8, int glyph_h = 16) const;

private:
    VgaTiming t_;
    VgaMeasured m_;
    uint64_t n_ = 0;
    // sync phases
    bool hs_prev_ = true, vs_prev_ = true;
    uint64_t hs_change_ = 0, vs_change_ = 0;
    uint64_t hs_phase_[2] = {0, 0}, vs_phase_[2] = {0, 0};
    int hs_level_ = -1, vs_level_ = -1;         // the sync pulse's level once known
    // position
    int64_t hpos_ = -1, line_ = -1;
    uint64_t last_hs_edge_ = 0;
    int64_t line_len_first_ = -1;
    int vsync_lines_ = 0;
    // the frame being captured and the last whole one
    std::vector<uint16_t> cur_, last_;
    int bx0_, bx1_, by0_, by1_;
    int64_t last_v_total_ = -1;

    void on_hsync();
    void on_vsync();
};

#endif
