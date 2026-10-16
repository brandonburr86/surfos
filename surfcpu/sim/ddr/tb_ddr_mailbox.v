/*
SurfCPU DDR mailbox test
--------------------
File: tb_ddr_mailbox.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for surf_ddr_mailbox between two unrelated clocks (periods from plusargs, in
ps, with a random phase): the source sends numbered messages whenever it may (and sometimes
tries when it may not, which must be ignored), the destination takes them after a random
wait (and sometimes "takes" when there is nothing, also ignored), and either clock stops now
and then. Checked: every message arrives exactly once, in order, unchanged, and the
destination's view of it never changes while it waits. Prints PASS or stops with $fatal.

    vvp tb_ddr_mailbox.vvp +seed=N +src_ps=P +dst_ps=P +count=N
*/
`timescale 1ns/1ps
module tb_ddr_mailbox;
    integer seed = 1, seed0 = 1, src_ps = 40000, dst_ps = 12307, count = 5000;
    real    src_half, dst_half;
    reg     src_clk = 0, dst_clk = 0;
    reg     src_run = 1, dst_run = 1;

    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        seed0 = seed;
        if ($value$plusargs("src_ps=%d", src_ps)) ;
        if ($value$plusargs("dst_ps=%d", dst_ps)) ;
        if ($value$plusargs("count=%d", count)) ;
        src_half = src_ps / 2000.0;
        dst_half = dst_ps / 2000.0;
    end
    initial begin #0.001; forever begin #(src_half); if (src_run) src_clk = ~src_clk; end end
    initial begin #(0.003 + ($random(seed) & 255) / 100.0); forever begin #(dst_half); if (dst_run) dst_clk = ~dst_clk; end end

    reg         src_send = 0;
    reg  [31:0] src_data = 0;
    wire        src_ready;
    wire        dst_valid;
    wire [31:0] dst_data;
    reg         dst_take = 0;

    surf_ddr_mailbox #(.WIDTH(32)) dut (
        .src_clk(src_clk), .src_send(src_send), .src_data(src_data), .src_ready(src_ready),
        .dst_clk(dst_clk), .dst_valid(dst_valid), .dst_data(dst_data), .dst_take(dst_take));

    function [31:0] msg;                 // message number n, scrambled so every bit moves
        input [31:0] n;
        msg = n ^ (n << 13) ^ 32'h5A5A_0F0F ^ (n * 32'h9E37_79B9);
    endfunction

    integer sent = 0, got = 0, errors = 0, ignored_sends = 0, ignored_takes = 0;

    // ---- source: decide before the edge from the current src_ready (a register output)
    always @(negedge src_clk) begin
        src_send <= 1'b0;
        if (sent < count && ($random(seed) & 3) != 0) begin
            src_send <= 1'b1;
            src_data <= src_ready ? msg(sent) : 32'hDEAD_BEEF;   // a send while not ready is ignored
        end
    end
    always @(posedge src_clk) begin
        if (src_send && src_ready) sent = sent + 1;
        else if (src_send) ignored_sends = ignored_sends + 1;
    end

    // ---- destination: wait 0..3 cycles once something is there, then take it
    integer wait_left = 0;
    reg     seen = 0;
    reg [31:0] seen_data = 0;
    always @(negedge dst_clk) begin
        dst_take <= 1'b0;
        if (dst_valid) begin
            if (seen && dst_data !== seen_data) begin
                $display("mailbox: data changed while waiting: %h -> %h", seen_data, dst_data);
                errors = errors + 1;
            end
            if (!seen) begin seen = 1; seen_data = dst_data; wait_left = $random(seed) & 3; end
            if (wait_left == 0) dst_take <= 1'b1;
            else wait_left = wait_left - 1;
        end else if (($random(seed) & 15) == 0) begin
            dst_take <= 1'b1;                    // nothing there: must be ignored
        end
    end
    always @(posedge dst_clk) begin
        if (dst_take && dst_valid) begin
            if (dst_data !== msg(got)) begin
                $display("mailbox: message %0d is %h, expected %h", got, dst_data, msg(got));
                errors = errors + 1;
            end
            got = got + 1;
            seen = 0;
        end else if (dst_take) begin
            ignored_takes = ignored_takes + 1;
        end
    end

    // ---- stop either clock now and then
    integer stops = 0;
    initial begin
        #1000;
        while (got < count) begin
            #(($random(seed) & 32'h3FFF) + 500);
            if (($random(seed) & 1) == 0) begin src_run = 0; #(($random(seed) & 1023) + 50); src_run = 1; end
            else                          begin dst_run = 0; #(($random(seed) & 1023) + 50); dst_run = 1; end
            stops = stops + 1;
        end
    end

    initial begin
        #1;
        wait (got == count);
        #2000;
        if (got != sent) begin $display("mailbox: sent %0d, got %0d", sent, got); errors = errors + 1; end
        if (dst_valid) begin $display("mailbox: a message appeared from nowhere"); errors = errors + 1; end
        if (errors) $fatal(1, "tb_ddr_mailbox: %0d errors", errors);
        $display("tb_ddr_mailbox: PASS seed=%0d src=%0dps dst=%0dps messages=%0d ignored_sends=%0d ignored_takes=%0d clock_stops=%0d",
                 seed0, src_ps, dst_ps, got, ignored_sends, ignored_takes, stops);
        $finish;
    end
    initial begin #50_000_000; $fatal(1, "tb_ddr_mailbox: time limit (sent %0d, got %0d)", sent, got); end
endmodule
