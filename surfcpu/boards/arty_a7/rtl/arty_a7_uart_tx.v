/*
SurfCPU Arty A7 baseline UART transmitter
--------------------
File: arty_a7_uart_tx.v Date: 10/15/26 (SurfCPU)
--------------------
8N1 transmitter for the milestone 1 baseline (no CPU, so no surf_uart): one start bit, eight
data bits LSB first, one stop bit, each DIV clk cycles long. A byte is taken at a clock edge
where valid and ready are both 1; ready is 1 whenever the line is idle (the previous stop
bit has been sent). tx is a register and idles high.
*/
module arty_a7_uart_tx #(
    parameter integer DIV = 16             // clk cycles per bit (at least 2)
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       valid,
    input  wire [7:0] data,
    output wire       ready,
    output reg        tx = 1'b1
);
    localparam integer  CW     = $clog2(DIV);
    localparam integer  LAST_I = DIV - 1;
    localparam [CW-1:0] LAST   = LAST_I[CW-1:0];

    reg [8:0]    shift = 9'h1FF;               // stop bit then the data, sent from bit 0
    reg [3:0]    bits  = 4'd0;                 // bit times left, including the one on the line
    reg [CW-1:0] cnt   = {CW{1'b0}};

    assign ready = (bits == 4'd0);

    always @(posedge clk) begin
        if (rst) begin
            shift <= 9'h1FF;
            bits  <= 4'd0;
            cnt   <= {CW{1'b0}};
            tx    <= 1'b1;
        end else if (bits == 4'd0) begin
            if (valid) begin
                tx    <= 1'b0;                 // start bit
                shift <= {1'b1, data};
                bits  <= 4'd10;
                cnt   <= LAST;
            end
        end else if (cnt != {CW{1'b0}}) begin
            cnt <= cnt - 1'b1;
        end else begin
            bits <= bits - 4'd1;
            cnt  <= LAST;
            if (bits != 4'd1) begin
                tx    <= shift[0];
                shift <= {1'b1, shift[8:1]};
            end
        end
    end
endmodule
