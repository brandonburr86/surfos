/*
SurfCPU openXC7 flow demo
--------------------
File: flow_demo.v Date: 10/15/26 (SurfCPU)
--------------------
The flow track's own small design for proving the open-source bitstream flow end to end
(docs/OPEN-FLOW.md); it is not part of the SoC (the board track owns the milestone 1
baseline). On the Arty A7 it shows

    LD4         a 1 Hz heartbeat (on for the first half of each second)
    LD5..LD7    the low three bits of the line counter
    USB-UART    one line per second at 115200 8N1:  "SurfCPU openXC7 flow demo #0000" CR LF
                (the counter in four hex digits; the first line leaves right after reset)

and the RESET button (ck_rst, low while pressed) restarts everything at #0000. Port names
are the ones in Digilent's master XDC, so the constraints are its lines uncommented.
PERIOD is the number of clock cycles from one line to the next (one second by default).
*/
module flow_demo #(
    parameter integer CLK_HZ = 100000000,
    parameter integer BAUD   = 115200,
    parameter integer PERIOD = 100000000
) (
    input  wire       CLK100MHZ,
    input  wire       ck_rst,
    output wire [3:0] led,
    output wire       uart_rxd_out
);
    localparam integer DIV      = (CLK_HZ + BAUD / 2) / BAUD;    // clock cycles per bit
    localparam integer DIV_W    = $clog2(DIV);
    localparam integer PERIOD_W = $clog2(PERIOD);
    localparam integer TEXT_LEN = 27;
    localparam [8*TEXT_LEN-1:0] TEXT = "SurfCPU openXC7 flow demo #";

    // 32-bit constants, used through part-selects of the width each counter needs
    localparam [31:0] DIV_LAST32    = DIV - 1;
    localparam [31:0] PERIOD_LAST32 = PERIOD - 1;
    localparam [31:0] HALF_LAST32   = PERIOD / 2 - 1;
    localparam [31:0] TEXT_END32    = TEXT_LEN;                  // first index after the text
    localparam [31:0] LINE_LAST32   = TEXT_LEN + 5;              // 4 hex digits, CR, LF
    localparam [DIV_W-1:0]    DIV_LAST    = DIV_LAST32[DIV_W-1:0];
    localparam [PERIOD_W-1:0] PERIOD_LAST = PERIOD_LAST32[PERIOD_W-1:0];
    localparam [PERIOD_W-1:0] HALF_LAST   = HALF_LAST32[PERIOD_W-1:0];
    localparam [5:0]          TEXT_END    = TEXT_END32[5:0];
    localparam [5:0]          LINE_LAST   = LINE_LAST32[5:0];

    wire clk = CLK100MHZ;
    wire rst;

    surf_reset_sync #(.STAGES(3)) reset_sync (.clk(clk), .arst(~ck_rst), .rst(rst));

    // ---------------------------------------------------------------- the one-second beat
    reg [PERIOD_W-1:0] period_cnt;
    reg                heartbeat;
    wire               tick = (period_cnt == PERIOD_LAST);

    always @(posedge clk) begin
        if (rst) begin
            period_cnt <= {PERIOD_W{1'b0}};
            heartbeat  <= 1'b1;
        end else begin
            period_cnt <= tick ? {PERIOD_W{1'b0}} : period_cnt + 1'b1;
            heartbeat  <= tick || (period_cnt < HALF_LAST);
        end
    end

    // ---------------------------------------------------------------- the line, character by character
    reg  [5:0]  char_idx;
    reg         sending;
    reg  [15:0] line_cnt;
    reg         tx_active;
    wire        tx_load = sending && !tx_active;

    function [7:0] hex_ascii(input [3:0] n);
        hex_ascii = (n < 4'd10) ? 8'h30 + {4'h0, n} : 8'h37 + {4'h0, n};
    endfunction

    reg [7:0] tx_char;
    always @(*) begin
        case (char_idx)
            TEXT_END:        tx_char = hex_ascii(line_cnt[15:12]);
            TEXT_END + 6'd1: tx_char = hex_ascii(line_cnt[11:8]);
            TEXT_END + 6'd2: tx_char = hex_ascii(line_cnt[7:4]);
            TEXT_END + 6'd3: tx_char = hex_ascii(line_cnt[3:0]);
            TEXT_END + 6'd4: tx_char = 8'h0D;
            TEXT_END + 6'd5: tx_char = 8'h0A;
            default:         tx_char = (char_idx < TEXT_END)
                                       ? TEXT[8 * ({26'd0, TEXT_END - 6'd1 - char_idx}) +: 8] : 8'h3F;
        endcase
    end

    always @(posedge clk) begin
        if (rst) begin
            char_idx <= 6'd0;
            sending  <= 1'b1;
            line_cnt <= 16'h0000;
        end else if (tx_load) begin
            if (char_idx == LINE_LAST) begin
                char_idx <= 6'd0;
                sending  <= 1'b0;
                line_cnt <= line_cnt + 1'b1;
            end else begin
                char_idx <= char_idx + 1'b1;
            end
        end else if (tick) begin
            sending <= 1'b1;
        end
    end

    // ---------------------------------------------------------------- 8N1 transmitter
    reg [9:0]       tx_shift;     // stop, data[7:0], start: bit 0 is on the line
    reg [3:0]       tx_bits;
    reg [DIV_W-1:0] tx_cnt;

    always @(posedge clk) begin
        if (rst) begin
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

    assign uart_rxd_out = tx_shift[0];
    assign led          = {line_cnt[2:0], heartbeat};
endmodule
