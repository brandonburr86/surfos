/*
SurfCPU openXC7 flow demo testbench
--------------------
File: flow_demo_tb.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for flow_demo, for the RTL and (with -DNETLIST) for a gate-level netlist of
the same module made by the openXC7 flow, which cannot take parameters. It decodes the UART
output with its own 8N1 receiver and checks every line's text and counter, the exact bit
time (characters of a line start exactly 10 bit times and one clock apart), the line period, the LED
counter and the heartbeat, and that the RESET button restarts the count: it is pressed at a
random moment (+seed=N) for a random time while a line is being sent.

Plusargs: +seed=N (default 1), +lines=N lines to check before the reset press (default 3,
1 with NETLIST), +noreset skips the reset press. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module flow_demo_tb;
`ifdef NETLIST
    localparam integer CLK_HZ = 100000000;
    localparam integer BAUD   = 115200;
    localparam integer PERIOD = 100000000;
`else
    parameter integer CLK_HZ = 1152000;      // 10 clocks per bit, so a run takes milliseconds
    parameter integer BAUD   = 115200;
    parameter integer PERIOD = 4000;
`endif
    localparam integer DIV      = (CLK_HZ + BAUD / 2) / BAUD;
    localparam integer LINE_LEN = 33;
    // The transmitter loads the next character on the clock after the stop bit's last one,
    // so the characters of a line start exactly 10 bit times plus one clock apart.
    localparam integer CHAR_CYCLES = 10 * DIV + 1;

    reg clk = 1'b0;
    always #5 clk = ~clk;

    reg        ck_rst = 1'b0;                // the button is pressed at power-up
    wire [3:0] led;
    wire       tx;

`ifdef NETLIST
    flow_demo dut (.CLK100MHZ(clk), .ck_rst(ck_rst), .led(led), .uart_rxd_out(tx));
`else
    flow_demo #(.CLK_HZ(CLK_HZ), .BAUD(BAUD), .PERIOD(PERIOD))
        dut (.CLK100MHZ(clk), .ck_rst(ck_rst), .led(led), .uart_rxd_out(tx));
`endif

    integer seed = 1, seed0;
    integer lines = 3;
    integer errors = 0;
    integer cyc = 0;                         // clock cycles since the button was released
    always @(posedge clk) cyc <= cyc + 1;

    // ---------------------------------------------------------------- the expected text
    reg [8*27-1:0] text = "SurfCPU openXC7 flow demo #";
    reg [8*16-1:0] hexd = "0123456789ABCDEF";

    function [7:0] expected_char(input integer i, input integer count);
        reg [3:0] nib;
        begin
            if (i < 27) expected_char = text[8 * (26 - i) +: 8];
            else if (i < 31) begin
                nib = count >> (4 * (30 - i));
                expected_char = hexd[8 * (15 - nib) +: 8];
            end else if (i == 31) expected_char = 8'h0D;
            else expected_char = 8'h0A;
        end
    endfunction

    // ---------------------------------------------------------------- the receiver
    // Samples on the falling clock edge, in the middle of each bit. start_cyc is the cycle of
    // the start bit's falling edge.
    task receive_char(output [7:0] c, output integer start_cyc);
        integer b;
        begin
            @(negedge tx);
            start_cyc = cyc;
            repeat (DIV / 2) @(negedge clk);
            if (tx !== 1'b0) begin
                $display("rx: start bit not low at cycle %0d", cyc); errors = errors + 1;
            end
            for (b = 0; b < 8; b = b + 1) begin
                repeat (DIV) @(negedge clk);
                c[b] = tx;
            end
            repeat (DIV) @(negedge clk);
            if (tx !== 1'b1) begin
                $display("rx: stop bit not high at cycle %0d", cyc); errors = errors + 1;
            end
        end
    endtask

    // Receives one line and checks it against counter value `count`; line_start is the cycle
    // at which its first start bit began.
    task receive_line(input integer count, output integer line_start);
        integer i, s, prev;
        reg [7:0] c;
        begin
            for (i = 0; i < LINE_LEN; i = i + 1) begin
                receive_char(c, s);
                if (i == 0) line_start = s;
                else if (s - prev != CHAR_CYCLES) begin
                    $display("rx: char %0d of line %0d started %0d cycles after the previous, not %0d",
                             i, count, s - prev, CHAR_CYCLES);
                    errors = errors + 1;
                end
                prev = s;
                if (c !== expected_char(i, count)) begin
                    $display("rx: line %0d char %0d is %h, expected %h", count, i, c, expected_char(i, count));
                    errors = errors + 1;
                end
            end
            if (led[3:1] !== count[2:0] + 3'd1) begin
                $display("led: counter %b after line %0d, expected %b", led[3:1], count, count[2:0] + 3'd1);
                errors = errors + 1;
            end
        end
    endtask

    // Expects a line to start `want` cycles after the release of the button (a few cycles
    // later: the reset synchronizer and the load take some).
    task check_start(input integer got, input integer want, input integer count);
        begin
            if (got < want || got > want + 6) begin
                $display("line %0d started at cycle %0d, expected %0d..%0d", count, got, want, want + 6);
                errors = errors + 1;
            end
        end
    endtask

    // The heartbeat: on a quarter of a period after each release or tick, off at three quarters.
    integer hb_checks = 0;
    always @(negedge clk) begin
        if (ck_rst && cyc > 0 && (cyc % PERIOD == PERIOD / 4 || cyc % PERIOD == 3 * PERIOD / 4)) begin
            hb_checks = hb_checks + 1;
            if (led[0] !== (cyc % PERIOD == PERIOD / 4)) begin
                $display("heartbeat %b at cycle %0d", led[0], cyc); errors = errors + 1;
            end
        end
    end

    integer n, start, press_at, press_len;
    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        seed0 = seed;
`ifdef NETLIST
        lines = 1;
`endif
        if ($value$plusargs("lines=%d", lines)) $display("lines %0d", lines);
        $display("flow_demo_tb: CLK_HZ %0d BAUD %0d PERIOD %0d DIV %0d", CLK_HZ, BAUD, PERIOD, DIV);

        repeat (7) @(posedge clk);
        if (tx !== 1'b1) begin $display("tx not idle in reset"); errors = errors + 1; end
        @(negedge clk) ck_rst = 1'b1;          // release the button
        cyc = 0;

        for (n = 0; n < lines; n = n + 1) begin
            receive_line(n, start);
            check_start(start, n * PERIOD, n);
        end

`ifndef NETLIST
        if (!$test$plusargs("noreset")) begin
            // press RESET in the middle of line `lines`, then expect #0000 again
            press_at  = lines * PERIOD + 10 + ({$random(seed)} % (LINE_LEN * CHAR_CYCLES - 20));
            press_len = 1 + ({$random(seed)} % 50);
            $display("reset: pressed at cycle %0d for %0d cycles", press_at, press_len);
            wait (cyc == press_at);
            @(negedge clk) ck_rst = 1'b0;
            repeat (2) @(negedge clk);
            if (tx !== 1'b1 || led[3:1] !== 3'd0) begin
                $display("reset: tx %b leds %b while the button is pressed", tx, led); errors = errors + 1;
            end
            repeat (press_len) @(negedge clk);
            ck_rst = 1'b1;
            cyc = 0;
            receive_line(0, start);
            check_start(start, 0, 0);
            receive_line(1, start);
            check_start(start, PERIOD, 1);
        end
        if (hb_checks < 2) begin $display("heartbeat checked only %0d times", hb_checks); errors = errors + 1; end
`endif
        if (errors) $fatal(1, "flow_demo_tb: %0d errors", errors);
        $display("flow_demo_tb: PASS (%0d lines, seed %0d, %0d cycles since the last release)", lines, seed0, cyc);
        $finish;
    end
endmodule
