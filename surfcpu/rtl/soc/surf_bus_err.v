/*
SurfCPU SurfBus error slave
--------------------
File: surf_bus_err.v Date: 10/15/26 (SurfCPU)
--------------------
Answers every request with an error in the next cycle (docs/SURFBUS.md section 3): the
target of the decoders for an unmapped address, a fetch from a region without fetch, or a
write to a region without write. It has no state but the pending response, so an error
access has no effect anywhere.
*/
module surf_bus_err (
    input  wire        clk,
    input  wire        rst,
    input  wire        s_req_valid,
    output wire        s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output wire        s_rsp_valid,
    output wire [31:0] s_rsp_rdata,
    output wire        s_rsp_err
);
    reg rsp_q;

    always @(posedge clk) begin
        if (rst) rsp_q <= 1'b0;
        else     rsp_q <= s_req_valid;
    end

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = rsp_q;
    assign s_rsp_rdata = 32'h0;
    assign s_rsp_err   = rsp_q;

    // the request fields do not matter: every request fails
    wire unused_ok = &{1'b0, s_req_addr, s_req_write, s_req_be, s_req_wdata, s_req_fetch};
endmodule
