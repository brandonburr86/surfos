/*
SurfCPU Arty A7 clock and reset test
--------------------
File: tb_clocks.v Date: 10/15/26 (SurfCPU)
--------------------
Simulates boards/arty_a7/rtl/arty_a7_clocks.v with the clock tile models of sim/board/prims
(Icarus, and Verilator --timing). Checks, against spec/soc.toml through surfcpu_soc.vh:
 * both tiles lock and each reset stays asserted until its tile has locked, then releases
   synchronously (3 edges of its own clock);
 * sys_clk is exactly clocks.sys_hz and pix_clk is clocks.pix_hz within 0.5 Hz, measured over
   200000 cycles each;
 * the RESET button asserts both resets at once and they release again after relock;
 * a stopped oscillator drops both locks and resets, and they recover when it restarts.
Prints PASS or stops with $fatal.
*/
`timescale 1ps/1ps
module tb_clocks;
    `include "surfcpu_soc.vh"

    reg clk100 = 1'b0;
    reg osc_on = 1'b1;
    always begin
        #5000;
        if (osc_on) clk100 = ~clk100;
    end

    reg  rst_btn = 1'b0;
    wire sys_clk, sys_rst, pix_clk, pix_rst, sys_locked, pix_locked;

    arty_a7_clocks dut (
        .clk100(clk100), .rst_btn(rst_btn),
        .sys_clk(sys_clk), .sys_rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .sys_locked(sys_locked), .pix_locked(pix_locked)
    );

    // ---------------------------------------------------------------- reset rules
    // A reset may only be low while its tile is locked: losing lock asserts it (within 1 ps,
    // the reset synchronizer's asynchronous preset) and it is never released before lock.
    always @(negedge sys_locked) begin #1; if (sys_rst !== 1'b1) $fatal(1, "sys_rst not asserted on loss of lock"); end
    always @(negedge pix_locked) begin #1; if (pix_rst !== 1'b1) $fatal(1, "pix_rst not asserted on loss of lock"); end
    always @(negedge sys_rst) if (sys_locked !== 1'b1) $fatal(1, "sys_rst released while the PLL is unlocked");
    always @(negedge pix_rst) if (pix_locked !== 1'b1) $fatal(1, "pix_rst released while the MMCM is unlocked");

    // Count each domain's clock edges since its tile locked: the reset must release on the 3rd.
    // (Blocking, so the count is current when the reset's own update arrives.)
    integer sys_edges = 0, pix_edges = 0;
    always @(posedge sys_clk or negedge sys_locked) if (!sys_locked) sys_edges = 0; else sys_edges = sys_edges + 1;
    always @(posedge pix_clk or negedge pix_locked) if (!pix_locked) pix_edges = 0; else pix_edges = pix_edges + 1;
    always @(negedge sys_rst) if (sys_edges != 3 || rst_btn) $fatal(1, "sys_rst released after %0d edges (want 3)", sys_edges);
    always @(negedge pix_rst) if (pix_edges != 3 || rst_btn) $fatal(1, "pix_rst released after %0d edges (want 3)", pix_edges);

    // ---------------------------------------------------------------- frequency measurement
    task measure(input integer which, input integer n, output real hz);
        realtime t0;
        integer  i;
        begin
            if (which == 0) @(posedge sys_clk); else @(posedge pix_clk);
            t0 = $realtime;
            for (i = 0; i < n; i = i + 1)
                if (which == 0) @(posedge sys_clk); else @(posedge pix_clk);
            hz = n * 1.0e12 / ($realtime - t0);
        end
    endtask

    task wait_ready(input [8*16-1:0] what);
        integer i;
        begin
            for (i = 0; i < 1000000 && (sys_rst !== 1'b0 || pix_rst !== 1'b0); i = i + 1) #1000;
            if (sys_rst !== 1'b0 || pix_rst !== 1'b0) $fatal(1, "%0s: resets still asserted after 1 ms", what);
        end
    endtask

    real hz;
    initial begin
        #1;
        if (sys_rst !== 1'b1 || pix_rst !== 1'b1) $fatal(1, "resets not asserted at power-up");
        wait_ready("power-up");
        $display("tb_clocks: locked and out of reset at %0.3f us", $realtime / 1.0e6);

        measure(0, 200000, hz);
        $display("tb_clocks: sys_clk %0.4f Hz (spec %0d)", hz, SURF_SYS_HZ);
        if (hz < SURF_SYS_HZ - 0.5 || hz > SURF_SYS_HZ + 0.5) $fatal(1, "sys_clk is not clocks.sys_hz");
        measure(1, 200000, hz);
        $display("tb_clocks: pix_clk %0.4f Hz (spec %0d)", hz, SURF_PIX_HZ);
        if (hz < SURF_PIX_HZ - 0.5 || hz > SURF_PIX_HZ + 0.5) $fatal(1, "pix_clk is not clocks.pix_hz");

        // RESET button: both resets assert at once (asynchronously), both tiles restart
        #3333;
        rst_btn = 1'b1;
        #1;
        if (sys_rst !== 1'b1 || pix_rst !== 1'b1) $fatal(1, "the RESET button did not assert both resets at once");
        #2000000;
        if (sys_locked !== 1'b0 || pix_locked !== 1'b0) $fatal(1, "tiles still locked while held in reset");
        rst_btn = 1'b0;
        wait_ready("after RESET");
        $display("tb_clocks: RESET button: both domains reset and recovered");

        // oscillator stops: locks and resets drop; it restarts: everything recovers
        osc_on = 1'b0;
        #100000;
        if (sys_locked !== 1'b0 || pix_locked !== 1'b0 || sys_rst !== 1'b1 || pix_rst !== 1'b1)
            $fatal(1, "a stopped oscillator did not drop the locks and assert the resets");
        osc_on = 1'b1;
        wait_ready("after the oscillator restarted");
        measure(1, 1000, hz);
        $display("tb_clocks: stopped oscillator: locks dropped, resets asserted, recovered");
        $display("tb_clocks: PASS");
        $finish;
    end
endmodule
