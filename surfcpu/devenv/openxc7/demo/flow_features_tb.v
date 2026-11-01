/*
SurfCPU openXC7 feature probe testbench
--------------------
File: flow_features_tb.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for flow_features_core with a 25 MHz system clock, a 25.1736 MHz pixel
clock (period 39.724 ns, unrelated to the system clock) and the real 64 KiB ROM and 115200
baud; only the line period is shortened (PERIOD). It decodes the UART lines and checks the
text, the counter, rom= against the checksum computed here from the ROM's definition, mul=
against the 64-bit product computed here, and pix= (from the second line on) against the
cycle count the two clock periods give (within 8); also the open-drain output following sw0, the LEDs
following the pin and the locked input. Prints the expected rom= value for a real board.
+lines=N (default 3). Prints PASS or stops with $fatal.
*/
`timescale 1ps/1ps
module flow_features_tb;
    parameter integer SYS_HZ    = 25000000;
    parameter integer BAUD      = 115200;
    parameter integer PERIOD    = 200000;
    parameter integer ROM_WORDS = 16384;
    localparam integer SYS_HALF = 20000;                // ps: 25 MHz
    localparam integer PIX_HALF = 19862;                // ps: 39.724 ns, about 25.1736 MHz
    localparam integer DIV      = (SYS_HZ + BAUD / 2) / BAUD;
    localparam integer LINE_LEN = 71;

    reg sys_clk = 1'b0, pix_clk = 1'b0;
    always #(SYS_HALF) sys_clk = ~sys_clk;
    always #(PIX_HALF) pix_clk = ~pix_clk;
    reg sys_rst = 1'b1, pix_rst = 1'b1, locked = 1'b0, sw0 = 1'b0, od_in = 1'b1;
    wire od_pull_low, tx;
    wire [3:0] led;

    flow_features_core #(.SYS_HZ(SYS_HZ), .BAUD(BAUD), .PERIOD(PERIOD), .ROM_WORDS(ROM_WORDS)) dut (
        .sys_clk(sys_clk), .sys_rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst), .locked(locked),
        .sw0(sw0), .od_in(od_in), .od_pull_low(od_pull_low), .led(led), .uart_tx(tx));

    integer errors = 0, lines = 3;
    integer cyc = 0;
    always @(posedge sys_clk) cyc <= cyc + 1;

    // ---------------------------------------------------------------- expected values
    reg [31:0] rom_expect;
    integer k;
    initial begin
        rom_expect = 32'h0;
        for (k = 0; k < ROM_WORDS; k = k + 1)
            rom_expect = {rom_expect[30:0], rom_expect[31]} ^ (k * 32'h9E3779B1 ^ 32'h5A5A0F0F);
    end
    reg [8*16-1:0] hexd = "0123456789ABCDEF";
    function [7:0] hexc(input [31:0] v, input integer k);     // digit k of 8, 0 = most significant
        hexc = hexd[8 * (15 - ((v >> (4 * (7 - k))) & 15)) +: 8];
    endfunction

    // ---------------------------------------------------------------- receiver
    task receive_char(output [7:0] c);
        integer b;
        begin
            @(negedge tx);
            repeat (DIV / 2) @(negedge sys_clk);
            if (tx !== 1'b0) begin $display("rx: bad start bit at cycle %0d", cyc); errors = errors + 1; end
            for (b = 0; b < 8; b = b + 1) begin
                repeat (DIV) @(negedge sys_clk);
                c[b] = tx;
            end
            repeat (DIV) @(negedge sys_clk);
            if (tx !== 1'b1) begin $display("rx: bad stop bit at cycle %0d", cyc); errors = errors + 1; end
        end
    endtask

    reg [8*LINE_LEN-1:0] line;
    task receive_line;
        integer i;
        reg [7:0] c;
        begin
            for (i = 0; i < LINE_LEN; i = i + 1) begin
                receive_char(c);
                line[8 * (LINE_LEN - 1 - i) +: 8] = c;
            end
        end
    endtask

    function [31:0] parse_hex(input integer at, input integer digits);   // from `line`
        integer d;
        reg [7:0] c;
        begin
            parse_hex = 0;
            for (d = 0; d < digits; d = d + 1) begin
                c = line[8 * (LINE_LEN - 1 - at - d) +: 8];
                parse_hex = (parse_hex << 4) | ((c >= "A") ? c - "A" + 10 : c - "0");
            end
        end
    endfunction

    task check_line(input integer n);
        reg [8*LINE_LEN-1:0] want;
        reg [63:0] product;
        reg [31:0] pix;
        integer i, pix_want;
        begin
            product = rom_expect * {n[15:0], n[15:0]};
            want = {"SurfCPU openXC7 features #", hexc(n, 4), hexc(n, 5), hexc(n, 6), hexc(n, 7),
                    " rom=", hexc(rom_expect, 0), hexc(rom_expect, 1), hexc(rom_expect, 2), hexc(rom_expect, 3),
                    hexc(rom_expect, 4), hexc(rom_expect, 5), hexc(rom_expect, 6), hexc(rom_expect, 7),
                    " mul=", hexc(product[63:32], 0), hexc(product[63:32], 1), hexc(product[63:32], 2),
                    hexc(product[63:32], 3), hexc(product[63:32], 4), hexc(product[63:32], 5),
                    hexc(product[63:32], 6), hexc(product[63:32], 7), " pix=", "00000000", 8'h0D, 8'h0A};
            // compare everything but the pix= digits (61..68)
            for (i = 0; i < LINE_LEN; i = i + 1)
                if ((i < 61 || i > 68) && line[8 * (LINE_LEN - 1 - i) +: 8] !== want[8 * (LINE_LEN - 1 - i) +: 8]) begin
                    $display("line %0d char %0d is %h, expected %h", n, i,
                             line[8 * (LINE_LEN - 1 - i) +: 8], want[8 * (LINE_LEN - 1 - i) +: 8]);
                    errors = errors + 1;
                end
            pix = parse_hex(61, 8);
            // the crossing hands over a copy a few cycles old, so one period's count can be off by
            // up to a handshake round trip
            pix_want = $rtoi(1.0 * PERIOD * SYS_HALF / PIX_HALF + 0.5);
            if (n > 0 && (pix < pix_want - 8 || pix > pix_want + 8)) begin
                $display("line %0d: pix=%0d, expected %0d +- 8", n, pix, pix_want); errors = errors + 1;
            end
            $display("line %0d: %s", n, line[8 * LINE_LEN - 1:16]);
        end
    endtask

    integer n;
    initial begin
        if ($value$plusargs("lines=%d", lines)) $display("lines %0d", lines);
        #1000000;
        @(negedge sys_clk) sys_rst = 1'b0;
        @(negedge pix_clk) pix_rst = 1'b0;
        locked = 1'b1;
        for (n = 0; n < lines; n = n + 1) begin
            receive_line;
            check_line(n);
            // between lines: the open-drain output follows sw0, the LEDs follow the inputs
            sw0 = n[0]; od_in = ~n[0];
            repeat (4) @(negedge sys_clk);
            if (od_pull_low !== n[0] || led[1] !== ~n[0] || led[3] !== 1'b1) begin
                $display("io: sw0 %b -> od_pull_low %b, od_in %b -> led[1] %b, led[3] %b",
                         n[0], od_pull_low, ~n[0], led[1], led[3]);
                errors = errors + 1;
            end
        end
        $display("flow_features_tb: expected on a board: rom=%h", rom_expect);
        if (errors) $fatal(1, "flow_features_tb: %0d errors", errors);
        $display("flow_features_tb: PASS (%0d lines, ROM %0d words, DIV %0d)", lines, ROM_WORDS, DIV);
        $finish;
    end
endmodule
