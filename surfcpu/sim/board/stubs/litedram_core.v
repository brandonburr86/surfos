/*
SurfCPU LiteDRAM core stand-in for the board top's lint (no behaviour)
--------------------
File: litedram_core.v Date: 10/16/26 (SurfCPU)
--------------------
The ports of litedram_core, the LiteDRAM core ip/litedram/generate.sh makes from
ip/litedram/arty.yml (build/litedram/arty/gateware/litedram_core.v, which needs the pinned
generator and the network the first time). With it, make lint (lint-board) checks the top's DDR
variant (ARTY_HAS_DDR) without generating anything. Its outputs are constants: calibration never
finishes, no user-port handshake, the DDR3 pins idle. ip/litedram/inspect_core.py checks that
these ports are exactly the generated core's (make litedram-gen). Lint and simulation only:
tools/fpga-build.sh builds the generated core, never this file.
*/
/* verilator lint_off UNUSEDSIGNAL */
module litedram_core (
    input  wire          clk,
    output wire   [13:0] ddram_a,
    output wire    [2:0] ddram_ba,
    output wire          ddram_cas_n,
    output wire          ddram_cke,
    output wire          ddram_clk_n,
    output wire          ddram_clk_p,
    output wire          ddram_cs_n,
    output wire    [1:0] ddram_dm,
    inout  wire   [15:0] ddram_dq,
    inout  wire    [1:0] ddram_dqs_n,
    inout  wire    [1:0] ddram_dqs_p,
    output wire          ddram_odt,
    output wire          ddram_ras_n,
    output wire          ddram_reset_n,
    output wire          ddram_we_n,
    output wire          init_done,
    output wire          init_error,
    output wire          pll_locked,
    input  wire          rst,
    input  wire          uart_rx,
    output reg           uart_tx,
    output wire          user_clk,
    input  wire   [23:0] user_port_native_0_cmd_addr,
    output wire          user_port_native_0_cmd_ready,
    input  wire          user_port_native_0_cmd_valid,
    input  wire          user_port_native_0_cmd_we,
    output wire  [127:0] user_port_native_0_rdata_data,
    input  wire          user_port_native_0_rdata_ready,
    output wire          user_port_native_0_rdata_valid,
    input  wire  [127:0] user_port_native_0_wdata_data,
    output wire          user_port_native_0_wdata_ready,
    input  wire          user_port_native_0_wdata_valid,
    input  wire   [15:0] user_port_native_0_wdata_we,
    output wire          user_rst
);
    initial uart_tx = 1'b1;
    assign {ddram_a, ddram_ba} = 17'h0;
    assign {ddram_cas_n, ddram_ras_n, ddram_we_n, ddram_cs_n} = 4'hF;
    assign {ddram_cke, ddram_odt, ddram_reset_n} = 3'b000;
    assign {ddram_clk_p, ddram_clk_n} = 2'b01;
    assign ddram_dm = 2'b00;
    assign ddram_dq = 16'hzzzz;
    assign ddram_dqs_p = 2'bzz;
    assign ddram_dqs_n = 2'bzz;
    assign {init_done, init_error, pll_locked} = 3'b000;
    assign user_clk = clk;
    assign user_rst = 1'b1;
    assign user_port_native_0_cmd_ready = 1'b0;
    assign user_port_native_0_wdata_ready = 1'b0;
    assign user_port_native_0_rdata_valid = 1'b0;
    assign user_port_native_0_rdata_data = 128'h0;
endmodule
/* verilator lint_on UNUSEDSIGNAL */
