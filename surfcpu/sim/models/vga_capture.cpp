/*
SurfCPU VGA capture
--------------------
File: vga_capture.cpp Date: 10/15/26 (SurfCPU)
--------------------
The capture and text decoder of vga_capture.h.
*/
#include "vga_capture.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

// ------------------------------------------------------------------ font

bool VgaFont::load_c(const std::string &path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const std::regex entry(R"(\[0x([0-9a-fA-F]{2})\]\s*=\s*\{([^}]*)\})");
    const std::regex byte(R"(0x([0-9a-fA-F]{1,2}))");
    int found = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), entry); it != std::sregex_iterator(); ++it) {
        const int idx = std::stoi((*it)[1].str(), nullptr, 16);
        const std::string body = (*it)[2].str();
        int r = 0;
        for (auto b = std::sregex_iterator(body.begin(), body.end(), byte); b != std::sregex_iterator() && r < 16; ++b)
            rows[idx][r++] = static_cast<uint8_t>(std::stoi((*b)[1].str(), nullptr, 16));
        if (r == 16) found++;
    }
    loaded = found == 256;
    return loaded;
}

bool VgaFont::load_verilog(const std::string &path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const std::regex assign(R"(rom\[12'h([0-9a-fA-F]{3})\]\s*=\s*8'h([0-9a-fA-F]{2}))");
    int found = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), assign); it != std::sregex_iterator(); ++it) {
        const int a = std::stoi((*it)[1].str(), nullptr, 16);
        rows[a >> 4][a & 15] = static_cast<uint8_t>(std::stoi((*it)[2].str(), nullptr, 16));
        found++;
    }
    loaded = found == 4096;
    return loaded;
}

std::string VgaText::printable(int row) const {
    std::string s = lines[row];
    for (char &c : s)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '.';
    return s;
}

// ------------------------------------------------------------------ capture

VgaCapture::VgaCapture(const VgaTiming &t) : t_(t) {
    cur_.assign(static_cast<size_t>(t_.h_active) * t_.v_active, 0);
    last_ = cur_;
    bx0_ = by0_ = 1 << 30;
    bx1_ = by1_ = -1;
}

void VgaCapture::on_hsync() {
    if (last_hs_edge_) {
        const int64_t len = static_cast<int64_t>(n_ - last_hs_edge_);
        m_.lines_seen++;
        if (line_len_first_ < 0) line_len_first_ = len;
        else if (len != line_len_first_) m_.lines_odd++;
        m_.h_total = static_cast<int>(len);
    }
    last_hs_edge_ = n_;
    hpos_ = 0;
    if (line_ >= 0) line_++;
    if (vs_level_ >= 0 && vs_prev_ == (vs_level_ == 1)) vsync_lines_++;   // a line start inside the vsync pulse
}

void VgaCapture::on_vsync() {
    if (line_ >= 0) {                                // a whole frame ended here
        const int64_t total = line_;                 // line starts since the last vsync edge
        if (last_v_total_ >= 0 && total != last_v_total_) m_.frames_odd++;
        last_v_total_ = total;
        m_.v_total = static_cast<int>(total);
        m_.frames++;
        last_ = cur_;
        m_.x0 = bx0_ <= bx1_ ? bx0_ : -1;
        m_.x1 = bx1_;
        m_.y0 = by0_ <= by1_ ? by0_ : -1;
        m_.y1 = by1_;
        if (m_.x0 >= 0 && m_.h_sync > 0) {
            m_.h_back = m_.x0 - m_.h_sync;
            m_.h_front = m_.h_total - 1 - m_.x1;
        }
        if (m_.y0 >= 0 && m_.v_sync > 0) {
            m_.v_back = m_.y0 - m_.v_sync;
            m_.v_front = m_.v_total - 1 - m_.y1;
        }
    }
    bx0_ = by0_ = 1 << 30;
    bx1_ = by1_ = -1;
    line_len_first_ = -1;
    line_ = 0;
    vsync_lines_ = 0;
    std::fill(cur_.begin(), cur_.end(), 0);
}

void VgaCapture::tick(uint8_t r4, uint8_t g4, uint8_t b4, bool hs, bool vs) {
    ++n_;
    // sync phases and polarity: the shorter phase is the pulse
    bool hs_lead = false, vs_lead = false;
    if (hs != hs_prev_) {
        hs_phase_[hs_prev_ ? 1 : 0] = n_ - hs_change_;
        hs_change_ = n_;
        if (hs_phase_[0] && hs_phase_[1]) {
            hs_level_ = hs_phase_[0] < hs_phase_[1] ? 0 : 1;
            m_.h_sync = static_cast<int>(hs_phase_[hs_level_]);
        }
        if (hs_level_ >= 0 && (hs ? 1 : 0) == hs_level_) hs_lead = true;
    }
    if (vs != vs_prev_) {
        const bool leaving_pulse = vs_level_ >= 0 && (vs_prev_ ? 1 : 0) == vs_level_;
        vs_phase_[vs_prev_ ? 1 : 0] = n_ - vs_change_;
        vs_change_ = n_;
        if (vs_phase_[0] && vs_phase_[1]) vs_level_ = vs_phase_[0] < vs_phase_[1] ? 0 : 1;
        if (leaving_pulse) m_.v_sync = vsync_lines_;
        if (vs_level_ >= 0 && (vs ? 1 : 0) == vs_level_) vs_lead = true;
    }
    if (hs_level_ >= 0 && vs_level_ >= 0) {
        m_.polarity_known = true;
        m_.hsync_negative = hs_level_ == 0;
        m_.vsync_negative = vs_level_ == 0;
    }
    hs_prev_ = hs;
    vs_prev_ = vs;
    // a line starting with the vsync edge belongs to the new frame
    if (hs_lead) on_hsync();
    if (vs_lead) on_vsync();
    if (hpos_ >= 0) {
        const uint16_t px = static_cast<uint16_t>((r4 & 15) << 8 | (g4 & 15) << 4 | (b4 & 15));
        if (line_ >= 0) {
            const int64_t x = hpos_ - (t_.h_sync + t_.h_back);
            const int64_t y = line_ - (t_.v_sync + t_.v_back);
            const bool inside = x >= 0 && x < t_.h_active && y >= 0 && y < t_.v_active;
            if (inside) cur_[static_cast<size_t>(y) * t_.h_active + static_cast<size_t>(x)] = px;
            if (px) {
                if (!inside) m_.blank_pixels++;
                if (hpos_ < bx0_) bx0_ = static_cast<int>(hpos_);
                if (hpos_ > bx1_) bx1_ = static_cast<int>(hpos_);
                if (line_ < by0_) by0_ = static_cast<int>(line_);
                if (line_ > by1_) by1_ = static_cast<int>(line_);
            }
        }
        hpos_++;
    }
}

bool VgaCapture::write_ppm(const std::string &path) const {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", t_.h_active, t_.v_active);
    std::vector<uint8_t> row(static_cast<size_t>(t_.h_active) * 3);
    for (int y = 0; y < t_.v_active; y++) {
        for (int x = 0; x < t_.h_active; x++) {
            const uint16_t p = pixel(x, y);
            row[3 * x + 0] = static_cast<uint8_t>(((p >> 8) & 15) * 17);
            row[3 * x + 1] = static_cast<uint8_t>(((p >> 4) & 15) * 17);
            row[3 * x + 2] = static_cast<uint8_t>((p & 15) * 17);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    return std::fclose(f) == 0;
}

// ------------------------------------------------------------------ text

namespace {

using Glyph = std::array<uint8_t, 16>;

// code points in the order a decoder prefers them when glyphs look alike
std::vector<int> preference() {
    std::vector<int> order;
    for (int c = 0x20; c <= 0x7E; c++) order.push_back(c);
    for (int c = 0x80; c <= 0xFF; c++) order.push_back(c);
    for (int c = 0x01; c <= 0x1F; c++) order.push_back(c);
    order.push_back(0x7F);
    order.push_back(0x00);
    return order;
}

}  // namespace

VgaText VgaCapture::decode_text(const VgaFont &font, int gw, int gh) const {
    VgaText out;
    out.cols = t_.h_active / gw;
    out.rows = t_.v_active / gh;
    out.lines.assign(out.rows, std::string(out.cols, ' '));
    out.fg.assign(static_cast<size_t>(out.cols) * out.rows, 0);
    out.bg = out.fg;
    std::map<Glyph, int> exact, top14;                 // bitmap -> code point; rows 0-13 -> code point
    for (int c : preference()) {
        Glyph g;
        std::memcpy(g.data(), font.rows[c], 16);
        exact.emplace(g, c);
        Glyph t = g;
        t[14] = t[15] = 0;
        top14.emplace(t, c);
    }
    std::map<uint16_t, uint64_t> hist;                 // the frame's commonest colour: the background
    for (uint16_t p : last_) hist[p]++;
    uint16_t common = 0;
    uint64_t best = 0;
    for (const auto &kv : hist)
        if (kv.second > best) { best = kv.second; common = kv.first; }

    for (int row = 0; row < out.rows; row++) {
        for (int col = 0; col < out.cols; col++) {
            const size_t ci = static_cast<size_t>(row) * out.cols + col;
            uint16_t colors[3];
            int ncol = 0;
            for (int y = 0; y < gh && ncol < 3; y++)
                for (int x = 0; x < gw && ncol < 3; x++) {
                    const uint16_t p = pixel(col * gw + x, row * gh + y);
                    bool seen = false;
                    for (int k = 0; k < ncol; k++) seen = seen || colors[k] == p;
                    if (!seen) colors[ncol++] = p;
                }
            if (ncol == 1) {
                out.uniform++;
                out.bg[ci] = out.fg[ci] = colors[0];
                continue;                               // a space
            }
            if (ncol > 2) {
                out.unmatched++;
                out.lines[row][col] = '?';
                continue;
            }
            Glyph ma{};                                 // pixels of colour 0 set
            for (int y = 0; y < gh; y++) {
                uint8_t bits = 0;
                for (int x = 0; x < gw; x++)
                    if (pixel(col * gw + x, row * gh + y) == colors[0]) bits |= static_cast<uint8_t>(0x80 >> x);
                ma[y] = bits;
            }
            Glyph mb;
            for (int y = 0; y < 16; y++) mb[y] = static_cast<uint8_t>(~ma[y]);
            auto fa = exact.find(ma), fb = exact.find(mb);
            int code = -1, fgi = 0;
            if (fa != exact.end() && fb != exact.end()) {
                // both readings work (a glyph and its inverse): the background is the common colour
                if (colors[1] == common) { code = fa->second; fgi = 0; }
                else { code = fb->second; fgi = 1; }
            } else if (fa != exact.end()) {
                code = fa->second; fgi = 0;
            } else if (fb != exact.end()) {
                code = fb->second; fgi = 1;
            } else {
                // the cursor: rows 14-15 solid in the foreground over a glyph's rows 0-13
                for (int k = 0; k < 2 && code < 0; k++) {
                    const Glyph &m = k == 0 ? ma : mb;
                    if (m[14] != 0xFF || m[15] != 0xFF) continue;
                    Glyph t = m;
                    t[14] = t[15] = 0;
                    auto it = top14.find(t);
                    if (it != top14.end()) {
                        code = it->second;
                        fgi = k;
                        out.cursor_col = col;
                        out.cursor_row = row;
                    }
                }
            }
            if (code < 0) {
                out.unmatched++;
                out.lines[row][col] = '?';
                continue;
            }
            out.lines[row][col] = static_cast<char>(code);
            out.fg[ci] = colors[fgi];
            out.bg[ci] = colors[1 - fgi];
        }
    }
    return out;
}
