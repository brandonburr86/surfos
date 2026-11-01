/*
SurfCPU data-bus decoder
--------------------
File: surf_dbus_decoder.v Date: 10/16/26 (SurfCPU)
--------------------
The D decoder of docs/SURFBUS.md section 6: the core's data port reaches every region of
docs/MEMORY-MAP.md. The boot ROM takes reads only (a write goes to the error slave and
changes nothing); MMIO windows and text RAM take no fetches (the data port never fetches,
but a request marked as a fetch follows the fetch rules anyway); the reserved Ethernet
window, the DDR window while HAS_DDR is 0, the DDR controller's registers (ddrc) unless
HAS_DDRC is 1, the gpu and vram windows unless HAS_GPU is 1 (docs/GPU.md), and every unmapped
address go to the decoder's own error slave. Target numbers are in surf_bus_targets.vh; the DDR target is the arbiter's
data side, where the calibration gate answers with errors until DDR is usable.
*/
`include "surf_bus_targets.vh"

module surf_dbus_decoder #(
    parameter integer HAS_DDR  = 1,
    parameter integer HAS_DDRC = 0,    // the ddrc registers decode (a controller that needs software)
    parameter integer HAS_GPU  = 0     // the gpu and vram windows decode (docs/GPU.md)
) (
    input  wire                         clk,
    input  wire                         rst,
    // from the core's data port
    input  wire                         s_req_valid,
    output wire                         s_req_ready,
    input  wire [31:0]                  s_req_addr,
    input  wire                         s_req_write,
    input  wire [3:0]                   s_req_be,
    input  wire [31:0]                  s_req_wdata,
    input  wire                         s_req_fetch,
    output wire                         s_rsp_valid,
    output wire [31:0]                  s_rsp_rdata,
    output wire                         s_rsp_err,
    // to the targets (shared request fields, one req_valid per target)
    output wire [`SURF_DT_COUNT-1:0]    m_req_valid,
    input  wire [`SURF_DT_COUNT-1:0]    m_req_ready,
    output wire [31:0]                  m_req_addr,
    output wire                         m_req_write,
    output wire [3:0]                   m_req_be,
    output wire [31:0]                  m_req_wdata,
    output wire                         m_req_fetch,
    input  wire [`SURF_DT_COUNT-1:0]    m_rsp_valid,
    input  wire [32*`SURF_DT_COUNT-1:0] m_rsp_rdata,
    input  wire [`SURF_DT_COUNT-1:0]    m_rsp_err
);
    `include "surfcpu_soc.vh"

    localparam integer N = `SURF_DT_COUNT;

    wire [31:0] a  = s_req_addr;
    wire        wr = s_req_write;
    wire        nf = !s_req_fetch;    // regions without fetch take only non-fetch requests

    wire [N-1:0] hit;
    assign hit[`SURF_DT_ROM]     = ((a & ~SURF_BOOTROM_MASK) == SURF_BOOTROM_BASE) && !wr;
    assign hit[`SURF_DT_SCRATCH] = ((a & ~SURF_SCRATCH_MASK) == SURF_SCRATCH_BASE);
    assign hit[`SURF_DT_UART]    = ((a & ~SURF_UART_MASK)    == SURF_UART_BASE)    && nf;
    assign hit[`SURF_DT_TIMER]   = ((a & ~SURF_TIMER_MASK)   == SURF_TIMER_BASE)   && nf;
    assign hit[`SURF_DT_INTC]    = ((a & ~SURF_INTC_MASK)    == SURF_INTC_BASE)    && nf;
    assign hit[`SURF_DT_GPIO]    = ((a & ~SURF_GPIO_MASK)    == SURF_GPIO_BASE)    && nf;
    assign hit[`SURF_DT_SPI]     = ((a & ~SURF_SPI_MASK)     == SURF_SPI_BASE)     && nf;
    assign hit[`SURF_DT_PS2]     = ((a & ~SURF_PS2_MASK)     == SURF_PS2_BASE)     && nf;
    assign hit[`SURF_DT_VIDEO]   = ((a & ~SURF_VIDEO_MASK)   == SURF_VIDEO_BASE)   && nf;
    assign hit[`SURF_DT_SYSINFO] = ((a & ~SURF_SYSINFO_MASK) == SURF_SYSINFO_BASE) && nf;
    assign hit[`SURF_DT_TEXTRAM] = ((a & ~SURF_TEXTRAM_MASK) == SURF_TEXTRAM_BASE) && nf;
    assign hit[`SURF_DT_GPU]     = (HAS_GPU != 0) && ((a & ~SURF_GPU_MASK) == SURF_GPU_BASE) && nf;
    assign hit[`SURF_DT_VRAM]    = (HAS_GPU != 0) && ((a & ~SURF_VRAM_MASK) == SURF_VRAM_BASE) && nf;
    assign hit[`SURF_DT_DDRC]    = (HAS_DDRC != 0) && ((a & ~SURF_DDRC_MASK) == SURF_DDRC_BASE) && nf;
    assign hit[`SURF_DT_DDR]     = (HAS_DDR != 0) && ((a & ~SURF_DDR_MASK) == SURF_DDR_BASE);
    wire to_err = ~|hit;

    // the error slave is the demultiplexer's last port
    wire        e_req_valid, e_req_ready, e_rsp_valid, e_rsp_err;
    wire [31:0] e_rsp_rdata;
    wire [31:0] e_req_addr, e_req_wdata;
    wire        e_req_write, e_req_fetch;
    wire [3:0]  e_req_be;

    surf_bus_demux #(.N(N + 1)) u_demux (
        .clk(clk), .rst(rst), .sel({to_err, hit}),
        .s_req_valid(s_req_valid), .s_req_ready(s_req_ready), .s_req_addr(s_req_addr),
        .s_req_write(s_req_write), .s_req_be(s_req_be), .s_req_wdata(s_req_wdata),
        .s_req_fetch(s_req_fetch), .s_rsp_valid(s_rsp_valid), .s_rsp_rdata(s_rsp_rdata),
        .s_rsp_err(s_rsp_err),
        .m_req_valid({e_req_valid, m_req_valid}), .m_req_ready({e_req_ready, m_req_ready}),
        .m_req_addr(e_req_addr), .m_req_write(e_req_write), .m_req_be(e_req_be),
        .m_req_wdata(e_req_wdata), .m_req_fetch(e_req_fetch),
        .m_rsp_valid({e_rsp_valid, m_rsp_valid}), .m_rsp_rdata({e_rsp_rdata, m_rsp_rdata}),
        .m_rsp_err({e_rsp_err, m_rsp_err}));

    assign m_req_addr  = e_req_addr;
    assign m_req_write = e_req_write;
    assign m_req_be    = e_req_be;
    assign m_req_wdata = e_req_wdata;
    assign m_req_fetch = e_req_fetch;

    surf_bus_err u_err (
        .clk(clk), .rst(rst),
        .s_req_valid(e_req_valid), .s_req_ready(e_req_ready), .s_req_addr(e_req_addr),
        .s_req_write(e_req_write), .s_req_be(e_req_be), .s_req_wdata(e_req_wdata),
        .s_req_fetch(e_req_fetch), .s_rsp_valid(e_rsp_valid), .s_rsp_rdata(e_rsp_rdata),
        .s_rsp_err(e_rsp_err));
endmodule
