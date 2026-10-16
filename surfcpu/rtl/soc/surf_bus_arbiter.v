/*
SurfCPU SurfBus arbiter
--------------------
File: surf_bus_arbiter.v Date: 10/15/26 (SurfCPU)
--------------------
The DDR arbiter of docs/SURFBUS.md section 6: two masters (a = the fetch side, b = the data
side) share one slave, one request at a time, data first. When the slave is free, b's
request wins over a's. A request offered to the slave but not yet accepted stays the
offered one until it is accepted, even if the other side asks meanwhile (the slave must
never see a request withdrawn, rule 2). After acceptance no new request is offered until
the response has come back; the response goes to the side that issued the request. Each
side keeps its own single outstanding request (rule 6), so a side waits while the other is
being served.
*/
module surf_bus_arbiter (
    input  wire        clk,
    input  wire        rst,
    // side a: the fetch side (served when b does not ask)
    input  wire        a_req_valid,
    output wire        a_req_ready,
    input  wire [31:0] a_req_addr,
    input  wire        a_req_write,
    input  wire [3:0]  a_req_be,
    input  wire [31:0] a_req_wdata,
    input  wire        a_req_fetch,
    output wire        a_rsp_valid,
    output wire [31:0] a_rsp_rdata,
    output wire        a_rsp_err,
    // side b: the data side (first)
    input  wire        b_req_valid,
    output wire        b_req_ready,
    input  wire [31:0] b_req_addr,
    input  wire        b_req_write,
    input  wire [3:0]  b_req_be,
    input  wire [31:0] b_req_wdata,
    input  wire        b_req_fetch,
    output wire        b_rsp_valid,
    output wire [31:0] b_rsp_rdata,
    output wire        b_rsp_err,
    // to the shared slave
    output wire        m_req_valid,
    input  wire        m_req_ready,
    output wire [31:0] m_req_addr,
    output wire        m_req_write,
    output wire [3:0]  m_req_be,
    output wire [31:0] m_req_wdata,
    output wire        m_req_fetch,
    input  wire        m_rsp_valid,
    input  wire [31:0] m_rsp_rdata,
    input  wire        m_rsp_err
);
    reg busy;     // a request was accepted by the slave; its response is due
    reg owner;    // who issued it: 1 = b, 0 = a
    reg lock;     // a request was offered last cycle and not accepted: keep offering it
    reg lock_b;   // ... and it was b's

    wire gnt_b = !busy && (lock ? lock_b  : b_req_valid);
    wire gnt_a = !busy && (lock ? !lock_b : (!b_req_valid && a_req_valid));

    assign m_req_valid = (gnt_b && b_req_valid) || (gnt_a && a_req_valid);
    assign m_req_addr  = gnt_b ? b_req_addr  : a_req_addr;
    assign m_req_write = gnt_b ? b_req_write : a_req_write;
    assign m_req_be    = gnt_b ? b_req_be    : a_req_be;
    assign m_req_wdata = gnt_b ? b_req_wdata : a_req_wdata;
    assign m_req_fetch = gnt_b ? b_req_fetch : a_req_fetch;

    assign b_req_ready = gnt_b && m_req_ready;
    assign a_req_ready = gnt_a && m_req_ready;

    always @(posedge clk) begin
        if (rst) begin
            busy   <= 1'b0;
            owner  <= 1'b0;
            lock   <= 1'b0;
            lock_b <= 1'b0;
        end else if (m_req_valid && m_req_ready) begin
            busy  <= 1'b1;
            owner <= gnt_b;
            lock  <= 1'b0;
        end else begin
            if (busy && m_rsp_valid) busy <= 1'b0;
            lock   <= m_req_valid;
            lock_b <= gnt_b;
        end
    end

    // responses go back to the side that issued the request; a stray one is dropped
    wire rsp = busy && m_rsp_valid;
    assign a_rsp_valid = rsp && !owner;
    assign b_rsp_valid = rsp && owner;
    assign a_rsp_rdata = m_rsp_rdata;
    assign b_rsp_rdata = m_rsp_rdata;
    assign a_rsp_err   = m_rsp_err;
    assign b_rsp_err   = m_rsp_err;
endmodule
