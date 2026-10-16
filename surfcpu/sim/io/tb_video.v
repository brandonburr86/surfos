/*
SurfCPU surf_video test top
--------------------
File: tb_video.v Date: 10/15/26 (SurfCPU)
--------------------
surf_video with a stand-in for surf_textram (which the soc track owns): a 4096-word RAM
written from the sys clock by the C++ harness (tram_we/waddr/wdata) and read on pix_clk
with the pixel port's behaviour of MODULES.md: the word for text_addr appears after one
pix_clk edge. Simulation only.
*/
module tb_video (
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
    output wire [11:0] text_addr,
    output wire [3:0]  vga_r,
    output wire [3:0]  vga_g,
    output wire [3:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs
);
    reg [31:0] tram [0:4095];
    always @(posedge clk) if (tram_we) tram[tram_waddr] <= tram_wdata;

    reg [31:0] text_data = 32'd0;
    always @(posedge pix_clk) text_data <= tram[text_addr];

    surf_video #(.PIX_HZ(25177305)) dut (
        .clk(clk), .rst(rst),
        .s_req_valid(s_req_valid), .s_req_ready(s_req_ready), .s_req_addr(s_req_addr),
        .s_req_write(s_req_write), .s_req_be(s_req_be), .s_req_wdata(s_req_wdata),
        .s_req_fetch(s_req_fetch), .s_rsp_valid(s_rsp_valid), .s_rsp_rdata(s_rsp_rdata),
        .s_rsp_err(s_rsp_err), .irq(irq),
        .pix_clk(pix_clk), .pix_rst(pix_rst), .text_addr(text_addr), .text_data(text_data),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs));
endmodule
