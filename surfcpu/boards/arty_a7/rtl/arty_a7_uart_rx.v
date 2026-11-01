/*
SurfCPU Arty A7 baseline UART receiver
--------------------
File: arty_a7_uart_rx.v Date: 10/15/26 (SurfCPU)
--------------------
8N1 receiver for the milestone 1 baseline, which only shows the last byte received (so the
owner can check the FPGA's RX pin before there is a CPU). The pin is synchronized with
surf_sync; a falling edge starts a frame, the start bit is checked at its middle (a shorter
glitch is ignored), the data bits are sampled at their middles, and a byte whose stop bit is 0
is dropped. valid pulses for one clk cycle with data.
*/
module arty_a7_uart_rx #(
    parameter integer DIV = 16             // clk cycles per bit (at least 4)
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       rx,                  // the pin, asynchronous
    output reg        valid = 1'b0,
    output reg  [7:0] data  = 8'h00
);
    localparam integer  CW     = $clog2(DIV);
    localparam integer  FULL_I = DIV - 1;
    localparam integer  HALF_I = DIV / 2 - 1;
    localparam [CW-1:0] FULL   = FULL_I[CW-1:0];
    localparam [CW-1:0] HALF   = HALF_I[CW-1:0];

    wire rx_s;
    surf_sync #(.STAGES(2), .INIT(1'b1)) u_sync (.clk(clk), .rst(rst), .d(rx), .q(rx_s));

    reg          busy  = 1'b0;
    reg [3:0]    bits  = 4'd0;                 // 0 = start bit, 1..8 = data, 9 = stop
    reg [CW-1:0] cnt   = {CW{1'b0}};
    reg [7:0]    shift = 8'h00;

    always @(posedge clk) begin
        valid <= 1'b0;
        if (rst) begin
            busy <= 1'b0;
            bits <= 4'd0;
            cnt  <= {CW{1'b0}};
        end else if (!busy) begin
            if (!rx_s) begin                   // falling edge: wait to the middle of the start bit
                busy <= 1'b1;
                bits <= 4'd0;
                cnt  <= HALF;
            end
        end else if (cnt != {CW{1'b0}}) begin
            cnt <= cnt - 1'b1;
        end else begin
            cnt <= FULL;
            if (bits == 4'd0) begin
                if (rx_s) busy <= 1'b0;        // not a start bit after all
                bits <= 4'd1;
            end else if (bits != 4'd9) begin
                shift <= {rx_s, shift[7:1]};
                bits  <= bits + 4'd1;
            end else begin
                busy <= 1'b0;
                if (rx_s) begin                // good stop bit
                    valid <= 1'b1;
                    data  <= shift;
                end
            end
        end
    end
endmodule
