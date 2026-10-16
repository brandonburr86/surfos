/*
SurfCPU: the board's LiteDRAM core in simulation (LiteDRAM's controller with its DRAM model)
--------------------
File: litedram_simcore.v Date: 10/16/26 (SurfCPU)
--------------------
A litedram_core with exactly the ports of the board's core (ip/litedram/arty.yml), for the RTL
simulation of the DDR top (sim/board/ddr_top_sim.sh): inside is litedram_sim, the core
generated from the same arty.yml with --sim (make litedram-gen): LiteDRAM's real controller,
crossbar, native port and control and status registers (the DFII injectors, init_done,
init_error) with the board's settings, and LiteDRAM's behavioural DRAM model in place of the
Artix-7 PHY, the DDR3L chip and the PLL. Simulation only.

Like the board's core it has no CPU: the control port (wb_ctrl) goes straight to litedram_sim's,
so the firmware in the SoC's boot ROM initializes it through surf_ddr_csr exactly as on the
board (firmware/common/ddrinit.c): the mode registers, ZQ calibration and read leveling go to
LiteDRAM's DFII and the DRAM model (the model has no PHY registers: its delays and bitslips are
absent, so every delay works and leveling finds lane windows of the full 32 taps at bitslip 0),
and init_done (or init_error) is the firmware's to write. What it cannot be: the PHY
(ISERDESE2/OSERDESE2/IDELAYE2), the pins and the PLL. In their place:
  * the DDR3L pins stay idle (outputs at their reset levels, DQ and DQS released);
  * the core runs on its own 80 MHz clock (the board's PLL makes 80 MHz from the oscillator on
    clk; here a free-running 12.5 ns clock, phase unrelated), pll_locked is 1, and rst (the
    RESET button) does not reach it: the simulation core has no reset input.
*/
`timescale 1ps/1ps
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
    output wire          user_rst,
    output wire          wb_ctrl_ack,
    input  wire   [29:0] wb_ctrl_adr,
    input  wire    [1:0] wb_ctrl_bte,
    input  wire    [2:0] wb_ctrl_cti,
    input  wire          wb_ctrl_cyc,
    output wire   [31:0] wb_ctrl_dat_r,
    input  wire   [31:0] wb_ctrl_dat_w,
    output wire          wb_ctrl_err,
    input  wire    [3:0] wb_ctrl_sel,
    input  wire          wb_ctrl_stb,
    input  wire          wb_ctrl_we
);
    assign {ddram_a, ddram_ba} = 17'h0;
    assign {ddram_cas_n, ddram_ras_n, ddram_we_n, ddram_cs_n} = 4'hF;
    assign {ddram_cke, ddram_odt, ddram_reset_n} = 3'b000;
    assign {ddram_clk_p, ddram_clk_n} = 2'b01;
    assign ddram_dm = 2'b00;
    assign ddram_dq = 16'hzzzz;
    assign ddram_dqs_p = 2'bzz;
    assign ddram_dqs_n = 2'bzz;
    assign pll_locked = 1'b1;

    // ---------------------------------------------------------------- the core's own 80 MHz
    reg clk80 = 1'b0;
    always #6250 clk80 = ~clk80;

    litedram_sim core (
        .clk(clk80), .sim_trace(1'b0),
        .init_done(init_done), .init_error(init_error),
        .user_clk(user_clk), .user_rst(user_rst),
        .user_port_native_0_cmd_addr(user_port_native_0_cmd_addr),
        .user_port_native_0_cmd_ready(user_port_native_0_cmd_ready),
        .user_port_native_0_cmd_valid(user_port_native_0_cmd_valid),
        .user_port_native_0_cmd_we(user_port_native_0_cmd_we),
        .user_port_native_0_rdata_data(user_port_native_0_rdata_data),
        .user_port_native_0_rdata_ready(user_port_native_0_rdata_ready),
        .user_port_native_0_rdata_valid(user_port_native_0_rdata_valid),
        .user_port_native_0_wdata_data(user_port_native_0_wdata_data),
        .user_port_native_0_wdata_ready(user_port_native_0_wdata_ready),
        .user_port_native_0_wdata_valid(user_port_native_0_wdata_valid),
        .user_port_native_0_wdata_we(user_port_native_0_wdata_we),
        .wb_ctrl_adr(wb_ctrl_adr), .wb_ctrl_dat_w(wb_ctrl_dat_w), .wb_ctrl_sel(wb_ctrl_sel),
        .wb_ctrl_cyc(wb_ctrl_cyc), .wb_ctrl_stb(wb_ctrl_stb), .wb_ctrl_we(wb_ctrl_we),
        .wb_ctrl_cti(wb_ctrl_cti), .wb_ctrl_bte(wb_ctrl_bte), .wb_ctrl_ack(wb_ctrl_ack),
        .wb_ctrl_dat_r(wb_ctrl_dat_r), .wb_ctrl_err(wb_ctrl_err));
endmodule
/* verilator lint_on UNUSEDSIGNAL */
