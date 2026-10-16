/*
SurfCPU surf_video stand-in
--------------------
File: surf_video.v Date: 10/15/26 (SurfCPU)
--------------------
Simulation stand-in for the io track's VGA text display, used by mk/area-soc.mk only while
rtl/peripherals/surf_video.v does not exist; never part of a synthesized design. It has the
exact port list of docs/MODULES.md section 3 and keeps the SurfBus rules of the real block
(the registers of MEMORY-MAP.md, palette included, answer; unknown offsets, non-word accesses
and fetches get an error) but shows nothing: CTRL reads its reset value (ENABLE, test
pattern), INFO the text geometry, PIXCLK the pixel clock, everything else 0; writes are
ignored; the picture is black with both syncs inactive (high) and text address 0.
*/
module surf_video #(
    parameter integer PIX_HZ = 25173611
) (
    input  wire        clk,
    input  wire        rst,
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
    input  wire        pix_clk,
    input  wire        pix_rst,
    output wire [11:0] text_addr,
    input  wire [31:0] text_data,
    output wire [3:0]  vga_r,
    output wire [3:0]  vga_g,
    output wire [3:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs
);
    `include "surfcpu_soc.vh"

    localparam [31:0] PIXCLK_W = PIX_HZ;
    localparam [31:0] INFO     = (32'd80 << SURF_VIDEO_INFO_COLS_LSB) |
                                 (32'd30 << SURF_VIDEO_INFO_ROWS_LSB) |
                                 (32'd8  << SURF_VIDEO_INFO_GLYPH_W_LSB) |
                                 (32'd16 << SURF_VIDEO_INFO_GLYPH_H_LSB);
    localparam [9:0]  PAL_LO   = SURF_VIDEO_PALETTE[11:2];
    localparam [31:0] PAL_HI_W = (SURF_VIDEO_PALETTE >> 2) + SURF_VIDEO_PALETTE_COUNT - 1;
    localparam [9:0]  PAL_HI   = PAL_HI_W[9:0];

    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    reg        known;
    reg [31:0] rd_mux;
    always @* begin
        known  = 1'b1;
        rd_mux = 32'h0;
        case (woff)
            SURF_VIDEO_CTRL[11:2]:   rd_mux = SURF_VIDEO_CTRL_ENABLE_MASK;
            SURF_VIDEO_INFO[11:2]:   rd_mux = INFO;
            SURF_VIDEO_PIXCLK[11:2]: rd_mux = PIXCLK_W;
            SURF_VIDEO_STATUS[11:2], SURF_VIDEO_CURSOR[11:2], SURF_VIDEO_SCROLL[11:2],
            SURF_VIDEO_IE[11:2]:     rd_mux = 32'h0;
            default:                 known  = (woff >= PAL_LO) && (woff <= PAL_HI);
        endcase
    end

    reg        rsp_valid_q, rsp_err_q;
    reg [31:0] rsp_rdata_q;
    always @(posedge clk) begin
        if (rst) begin
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'h0;
        end else begin
            rsp_valid_q <= s_req_valid;
            rsp_err_q   <= s_req_valid && !(word_acc && known);
            rsp_rdata_q <= (s_req_valid && word_acc && known && !s_req_write) ? rd_mux : 32'h0;
        end
    end

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_err   = rsp_err_q;
    assign s_rsp_rdata = rsp_rdata_q;
    assign irq         = 1'b0;
    assign text_addr   = 12'h000;
    assign vga_r       = 4'h0;
    assign vga_g       = 4'h0;
    assign vga_b       = 4'h0;
    assign vga_hs      = 1'b1;
    assign vga_vs      = 1'b1;

    wire unused_ok = &{1'b0, s_req_addr[31:12], s_req_wdata, pix_clk, pix_rst, text_data};
endmodule
