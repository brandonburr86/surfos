/*
SurfCPU synchronous FIFO
--------------------
File: surf_fifo.v Date: 10/15/26 (SurfCPU)
--------------------
Single-clock FIFO with first-word fall-through: rd_data is the oldest entry whenever
empty is 0, and rd_en removes it. A push while full and a pop while empty are ignored
(the caller decides whether that is an error). Small depths map to distributed RAM.
*/
module surf_fifo #(
    parameter integer WIDTH      = 8,
    parameter integer DEPTH_LOG2 = 4     // 2**DEPTH_LOG2 entries
) (
    input  wire                  clk,
    input  wire                  rst,    // synchronous, active high: empties the FIFO
    input  wire                  wr_en,
    input  wire [WIDTH-1:0]      wr_data,
    input  wire                  rd_en,
    output wire [WIDTH-1:0]      rd_data,
    output wire                  empty,
    output wire                  full,
    output wire [DEPTH_LOG2:0]   count
);
    localparam integer DEPTH = 1 << DEPTH_LOG2;

    reg [WIDTH-1:0]    mem [0:DEPTH-1];
    reg [DEPTH_LOG2:0] wptr = {(DEPTH_LOG2+1){1'b0}};   // the extra top bit tells full from empty
    reg [DEPTH_LOG2:0] rptr = {(DEPTH_LOG2+1){1'b0}};

    wire do_wr = wr_en && !full;
    wire do_rd = rd_en && !empty;

    assign empty   = (wptr == rptr);
    assign full    = (wptr[DEPTH_LOG2-1:0] == rptr[DEPTH_LOG2-1:0]) &&
                     (wptr[DEPTH_LOG2] != rptr[DEPTH_LOG2]);
    assign count   = wptr - rptr;
    assign rd_data = mem[rptr[DEPTH_LOG2-1:0]];

    always @(posedge clk) begin
        if (do_wr) mem[wptr[DEPTH_LOG2-1:0]] <= wr_data;
        if (rst) begin
            wptr <= {(DEPTH_LOG2+1){1'b0}};
            rptr <= {(DEPTH_LOG2+1){1'b0}};
        end else begin
            if (do_wr) wptr <= wptr + 1'b1;
            if (do_rd) rptr <= rptr + 1'b1;
        end
    end
endmodule
