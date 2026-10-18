/*
SurfCPU surf_video test top
--------------------
File: tb_video.v Date: 10/15/26 (SurfCPU)
--------------------
surf_video with a stand-in for surf_textram (which the soc track owns): a 4096-word RAM
written from the sys clock by the C++ harness (tram_we/waddr/wdata) and read on pix_clk
with the pixel port's behaviour of MODULES.md: the word for text_addr appears after one
pix_clk edge. Simulation only.
HAS_GFX (10/18/26, SurfGPU) builds surf_video with its graphics modes, and a stand-in for
surf_vram's pixel port: 76,800 words written by the harness (vram_we/waddr/wdata), the word
for vram_addr two pix_clk edges later, 0 at or above 76,800 (docs/GPU.md section 2).
*/
module tb_video #(
    parameter integer HAS_GFX = 0
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        pix_clk,
    input  wire        pix_rst,
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
    input  wire        tram_we,
    input  wire [11:0] tram_waddr,
    input  wire [31:0] tram_wdata,
    input  wire        vram_we,
    input  wire [16:0] vram_waddr,
    input  wire [31:0] vram_wdata,
    output wire        vblank,
    output wire [11:0] text_addr,
    output wire [3:0]  vga_r,
    output wire [3:0]  vga_g,
    output wire [3:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs
);
    reg [31:0] tram [0:4095];
    integer t;
    initial for (t = 0; t < 4096; t = t + 1) tram[t] = 32'd0;          // as surf_textram starts
    always @(posedge clk) if (tram_we) tram[tram_waddr] <= tram_wdata;

    reg [31:0] text_data = 32'd0;
    always @(posedge pix_clk) text_data <= tram[text_addr];

    reg [31:0] vram [0:76799];
    integer k;
    initial for (k = 0; k < 76800; k = k + 1) vram[k] = 32'd0;
    always @(posedge clk) if (vram_we && vram_waddr < 17'd76800) vram[vram_waddr] <= vram_wdata;
    wire [16:0] vram_addr;
    reg  [31:0] vram_q1 = 32'd0, vram_data = 32'd0;
    reg         vram_ok1 = 1'b0;
    always @(posedge pix_clk) begin
        vram_q1   <= vram[vram_addr < 17'd76800 ? vram_addr : 17'd0];
        vram_ok1  <= vram_addr < 17'd76800;
        vram_data <= vram_ok1 ? vram_q1 : 32'd0;
    end

`include "surfcpu_soc.vh"
    surf_video #(.PIX_HZ(SURF_PIX_HZ), .HAS_GFX(HAS_GFX)) dut (
        .clk(clk), .rst(rst),
        .s_req_valid(s_req_valid), .s_req_ready(s_req_ready), .s_req_addr(s_req_addr),
        .s_req_write(s_req_write), .s_req_be(s_req_be), .s_req_wdata(s_req_wdata),
        .s_req_fetch(s_req_fetch), .s_rsp_valid(s_rsp_valid), .s_rsp_rdata(s_rsp_rdata),
        .s_rsp_err(s_rsp_err), .irq(irq),
        .pix_clk(pix_clk), .pix_rst(pix_rst), .text_addr(text_addr), .text_data(text_data),
        .vram_addr(vram_addr), .vram_data(vram_data), .vblank(vblank),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs));
endmodule
