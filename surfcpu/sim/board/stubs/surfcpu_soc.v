/*
SurfCPU SoC stand-in for the board top (simulation and lint only)
--------------------
File: surfcpu_soc.v Date: 10/15/26 (SurfCPU)
--------------------
The soc track writes rtl/soc/surfcpu_soc.v. Until it is merged, this stand-in with exactly the
parameters and ports of MODULES.md section 4 lets surfcpu_arty_a7_top lint, elaborate and
simulate; mk/area-board.mk uses it only when rtl/soc/surfcpu_soc.v does not exist, and
tools/fpga-build.sh refuses to build the full top without the real one, so it can never end
up in a bitstream.

It ties every output to its idle level (UART and chip select high, syncs high, open-drain
lines released, no DDR request) and does nothing else. The board test drives these idle
values through its own registers (drive_*) to check the top's wiring pin by pin.
*/
// The parameters only exist for the port-for-port match with MODULES.md; the stand-in uses none.
/* verilator lint_off UNUSEDPARAM */
module surfcpu_soc #(
    parameter         BOOTROM_INIT = "",
    parameter [31:0]  BOARD_ID     = 32'h0000_00F0,
    parameter integer SYS_HZ       = 25000000,
    parameter integer PIX_HZ       = 25177305,
    parameter [31:0]  BUILD_ID     = 32'h0,
    parameter [31:0]  BUILD_FLAGS  = 32'h0,
    parameter [31:0]  BUILD_TIME   = 32'h0,
    parameter integer HAS_DDR      = 1,
    parameter [31:0]  DDR_SIZE     = 32'h1000_0000
) (
    input  wire        clk, rst, pix_clk, pix_rst,
    input  wire        uart_rx,  output wire uart_tx,
    output wire [3:0]  led,      output wire [11:0] rgb,  input wire [3:0] sw, input wire [3:0] btn,
    output wire        sd_sclk, sd_mosi, sd_cs_n,  input wire sd_miso, sd_cd_n,
    input  wire        ps2_clk_i, ps2_data_i,      output wire ps2_clk_oe, ps2_data_oe,
    output wire [3:0]  vga_r, vga_g, vga_b,        output wire vga_hs, vga_vs,
    // DDR: a SurfBus master port toward surf_ddr_bridge (or the simulation's DDR model)
    output wire        ddr_req_valid, input wire ddr_req_ready, output wire [31:0] ddr_req_addr,
    output wire        ddr_req_write, output wire [3:0] ddr_req_be, output wire [31:0] ddr_req_wdata,
    output wire        ddr_req_fetch,
    input  wire        ddr_rsp_valid, input wire [31:0] ddr_rsp_rdata, input wire ddr_rsp_err,
    input  wire        ddr_calib_done, ddr_calib_fail, ddr_timeout
`ifdef SURFCPU_RVFI
    , `SURFCPU_RVFI_OUTPUTS
`endif
);
    // Outputs: idle levels, held in registers a testbench may overwrite.
    reg        drive_uart_tx = 1'b1;
    reg [3:0]  drive_led     = 4'h0;
    reg [11:0] drive_rgb     = 12'h000;
    reg [2:0]  drive_sd      = 3'b100;           // {cs_n, mosi, sclk}
    reg [1:0]  drive_ps2_oe  = 2'b00;            // {data, clk}: released
    reg [13:0] drive_vga     = {12'h000, 2'b11}; // {r, g, b, hs, vs}: black, syncs inactive (high)

    assign uart_tx        = drive_uart_tx;
    assign led            = drive_led;
    assign rgb            = drive_rgb;
    assign {sd_cs_n, sd_mosi, sd_sclk} = drive_sd;
    assign {ps2_data_oe, ps2_clk_oe}   = drive_ps2_oe;
    assign {vga_r, vga_g, vga_b, vga_hs, vga_vs} = drive_vga;

    assign ddr_req_valid = 1'b0;
    assign ddr_req_addr  = 32'h0;
    assign ddr_req_write = 1'b0;
    assign ddr_req_be    = 4'h0;
    assign ddr_req_wdata = 32'h0;
    assign ddr_req_fetch = 1'b0;

    // Inputs are observed by the testbench through these ports; nothing here uses them.
    wire unused_ok = &{1'b0, clk, rst, pix_clk, pix_rst, uart_rx, sw, btn, sd_miso, sd_cd_n,
                       ps2_clk_i, ps2_data_i, ddr_req_ready, ddr_rsp_valid, ddr_rsp_rdata,
                       ddr_rsp_err, ddr_calib_done, ddr_calib_fail, ddr_timeout};
`ifdef SURFCPU_RVFI
    // (RVFI outputs are not driven by the stand-in)
`endif
endmodule
/* verilator lint_on UNUSEDPARAM */
