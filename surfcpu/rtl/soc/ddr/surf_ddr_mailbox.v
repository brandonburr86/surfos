/*
SurfCPU DDR bridge mailbox
--------------------
File: surf_ddr_mailbox.v Date: 10/15/26 (SurfCPU)
--------------------
A one-message box between two unrelated clocks, used by surf_ddr_bridge for its request
(sys -> ui) and response (ui -> sys) paths. Two-phase handshake: the source copies a message
into hold and flips req; the destination sees req (through two flip-flops) differ from ack,
reads hold, and flips ack when it takes the message; the source sees ack (through two
flip-flops) catch up and may send again. Every message sent is delivered exactly once, in
order, whole (hold does not change between a send and the source seeing it taken).

There is deliberately no reset. The toggles and their synchronizers start from their
initial values (the FPGA configuration, or time 0 in simulation) and keep their relationship
for ever, so resetting either clock domain alone can neither invent a message nor lose one:
the users stop sending and taking while their own domain is in reset, and a message already
in the box stays there until it is taken. Each side's latency is two or three cycles of the
other side's clock; neither clock may be assumed faster.

Timing: hold -> the destination's capture registers, and req/ack -> their first synchronizer
flip-flop, are clock-domain crossings. Constrain them with set_max_delay -datapath_only (at
most one destination clock period; docs/DDR.md); the synchronizers carry ASYNC_REG.
*/
module surf_ddr_mailbox #(
    parameter integer WIDTH = 32
) (
    // source side
    input  wire             src_clk,
    input  wire             src_send,   // send src_data now; acted on only while src_ready
    input  wire [WIDTH-1:0] src_data,
    output wire             src_ready,  // the box is empty: a send in this cycle is taken
    // destination side
    input  wire             dst_clk,
    output wire             dst_valid,  // a message is waiting
    output wire [WIDTH-1:0] dst_data,   // that message (stable while dst_valid; capture it)
    input  wire             dst_take    // remove the message; acted on only while dst_valid
);
    // ---------------------------------------------------------------- source domain
    reg [WIDTH-1:0] hold = {WIDTH{1'b0}};
    reg             req  = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg [1:0] ack_sync = 2'b00;
    reg             ack  = 1'b0;        // destination domain, declared here for ack_sync

    assign src_ready = (req == ack_sync[1]);

    always @(posedge src_clk) begin
        ack_sync <= {ack_sync[0], ack};
        if (src_send && src_ready) begin
            hold <= src_data;
            req  <= ~req;
        end
    end

    // ---------------------------------------------------------------- destination domain
    (* ASYNC_REG = "TRUE" *) reg [1:0] req_sync = 2'b00;

    assign dst_valid = (req_sync[1] != ack);
    assign dst_data  = hold;

    always @(posedge dst_clk) begin
        req_sync <= {req_sync[0], req};
        if (dst_take && dst_valid)
            ack <= ~ack;
    end
endmodule
