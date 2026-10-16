/*
SurfCPU Arty A7 top: board wiring test
--------------------
File: tb_top.v Date: 10/15/26 (SurfCPU)
--------------------
Simulates surfcpu_arty_a7_top from its pins with the clock tile and IOBUF models of
sim/board/prims, and checks the board-level wiring around surfcpu_soc:
  * the SoC gets sys_clk at clocks.sys_hz and pix_clk at clocks.pix_hz, each with its own
    reset released after lock, and BOARD_ID / SYS_HZ / PIX_HZ / HAS_DDR as the spec says;
  * every input pin reaches the SoC port of the same signal (random values, bit by bit);
  * the PS/2 lines are open drain: released lines read high through the pull-ups, the SoC's
    _oe pulls a line low, a device pulling a line low is seen on _i, and the FPGA never
    drives a PS/2 line high;
With the stand-in SoC (SURFCPU_SOC_STUB, until rtl/soc/surfcpu_soc.v exists) it also drives
every SoC output through the stand-in's drive_* registers and checks the pin it lands on.
Prints PASS or stops with $fatal.
*/
`timescale 1ps/1ps
module tb_top;
    `include "surfcpu_soc.vh"

    reg clk100 = 1'b0;
    always #5000 clk100 = ~clk100;

    reg        ck_rst_n = 1'b1;
    reg  [3:0] sw = 4'h0, btn = 4'h0;
    reg        uart_rx = 1'b1, sd_miso = 1'b1, sd_cd_n = 1'b1, sd_dat1 = 1'b1, sd_dat2 = 1'b1;
    reg        ps2_clk2 = 1'b1, ps2_data2 = 1'b1;
    wire [3:0] led;
    wire [11:0] rgb;
    wire       uart_tx, sd_cs_n, sd_mosi, sd_sclk;
    wire [3:0] vga_r, vga_g, vga_b;
    wire       vga_hs, vga_vs;
    wire       ps2_clk, ps2_data;

    // the PS/2 adapter's pull-ups, and a device that can pull either line low
    reg dev_clk_low = 1'b0, dev_data_low = 1'b0;
    pullup (ps2_clk);
    pullup (ps2_data);
    assign ps2_clk  = dev_clk_low  ? 1'b0 : 1'bz;
    assign ps2_data = dev_data_low ? 1'b0 : 1'bz;

    surfcpu_arty_a7_top #(.BOARD("arty_a7_100t"), .BUILD_ID(32'h89abcdef)) dut (
        .clk100(clk100), .ck_rst_n(ck_rst_n), .sw(sw), .btn(btn), .led(led), .rgb(rgb),
        .uart_rx(uart_rx), .uart_tx(uart_tx),
        .sd_cs_n(sd_cs_n), .sd_mosi(sd_mosi), .sd_miso(sd_miso), .sd_sclk(sd_sclk),
        .sd_cd_n(sd_cd_n), .sd_dat1(sd_dat1), .sd_dat2(sd_dat2),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs),
        .ps2_clk(ps2_clk), .ps2_data(ps2_data), .ps2_clk2(ps2_clk2), .ps2_data2(ps2_data2)
    );

    integer seed = 1, i;
    reg [31:0] r;
    realtime t0;

    task settle;
        #50000;                                  // 50 ns: the pins have no clocked path here
    endtask

    initial begin
        if ($value$plusargs("seed=%d", seed)) begin end

        // ---------------------------------------------------------- clocks, resets, parameters
        wait (dut.u_soc.rst === 1'b0 && dut.u_soc.pix_rst === 1'b0);
        @(posedge dut.u_soc.clk);
        t0 = $realtime;
        repeat (1000) @(posedge dut.u_soc.clk);
        if ($realtime - t0 != 1000 * 1.0e12 / SURF_SYS_HZ) $fatal(1, "SoC clk is not clocks.sys_hz");
        @(posedge dut.u_soc.pix_clk);
        t0 = $realtime;
        repeat (100000) @(posedge dut.u_soc.pix_clk);
        if ((100000 * 1.0e12) / ($realtime - t0) < SURF_PIX_HZ - 0.5 ||
            (100000 * 1.0e12) / ($realtime - t0) > SURF_PIX_HZ + 0.5) $fatal(1, "SoC pix_clk is not clocks.pix_hz");
        if (dut.u_soc.BOARD_ID !== SURF_BOARD_ARTY_A7_100T || dut.u_soc.SYS_HZ != SURF_SYS_HZ ||
            dut.u_soc.PIX_HZ != SURF_PIX_HZ || dut.u_soc.HAS_DDR != 0 || dut.u_soc.BUILD_ID !== 32'h89abcdef ||
            dut.u_soc.DDR_SIZE !== SURF_DDR_SIZE)
            $fatal(1, "SoC parameters are not the spec's");
        $display("tb_top: SoC clocks %0d / %0d Hz, resets released, BOARD_ID %h, HAS_DDR 0",
                 SURF_SYS_HZ, SURF_PIX_HZ, dut.u_soc.BOARD_ID);

        // ---------------------------------------------------------- inputs reach the SoC
        for (i = 0; i < 200; i = i + 1) begin
            r = $random(seed);
            sw = r[3:0]; btn = r[7:4]; uart_rx = r[8]; sd_miso = r[9]; sd_cd_n = r[10];
            settle;
            if (dut.u_soc.sw !== sw || dut.u_soc.btn !== btn || dut.u_soc.uart_rx !== uart_rx ||
                dut.u_soc.sd_miso !== sd_miso || dut.u_soc.sd_cd_n !== sd_cd_n)
                $fatal(1, "an input pin does not reach its SoC port (pattern %h)", r);
        end
        $display("tb_top: sw, btn, uart_rx, sd_miso, sd_cd_n reach the SoC (200 random patterns)");

        // ---------------------------------------------------------- PS/2 open drain
        settle;
        if (ps2_clk !== 1'b1 || ps2_data !== 1'b1 || dut.u_soc.ps2_clk_i !== 1'b1 || dut.u_soc.ps2_data_i !== 1'b1)
            $fatal(1, "released PS/2 lines do not read high");
        dev_clk_low = 1'b1;
        settle;
        if (ps2_clk !== 1'b0 || dut.u_soc.ps2_clk_i !== 1'b0 || dut.u_soc.ps2_data_i !== 1'b1)
            $fatal(1, "a device pulling PS/2 clock low is not seen");
        dev_clk_low = 1'b0;
        dev_data_low = 1'b1;
        settle;
        if (ps2_data !== 1'b0 || dut.u_soc.ps2_data_i !== 1'b0 || dut.u_soc.ps2_clk_i !== 1'b1)
            $fatal(1, "a device pulling PS/2 data low is not seen");
        dev_data_low = 1'b0;
        if (dut.u_ps2_clk.I !== 1'b0 || dut.u_ps2_data.I !== 1'b0)
            $fatal(1, "the PS/2 IOBUFs can drive a 1");
`ifdef SURFCPU_SOC_STUB
        dut.u_soc.drive_ps2_oe = 2'b01;          // SoC pulls clock low
        settle;
        if (ps2_clk !== 1'b0 || ps2_data !== 1'b1 || dut.u_soc.ps2_clk_i !== 1'b0) $fatal(1, "ps2_clk_oe does not pull clock low");
        dut.u_soc.drive_ps2_oe = 2'b10;          // SoC pulls data low
        settle;
        if (ps2_clk !== 1'b1 || ps2_data !== 1'b0 || dut.u_soc.ps2_data_i !== 1'b0) $fatal(1, "ps2_data_oe does not pull data low");
        dut.u_soc.drive_ps2_oe = 2'b00;
        settle;
        if (ps2_clk !== 1'b1 || ps2_data !== 1'b1) $fatal(1, "PS/2 lines not released");
`endif
        $display("tb_top: PS/2 lines are open drain (released high, pulled low by either side)");

`ifdef SURFCPU_SOC_STUB
        // ---------------------------------------------------------- SoC outputs reach their pins
        for (i = 0; i < 200; i = i + 1) begin
            r = $random(seed);
            dut.u_soc.drive_uart_tx = r[0];
            dut.u_soc.drive_led     = r[4:1];
            dut.u_soc.drive_rgb     = r[16:5];
            dut.u_soc.drive_sd      = r[19:17];
            dut.u_soc.drive_vga     = {r[31:20], r[1:0]};
            settle;
            if (uart_tx !== r[0] || led !== r[4:1] || rgb !== r[16:5] ||
                {sd_cs_n, sd_mosi, sd_sclk} !== r[19:17] ||
                {vga_r, vga_g, vga_b, vga_hs, vga_vs} !== {r[31:20], r[1:0]})
                $fatal(1, "an SoC output does not reach its pin (pattern %h)", r);
        end
        $display("tb_top: uart_tx, led, rgb, sd_*, vga_* reach their pins (200 random patterns)");
`endif

        // ---------------------------------------------------------- RESET reaches both domains
        ck_rst_n = 1'b0;
        #1;
        if (dut.u_soc.rst !== 1'b1 || dut.u_soc.pix_rst !== 1'b1) $fatal(1, "RESET does not reset the SoC");
        #1000000;
        ck_rst_n = 1'b1;
        wait (dut.u_soc.rst === 1'b0 && dut.u_soc.pix_rst === 1'b0);
        $display("tb_top: RESET resets both SoC domains");
        $display("tb_top: PASS");
        $finish;
    end

    initial begin
        #100000000000;
        $fatal(1, "timeout");
    end
endmodule
