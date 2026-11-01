/*
SurfCPU openXC7 feature probe, portable core
--------------------
File: flow_features_core.v Date: 10/15/26 (SurfCPU)
--------------------
Exercises, through the open flow, what the SoC will need beyond flow_demo, with results the
owner can read on the UART: 64 KiB of initialized block RAM (the size of SurfBoot's ROM),
32x32 multiplies (DSP48E1), a second clock from a fractional MMCM setting (the planned pixel
clock) crossed into the system clock with rtl/lib blocks, and an open-drain pin. Once per
PERIOD system clocks it sends at BAUD, 8N1:

    SurfCPU openXC7 features #nnnn rom=ssssssss mul=pppppppp pix=ffffffff  CR LF

  rom  a checksum over the whole ROM, walked continuously: proves the initial contents
       arrived (expected value: docs/OPEN-FLOW.md); word i holds i * 0x9E3779B1 ^ 0x5A5A0F0F,
       and the checksum is sum = rotl(sum, 1) ^ word over i = 0 .. ROM_WORDS-1 from 0
  mul  bits 63..32 of rom * {nnnn, nnnn}, a full 32x32 product
  pix  pixel-clock cycles counted during the last PERIOD system clocks (with PERIOD = one
       second this is the pixel clock in Hz; docs/OPEN-FLOW.md lists the values to expect)

LEDs: 0 a 1 Hz heartbeat, 1 the open-drain pin as read back, 2 bit 24 of the pixel-clock
counter (about 0.75 Hz), 3 both clocks locked. od_pull_low follows sw0: the pin is pulled
low while the switch is on and released otherwise. Not part of the SoC.
*/
module flow_features_core #(
    parameter integer SYS_HZ    = 25000000,
    parameter integer BAUD      = 115200,
    parameter integer PERIOD    = 25000000,
    parameter integer ROM_WORDS = 16384
) (
    input  wire       sys_clk,
    input  wire       sys_rst,
    input  wire       pix_clk,
    input  wire       pix_rst,
    input  wire       locked,         // both clock generators locked (asynchronous)
    input  wire       sw0,            // asynchronous
    input  wire       od_in,          // the open-drain pin's level (asynchronous)
    output wire       od_pull_low,
    output wire [3:0] led,
    output wire       uart_tx
);
    localparam integer DIV      = (SYS_HZ + BAUD / 2) / BAUD;
    localparam integer DIV_W    = $clog2(DIV);
    localparam integer PERIOD_W = $clog2(PERIOD);
    localparam integer ADDR_W   = $clog2(ROM_WORDS);
    localparam [31:0] DIV_LAST32    = DIV - 1;
    localparam [31:0] PERIOD_LAST32 = PERIOD - 1;
    localparam [31:0] HALF_LAST32   = PERIOD / 2 - 1;
    localparam [31:0] ROM_LAST32    = ROM_WORDS - 1;
    localparam [DIV_W-1:0]    DIV_LAST    = DIV_LAST32[DIV_W-1:0];
    localparam [PERIOD_W-1:0] PERIOD_LAST = PERIOD_LAST32[PERIOD_W-1:0];
    localparam [PERIOD_W-1:0] HALF_LAST   = HALF_LAST32[PERIOD_W-1:0];
    localparam [ADDR_W-1:0]   ROM_LAST    = ROM_LAST32[ADDR_W-1:0];
    localparam [6:0]          LINE_LAST   = 7'd70;                // 71 characters with CR LF

    // ---------------------------------------------------------------- synchronizers
    wire sw0_s, od_s, locked_s;
    surf_sync sync_sw  (.clk(sys_clk), .rst(sys_rst), .d(sw0),    .q(sw0_s));
    surf_sync sync_od  (.clk(sys_clk), .rst(sys_rst), .d(od_in),  .q(od_s));
    surf_sync sync_lck (.clk(sys_clk), .rst(sys_rst), .d(locked), .q(locked_s));

    // ---------------------------------------------------------------- 64 KiB ROM in block RAM
    (* rom_style = "block" *) reg [31:0] rom [0:ROM_WORDS-1];
    integer i;
    initial begin
        for (i = 0; i < ROM_WORDS; i = i + 1)
            rom[i] = i * 32'h9E3779B1 ^ 32'h5A5A0F0F;
    end

    reg [ADDR_W-1:0] rom_addr;
    reg [31:0]       rom_q;
    reg              rom_q_valid, rom_last;     // rom_last: rom_q holds the last word
    reg [31:0]       rom_acc, rom_sum;
    always @(posedge sys_clk) rom_q <= rom[rom_addr];          // the block RAM's read port

    always @(posedge sys_clk) begin
        if (sys_rst) begin
            rom_addr    <= {ADDR_W{1'b0}};
            rom_last    <= 1'b0;
            rom_q_valid <= 1'b0;
            rom_acc     <= 32'h0;
            rom_sum     <= 32'h0;
        end else begin
            rom_addr    <= rom_addr + 1'b1;                     // wraps: passes run back to back
            rom_last    <= (rom_addr == ROM_LAST);
            rom_q_valid <= 1'b1;
            if (rom_q_valid) begin
                if (rom_last) begin
                    rom_sum <= {rom_acc[30:0], rom_acc[31]} ^ rom_q;
                    rom_acc <= 32'h0;
                end else begin
                    rom_acc <= {rom_acc[30:0], rom_acc[31]} ^ rom_q;
                end
            end
        end
    end

    // ---------------------------------------------------------------- the pixel clock, measured
    reg  [31:0] pix_cnt;
    always @(posedge pix_clk) pix_cnt <= pix_rst ? 32'h0 : pix_cnt + 1'b1;
    wire [31:0] pix_seen;
    surf_cdc_word #(.WIDTH(32)) pix_cross (.src_clk(pix_clk), .src_rst(pix_rst), .src_data(pix_cnt),
                                           .dst_clk(sys_clk), .dst_rst(sys_rst), .dst_data(pix_seen));

    // ---------------------------------------------------------------- the beat and the snapshots
    reg [PERIOD_W-1:0] period_cnt;
    reg                heartbeat;
    wire               tick = (period_cnt == PERIOD_LAST);
    reg  [15:0]        line_cnt;
    reg  [31:0]        pix_last;
    reg  [31:0]        snap_rom, snap_mul, snap_pix;
    reg  [15:0]        snap_line;
    // Only the high half is printed; it needs the whole 32x32 product, so the low half being
    // unused costs no coverage of the multiplier.
    /* verilator lint_off UNUSEDSIGNAL */
    reg  [63:0]        product;
    /* verilator lint_on UNUSEDSIGNAL */
    reg                sending;
    reg  [6:0]         char_idx;
    reg                tx_active;
    wire               tx_load = sending && !tx_active;

    always @(posedge sys_clk) begin
        product <= rom_sum * {line_cnt, line_cnt};              // 32 x 32 -> 64 (DSP48E1)
        if (sys_rst) begin
            period_cnt <= {PERIOD_W{1'b0}};
            heartbeat  <= 1'b1;
            pix_last   <= 32'h0;
        end else begin
            period_cnt <= tick ? {PERIOD_W{1'b0}} : period_cnt + 1'b1;
            heartbeat  <= tick || (period_cnt < HALF_LAST);
            if (tick) pix_last <= pix_seen;
        end
    end

    // ---------------------------------------------------------------- the line
    localparam [8*26-1:0] TEXT = "SurfCPU openXC7 features #";

    function [7:0] hex_ascii(input [3:0] n);
        hex_ascii = (n < 4'd10) ? 8'h30 + {4'h0, n} : 8'h37 + {4'h0, n};
    endfunction
    function [3:0] nibble(input [31:0] v, input [2:0] k);     // k = 0 is the most significant
        nibble = v[31 - 4 * k -: 4];
    endfunction

    reg [7:0] tx_char;
    always @(*) begin
        if (char_idx < 7'd26)      tx_char = TEXT[8 * (25 - char_idx) +: 8];
        else if (char_idx < 7'd30) tx_char = hex_ascii(nibble({16'h0, snap_line}, char_idx[2:0] + 3'd2));
        else if (char_idx == 7'd30 || char_idx == 7'd43 || char_idx == 7'd56) tx_char = 8'h20;
        else if (char_idx < 7'd35) tx_char = (char_idx == 7'd31) ? "r" : (char_idx == 7'd32) ? "o" :
                                             (char_idx == 7'd33) ? "m" : "=";
        else if (char_idx < 7'd43) tx_char = hex_ascii(nibble(snap_rom, char_idx[2:0] - 3'd3));
        else if (char_idx < 7'd48) tx_char = (char_idx == 7'd44) ? "m" : (char_idx == 7'd45) ? "u" :
                                             (char_idx == 7'd46) ? "l" : "=";
        else if (char_idx < 7'd56) tx_char = hex_ascii(nibble(snap_mul, char_idx[2:0]));
        else if (char_idx < 7'd61) tx_char = (char_idx == 7'd57) ? "p" : (char_idx == 7'd58) ? "i" :
                                             (char_idx == 7'd59) ? "x" : "=";
        else if (char_idx < 7'd69) tx_char = hex_ascii(nibble(snap_pix, char_idx[2:0] - 3'd5));
        else if (char_idx == 7'd69) tx_char = 8'h0D;
        else                        tx_char = 8'h0A;
    end

    always @(posedge sys_clk) begin
        if (sys_rst) begin
            char_idx  <= 7'd0;
            sending   <= 1'b0;
            line_cnt  <= 16'h0;
            snap_line <= 16'h0;
            snap_rom  <= 32'h0;
            snap_mul  <= 32'h0;
            snap_pix  <= 32'h0;
        end else if (tx_load) begin
            if (char_idx == LINE_LAST) begin
                char_idx <= 7'd0;
                sending  <= 1'b0;
                line_cnt <= line_cnt + 1'b1;
            end else begin
                char_idx <= char_idx + 1'b1;
            end
        end else if (tick) begin
            // the line starts one tick after reset, when the ROM has been walked many times
            sending   <= 1'b1;
            snap_line <= line_cnt;
            snap_rom  <= rom_sum;
            snap_mul  <= product[63:32];
            snap_pix  <= pix_seen - pix_last;
        end
    end

    // ---------------------------------------------------------------- 8N1 transmitter
    reg [9:0]       tx_shift;
    reg [3:0]       tx_bits;
    reg [DIV_W-1:0] tx_cnt;
    always @(posedge sys_clk) begin
        if (sys_rst) begin
            tx_shift  <= 10'h3FF;
            tx_bits   <= 4'd0;
            tx_cnt    <= {DIV_W{1'b0}};
            tx_active <= 1'b0;
        end else if (tx_load) begin
            tx_shift  <= {1'b1, tx_char, 1'b0};
            tx_bits   <= 4'd10;
            tx_cnt    <= DIV_LAST;
            tx_active <= 1'b1;
        end else if (tx_active) begin
            if (tx_cnt == {DIV_W{1'b0}}) begin
                tx_cnt    <= DIV_LAST;
                tx_shift  <= {1'b1, tx_shift[9:1]};
                tx_bits   <= tx_bits - 1'b1;
                tx_active <= (tx_bits != 4'd1);
            end else begin
                tx_cnt <= tx_cnt - 1'b1;
            end
        end
    end

    assign uart_tx     = tx_shift[0];
    assign od_pull_low = sw0_s;
    assign led         = {locked_s, pix_cnt[24], od_s, heartbeat};
endmodule
