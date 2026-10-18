/*
SurfGPU engine test top
--------------------
File: tb_gpu.v Date: 10/18/26 (SurfGPU)
--------------------
surf_gpu and surf_vram as the SoC wires them (docs/GPU.md section 7), behind one SurfBus slave
port: a request in the gpu window goes to surf_gpu's registers, anything else to VRAM's CPU
window (the C++ harness sends VRAM addresses as offsets with the vram base). Responses come back
from the slave that took the request (both answer in the next cycle). VRAM's pixel port is the
harness's backdoor: pix_clk is clk here and pix_addr / pix_data read a word two edges later, so
the harness can compare all of VRAM with the C model without touching port A. vblank is driven
by the harness. Simulation only.
*/
module tb_gpu (
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
    input  wire        vblank,
    input  wire [16:0] pix_addr,
    output wire [31:0] pix_data,
    // the engine's port, for the harness's watch on it
    output wire        m_req,
    output wire        m_gnt,
    output wire        m_we,
    output wire [3:0]  m_be,
    output wire [16:0] m_addr,
    output wire [31:0] m_wdata
);
`include "surfcpu_soc.vh"
    wire to_gpu = (s_req_addr & ~SURF_GPU_MASK) == SURF_GPU_BASE;
    reg  sel_gpu = 1'b0;
    always @(posedge clk) if (s_req_valid) sel_gpu <= to_gpu;

    wire        g_ready, g_rv, g_re, v_ready, v_rv, v_re;
    wire [31:0] g_rd, v_rd;
    wire [31:0] m_rdata;
    wire        m_rvalid;

    surf_gpu u_gpu (
        .clk(clk), .rst(rst),
        .s_req_valid(s_req_valid && to_gpu), .s_req_ready(g_ready), .s_req_addr(s_req_addr),
        .s_req_write(s_req_write), .s_req_be(s_req_be), .s_req_wdata(s_req_wdata), .s_req_fetch(s_req_fetch),
        .s_rsp_valid(g_rv), .s_rsp_rdata(g_rd), .s_rsp_err(g_re), .irq(irq), .vblank(vblank),
        .m_req(m_req), .m_we(m_we), .m_be(m_be), .m_addr(m_addr), .m_wdata(m_wdata),
        .m_gnt(m_gnt), .m_rvalid(m_rvalid), .m_rdata(m_rdata));

    surf_vram u_vram (
        .clk(clk), .rst(rst),
        .s_req_valid(s_req_valid && !to_gpu), .s_req_ready(v_ready), .s_req_addr(s_req_addr),
        .s_req_write(s_req_write), .s_req_be(s_req_be), .s_req_wdata(s_req_wdata), .s_req_fetch(s_req_fetch),
        .s_rsp_valid(v_rv), .s_rsp_rdata(v_rd), .s_rsp_err(v_re),
        .g_req(m_req), .g_we(m_we), .g_be(m_be), .g_addr(m_addr), .g_wdata(m_wdata),
        .g_gnt(m_gnt), .g_rvalid(m_rvalid), .g_rdata(m_rdata),
        .pix_clk(clk), .pix_addr(pix_addr), .pix_data(pix_data));

    assign s_req_ready = to_gpu ? g_ready : v_ready;
    assign s_rsp_valid = g_rv | v_rv;
    assign s_rsp_rdata = sel_gpu ? g_rd : v_rd;
    assign s_rsp_err   = sel_gpu ? g_re : v_re;
endmodule
