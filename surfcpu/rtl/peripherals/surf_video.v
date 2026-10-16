/*
SurfCPU VGA text display
--------------------
File: surf_video.v Date: 10/15/26 (SurfCPU)
--------------------
The video block of MEMORY-MAP.md (video): 640x480 at about 60 Hz from pix_clk (800 x 525
total; horizontal 640/16/96/48, vertical 480/10/2/33; both syncs negative), RGB444 out.
Modes: a colour-bar test pattern (the reset mode, so a monitor shows a picture before any
software runs), 80x30 text of 8x16 glyphs from text RAM, or black. ENABLE = 0 blanks the
picture; the syncs always run.

Registers live in the clk (sys) domain; the picture in the pix_clk domain. The crossings
all use rtl/lib: every setting the picture needs (CTRL, CURSOR, SCROLL and the palette)
goes over in one surf_cdc_word, so it is never torn; the scan-out takes CTRL, CURSOR and
SCROLL once per frame, at the start of the last blank line, so a frame is drawn with one
set of values; the palette applies as it arrives. Vertical blanking comes back as a level
(surf_sync, STATUS.VBLANK) and a pulse at its start (surf_pulse_sync), which sets
VBLANK_EV (the interrupt) and counts STATUS.FRAME.

Scan-out pipeline, one pixel per pix_clk; the counters run four cycles ahead of the pins
and the syncs go through the same four stages, so pixels and syncs line up exactly:
  S0  hcnt, vcnt: visible, syncs, cell = ((row + SCROLL) mod 30) * 80 + col, cursor hit,
      the test pattern colour
  S1  text_addr (registered) -> text RAM
  S2  text_data (the RAM's registered word) -> font ROM address {glyph, glyph row};
      foreground and background latched
  S3  font row -> pixel bit -> palette colour (or the pattern, or black)
  S4  vga_r/g/b, vga_hs, vga_vs registered
The cursor is the cell's foreground colour on glyph rows 14 and 15, blinking with a period
of 32 frames (about 1.9 Hz) when CURSOR_BLINK is set.

The test pattern: a one-pixel white border round the picture (to check its edges on a
monitor); above line 360 eight bars of 80 pixels (white, yellow, cyan, green, magenta,
red, blue, black); below, four bands of 30 lines with 16 steps of 40 pixels of grey, red,
green and blue (every level of each 4-bit channel).
*/
module surf_video #(
    parameter integer PIX_HZ = 25177305
) (
    input  wire        clk,
    input  wire        rst,
    // SurfBus slave (SURFBUS.md)
    input  wire        s_req_valid,
    output wire        s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output wire        s_rsp_valid,
    output wire [31:0] s_rsp_rdata,
    output wire        s_rsp_err,
    output wire        irq,
    // picture side
    input  wire        pix_clk,
    input  wire        pix_rst,
    output wire [11:0] text_addr,    // cell number row*80+col, to surf_textram's pixel port
    input  wire [31:0] text_data,    // the cell, one pix_clk edge after text_addr
    output wire [3:0]  vga_r,
    output wire [3:0]  vga_g,
    output wire [3:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs
);
`include "surfcpu_soc.vh"

    // ---------------------------------------------------------------- geometry
    localparam [9:0] H_VIS = 10'd640, H_FP = 10'd16, H_SYNC = 10'd96, H_BP = 10'd48;
    localparam [9:0] V_VIS = 10'd480, V_FP = 10'd10, V_SYNC = 10'd2,  V_BP = 10'd33;
    localparam [9:0] H_TOT = H_VIS + H_FP + H_SYNC + H_BP;       // 800
    localparam [9:0] V_TOT = V_VIS + V_FP + V_SYNC + V_BP;       // 525
    localparam [6:0] COLS = 7'd80;
    localparam [4:0] ROWS = 5'd30;
    localparam [3:0] CURSOR_TOP = 4'd14;                         // cursor on glyph rows 14..15

    // the 16 CGA colours as RGB444, entry 15 first
    localparam [191:0] PAL_RESET = {
        12'hFFF, 12'hFF5, 12'hF5F, 12'hF55, 12'h5FF, 12'h5F5, 12'h55F, 12'h555,
        12'hAAA, 12'hA50, 12'hA0A, 12'hA00, 12'h0AA, 12'h0A0, 12'h00A, 12'h000};
    localparam [4:0] CTRL_RESET = 5'b00001;                       // ENABLE = 1, MODE = 0 (bars)

    // ---------------------------------------------------------------- bus decode
    wire [11:0] off      = s_req_addr[11:0];
    wire        hit_ctrl = (off == SURF_VIDEO_CTRL[11:0]);
    wire        hit_stat = (off == SURF_VIDEO_STATUS[11:0]);
    wire        hit_cur  = (off == SURF_VIDEO_CURSOR[11:0]);
    wire        hit_scr  = (off == SURF_VIDEO_SCROLL[11:0]);
    wire        hit_ie   = (off == SURF_VIDEO_IE[11:0]);
    wire        hit_info = (off == SURF_VIDEO_INFO[11:0]);
    wire        hit_pclk = (off == SURF_VIDEO_PIXCLK[11:0]);
    wire        hit_pal  = (off >= SURF_VIDEO_PALETTE[11:0]) &&
                           (off < SURF_VIDEO_PALETTE[11:0] + 12'd4 * SURF_VIDEO_PALETTE_COUNT[11:0]) &&
                           (off[1:0] == 2'b00);
    wire [11:0] pal_off  = off - SURF_VIDEO_PALETTE[11:0];
    wire [3:0]  pal_idx  = pal_off[5:2];
    wire        known    = hit_ctrl | hit_stat | hit_cur | hit_scr | hit_ie | hit_info | hit_pclk | hit_pal;
    wire        legal    = known && (s_req_be == 4'b1111) && !s_req_fetch;
    wire        accept   = s_req_valid;                        // always ready: one-cycle answers
    wire        wr       = accept && legal && s_req_write;
    assign s_req_ready = 1'b1;

    // ---------------------------------------------------------------- sys-domain registers
    reg [4:0]   ctrl_q   = CTRL_RESET;      // {CURSOR_BLINK, CURSOR_EN, MODE[1:0], ENABLE}
    reg [6:0]   cur_col  = 7'd0;
    reg [4:0]   cur_row  = 5'd0;
    reg [4:0]   scroll_q = 5'd0;
    reg         ie_q     = 1'b0;
    reg [191:0] pal_q    = PAL_RESET;       // entry n in bits 12n+11 : 12n
    reg         st_ev    = 1'b0;
    reg [15:0]  frame_q  = 16'd0;

    wire vb_level;                          // from the pix domain
    wire vb_start;                          // one clk pulse per frame

    always @(posedge clk) begin
        if (rst) begin
            ctrl_q   <= CTRL_RESET;
            cur_col  <= 7'd0;
            cur_row  <= 5'd0;
            scroll_q <= 5'd0;
            ie_q     <= 1'b0;
            pal_q    <= PAL_RESET;
            st_ev    <= 1'b0;
            frame_q  <= 16'd0;
        end else begin
            if (wr && hit_ctrl) begin
                ctrl_q[0]   <= s_req_wdata[SURF_VIDEO_CTRL_ENABLE_LSB];
                ctrl_q[2:1] <= s_req_wdata[SURF_VIDEO_CTRL_MODE_LSB +: 2];
                ctrl_q[3]   <= s_req_wdata[SURF_VIDEO_CTRL_CURSOR_EN_LSB];
                ctrl_q[4]   <= s_req_wdata[SURF_VIDEO_CTRL_CURSOR_BLINK_LSB];
            end
            if (wr && hit_cur) begin
                cur_col <= s_req_wdata[SURF_VIDEO_CURSOR_COL_LSB +: 7];
                cur_row <= s_req_wdata[SURF_VIDEO_CURSOR_ROW_LSB +: 5];
            end
            if (wr && hit_scr) scroll_q <= s_req_wdata[SURF_VIDEO_SCROLL_ROW_LSB +: 5];
            if (wr && hit_ie)  ie_q     <= s_req_wdata[SURF_VIDEO_IE_VBLANK_LSB];
            if (wr && hit_pal) pal_q[pal_idx * 12 +: 12] <= s_req_wdata[11:0];
            if (wr && hit_stat && s_req_wdata[SURF_VIDEO_STATUS_VBLANK_EV_LSB]) st_ev <= 1'b0;
            if (vb_start) begin                 // a new blanking interval: set wins over clear
                st_ev   <= 1'b1;
                frame_q <= frame_q + 16'd1;
            end
        end
    end

    // ---------------------------------------------------------------- sys -> pix settings
    localparam integer CFG_W = 192 + 5 + 5 + 7 + 5;
    localparam [CFG_W-1:0] CFG_RESET = {PAL_RESET, 5'd0, 5'd0, 7'd0, CTRL_RESET};
    wire [CFG_W-1:0] cfg_sys = {pal_q, scroll_q, cur_row, cur_col, ctrl_q};
    wire [CFG_W-1:0] cfg_pix;
    surf_cdc_word #(.WIDTH(CFG_W), .INIT(CFG_RESET)) u_cfg_cdc (
        .src_clk(clk), .src_rst(rst), .src_data(cfg_sys),
        .dst_clk(pix_clk), .dst_rst(pix_rst), .dst_data(cfg_pix));

    wire [4:0]   p_ctrl   = cfg_pix[4:0];
    wire [6:0]   p_curcol = cfg_pix[11:5];
    wire [4:0]   p_currow = cfg_pix[16:12];
    wire [4:0]   p_scroll = cfg_pix[21:17];
    wire [191:0] p_pal    = cfg_pix[213:22];

    // ---------------------------------------------------------------- pix: timing
    reg [9:0] hcnt = 10'd0, vcnt = 10'd0;
    always @(posedge pix_clk) begin
        if (pix_rst) begin
            hcnt <= 10'd0;
            vcnt <= 10'd0;
        end else if (hcnt == H_TOT - 10'd1) begin
            hcnt <= 10'd0;
            vcnt <= (vcnt == V_TOT - 10'd1) ? 10'd0 : vcnt + 10'd1;
        end else begin
            hcnt <= hcnt + 10'd1;
        end
    end

    // per-frame copies of the settings, taken at the start of the last blank line
    wire frame_latch = (hcnt == 10'd0) && (vcnt == V_TOT - 10'd1);
    wire vb_begin    = (hcnt == 10'd0) && (vcnt == V_VIS);
    reg       f_enable = 1'b1;
    reg [1:0] f_mode   = 2'd0;
    reg       f_curen  = 1'b0, f_blink = 1'b0;
    reg [6:0] f_curcol = 7'd0;
    reg [4:0] f_currow = 5'd0;
    reg [4:0] f_scroll = 5'd0;
    reg [4:0] pframe   = 5'd0;              // frames, for the cursor blink
    reg       vb_q     = 1'b0;              // in vertical blanking (registered for the crossing)
    reg       vb_pulse = 1'b0;
    always @(posedge pix_clk) begin
        if (pix_rst) begin
            f_enable <= CTRL_RESET[0];
            f_mode   <= CTRL_RESET[2:1];
            f_curen  <= 1'b0;
            f_blink  <= 1'b0;
            f_curcol <= 7'd0;
            f_currow <= 5'd0;
            f_scroll <= 5'd0;
            pframe   <= 5'd0;
            vb_q     <= 1'b0;
            vb_pulse <= 1'b0;
        end else begin
            vb_q     <= (vcnt >= V_VIS);
            vb_pulse <= vb_begin;
            if (vb_begin) pframe <= pframe + 5'd1;
            if (frame_latch) begin
                f_enable <= p_ctrl[0];
                f_mode   <= p_ctrl[2:1];
                f_curen  <= p_ctrl[3];
                f_blink  <= p_ctrl[4];
                f_curcol <= p_curcol;
                f_currow <= p_currow;
                f_scroll <= (p_scroll >= ROWS) ? 5'd0 : p_scroll;   // 30 and 31 act as 0
            end
        end
    end

    // ---------------------------------------------------------------- pix: S0
    wire       vis0  = (hcnt < H_VIS) && (vcnt < V_VIS);
    wire       hs0   = !((hcnt >= H_VIS + H_FP) && (hcnt < H_VIS + H_FP + H_SYNC));
    wire       vs0   = !((vcnt >= V_VIS + V_FP) && (vcnt < V_VIS + V_FP + V_SYNC));
    wire [6:0] col0  = hcnt[9:3];
    wire [4:0] srow0 = vcnt[8:4];
    wire [3:0] gl0   = vcnt[3:0];
    wire [2:0] px0   = hcnt[2:0];
    wire [5:0] tsum  = {1'b0, srow0} + {1'b0, f_scroll};
    wire [5:0] trow0 = (tsum >= {1'b0, ROWS}) ? tsum - {1'b0, ROWS} : tsum;
    wire [11:0] cell0 = {trow0, 6'd0} + {2'd0, trow0, 4'd0} + {5'd0, col0};   // trow*80 + col
    wire       blink_on = !f_blink || pframe[4];
    wire       cur0  = f_curen && blink_on && (col0 == f_curcol) && (srow0 == f_currow) && (gl0 >= CURSOR_TOP);

    // the test pattern
    wire [17:0] h205  = {8'd0, hcnt} * 18'd205;   // h*205 >> 14 = h/80, >> 13 = h/40, exact below 640
    wire [2:0]  bar   = h205[16:14];
    wire [3:0]  level = h205[16:13];
    wire        border = (hcnt == 10'd0) || (hcnt == H_VIS - 10'd1) || (vcnt == 10'd0) || (vcnt == V_VIS - 10'd1);
    reg  [11:0] pat0;
    always @(*) begin
        if (border)
            pat0 = 12'hFFF;
        else if (vcnt < 10'd360)                  // R where bar is 0 1 4 5, G 0..3, B the even ones
            pat0 = {{4{!bar[1]}}, {4{!bar[2]}}, {4{!bar[0]}}};
        else if (vcnt < 10'd390)
            pat0 = {level, level, level};
        else if (vcnt < 10'd420)
            pat0 = {level, 8'h00};
        else if (vcnt < 10'd450)
            pat0 = {4'h0, level, 4'h0};
        else
            pat0 = {8'h00, level};
    end

    // ---------------------------------------------------------------- pix: S1..S4
    reg [11:0] addr1 = 12'd0;
    reg [2:0]  px1 = 3'd0, px2 = 3'd0, px3 = 3'd0;
    reg [3:0]  gl1 = 4'd0, gl2 = 4'd0;
    reg        vis1 = 1'b0, vis2 = 1'b0, vis3 = 1'b0;
    reg        hs1 = 1'b1, hs2 = 1'b1, hs3 = 1'b1;
    reg        vs1 = 1'b1, vs2 = 1'b1, vs3 = 1'b1;
    reg        cur1 = 1'b0, cur2 = 1'b0, cur3 = 1'b0;
    reg [11:0] pat1 = 12'd0, pat2 = 12'd0, pat3 = 12'd0;
    reg [3:0]  fg3 = 4'd0, bg3 = 4'd0;
    reg [3:0]  r4 = 4'd0, g4 = 4'd0, b4 = 4'd0;
    reg        hs4 = 1'b1, vs4 = 1'b1;

    wire [7:0] font_row;
    surf_video_font u_font (.clk(pix_clk), .addr({text_data[7:0], gl2}), .data(font_row));

    always @(posedge pix_clk) begin
        if (pix_rst) begin
            addr1 <= 12'd0;
            px1 <= 3'd0; px2 <= 3'd0; px3 <= 3'd0;
            gl1 <= 4'd0; gl2 <= 4'd0;
            vis1 <= 1'b0; vis2 <= 1'b0; vis3 <= 1'b0;
            hs1 <= 1'b1; hs2 <= 1'b1; hs3 <= 1'b1;
            vs1 <= 1'b1; vs2 <= 1'b1; vs3 <= 1'b1;
            cur1 <= 1'b0; cur2 <= 1'b0; cur3 <= 1'b0;
            pat1 <= 12'd0; pat2 <= 12'd0; pat3 <= 12'd0;
            fg3 <= 4'd0; bg3 <= 4'd0;
        end else begin
            // S0 -> S1
            addr1 <= cell0;
            px1 <= px0;  gl1 <= gl0;  vis1 <= vis0;  hs1 <= hs0;  vs1 <= vs0;  cur1 <= cur0;  pat1 <= pat0;
            // S1 -> S2 (the text RAM registers the cell meanwhile)
            px2 <= px1;  gl2 <= gl1;  vis2 <= vis1;  hs2 <= hs1;  vs2 <= vs1;  cur2 <= cur1;  pat2 <= pat1;
            // S2 -> S3 (the font ROM registers the glyph row meanwhile)
            px3 <= px2;  vis3 <= vis2;  hs3 <= hs2;  vs3 <= vs2;  cur3 <= cur2;  pat3 <= pat2;
            fg3 <= text_data[11:8];
            bg3 <= text_data[15:12];
        end
    end

    wire        on3   = font_row[3'd7 - px3] || cur3;
    wire [3:0]  idx3  = on3 ? fg3 : bg3;
    wire [11:0] txt3  = p_pal[idx3 * 12 +: 12];
    wire [11:0] rgb3  = (!vis3 || !f_enable) ? 12'h000 :
                        (f_mode == 2'd0)     ? pat3 :
                        (f_mode == 2'd1)     ? txt3 : 12'h000;

    always @(posedge pix_clk) begin
        if (pix_rst) begin
            r4  <= 4'd0;
            g4  <= 4'd0;
            b4  <= 4'd0;
            hs4 <= 1'b1;
            vs4 <= 1'b1;
        end else begin                          // S3 -> S4: the pins
            r4  <= rgb3[11:8];
            g4  <= rgb3[7:4];
            b4  <= rgb3[3:0];
            hs4 <= hs3;
            vs4 <= vs3;
        end
    end

    assign text_addr = addr1;
    assign vga_r  = r4;
    assign vga_g  = g4;
    assign vga_b  = b4;
    assign vga_hs = hs4;
    assign vga_vs = vs4;

    // ---------------------------------------------------------------- pix -> sys
    surf_sync #(.STAGES(2), .INIT(1'b0)) u_vb_sync (
        .clk(clk), .rst(rst), .d(vb_q), .q(vb_level));
    surf_pulse_sync u_vb_pulse (
        .src_clk(pix_clk), .src_rst(pix_rst), .src_pulse(vb_pulse),
        .dst_clk(clk), .dst_rst(rst), .dst_pulse(vb_start));

    // ---------------------------------------------------------------- reads and responses
    localparam [31:0] PIX_HZ_32 = PIX_HZ;
    reg [31:0] rdata;
    always @(*) begin
        rdata = 32'd0;
        if (hit_ctrl) begin
            rdata[SURF_VIDEO_CTRL_ENABLE_LSB]       = ctrl_q[0];
            rdata[SURF_VIDEO_CTRL_MODE_LSB +: 2]    = ctrl_q[2:1];
            rdata[SURF_VIDEO_CTRL_CURSOR_EN_LSB]    = ctrl_q[3];
            rdata[SURF_VIDEO_CTRL_CURSOR_BLINK_LSB] = ctrl_q[4];
        end
        if (hit_stat) begin
            rdata[SURF_VIDEO_STATUS_VBLANK_LSB]     = vb_level;
            rdata[SURF_VIDEO_STATUS_VBLANK_EV_LSB]  = st_ev;
            rdata[SURF_VIDEO_STATUS_FRAME_LSB +: 16] = frame_q;
        end
        if (hit_cur) begin
            rdata[SURF_VIDEO_CURSOR_COL_LSB +: 7] = cur_col;
            rdata[SURF_VIDEO_CURSOR_ROW_LSB +: 5] = cur_row;
        end
        if (hit_scr)  rdata[SURF_VIDEO_SCROLL_ROW_LSB +: 5] = scroll_q;
        if (hit_ie)   rdata[SURF_VIDEO_IE_VBLANK_LSB] = ie_q;
        if (hit_info) begin
            rdata[SURF_VIDEO_INFO_COLS_LSB +: 8]    = {1'b0, COLS};
            rdata[SURF_VIDEO_INFO_ROWS_LSB +: 8]    = {3'd0, ROWS};
            rdata[SURF_VIDEO_INFO_GLYPH_W_LSB +: 4] = 4'd8;
            rdata[SURF_VIDEO_INFO_GLYPH_H_LSB +: 5] = 5'd16;
        end
        if (hit_pclk) rdata = PIX_HZ_32;
        if (hit_pal)  rdata[11:0] = pal_q[pal_idx * 12 +: 12];
    end

    reg        rsp_valid_q = 1'b0;
    reg        rsp_err_q   = 1'b0;
    reg [31:0] rsp_rdata_q = 32'd0;
    always @(posedge clk) begin
        if (rst) begin
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'd0;
        end else begin
            rsp_valid_q <= accept;
            rsp_err_q   <= accept && !legal;
            rsp_rdata_q <= (accept && legal && !s_req_write) ? rdata : 32'd0;
        end
    end

    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_rdata = rsp_rdata_q;
    assign s_rsp_err   = rsp_err_q;
    assign irq         = st_ev && ie_q;

    // unused: address bits above the window, write data above the fields, the reserved
    // half of each text cell, and the counter bits the pattern does not need
    wire unused_ok = &{1'b0, s_req_addr[31:12], s_req_wdata[31:13], text_data[31:16],
                       h205[17], h205[12:0], pal_off[11:6], pal_off[1:0], vcnt[9]};
endmodule
