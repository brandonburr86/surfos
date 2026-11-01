/*
SurfCPU rtl/lib tests
--------------------
File: tb_lib.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for the shared building blocks: surf_fifo against a model with random
pushes and pops, surf_cdc_word across two unrelated clocks (no torn values, convergence),
surf_pulse_sync (every pulse arrives exactly once), surf_reset_sync and surf_sync timing.
Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_lib;
    // ---------------------------------------------------------------- clocks
    reg clk_a = 0, clk_b = 0;
    always #5     clk_a = ~clk_a;     // 100 MHz
    always #19.86 clk_b = ~clk_b;     // about 25.18 MHz, unrelated
    reg rst_a = 1, rst_b = 1;

    integer seed = 1;
    integer errors = 0;

    // ---------------------------------------------------------------- surf_fifo
    reg        f_wr = 0, f_rd = 0;
    reg  [7:0] f_wdata = 0;
    wire [7:0] f_rdata;
    wire       f_empty, f_full;
    wire [4:0] f_count;
    surf_fifo #(.WIDTH(8), .DEPTH_LOG2(4)) fifo (
        .clk(clk_a), .rst(rst_a), .wr_en(f_wr), .wr_data(f_wdata), .rd_en(f_rd),
        .rd_data(f_rdata), .empty(f_empty), .full(f_full), .count(f_count));

    reg [7:0] model [0:15];
    integer m_head = 0, m_tail = 0, m_count = 0;
    integer i;

    task fifo_test;
        integer n;
        reg do_rd, do_wr;
        begin
            for (n = 0; n < 20000; n = n + 1) begin
                @(negedge clk_a);
                if (f_count !== m_count[4:0]) begin
                    $display("fifo: count %0d, model %0d", f_count, m_count); errors = errors + 1;
                end
                if (f_empty !== (m_count == 0) || f_full !== (m_count == 16)) begin
                    $display("fifo: flags empty=%b full=%b, model count %0d", f_empty, f_full, m_count);
                    errors = errors + 1;
                end
                if (!f_empty && f_rdata !== model[m_head]) begin
                    $display("fifo: head %h, model %h", f_rdata, model[m_head]); errors = errors + 1;
                end
                f_wr    = ($random(seed) & 3) != 0;          // pushes slightly more often than pops
                f_rd    = ($random(seed) & 1) != 0;
                f_wdata = $random(seed);
                // the FIFO decides on the flags before the edge: a pop needs an entry, a push room
                do_rd = f_rd && m_count > 0;
                do_wr = f_wr && m_count < 16;
                @(posedge clk_a); #1;
                if (do_rd) begin m_head = (m_head + 1) % 16; m_count = m_count - 1; end
                if (do_wr) begin model[m_tail] = f_wdata; m_tail = (m_tail + 1) % 16; m_count = m_count + 1; end
            end
            f_wr = 0; f_rd = 0;
        end
    endtask

    // ---------------------------------------------------------------- surf_cdc_word
    reg  [31:0] c_src = 32'h0000_0000;
    wire [31:0] c_dst;
    surf_cdc_word #(.WIDTH(32)) cdc (
        .src_clk(clk_a), .src_rst(rst_a), .src_data(c_src),
        .dst_clk(clk_b), .dst_rst(rst_b), .dst_data(c_dst));

    // dst may only ever show values the source had: here every source value has all its
    // bytes equal, so a torn value would show different bytes
    always @(posedge clk_b) begin
        if (!rst_b && !(c_dst[31:24] == c_dst[23:16] && c_dst[23:16] == c_dst[15:8] &&
                        c_dst[15:8] == c_dst[7:0])) begin
            $display("cdc_word: torn value %h", c_dst); errors = errors + 1;
        end
    end

    task cdc_test;
        integer n;
        reg [7:0] b;
        begin
            for (n = 0; n < 3000; n = n + 1) begin
                b = $random(seed);
                @(negedge clk_a); c_src = {b, b, b, b};
                repeat ($random(seed) & 7) @(posedge clk_a);
            end
            c_src = 32'h5A5A_5A5A;
            repeat (40) @(posedge clk_b);
            if (c_dst !== 32'h5A5A_5A5A) begin
                $display("cdc_word: did not converge, %h", c_dst); errors = errors + 1;
            end
        end
    endtask

    // ---------------------------------------------------------------- surf_pulse_sync
    reg  p_src = 0;
    wire p_dst;
    surf_pulse_sync ps (.src_clk(clk_a), .src_rst(rst_a), .src_pulse(p_src),
                        .dst_clk(clk_b), .dst_rst(rst_b), .dst_pulse(p_dst));
    integer p_sent = 0, p_got = 0;
    always @(posedge clk_b) if (p_dst) p_got = p_got + 1;

    task pulse_test;
        integer n;
        begin
            for (n = 0; n < 500; n = n + 1) begin
                @(negedge clk_a); p_src = 1; p_sent = p_sent + 1;   // drive away from the sampling edge
                @(negedge clk_a); p_src = 0;
                repeat (16 + ($random(seed) & 15)) @(posedge clk_a);   // >= 3 dst cycles apart
            end
            repeat (10) @(posedge clk_b);
            if (p_got != p_sent) begin
                $display("pulse_sync: sent %0d, got %0d", p_sent, p_got); errors = errors + 1;
            end
        end
    endtask

    // ---------------------------------------------------------------- surf_reset_sync, surf_sync
    reg  r_arst = 1;
    wire r_rst;
    surf_reset_sync #(.STAGES(3)) rs (.clk(clk_a), .arst(r_arst), .rst(r_rst));
    reg  s_d = 0;
    wire s_q;
    surf_sync #(.STAGES(2)) sy (.clk(clk_a), .rst(1'b0), .d(s_d), .q(s_q));

    task reset_test;
        integer n;
        begin
            @(negedge clk_a); r_arst = 0;
            for (n = 1; n <= 3; n = n + 1) begin
                @(posedge clk_a); #1;
                if (r_rst !== (n < 3)) begin
                    $display("reset_sync: after %0d edges rst=%b", n, r_rst); errors = errors + 1;
                end
            end
            #2 r_arst = 1; #1;
            if (r_rst !== 1'b1) begin $display("reset_sync: not asynchronous"); errors = errors + 1; end
            @(negedge clk_a); s_d = 1;
            @(posedge clk_a); #1; if (s_q !== 1'b0) begin $display("sync: too early"); errors = errors + 1; end
            @(posedge clk_a); #1; if (s_q !== 1'b1) begin $display("sync: too late"); errors = errors + 1; end
        end
    endtask

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        repeat (4) @(posedge clk_b);
        rst_a = 0; rst_b = 0;
        fifo_test;
        cdc_test;
        pulse_test;
        reset_test;
        if (errors) $fatal(1, "tb_lib: %0d errors", errors);
        $display("tb_lib: PASS (fifo 20000 steps, cdc 3000 values, %0d pulses)", p_sent);
        $finish;
    end
endmodule
