/*
SurfCPU Arty A7: read every word of a synthesized boot ROM
--------------------
File: tb_rom_read.v Date: 10/16/26 (SurfCPU)
--------------------
For boards/arty_a7/romcheck.py: simulates surf_bootrom as Yosys synthesized it (its netlist,
Yosys's cell models and the block RAM models of sim/board/prims), reads all 16384 words on
port A (addresses up) and port B (addresses down) at once, and compares each with the .mem
file given as +mem=FILE. Prints "romcheck: N words on each port, M mismatches" and the first
mismatches. Simulation only.
*/
`timescale 1ns/1ps
module tb_rom_read;
    `include "surfcpu_soc.vh"
    localparam integer WORDS = SURF_BOOTROM_SIZE / 4;

    reg clk = 1'b0;
    always #5 clk = ~clk;
    reg  [31:0] golden [0:WORDS-1];
    reg  [31:0] a_addr = 32'h0, b_addr = 32'h0;
    wire [31:0] a_q, b_q;
    reg  [8*1024-1:0] file;
    integer i, bad = 0;

    /* verilator lint_off PINCONNECTEMPTY */
    surf_bootrom dut (
        .clk(clk), .rst(1'b0),
        .a_req_valid(1'b1), .a_req_ready(), .a_req_addr(a_addr), .a_req_write(1'b0), .a_req_be(4'hF),
        .a_req_wdata(32'h0), .a_req_fetch(1'b1), .a_rsp_valid(), .a_rsp_rdata(a_q), .a_rsp_err(),
        .b_req_valid(1'b1), .b_req_ready(), .b_req_addr(b_addr), .b_req_write(1'b0), .b_req_be(4'hF),
        .b_req_wdata(32'h0), .b_req_fetch(1'b0), .b_rsp_valid(), .b_rsp_rdata(b_q), .b_rsp_err());
    /* verilator lint_on PINCONNECTEMPTY */

    initial begin
        if (!$value$plusargs("mem=%s", file)) $fatal(1, "tb_rom_read: no +mem=FILE");
        for (i = 0; i < WORDS; i = i + 1) golden[i] = 32'h0;
        $readmemh(file, golden);
        for (i = 0; i < WORDS; i = i + 1) begin
            a_addr = i * 4;
            b_addr = (WORDS - 1 - i) * 4;
            @(posedge clk);
            #1;
            if (a_q !== golden[i]) begin
                bad = bad + 1;
                if (bad <= 5) $display("tb_rom_read: port A word %0d reads %h, the .mem has %h", i, a_q, golden[i]);
            end
            if (b_q !== golden[WORDS - 1 - i]) begin
                bad = bad + 1;
                if (bad <= 5) $display("tb_rom_read: port B word %0d reads %h, the .mem has %h",
                                       WORDS - 1 - i, b_q, golden[WORDS - 1 - i]);
            end
        end
        $display("romcheck: %0d words on each port, %0d mismatches", WORDS, bad);
        $finish;
    end
endmodule
