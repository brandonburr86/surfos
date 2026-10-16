/*
SurfCPU instruction fetch
--------------------
File: surfcpu_fetch.v Date: 10/15/26 (SurfCPU)
--------------------
The IF stage of surfcpu_core: a SurfBus master on the instruction port (docs/SURFBUS.md)
that fetches sequentially from pc and keeps up to four fetched words, oldest first, for
the ID stage (`id_*`, removed by `id_pop`). A fetch that gets an error response is kept
as an entry with `id_fault` set (insn reads 0), so the fault is raised in order, by the
instruction that would have been there.

Bus rules kept here:
  * at most one request outstanding: a new request is offered only when none is waiting
    for its response, or in the cycle that response arrives;
  * no withdrawal: a request offered and not accepted is offered again unchanged in the
    next cycle (hold_*), whatever happens in the pipeline;
  * a request is offered only when the buffer has room for its response next to every
    response it already expects, so a response is always taken in the cycle it arrives.
req_valid depends only on registers and on rsp_valid, never on req_ready or the pipeline.

`redirect` (a taken branch or jump from EX, or a trap, MRET or FENCE.I from MEM) empties
the buffer and makes redirect_pc the next address. Requests already offered or accepted
cannot be withdrawn: they complete, and their responses are dropped (hold_drop, out_drop).
*/
module surfcpu_fetch #(
    parameter [31:0] RESET_PC = 32'h0000_0000
) (
    input  wire        clk,
    input  wire        rst,
    // change of flow: drop everything fetched or in flight, continue at redirect_pc
    input  wire        redirect,
    input  wire [31:0] redirect_pc,
    // the oldest fetched instruction, for ID
    output wire        id_valid,
    output wire [31:0] id_pc,
    output wire [31:0] id_insn,
    output wire        id_fault,       // its fetch got an error response (instruction access fault)
    input  wire        id_pop,         // ID takes it this cycle
    // SurfBus instruction port (write, be, wdata and fetch are constant in surfcpu_core)
    output wire        ibus_req_valid,
    input  wire        ibus_req_ready,
    output wire [31:0] ibus_req_addr,
    input  wire        ibus_rsp_valid,
    input  wire [31:0] ibus_rsp_rdata,
    input  wire        ibus_rsp_err
);
    reg [31:0] pc_q;           // address of the next new request
    reg        hold_q;         // the request offered last cycle was not accepted: offer it again
    reg [31:0] hold_addr_q;
    reg        hold_drop_q;    // ... and drop its response (a redirect came after it was offered)
    reg        out_q;          // an accepted request is waiting for its response
    reg [31:2] out_addr_q;
    reg        out_drop_q;     // ... and that response is to be dropped

    wire [2:0] count;          // words in the buffer (0..4)
    wire       empty;
    wire       full;

    // A response for the current path arrives now (dropped ones never enter the buffer).
    wire rsp_keep = ibus_rsp_valid && !out_drop_q;
    // Room for one more response besides the buffered words and the one arriving now.
    wire room     = ({1'b0, count} + {3'b000, rsp_keep}) <= 4'd3;
    // One outstanding request: the previous response must have arrived, or arrive now.
    wire slot     = !out_q || ibus_rsp_valid;

    assign ibus_req_valid = !rst && (hold_q || (slot && room));
    assign ibus_req_addr  = hold_q ? hold_addr_q : pc_q;

    wire accept   = ibus_req_valid && ibus_req_ready;
    wire stall    = ibus_req_valid && !ibus_req_ready;
    // The request offered now belongs to a path that a redirect abandons.
    wire new_drop = (hold_q && hold_drop_q) || redirect;

    always @(posedge clk) begin
        if (rst) begin
            pc_q        <= RESET_PC;
            hold_q      <= 1'b0;
            hold_drop_q <= 1'b0;
            out_q       <= 1'b0;
            out_drop_q  <= 1'b0;
        end else begin
            // the request in flight
            if (accept) begin
                out_q      <= 1'b1;
                out_addr_q <= ibus_req_addr[31:2];
                out_drop_q <= new_drop;
            end else begin
                if (ibus_rsp_valid) out_q <= 1'b0;
                if (redirect)       out_drop_q <= 1'b1;
            end
            // the request offered and not accepted
            hold_q <= stall;
            if (stall) begin
                hold_addr_q <= ibus_req_addr;
                hold_drop_q <= new_drop;
            end
            // the next new address
            if (redirect)
                pc_q <= redirect_pc;
            else if (ibus_req_valid && !hold_q)
                pc_q <= pc_q + 32'd4;
        end
    end

    // The buffer of fetched words: {pc[31:2], fault, insn}. A redirect empties it, and the
    // response arriving in that cycle (if any) is dropped with it.
    wire [62:0] buf_out;
    surf_fifo #(.WIDTH(63), .DEPTH_LOG2(2)) fifo (
        .clk(clk),
        .rst(rst || redirect),
        .wr_en(rsp_keep),
        .wr_data({out_addr_q, ibus_rsp_err, ibus_rsp_err ? 32'd0 : ibus_rsp_rdata}),
        .rd_en(id_pop),
        .rd_data(buf_out),
        .empty(empty),
        .full(full),
        .count(count)
    );

    assign id_valid = !empty;
    assign id_pc    = {buf_out[62:33], 2'b00};
    assign id_fault = buf_out[32];
    assign id_insn  = buf_out[31:0];

`ifndef SYNTHESIS
    // The room rule means a kept response always finds space.
    always @(posedge clk) begin
        if (!rst && !redirect && rsp_keep && full)
            $display("surfcpu_fetch: ERROR: response arrived with the buffer full");
        if (!rst && ibus_rsp_valid && !out_q)
            $display("surfcpu_fetch: ERROR: response without an outstanding request");
    end
`endif
endmodule
