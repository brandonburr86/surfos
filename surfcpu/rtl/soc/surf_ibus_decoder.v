/*
SurfCPU instruction-bus decoder
--------------------
File: surf_ibus_decoder.v Date: 10/15/26 (SurfCPU)
--------------------
The I decoder of docs/SURFBUS.md section 6: the core's instruction port reaches only the
regions with fetch in docs/MEMORY-MAP.md (boot ROM port A, scratch RAM port A, DDR when
HAS_DDR is 1). Everything else, including MMIO, text RAM and unmapped addresses, goes to the
decoder's own error slave, which the core reports as an instruction access fault. The fetch
port never writes; a write offered here is answered with an error as well. Target numbers
are in surf_bus_targets.vh; the DDR target is the arbiter's fetch side.
*/
`include "surf_bus_targets.vh"

module surf_ibus_decoder #(
    parameter integer HAS_DDR = 1
) (
    input  wire                         clk,
    input  wire                         rst,
    // from the core's instruction port
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
    output wire [`SURF_IT_COUNT-1:0]    m_req_valid,
    input  wire [`SURF_IT_COUNT-1:0]    m_req_ready,
    output wire [31:0]                  m_req_addr,
    output wire                         m_req_write,
    output wire [3:0]                   m_req_be,
    output wire [31:0]                  m_req_wdata,
    output wire                         m_req_fetch,
    input  wire [`SURF_IT_COUNT-1:0]    m_rsp_valid,
    input  wire [32*`SURF_IT_COUNT-1:0] m_rsp_rdata,
    input  wire [`SURF_IT_COUNT-1:0]    m_rsp_err
);
    `include "surfcpu_soc.vh"

    localparam integer N = `SURF_IT_COUNT;

    wire [31:0] a      = s_req_addr;
    wire        in_rom = (a & ~SURF_BOOTROM_MASK) == SURF_BOOTROM_BASE;
    wire        in_scr = (a & ~SURF_SCRATCH_MASK) == SURF_SCRATCH_BASE;
    wire        in_ddr = (HAS_DDR != 0) && ((a & ~SURF_DDR_MASK) == SURF_DDR_BASE);
    wire        rd     = !s_req_write;

    wire [N-1:0] hit;
    assign hit[`SURF_IT_ROM]     = in_rom && rd;
    assign hit[`SURF_IT_SCRATCH] = in_scr && rd;
    assign hit[`SURF_IT_DDR]     = in_ddr && rd;
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
