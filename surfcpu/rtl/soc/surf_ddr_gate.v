/*
SurfCPU DDR calibration gate
--------------------
File: surf_ddr_gate.v Date: 10/15/26 (SurfCPU)
--------------------
Sits between the DDR arbiter and the SoC's DDR master port (docs/MODULES.md section 4): while
ddr_ok is 0 (no calibration yet, or calibration lost) it accepts every request itself and
answers it with an error in the next cycle, so nothing reaches the bridge before DDR is
usable. While ddr_ok is 1 requests and responses pass straight through. ddr_ok is looked at
when a request is first offered: a request already being offered to the bridge stays offered
until the bridge takes it (no withdrawal, docs/SURFBUS.md rule 2), whatever ddr_ok does
meanwhile; the bridge's watchdog bounds it. The arbiter in front sends one request at a time.
*/
module surf_ddr_gate (
    input  wire        clk,
    input  wire        rst,
    input  wire        ddr_ok,        // DDR usable (HAS_DDR and calibration done), clk domain
    // from the arbiter
    input  wire        s_req_valid,
    output wire        s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output wire        s_rsp_valid,
    output wire [31:0] s_rsp_rdata,
    output wire        s_rsp_err,
    // to the DDR bridge
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
    reg lock;    // a request was offered to the bridge last cycle and not accepted
    reg err_q;   // the request accepted last cycle gets an error now

    wire pass = ddr_ok || lock;

    assign m_req_valid = s_req_valid && pass;
    assign s_req_ready = pass ? m_req_ready : 1'b1;
    assign m_req_addr  = s_req_addr;
    assign m_req_write = s_req_write;
    assign m_req_be    = s_req_be;
    assign m_req_wdata = s_req_wdata;
    assign m_req_fetch = s_req_fetch;

    always @(posedge clk) begin
        if (rst) begin
            lock  <= 1'b0;
            err_q <= 1'b0;
        end else begin
            lock  <= s_req_valid && pass && !m_req_ready;
            err_q <= s_req_valid && !pass;
        end
    end

    assign s_rsp_valid = m_rsp_valid || err_q;
    assign s_rsp_err   = err_q || (m_rsp_valid && m_rsp_err);
    assign s_rsp_rdata = err_q ? 32'h0 : m_rsp_rdata;
endmodule
