/*
SurfCPU surf_intc testbench
--------------------
File: tb_intc.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for the interrupt controller through the SurfBus BFM: reset values, every
register against a model under random source levels and enables (PENDING, ENABLE, ACTIVE,
NEXT with every single source and none), irq_external after every step, writes to read-only
registers ignored, and the error rules (unknown offsets, non-word accesses, misaligned words
and fetches answer with an error and change nothing). A protocol monitor checks every
transfer is answered in the next cycle. +seed=N. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_intc;
    `include "surfcpu_soc.vh"

    reg clk = 1'b0;
    always #20 clk = ~clk;
    reg rst = 1'b1;
    integer seed = 1;
    integer errors = 0;

    wire        req_valid, req_ready, req_write, req_fetch, rsp_valid, rsp_err;
    wire [31:0] req_addr, req_wdata, rsp_rdata;
    wire [3:0]  req_be;
    reg  [31:0] sources = 32'h0;
    wire        irq;

    surfbus_bfm #(.NAME("bfm")) bfm (
        .clk(clk), .m_req_valid(req_valid), .m_req_ready(req_ready), .m_req_addr(req_addr),
        .m_req_write(req_write), .m_req_be(req_be), .m_req_wdata(req_wdata),
        .m_req_fetch(req_fetch), .m_rsp_valid(rsp_valid), .m_rsp_rdata(rsp_rdata),
        .m_rsp_err(rsp_err));
    surfbus_monitor #(.NAME("mon"), .MAX_LAT(1)) mon (
        .clk(clk), .rst(rst), .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_write(req_write), .req_be(req_be), .req_wdata(req_wdata), .req_fetch(req_fetch),
        .rsp_valid(rsp_valid), .rsp_rdata(rsp_rdata), .rsp_err(rsp_err));
    surf_intc dut (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid), .s_req_ready(req_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata),
        .s_req_fetch(req_fetch), .s_rsp_valid(rsp_valid), .s_rsp_rdata(rsp_rdata),
        .s_rsp_err(rsp_err), .sources(sources), .irq_external(irq));

    localparam [31:0] B = SURF_INTC_BASE;

    // reference: number of the lowest set bit, scanning upward (the RTL scans downward)
    function [31:0] lowest;
        input [31:0] v;
        integer j;
        begin
            j = 0;
            while (j < 32 && !v[j]) j = j + 1;
            lowest = (j < 32) ? j : 32'hFFFF_FFFF;
        end
    endfunction

    reg [31:0] en_model = 32'h0;

    task check_irq;
        begin
            #1;
            if (irq !== |(sources & en_model)) begin
                $display("ERROR irq_external %b, sources %h enable %h", irq, sources, en_model);
                errors = errors + 1;
            end
        end
    endtask

    task check_all;
        begin
            bfm.read_check(B + SURF_INTC_PENDING, 4'hF, sources, 32'hFFFF_FFFF);
            bfm.read_check(B + SURF_INTC_ENABLE, 4'hF, en_model, 32'hFFFF_FFFF);
            bfm.read_check(B + SURF_INTC_ACTIVE, 4'hF, sources & en_model, 32'hFFFF_FFFF);
            bfm.read_check(B + SURF_INTC_NEXT, 4'hF, lowest(sources & en_model), 32'hFFFF_FFFF);
            check_irq;
        end
    endtask

    // offsets with a register, for the error tests
    function is_reg;
        input [11:0] off;
        begin
            is_reg = (off == SURF_INTC_PENDING[11:0]) || (off == SURF_INTC_ENABLE[11:0]) ||
                     (off == SURF_INTC_ACTIVE[11:0]) || (off == SURF_INTC_NEXT[11:0]);
        end
    endfunction

    integer n, k, nerr_checks;
    reg [11:0] off;
    reg [3:0]  be;
    reg [31:0] regs [0:3];

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        bfm.seed = seed;
        regs[0] = SURF_INTC_PENDING; regs[1] = SURF_INTC_ENABLE;
        regs[2] = SURF_INTC_ACTIVE;  regs[3] = SURF_INTC_NEXT;
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;

        // reset values, with and without sources
        check_all;
        sources = 32'h8000_0011;
        check_all;

        // NEXT for every single enabled source, and the lowest of two
        en_model = 32'hFFFF_FFFF;
        bfm.write_ok(B + SURF_INTC_ENABLE, 4'hF, en_model);
        for (k = 0; k < 32; k = k + 1) begin
            sources = 32'h1 << k;
            check_all;
            sources = (32'h1 << k) | 32'h8000_0000;
            bfm.read_check(B + SURF_INTC_NEXT, 4'hF, k, 32'hFFFF_FFFF);
        end

        // random levels and enables
        for (n = 0; n < 3000; n = n + 1) begin
            case ({$random(seed)} % 7)
                0: sources = $random(seed) & $random(seed) & $random(seed);
                1: sources = $random(seed);
                2: begin
                       en_model = $random(seed) & $random(seed);
                       bfm.write_ok(B + SURF_INTC_ENABLE, 4'hF, en_model);
                   end
                3: bfm.read_check(B + SURF_INTC_PENDING, 4'hF, sources, 32'hFFFF_FFFF);
                4: bfm.read_check(B + SURF_INTC_ENABLE, 4'hF, en_model, 32'hFFFF_FFFF);
                5: bfm.read_check(B + SURF_INTC_ACTIVE, 4'hF, sources & en_model, 32'hFFFF_FFFF);
                default: bfm.read_check(B + SURF_INTC_NEXT, 4'hF, lowest(sources & en_model),
                                        32'hFFFF_FFFF);
            endcase
            check_irq;
        end

        // writes to the read-only registers are accepted and ignored
        sources  = 32'h0000_00A5;
        en_model = 32'h0000_000F;
        bfm.write_ok(B + SURF_INTC_ENABLE, 4'hF, en_model);
        bfm.write_ok(B + SURF_INTC_PENDING, 4'hF, 32'hFFFF_FFFF);
        bfm.write_ok(B + SURF_INTC_ACTIVE, 4'hF, 32'hFFFF_FFFF);
        bfm.write_ok(B + SURF_INTC_NEXT, 4'hF, 32'h0);
        check_all;

        // error rules: nothing changes
        nerr_checks = 0;
        for (n = 0; n < 400; n = n + 1) begin
            off = {$random(seed)} & 12'hFFC;
            if (!is_reg(off)) begin
                bfm.read_err(B + off, 4'hF);
                bfm.write_err(B + off, 4'hF, 32'hFFFF_FFFF);
                nerr_checks = nerr_checks + 2;
            end
        end
        for (k = 0; k < 4; k = k + 1) begin
            for (n = 0; n < 6; n = n + 1) begin
                case (n)
                    0: be = 4'b0001; 1: be = 4'b0010; 2: be = 4'b0100;
                    3: be = 4'b1000; 4: be = 4'b0011; default: be = 4'b1100;
                endcase
                bfm.read_err(B + regs[k], be);
                bfm.write_err(B + regs[k], be, 32'hFFFF_FFFF);
                nerr_checks = nerr_checks + 2;
            end
            bfm.read_err(B + regs[k] + 1, 4'hF);             // misaligned word
            bfm.write_err(B + regs[k] + 2, 4'hF, 32'hFFFF_FFFF);
            bfm.fetch_flag = 1'b1;                            // fetches never reach MMIO
            bfm.read_err(B + regs[k], 4'hF);
            bfm.fetch_flag = 1'b0;
            nerr_checks = nerr_checks + 3;
        end
        check_all;      // ENABLE is still 0xF

        repeat (2) @(posedge clk);   // let the monitor see the last response
        errors = errors + bfm.errors + mon.errors;
        if (mon.accepts != mon.responses) begin
            $display("ERROR %0d requests, %0d responses", mon.accepts, mon.responses);
            errors = errors + 1;
        end
        if (errors) $fatal(1, "tb_intc: %0d errors", errors);
        $display("tb_intc: PASS (%0d transfers, %0d error responses checked, %0d back-to-back)",
                 mon.accepts, nerr_checks, bfm.n_b2b);
        $finish;
    end
endmodule
