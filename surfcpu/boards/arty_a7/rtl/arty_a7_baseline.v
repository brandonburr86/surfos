/*
SurfCPU Arty A7 milestone 1 baseline (top level)
--------------------
File: arty_a7_baseline.v Date: 10/15/26 (SurfCPU)
--------------------
The first bitstream for the board: no CPU, only what proves the board, its clocks, the reset
and the serial link work (brief section 10, milestone 1). Its pins are WIRING.md's base set
(boards/arty_a7/xdc/arty_a7_base_pins.xdc); nothing is driven on the Pmod connectors, so it is
safe with or without adapters plugged in. arty_a7_clocks makes sys_clk and pix_clk and their
resets; arty_a7_baseline_core is the logic (LEDs, switch and button echo, the UART line).

BOARD is a spec/soc.toml [boards] name (arty_a7_35t or arty_a7_100t); the build passes it and
the build identity (tools/fpga-build.sh, docs/BOARD.md). TICK_CYCLES and PIX_TICK_CYCLES stay
0 (one real second) in every bitstream; only simulations shorten them.
*/
// The build sets BOARD and the build identity. Vivado takes them as generics (build.tcl);
// the open flow has no top-level parameter option, so tools/fpga-build.sh passes the same
// values to Yosys as these defines (FLOW=openxc7), which become the defaults. With neither,
// they are the defaults below (simulation, lint).
`ifndef ARTY_BUILD_BOARD
`define ARTY_BUILD_BOARD "arty_a7_100t"
`endif
`ifndef ARTY_BUILD_ID
`define ARTY_BUILD_ID 32'h0
`endif
`ifndef ARTY_BUILD_FLAGS
`define ARTY_BUILD_FLAGS 32'h0
`endif
`ifndef ARTY_BUILD_TIME
`define ARTY_BUILD_TIME 32'h0
`endif
module arty_a7_baseline #(
    parameter         BOARD           = `ARTY_BUILD_BOARD,
    parameter [31:0]  BUILD_ID        = `ARTY_BUILD_ID,
    parameter [31:0]  BUILD_FLAGS     = `ARTY_BUILD_FLAGS,
    parameter [31:0]  BUILD_TIME      = `ARTY_BUILD_TIME,
    parameter integer TICK_CYCLES     = 0,
    parameter integer PIX_TICK_CYCLES = 0
) (
    input  wire        clk100,     // 100 MHz oscillator
    input  wire        ck_rst_n,   // RESET button, low while pressed
    input  wire [3:0]  sw,
    input  wire [3:0]  btn,
    output wire [3:0]  led,
    output wire [11:0] rgb,
    input  wire        uart_rx,    // from the FT2232's TXD
    output wire        uart_tx     // to the FT2232's RXD
);
    wire sys_clk, sys_rst, pix_clk, pix_rst;

    /* verilator lint_off PINCONNECTEMPTY */   // the lock outputs are only for status
    arty_a7_clocks u_clocks (
        .clk100(clk100), .rst_btn(!ck_rst_n),
        .sys_clk(sys_clk), .sys_rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .sys_locked(), .pix_locked()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    arty_a7_baseline_core #(
        .BOARD(BOARD), .BUILD_ID(BUILD_ID), .BUILD_FLAGS(BUILD_FLAGS), .BUILD_TIME(BUILD_TIME),
        .TICK_CYCLES(TICK_CYCLES), .PIX_TICK_CYCLES(PIX_TICK_CYCLES)
    ) u_core (
        .clk(sys_clk), .rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .sw(sw), .btn(btn), .led(led), .rgb(rgb), .uart_rx(uart_rx), .uart_tx(uart_tx)
    );
endmodule
