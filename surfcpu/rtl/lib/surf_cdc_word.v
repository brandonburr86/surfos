/*
SurfCPU word synchronizer
--------------------
File: surf_cdc_word.v Date: 10/15/26 (SurfCPU)
--------------------
Keeps dst_data a copy of src_data from another clock domain, all bits at once: a request/
acknowledge toggle handshake moves a held copy across, so dst_data only ever shows values
src_data really had (never a mix of two). Latency is a few cycles of each clock, and a
value that lasts shorter than one round trip may be skipped: use it for registers and
status, not for streams. Timing: the hold -> dst_data path is a CDC path (constrain it with
set_max_delay -datapath_only, docs/BOARD.md); both reset sides recover on their own.
*/
module surf_cdc_word #(
    parameter integer     WIDTH = 32,
    parameter [WIDTH-1:0] INIT  = {WIDTH{1'b0}}
) (
    input  wire             src_clk,
    input  wire             src_rst,
    input  wire [WIDTH-1:0] src_data,
    input  wire             dst_clk,
    input  wire             dst_rst,
    output reg  [WIDTH-1:0] dst_data = INIT
);
    // source side: whenever the last transfer was acknowledged, hold a new copy and request
    reg [WIDTH-1:0] hold = INIT;
    reg             req  = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg [1:0] ack_sync = 2'b00;
    reg             ack  = 1'b0;    // destination side

    always @(posedge src_clk) begin
        if (src_rst) begin
            hold     <= INIT;
            req      <= 1'b0;
            ack_sync <= 2'b00;
        end else begin
            ack_sync <= {ack_sync[0], ack};
            if (ack_sync[1] == req) begin
                hold <= src_data;
                req  <= ~req;
            end
        end
    end

    // destination side: a changed request means hold is stable; copy it and acknowledge
    (* ASYNC_REG = "TRUE" *) reg [1:0] req_sync = 2'b00;

    always @(posedge dst_clk) begin
        if (dst_rst) begin
            dst_data <= INIT;
            ack      <= 1'b0;
            req_sync <= 2'b00;
        end else begin
            req_sync <= {req_sync[0], req};
            if (req_sync[1] != ack) begin
                dst_data <= hold;
                ack      <= ~ack;
            end
        end
    end
endmodule
