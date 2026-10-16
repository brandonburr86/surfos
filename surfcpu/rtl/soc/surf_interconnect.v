/*
SurfCPU SurfBus interconnect
--------------------
File: surf_interconnect.v Date: 10/15/26 (SurfCPU)
--------------------
docs/SURFBUS.md section 6 in one block: the I decoder (instruction port: boot ROM A, scratch
A, DDR, error), the D decoder (data port: boot ROM B, scratch B, the eight MMIO windows, text
RAM A, DDR, error), the DDR arbiter (data first, one request at a time) and the calibration
gate (with HAS_DDR = 1, DDR-window accesses get errors until ddr_calib_done; nothing reaches
the bridge before). With HAS_DDR = 0 the DDR window is unmapped: both decoders send it to
their error slaves and the DDR port stays idle. The other targets come out as two flattened
buses, numbered as in surf_bus_targets.vh: it_* (2 targets) and dt_* (11 targets), with
shared request fields and one req_valid, req_ready, response per target.
*/
`include "surf_bus_targets.vh"

module surf_interconnect #(
    parameter integer HAS_DDR = 1
) (
    input  wire                       clk,
    input  wire                       rst,
    input  wire                       ddr_calib_done,
    // the core's instruction port
    input  wire                       i_req_valid,
    output wire                       i_req_ready,
    input  wire [31:0]                i_req_addr,
    input  wire                       i_req_write,
    input  wire [3:0]                 i_req_be,
    input  wire [31:0]                i_req_wdata,
    input  wire                       i_req_fetch,
    output wire                       i_rsp_valid,
    output wire [31:0]                i_rsp_rdata,
    output wire                       i_rsp_err,
    // the core's data port
    input  wire                       d_req_valid,
    output wire                       d_req_ready,
    input  wire [31:0]                d_req_addr,
    input  wire                       d_req_write,
    input  wire [3:0]                 d_req_be,
    input  wire [31:0]                d_req_wdata,
    input  wire                       d_req_fetch,
    output wire                       d_rsp_valid,
    output wire [31:0]                d_rsp_rdata,
    output wire                       d_rsp_err,
    // I targets except DDR (boot ROM port A, scratch port A)
    output wire [`SURF_IT_DDR-1:0]    it_req_valid,
    input  wire [`SURF_IT_DDR-1:0]    it_req_ready,
    output wire [31:0]                it_req_addr,
    output wire                       it_req_write,
    output wire [3:0]                 it_req_be,
    output wire [31:0]                it_req_wdata,
    output wire                       it_req_fetch,
    input  wire [`SURF_IT_DDR-1:0]    it_rsp_valid,
    input  wire [32*`SURF_IT_DDR-1:0] it_rsp_rdata,
    input  wire [`SURF_IT_DDR-1:0]    it_rsp_err,
    // D targets except DDR (boot ROM B, scratch B, MMIO, text RAM A)
    output wire [`SURF_DT_DDR-1:0]    dt_req_valid,
    input  wire [`SURF_DT_DDR-1:0]    dt_req_ready,
    output wire [31:0]                dt_req_addr,
    output wire                       dt_req_write,
    output wire [3:0]                 dt_req_be,
    output wire [31:0]                dt_req_wdata,
    output wire                       dt_req_fetch,
    input  wire [`SURF_DT_DDR-1:0]    dt_rsp_valid,
    input  wire [32*`SURF_DT_DDR-1:0] dt_rsp_rdata,
    input  wire [`SURF_DT_DDR-1:0]    dt_rsp_err,
    // the DDR master port (toward surf_ddr_bridge)
    output wire                       ddr_req_valid,
    input  wire                       ddr_req_ready,
    output wire [31:0]                ddr_req_addr,
    output wire                       ddr_req_write,
    output wire [3:0]                 ddr_req_be,
    output wire [31:0]                ddr_req_wdata,
    output wire                       ddr_req_fetch,
    input  wire                       ddr_rsp_valid,
    input  wire [31:0]                ddr_rsp_rdata,
    input  wire                       ddr_rsp_err
);
    localparam integer NI = `SURF_IT_COUNT;
    localparam integer ND = `SURF_DT_COUNT;
    localparam integer KI = `SURF_IT_DDR;     // DDR is the last target of each decoder
    localparam integer KD = `SURF_DT_DDR;

    // ---------------------------------------------------------------- I decoder
    wire [NI-1:0]    iv, ir, irv, ire;
    wire [32*NI-1:0] ird;
    wire [31:0]      ia, iw;
    wire             iwr, ifh;
    wire [3:0]       ibe;

    surf_ibus_decoder #(.HAS_DDR(HAS_DDR)) u_idec (
        .clk(clk), .rst(rst),
        .s_req_valid(i_req_valid), .s_req_ready(i_req_ready), .s_req_addr(i_req_addr),
        .s_req_write(i_req_write), .s_req_be(i_req_be), .s_req_wdata(i_req_wdata),
        .s_req_fetch(i_req_fetch), .s_rsp_valid(i_rsp_valid), .s_rsp_rdata(i_rsp_rdata),
        .s_rsp_err(i_rsp_err),
        .m_req_valid(iv), .m_req_ready(ir), .m_req_addr(ia), .m_req_write(iwr), .m_req_be(ibe),
        .m_req_wdata(iw), .m_req_fetch(ifh), .m_rsp_valid(irv), .m_rsp_rdata(ird),
        .m_rsp_err(ire));

    assign it_req_valid        = iv[KI-1:0];
    assign ir[KI-1:0]          = it_req_ready;
    assign it_req_addr         = ia;
    assign it_req_write        = iwr;
    assign it_req_be           = ibe;
    assign it_req_wdata        = iw;
    assign it_req_fetch        = ifh;
    assign irv[KI-1:0]         = it_rsp_valid;
    assign ird[32*KI-1:0]      = it_rsp_rdata;
    assign ire[KI-1:0]         = it_rsp_err;

    // ---------------------------------------------------------------- D decoder
    wire [ND-1:0]    dv, dr, drv, dre;
    wire [32*ND-1:0] drd;
    wire [31:0]      da, dw;
    wire             dwr, dfh;
    wire [3:0]       dbe;

    surf_dbus_decoder #(.HAS_DDR(HAS_DDR)) u_ddec (
        .clk(clk), .rst(rst),
        .s_req_valid(d_req_valid), .s_req_ready(d_req_ready), .s_req_addr(d_req_addr),
        .s_req_write(d_req_write), .s_req_be(d_req_be), .s_req_wdata(d_req_wdata),
        .s_req_fetch(d_req_fetch), .s_rsp_valid(d_rsp_valid), .s_rsp_rdata(d_rsp_rdata),
        .s_rsp_err(d_rsp_err),
        .m_req_valid(dv), .m_req_ready(dr), .m_req_addr(da), .m_req_write(dwr), .m_req_be(dbe),
        .m_req_wdata(dw), .m_req_fetch(dfh), .m_rsp_valid(drv), .m_rsp_rdata(drd),
        .m_rsp_err(dre));

    assign dt_req_valid        = dv[KD-1:0];
    assign dr[KD-1:0]          = dt_req_ready;
    assign dt_req_addr         = da;
    assign dt_req_write        = dwr;
    assign dt_req_be           = dbe;
    assign dt_req_wdata        = dw;
    assign dt_req_fetch        = dfh;
    assign drv[KD-1:0]         = dt_rsp_valid;
    assign drd[32*KD-1:0]      = dt_rsp_rdata;
    assign dre[KD-1:0]         = dt_rsp_err;

    // ---------------------------------------------------------------- DDR
    generate
        if (HAS_DDR != 0) begin : g_ddr
            wire        g_req_valid, g_req_ready, g_req_write, g_req_fetch;
            wire [31:0] g_req_addr, g_req_wdata;
            wire [3:0]  g_req_be;
            wire        g_rsp_valid, g_rsp_err;
            wire [31:0] g_rsp_rdata;

            surf_bus_arbiter u_arb (
                .clk(clk), .rst(rst),
                .a_req_valid(iv[KI]), .a_req_ready(ir[KI]), .a_req_addr(ia), .a_req_write(iwr),
                .a_req_be(ibe), .a_req_wdata(iw), .a_req_fetch(ifh), .a_rsp_valid(irv[KI]),
                .a_rsp_rdata(ird[32*KI +: 32]), .a_rsp_err(ire[KI]),
                .b_req_valid(dv[KD]), .b_req_ready(dr[KD]), .b_req_addr(da), .b_req_write(dwr),
                .b_req_be(dbe), .b_req_wdata(dw), .b_req_fetch(dfh), .b_rsp_valid(drv[KD]),
                .b_rsp_rdata(drd[32*KD +: 32]), .b_rsp_err(dre[KD]),
                .m_req_valid(g_req_valid), .m_req_ready(g_req_ready), .m_req_addr(g_req_addr),
                .m_req_write(g_req_write), .m_req_be(g_req_be), .m_req_wdata(g_req_wdata),
                .m_req_fetch(g_req_fetch), .m_rsp_valid(g_rsp_valid), .m_rsp_rdata(g_rsp_rdata),
                .m_rsp_err(g_rsp_err));

            surf_ddr_gate u_gate (
                .clk(clk), .rst(rst), .ddr_ok(ddr_calib_done),
                .s_req_valid(g_req_valid), .s_req_ready(g_req_ready), .s_req_addr(g_req_addr),
                .s_req_write(g_req_write), .s_req_be(g_req_be), .s_req_wdata(g_req_wdata),
                .s_req_fetch(g_req_fetch), .s_rsp_valid(g_rsp_valid), .s_rsp_rdata(g_rsp_rdata),
                .s_rsp_err(g_rsp_err),
                .m_req_valid(ddr_req_valid), .m_req_ready(ddr_req_ready),
                .m_req_addr(ddr_req_addr), .m_req_write(ddr_req_write), .m_req_be(ddr_req_be),
                .m_req_wdata(ddr_req_wdata), .m_req_fetch(ddr_req_fetch),
                .m_rsp_valid(ddr_rsp_valid), .m_rsp_rdata(ddr_rsp_rdata),
                .m_rsp_err(ddr_rsp_err));
        end else begin : g_noddr
            // the decoders never select DDR: its targets stay silent and the port idle
            assign ir[KI]             = 1'b0;
            assign irv[KI]            = 1'b0;
            assign ird[32*KI +: 32]   = 32'h0;
            assign ire[KI]            = 1'b0;
            assign dr[KD]             = 1'b0;
            assign drv[KD]            = 1'b0;
            assign drd[32*KD +: 32]   = 32'h0;
            assign dre[KD]            = 1'b0;
            assign ddr_req_valid      = 1'b0;
            assign ddr_req_addr       = 32'h0;
            assign ddr_req_write      = 1'b0;
            assign ddr_req_be         = 4'h0;
            assign ddr_req_wdata      = 32'h0;
            assign ddr_req_fetch      = 1'b0;
            wire unused_ok = &{1'b0, iv[KI], dv[KD], ddr_calib_done, ddr_req_ready,
                               ddr_rsp_valid, ddr_rsp_rdata, ddr_rsp_err};
        end
    endgenerate
endmodule
