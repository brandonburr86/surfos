/*
SurfCPU on the Arty A7 (top level)
--------------------
File: surfcpu_arty_a7_top.v Date: 10/15/26 (SurfCPU)
--------------------
The whole SoC on the board: arty_a7_clocks makes sys_clk and pix_clk and their resets, and
surfcpu_soc (MODULES.md section 4) gets the board's pins. Every pin is in WIRING.md and in
boards/arty_a7/xdc/arty_a7_{base,pmod}_pins.xdc (tools/check-wiring.py checks they agree):

  base     clk100, ck_rst_n, sw, btn, led, rgb, uart_rx/uart_tx (the FT2232's UART)
  Pmod JA  Pmod MicroSD: sd_cs_n, sd_mosi, sd_miso, sd_sclk (SPI mode), sd_cd_n (card detect);
           sd_dat1, sd_dat2 are unused in SPI mode and only parked (inputs with pull-ups)
  Pmod JB  Pmod VGA J1: vga_r[3:0], vga_b[3:0]
  Pmod JC  Pmod VGA J2: vga_g[3:0], vga_hs, vga_vs
  Pmod JD  Pmod PS2 (6-pin, top row): ps2_data, ps2_clk (open drain through IOBUFs);
           ps2_data2, ps2_clk2 (the connector's spare pair) are only parked

PS/2 is open drain: an IOBUF per line drives 0 while the SoC's _oe is 1 and otherwise lets go,
so the adapter's and the keyboard's pull-ups make the high level (never more than the PS/2
supply: see WIRING.md, the adapter must be powered from the Pmod's 3.3 V). The FPGA never
drives a PS/2 line high.

DDR3L is not connected yet: HAS_DDR = 0, so the SoC answers every DDR-window access with an
error and its DDR master port is left idle; the ddr track adds the ddr3_* pins, the memory
controller and its clocks here (docs/BOARD.md section 7). Vendor primitives (IOBUF, and the
clock tiles in arty_a7_clocks): this file belongs under boards/ only.
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
`ifndef ARTY_BOOTROM_INIT
`define ARTY_BOOTROM_INIT ""
`endif
module surfcpu_arty_a7_top #(
    parameter         BOARD        = `ARTY_BUILD_BOARD,    // spec/soc.toml [boards] name
    parameter         BOOTROM_INIT = `ARTY_BOOTROM_INIT,   // $readmemh file for the boot ROM
    parameter [31:0]  BUILD_ID     = `ARTY_BUILD_ID,
    parameter [31:0]  BUILD_FLAGS  = `ARTY_BUILD_FLAGS,
    parameter [31:0]  BUILD_TIME   = `ARTY_BUILD_TIME,
    parameter integer HAS_DDR      = 0
) (
    input  wire        clk100,
    input  wire        ck_rst_n,
    input  wire [3:0]  sw,
    input  wire [3:0]  btn,
    output wire [3:0]  led,
    output wire [11:0] rgb,
    input  wire        uart_rx,
    output wire        uart_tx,
    // Pmod JA: microSD
    output wire        sd_cs_n,
    output wire        sd_mosi,
    input  wire        sd_miso,
    output wire        sd_sclk,
    input  wire        sd_cd_n,
    input  wire        sd_dat1,
    input  wire        sd_dat2,
    // Pmod JB + JC: VGA
    output wire [3:0]  vga_r,
    output wire [3:0]  vga_g,
    output wire [3:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs,
    // Pmod JD: PS/2
    inout  wire        ps2_clk,
    inout  wire        ps2_data,
    input  wire        ps2_clk2,
    input  wire        ps2_data2
);
    `include "surfcpu_soc.vh"

    // The waiver covers the string compare: BOARD and the names differ in length on purpose.
    /* verilator lint_off WIDTHEXPAND */
    localparam [31:0] BOARD_ID = (BOARD == "arty_a7_35t")  ? SURF_BOARD_ARTY_A7_35T :
                                 (BOARD == "arty_a7_100t") ? SURF_BOARD_ARTY_A7_100T : SURF_BOARD_SIM;
    /* verilator lint_on WIDTHEXPAND */

    wire sys_clk, sys_rst, pix_clk, pix_rst;

    /* verilator lint_off PINCONNECTEMPTY */   // the lock outputs are only for status
    arty_a7_clocks u_clocks (
        .clk100(clk100), .rst_btn(!ck_rst_n),
        .sys_clk(sys_clk), .sys_rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .sys_locked(), .pix_locked()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // ---------------------------------------------------------------- PS/2: open drain
    wire ps2_clk_i, ps2_data_i, ps2_clk_oe, ps2_data_oe;
    // (I/O standard, drive and slew are set in the XDC, the one place for pin properties)
    IOBUF u_ps2_clk  (.IO(ps2_clk),  .I(1'b0), .T(!ps2_clk_oe),  .O(ps2_clk_i));
    IOBUF u_ps2_data (.IO(ps2_data), .I(1'b0), .T(!ps2_data_oe), .O(ps2_data_i));

    // ---------------------------------------------------------------- the SoC
    // The DDR master port is idle with HAS_DDR = 0 (the SoC errors the window itself).
    /* verilator lint_off PINCONNECTEMPTY */
    surfcpu_soc #(
        .BOOTROM_INIT (BOOTROM_INIT),
        .BOARD_ID     (BOARD_ID),
        .SYS_HZ       (SURF_SYS_HZ),
        .PIX_HZ       (SURF_PIX_HZ),
        .BUILD_ID     (BUILD_ID),
        .BUILD_FLAGS  (BUILD_FLAGS),
        .BUILD_TIME   (BUILD_TIME),
        .HAS_DDR      (HAS_DDR),
        .DDR_SIZE     (SURF_DDR_SIZE)
    ) u_soc (
        .clk(sys_clk), .rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .uart_rx(uart_rx), .uart_tx(uart_tx),
        .led(led), .rgb(rgb), .sw(sw), .btn(btn),
        .sd_sclk(sd_sclk), .sd_mosi(sd_mosi), .sd_cs_n(sd_cs_n), .sd_miso(sd_miso), .sd_cd_n(sd_cd_n),
        .ps2_clk_i(ps2_clk_i), .ps2_data_i(ps2_data_i), .ps2_clk_oe(ps2_clk_oe), .ps2_data_oe(ps2_data_oe),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs),
        .ddr_req_valid(), .ddr_req_ready(1'b0), .ddr_req_addr(), .ddr_req_write(), .ddr_req_be(),
        .ddr_req_wdata(), .ddr_req_fetch(),
        .ddr_rsp_valid(1'b0), .ddr_rsp_rdata(32'h0), .ddr_rsp_err(1'b0),
        .ddr_calib_done(1'b0), .ddr_calib_fail(1'b0), .ddr_timeout(1'b0)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // The parked inputs (pulled up in the XDC) have no logic behind them.
    wire unused_ok = &{1'b0, sd_dat1, sd_dat2, ps2_clk2, ps2_data2};
endmodule
