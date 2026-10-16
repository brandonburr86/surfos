/*
SurfCPU SoC testbench
--------------------
File: tb_soc.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for the whole surfcpu_soc with the scripted core stand-in: the bus script
given with +script= drives both core ports through the real interconnect, memories and
peripherals (the io blocks are the stand-ins or the real ones, whichever the makefile found).
Around the SoC: sys_clk at 25 MHz and pix_clk at 25.18 MHz with their resets; the UART looped
back (tx to rx); the GPIO looped back (switches and buttons follow the green LEDs); the card
and keyboard pins idle; the DDR port on the Verilog DDR model (calibration delay, random
ready and latency, the watchdog emulation: see surfbus_ddr_model.v). Protocol monitors watch
the core's two ports and the DDR port. Parameters: ROM (BOOTROM_INIT), HAS_DDR. The stand-in
prints SCRIPT PASS or SCRIPT FAIL; this bench then prints its own summary, and any monitor or
model complaint contains "ERROR". +max_cycles=N bounds the run (default 6,000,000).
*/
`timescale 1ns/1ps
module tb_soc;
    `include "surfcpu_soc.vh"

    parameter         ROM     = "";
    parameter integer HAS_DDR = 1;

    reg clk = 1'b0, pix_clk = 1'b0;
    always #20 clk = ~clk;
    always #19.859 pix_clk = ~pix_clk;
    reg rst = 1'b1, pix_rst = 1'b1;
    initial begin
        repeat (5) @(posedge clk);
        rst <= 1'b0;
    end
    initial begin
        repeat (5) @(posedge pix_clk);
        pix_rst <= 1'b0;
    end

    wire        uart_tx;
    wire        uart_rx = uart_tx;              // loopback
    wire [3:0]  led;
    wire [11:0] rgb;
    wire [3:0]  sw  = led;                      // GPIO loopback
    wire [3:0]  btn = led;
    wire        sd_sclk, sd_mosi, sd_cs_n;
    wire        ps2_clk_oe, ps2_data_oe;
    wire        ps2_clk_i  = !ps2_clk_oe;       // open-drain lines with pull-ups
    wire        ps2_data_i = !ps2_data_oe;
    wire [3:0]  vga_r, vga_g, vga_b;
    wire        vga_hs, vga_vs;

    wire        ddr_req_valid, ddr_req_ready, ddr_req_write, ddr_req_fetch;
    wire        ddr_rsp_valid, ddr_rsp_err;
    wire [31:0] ddr_req_addr, ddr_req_wdata, ddr_rsp_rdata;
    wire [3:0]  ddr_req_be;
    wire        ddr_calib_done, ddr_calib_fail, ddr_timeout;

    surfcpu_soc #(
        .BOOTROM_INIT(ROM), .BOARD_ID(SURF_BOARD_SIM), .SYS_HZ(SURF_SYS_HZ), .PIX_HZ(SURF_PIX_HZ),
        .BUILD_ID(32'h1234_ABCD), .BUILD_FLAGS(SURF_SYSINFO_BUILD_FLAGS_SIM_MASK),
        .BUILD_TIME(32'h6720_0000), .HAS_DDR(HAS_DDR), .DDR_SIZE(SURF_DDR_SIZE)
    ) dut (
        .clk(clk), .rst(rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .uart_rx(uart_rx), .uart_tx(uart_tx),
        .led(led), .rgb(rgb), .sw(sw), .btn(btn),
        .sd_sclk(sd_sclk), .sd_mosi(sd_mosi), .sd_cs_n(sd_cs_n), .sd_miso(1'b1), .sd_cd_n(1'b1),
        .ps2_clk_i(ps2_clk_i), .ps2_data_i(ps2_data_i), .ps2_clk_oe(ps2_clk_oe),
        .ps2_data_oe(ps2_data_oe),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs),
        .ddr_req_valid(ddr_req_valid), .ddr_req_ready(ddr_req_ready), .ddr_req_addr(ddr_req_addr),
        .ddr_req_write(ddr_req_write), .ddr_req_be(ddr_req_be), .ddr_req_wdata(ddr_req_wdata),
        .ddr_req_fetch(ddr_req_fetch), .ddr_rsp_valid(ddr_rsp_valid),
        .ddr_rsp_rdata(ddr_rsp_rdata), .ddr_rsp_err(ddr_rsp_err),
        .ddr_calib_done(ddr_calib_done), .ddr_calib_fail(ddr_calib_fail),
        .ddr_timeout(ddr_timeout));

    surfbus_ddr_model ddr (
        .clk(clk), .rst(rst),
        .s_req_valid(ddr_req_valid), .s_req_ready(ddr_req_ready), .s_req_addr(ddr_req_addr),
        .s_req_write(ddr_req_write), .s_req_be(ddr_req_be), .s_req_wdata(ddr_req_wdata),
        .s_req_fetch(ddr_req_fetch), .s_rsp_valid(ddr_rsp_valid), .s_rsp_rdata(ddr_rsp_rdata),
        .s_rsp_err(ddr_rsp_err),
        .calib_done(ddr_calib_done), .calib_fail(ddr_calib_fail), .timeout(ddr_timeout));

    // ---------------------------------------------------------------- monitors
    surfbus_monitor #(.NAME("ibus"), .MAX_LAT(2000)) imon (
        .clk(clk), .rst(rst), .req_valid(dut.ibus_req_valid), .req_ready(dut.ibus_req_ready),
        .req_addr(dut.ibus_req_addr), .req_write(dut.ibus_req_write), .req_be(dut.ibus_req_be),
        .req_wdata(dut.ibus_req_wdata), .req_fetch(dut.ibus_req_fetch),
        .rsp_valid(dut.ibus_rsp_valid), .rsp_rdata(dut.ibus_rsp_rdata),
        .rsp_err(dut.ibus_rsp_err));
    surfbus_monitor #(.NAME("dbus"), .MAX_LAT(2000)) dmon (
        .clk(clk), .rst(rst), .req_valid(dut.dbus_req_valid), .req_ready(dut.dbus_req_ready),
        .req_addr(dut.dbus_req_addr), .req_write(dut.dbus_req_write), .req_be(dut.dbus_req_be),
        .req_wdata(dut.dbus_req_wdata), .req_fetch(dut.dbus_req_fetch),
        .rsp_valid(dut.dbus_rsp_valid), .rsp_rdata(dut.dbus_rsp_rdata),
        .rsp_err(dut.dbus_rsp_err));
    surfbus_monitor #(.NAME("ddr"), .MAX_LAT(1000)) ddrmon (
        .clk(clk), .rst(rst), .req_valid(ddr_req_valid), .req_ready(ddr_req_ready),
        .req_addr(ddr_req_addr), .req_write(ddr_req_write), .req_be(ddr_req_be),
        .req_wdata(ddr_req_wdata), .req_fetch(ddr_req_fetch), .rsp_valid(ddr_rsp_valid),
        .rsp_rdata(ddr_rsp_rdata), .rsp_err(ddr_rsp_err));

    // the instruction port never writes; nothing reaches DDR without it
    always @(posedge clk) begin
        if (!rst && dut.ibus_req_valid && (dut.ibus_req_write !== 1'b0 ||
            dut.ibus_req_be !== 4'hF || dut.ibus_req_fetch !== 1'b1)) begin
            $display("tb_soc: ERROR the instruction port offered a non-fetch request");
        end
        if (!rst && HAS_DDR == 0 && ddr_req_valid !== 1'b0)
            $display("tb_soc: ERROR the DDR port moved with HAS_DDR = 0");
    end

    // ---------------------------------------------------------------- run control
    integer max_cycles = 6000000;
    integer cycles = 0;
    initial begin
        if ($value$plusargs("max_cycles=%d", max_cycles)) begin end
    end
    always @(posedge clk) begin
        cycles = cycles + 1;
        if (cycles > max_cycles) $fatal(1, "tb_soc: ERROR no verdict after %0d cycles", max_cycles);
        if (dut.u_core.reported) begin
            $display("tb_soc: HAS_DDR=%0d ibus %0d requests (%0d errors), dbus %0d (%0d errors), DDR %0d reads %0d writes %0d fetches %0d stalls; monitor errors %0d %0d %0d; DDR model errors %0d",
                     HAS_DDR, imon.accepts, imon.err_responses, dmon.accepts, dmon.err_responses,
                     ddr.n_reads, ddr.n_writes, ddr.n_fetches, ddr.n_stalls, imon.errors,
                     dmon.errors, ddrmon.errors, ddr.errors);
            if (imon.errors + dmon.errors + ddrmon.errors + ddr.errors != 0)
                $display("tb_soc: ERROR protocol or model errors");
            $finish;
        end
    end
endmodule
