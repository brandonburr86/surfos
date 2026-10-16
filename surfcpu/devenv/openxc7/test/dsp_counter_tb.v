/*
SurfCPU Yosys DSP-packing reproducer testbench
--------------------
File: dsp_counter_tb.v Date: 10/15/26 (SurfCPU)
--------------------
Runs dsp_counter (the RTL, or a synthesized netlist of it with Yosys's cells_sim.v) for
2000 clocks after reset and compares every output on every clock with values computed
here from a model counter. Prints PASS, or the first mismatches and FAIL.
*/
`timescale 1ns/1ps
module dsp_counter_tb;
    reg clk = 1'b0, rst = 1'b1;
    always #5 clk = ~clk;
    wire [9:0] cnt;
    wire       hs;
    wire [3:0] level;
    dsp_counter dut (.clk(clk), .rst(rst), .cnt(cnt), .hs(hs), .level(level));

    integer model = 0, n, errors = 0;
    reg [17:0] p;
    initial begin
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;
        for (n = 0; n < 2000; n = n + 1) begin    // on a falling edge: compare, then advance
            p = model * 205;
            if (cnt !== model[9:0] || hs !== !(model >= 656 && model < 752) || level !== p[16:13]) begin
                if (errors < 5)
                    $display("clock %0d: cnt %0d hs %b level %0d, expected %0d %b %0d",
                             n, cnt, hs, level, model, !(model >= 656 && model < 752), p[16:13]);
                errors = errors + 1;
            end
            @(posedge clk) model = (model == 799) ? 0 : model + 1;
            @(negedge clk);
        end
        if (errors) $display("dsp_counter_tb: FAIL (%0d of 2000 clocks wrong)", errors);
        else        $display("dsp_counter_tb: PASS (2000 clocks)");
        $finish;
    end
endmodule
