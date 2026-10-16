/*
SurfCPU DDR mailbox proof harness
--------------------
File: mailbox_formal.sv Date: 10/15/26 (SurfCPU)
--------------------
Formal harness for surf_ddr_mailbox (mailbox.sby): both clocks and every input are free, so
the solver tries every interleaving of the two clocks' edges (any frequencies, any phases,
edges at the same instant, either clock stopping) and every sequence of sends, data and
takes. It counts the messages sent (send while ready) and taken (take while valid) and proves,
in every reachable state:
  * at most one message is ever in the box (no message overwritten or lost);
  * dst_valid only while a message is in the box, and dst_data is then that message, so after
    a take dst_valid stays low until a new message is sent (none is delivered twice);
  * src_ready only when the box is empty (the source never refills it early);
and, as a cover, that three messages can get through.
The synchronizers are modelled as plain flip-flops: metastability is outside the proof.
*/
module mailbox_formal #(
    parameter integer WIDTH = 4
) (
    input wire             src_clk,
    input wire             dst_clk,
    input wire             src_send,
    input wire [WIDTH-1:0] src_data,
    input wire             dst_take
);
    wire             src_ready, dst_valid;
    wire [WIDTH-1:0] dst_data;

    surf_ddr_mailbox #(.WIDTH(WIDTH)) dut (
        .src_clk(src_clk), .src_send(src_send), .src_data(src_data), .src_ready(src_ready),
        .dst_clk(dst_clk), .dst_valid(dst_valid), .dst_data(dst_data), .dst_take(dst_take));

    reg [3:0]       sent  = 4'd0;
    reg [3:0]       taken = 4'd0;
    reg [WIDTH-1:0] last  = {WIDTH{1'b0}};

    always @(posedge src_clk)
        if (src_send && src_ready) begin
            sent <= sent + 4'd1;
            last <= src_data;
        end
    always @(posedge dst_clk)
        if (dst_take && dst_valid)
            taken <= taken + 4'd1;

    wire [3:0] in_box = sent - taken;

    always @(*) begin
        assert (in_box <= 4'd1);
        if (dst_valid) assert (in_box == 4'd1 && dst_data == last);
        if (src_ready) assert (in_box == 4'd0);
        cover (taken == 4'd3);              // messages really get through
    end
endmodule
