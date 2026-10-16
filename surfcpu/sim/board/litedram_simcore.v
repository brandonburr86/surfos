/*
SurfCPU: the board's LiteDRAM core in simulation (LiteDRAM's controller with its DRAM model)
--------------------
File: litedram_simcore.v Date: 10/16/26 (SurfCPU)
--------------------
A litedram_core with exactly the ports of the board's core (ip/litedram/arty.yml), for the RTL
simulation of the DDR top (sim/board/ddr_top_sim.sh): inside is litedram_sim, the core
generated from the same arty.yml with --sim (make litedram-gen): LiteDRAM's real controller,
crossbar and native port with the board's settings, and LiteDRAM's behavioural DRAM model in
place of the Artix-7 PHY, the DDR3L chip and the PLL. Simulation only.

What it cannot be: the PHY (ISERDESE2/OSERDESE2/IDELAYE2), the pins, the PLL, and the
calibration BIOS. In their place:
  * the DDR3L pins stay idle (outputs at their reset levels, DQ and DQS released);
  * the core runs on clk itself (the board's PLL makes its 100 MHz from the same oscillator),
    pll_locked is 1, and rst (the RESET button) does not reach it: the simulation core has no
    reset input;
  * the BIOS's last step is done here: +dram_init_us=N microseconds after start (default 2000,
    instead of the real BIOS's half second or so) it writes init_done = 1 over the core's
    control port, or, with +dram_calib_fail, init_error = 1 and then init_done = 1, as
    liblitedram's sdram_init does when its memory test fails;
  * uart_tx stays idle: there is no BIOS to print.
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
    assign pll_locked = 1'b1;

    // ---------------------------------------------------------------- the BIOS's last step
    integer init_us = 2000;
    reg     calib_fail = 1'b0;
    reg     go = 1'b0;
    initial begin
        if ($value$plusargs("dram_init_us=%d", init_us)) begin end
        if ($test$plusargs("dram_calib_fail")) calib_fail = 1'b1;
        #(init_us * 1000000.0);
        go = 1'b1;
    end

    reg        wb_cyc = 1'b0, wb_adr2 = 1'b0, started = 1'b0;
    reg  [1:0] left = 2'd0;
    wire       wb_ack, wb_err;
    wire [31:0] wb_dat_r;
    always @(posedge user_clk) begin
        if (user_rst) begin
            wb_cyc <= 1'b0; wb_adr2 <= 1'b0; left <= 2'd0; started <= 1'b0;
        end else if (wb_cyc) begin
            if (wb_ack || wb_err) begin
                wb_cyc <= 1'b0; wb_adr2 <= 1'b0; left <= left - 2'd1;
            end
        end else if (go && !started) begin
            started <= 1'b1; wb_cyc <= 1'b1; wb_adr2 <= calib_fail;
            left <= calib_fail ? 2'd2 : 2'd1;
        end else if (left != 2'd0) begin
            wb_cyc <= 1'b1;                     // init_done after init_error
        end
    end

    litedram_sim core (
        .clk(clk), .sim_trace(1'b0),
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
        .wb_ctrl_adr({29'd0, wb_adr2}), .wb_ctrl_dat_w(32'd1), .wb_ctrl_sel(4'hF), .wb_ctrl_cyc(wb_cyc),
        .wb_ctrl_stb(wb_cyc), .wb_ctrl_we(1'b1), .wb_ctrl_cti(3'd0), .wb_ctrl_bte(2'd0),
        .wb_ctrl_ack(wb_ack), .wb_ctrl_dat_r(wb_dat_r), .wb_ctrl_err(wb_err));
endmodule
/* verilator lint_on UNUSEDSIGNAL */
