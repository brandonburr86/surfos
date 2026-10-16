/*
SurfCPU Arty A7 baseline: top-level test
--------------------
File: tb_baseline.v Date: 10/15/26 (SurfCPU)
--------------------
Simulates the whole milestone 1 top, arty_a7_baseline, from its pins: the 100 MHz oscillator,
the clock tile models of sim/board/prims, the RESET button, switches, buttons and the UART.
The "second" is shortened to TICK sys_clk cycles (13 ms) so a run is short; the UART runs at
its real 115200 baud from the real sys_clk. baseline_rt.cpp checks the real one-second
timing of the same logic. Checks:
  * after power-up the clocks lock, and the first line arrives one tick after reset;
  * every line is exactly the expected text (BOARD arty_a7_35t, its BOARD_ID, the build
    identity) with the uptime counting, lines exactly one tick apart;
  * the measured pixel clock count is clocks.pix_hz x TICK / clocks.sys_hz, +-1;
  * switches and buttons reach the LEDs (the RGB LEDs at a 1/8 duty cycle) and the line;
    a byte sent to the RX pin is shown; the LD0 green heartbeat is lit only in the first
    half of each tick;
  * pressing RESET restarts everything: the next line shows uptime 1 again (skipped with
    +short, which mk/area-board.mk uses for the slower Icarus run).
Runs in Icarus and in Verilator --timing. Prints PASS or stops with $fatal.
*/
`timescale 1ps/1ps
module tb_baseline;
    `include "surfcpu_soc.vh"

    localparam integer TICK     = 325000;                          // sys_clk cycles (13 ms)
    localparam real    PIX_TICK_R = 1.0 * SURF_PIX_HZ * TICK / SURF_SYS_HZ;  // pix_clk cycles in a tick
    localparam integer PIX_TICK = PIX_TICK_R;                          // (rounded)
    localparam integer DIV      = (SURF_SYS_HZ + 57600) / 115200;
    localparam real    BIT_PS   = DIV * 1.0e12 / SURF_SYS_HZ;
    localparam real    TICK_PS  = TICK * 1.0e12 / SURF_SYS_HZ;
    localparam [31:0]  BUILD_ID = 32'hcafe0123;
    localparam [31:0]  BUILD_TIME = 32'd1760000000;

    reg clk100 = 1'b0;
    always #5000 clk100 = ~clk100;

    reg        ck_rst_n = 1'b1;
    reg  [3:0] sw       = 4'h0;
    reg  [3:0] btn      = 4'h0;
    reg        uart_rx  = 1'b1;
    wire [3:0] led;
    wire [11:0] rgb;
    wire       uart_tx;

    arty_a7_baseline #(
        .BOARD("arty_a7_35t"), .BUILD_ID(BUILD_ID), .BUILD_FLAGS(32'h2), .BUILD_TIME(BUILD_TIME),
        .TICK_CYCLES(TICK), .PIX_TICK_CYCLES(PIX_TICK)
    ) dut (
        .clk100(clk100), .ck_rst_n(ck_rst_n), .sw(sw), .btn(btn), .led(led), .rgb(rgb),
        .uart_rx(uart_rx), .uart_tx(uart_tx)
    );

    // ---------------------------------------------------------------- expected text
    function [8*8-1:0] hex8(input [31:0] v);
        integer k;
        for (k = 0; k < 8; k = k + 1)
            hex8[8*k +: 8] = (v[4*k +: 4] < 10) ? 8'h30 + v[4*k +: 4] : 8'h57 + v[4*k +: 4];
    endfunction
    function [8*10-1:0] dec10(input [31:0] v);
        integer k;
        reg [31:0] x;
        begin
            x = v;
            for (k = 0; k < 10; k = k + 1) begin
                dec10[8*k +: 8] = 8'h30 + x % 10;
                x = x / 10;
            end
        end
    endfunction
    function [8*6-1:0] dec6(input [31:0] v);
        reg [8*10-1:0] d;
        begin
            d = dec10(v);
            dec6 = d[8*6-1:0];
        end
    endfunction
    function [8*4-1:0] bits4(input [3:0] v);
        integer k;
        for (k = 0; k < 4; k = k + 1) bits4[8*k +: 8] = v[k] ? "1" : "0";
    endfunction

    // The line, right-aligned in 200 characters; '?' marks the measured pixel clock digits.
    function [8*200-1:0] expect_line(input integer up, input [3:0] s, input [3:0] b, input [15:0] rx);
        expect_line = {"SurfCPU baseline arty_a7_35t 0x", hex8(SURF_BOARD_ARTY_A7_35T), " build ",
                       hex8(BUILD_ID), " clean sim ", dec10(BUILD_TIME), " | up ", dec6(up),
                       " s | pix ????????", " Hz | sw ", bits4(s), " btn ", bits4(b), " rx ",
                       rx, "\015\n"};
    endfunction

    // ---------------------------------------------------------------- UART monitor
    reg [8*200-1:0] line    = 0;
    reg [8*200-1:0] got     = 0;
    integer         nchar   = 0;
    integer         nlines  = 0;
    realtime        line_t0 = 0.0;
    realtime        got_t0  = 0.0;
    event           line_done;

    always begin : uart_mon
        integer b;
        reg [7:0] c;
        @(negedge uart_tx);
        if (nchar == 0) line_t0 = $realtime;
        #(BIT_PS / 2.0);
        if (uart_tx !== 1'b0) $fatal(1, "UART: start bit glitch");
        for (b = 0; b < 8; b = b + 1) begin
            #(BIT_PS);
            c[b] = uart_tx;
        end
        #(BIT_PS);
        if (uart_tx !== 1'b1) $fatal(1, "UART: stop bit is 0");
        line  = {line[8*199-1:0], c};
        nchar = nchar + 1;
        if (c == 8'h0A) begin
            got    = line;
            got_t0 = line_t0;
            line   = 0;
            nchar  = 0;
            nlines = nlines + 1;
            -> line_done;
        end
    end

    // Compare a received line with the expected one; returns the measured pixel count.
    task check_line(input integer up, input [3:0] s, input [3:0] b, input [15:0] rx, output integer pix);
        reg [8*200-1:0] e;
        reg [7:0]       ec, gc;
        integer         k;
        begin
            e   = expect_line(up, s, b, rx);
            pix = 0;
            for (k = 199; k >= 0; k = k - 1) begin
                ec = e[8*k +: 8];
                gc = got[8*k +: 8];
                if (ec == "?") begin
                    if (gc < "0" || gc > "9") $fatal(1, "line %0d: pix digit %h", nlines, gc);
                    pix = pix * 10 + (gc - "0");
                end else if (ec != gc) begin
                    $display("tb_baseline: got:      %0s", got);
                    $display("tb_baseline: expected: %0s", e);
                    $fatal(1, "line %0d differs at character %0d", nlines, 199 - k);
                end
            end
        end
    endtask

    // ---------------------------------------------------------------- LED observation (sys_clk)
    // Each tick: count LD0-green pulses in its first and second halves; and the BTN2 blue duty.
    wire sys_clk = dut.sys_clk;
    wire sys_rst = dut.sys_rst;
    integer cyc = 0, g_first = 0, g_second = 0, blue_on = 0, blue_n = 0;
    always @(posedge sys_clk) begin
        cyc = sys_rst ? 0 : cyc + 1;            // cycles since the reset released
        // what is sampled here was registered from sec_cnt = cyc - 2 (the core's counter)
        if (rgb[1]) begin
            if (((cyc + TICK - 2) % TICK) < TICK / 2) g_first = g_first + 1;
            else                         g_second = g_second + 1;
        end
        if (blue_n > 0) begin
            blue_n = blue_n - 1;
            if (rgb[8]) blue_on = blue_on + 1;
        end
    end

    // ---------------------------------------------------------------- the run
    integer pix, lo, hi, n0;
    realtime t_prev;
    initial begin
        lo = PIX_TICK - 1;                      // the count may differ by one either way
        hi = PIX_TICK + 1;
        sw = 4'b0101;

        @(line_done);
        check_line(1, 4'b0101, 4'b0000, "--", pix);
        $display("tb_baseline: line 1 at %0.3f ms: %0s", got_t0 / 1.0e9, got[8*200-1:16]);
        if (got_t0 < TICK_PS || got_t0 > TICK_PS + 1.0e9) $fatal(1, "first line not one tick after reset");
        t_prev = got_t0;
        if (led !== 4'b0101) $fatal(1, "LD4..LD7 do not show the switches");

        // press BTN2, send 'Z' (0x5a) to the RX pin
        btn = 4'b0100;
        send_byte(8'h5a);
        blue_on = 0;
        blue_n  = 8192;                          // four whole PWM periods
        @(line_done);
        check_line(2, 4'b0101, 4'b0100, "5a", pix);
        $display("tb_baseline: line 2 at %0.3f ms: %0s", got_t0 / 1.0e9, got[8*200-1:16]);
        if (got_t0 - t_prev < TICK_PS - 1.0 || got_t0 - t_prev > TICK_PS + 1.0)
            $fatal(1, "lines not exactly one tick apart (%0.3f ps)", got_t0 - t_prev);
        if (pix < lo || pix > hi) $fatal(1, "pix count %0d, want %0d..%0d", pix, lo, hi);
        if (blue_n != 0 || blue_on != 1024) $fatal(1, "BTN2's blue LED on %0d of 8192 cycles, want 1024 (1/8)", blue_on);
        if (g_first == 0 || g_second != 0) $fatal(1, "LD0 green lit outside the first half of the tick (%0d/%0d)", g_first, g_second);
        $display("tb_baseline: pix %0d in a %0d-cycle tick; RGB duty %0d/8192; heartbeat halves %0d/%0d",
                 pix, TICK, blue_on, g_first, g_second);

        if ($test$plusargs("short")) begin       // (Icarus runs stop here; Verilator does the rest)
            $display("tb_baseline: PASS (%0d lines, short run)", nlines);
            $finish;
        end

        // RESET (between two lines): everything restarts, the uptime begins at 1 again
        btn = 4'b0000;
        #100000000;
        ck_rst_n = 1'b0;
        #20000000;
        ck_rst_n = 1'b1;
        n0 = nlines;
        @(line_done);
        if (nlines != n0 + 1) $fatal(1, "line count");
        check_line(1, 4'b0101, 4'b0000, "--", pix);
        $display("tb_baseline: after RESET: %0s", got[8*200-1:16]);
        $display("tb_baseline: PASS (%0d lines)", nlines);
        $finish;
    end

    task send_byte(input [7:0] v);
        integer k;
        begin
            uart_rx = 1'b0;
            #(BIT_PS);
            for (k = 0; k < 8; k = k + 1) begin
                uart_rx = v[k];
                #(BIT_PS);
            end
            uart_rx = 1'b1;
            #(BIT_PS);
        end
    endtask

    initial begin
        #(TICK_PS * 8.0);
        $fatal(1, "timeout");
    end
endmodule
