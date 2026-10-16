/*
SurfCPU SurfBus demultiplexer
--------------------
File: surf_bus_demux.v Date: 10/15/26 (SurfCPU)
--------------------
One SurfBus master to N slaves (docs/SURFBUS.md). The decoder in front supplies sel, the
one-hot target of the request now offered; it depends only on the request fields, which the
master holds until acceptance (rule 2), so it is stable for the whole offer. Only that slave
sees req_valid, and the master sees that slave's req_ready. The response is taken from the
slave that accepted the master's request (pend), so the single outstanding request (rule 6)
is answered by the right slave whatever its latency, including a new request accepted in the
same cycle as the previous response. A response from a slave that owes none is ignored.
*/
module surf_bus_demux #(
    parameter integer N = 2
) (
    input  wire            clk,
    input  wire            rst,
    input  wire [N-1:0]    sel,
    // from the master
    input  wire            s_req_valid,
    output wire            s_req_ready,
    input  wire [31:0]     s_req_addr,
    input  wire            s_req_write,
    input  wire [3:0]      s_req_be,
    input  wire [31:0]     s_req_wdata,
    input  wire            s_req_fetch,
    output wire            s_rsp_valid,
    output wire [31:0]     s_rsp_rdata,
    output wire            s_rsp_err,
    // to the N slaves: the request fields are shared, req_valid is per slave
    output wire [N-1:0]    m_req_valid,
    input  wire [N-1:0]    m_req_ready,
    output wire [31:0]     m_req_addr,
    output wire            m_req_write,
    output wire [3:0]      m_req_be,
    output wire [31:0]     m_req_wdata,
    output wire            m_req_fetch,
    input  wire [N-1:0]    m_rsp_valid,
    input  wire [32*N-1:0] m_rsp_rdata,
    input  wire [N-1:0]    m_rsp_err
);
    reg [N-1:0] pend;   // one-hot: the slave that owes the response, 0 when none does

    assign m_req_valid = sel & {N{s_req_valid}};
    assign s_req_ready = |(sel & m_req_ready);
    assign m_req_addr  = s_req_addr;
    assign m_req_write = s_req_write;
    assign m_req_be    = s_req_be;
    assign m_req_wdata = s_req_wdata;
    assign m_req_fetch = s_req_fetch;

    wire [N-1:0] rsp_hit = m_rsp_valid & pend;
    reg  [31:0]  rdata_mux;
    integer i;
    always @* begin
        rdata_mux = 32'h0;
        for (i = 0; i < N; i = i + 1)
            if (pend[i]) rdata_mux = rdata_mux | m_rsp_rdata[32*i +: 32];
    end

    assign s_rsp_valid = |rsp_hit;
    assign s_rsp_err   = |(rsp_hit & m_rsp_err);
    assign s_rsp_rdata = rdata_mux;

    always @(posedge clk) begin
        if (rst)                             pend <= {N{1'b0}};
        else if (s_req_valid && s_req_ready) pend <= sel;
        else if (s_rsp_valid)                pend <= {N{1'b0}};
    end
endmodule
