/*
SurfCPU SurfBus master bus-functional model
--------------------
File: surfbus_bfm.v Date: 10/15/26 (SurfCPU)
--------------------
Testbench-only SurfBus master (docs/SURFBUS.md) driven by tasks. It drives on falling edges
and samples on rising edges, and keeps every master rule: a request stays offered, unchanged,
until accepted; one request outstanding; the next request may be offered in the same cycle as
the response (when the random gap is 0). Before each request it idles 0..max_gap cycles (no
gap half of the time) from its own seed, so slaves see random spacing and back-to-back
traffic. After acceptance it puts junk on the request fields, to catch a slave that looks at
them too late. A request not accepted, or not answered, within `timeout` cycles is fatal.
Results of the last transfer are in rdata, err, lat (cycles from acceptance to response) and
wait_acc (cycles the request waited for req_ready). Check failures count in `errors`.
*/
`timescale 1ns/1ps
module surfbus_bfm #(
    parameter NAME  = "bfm",
    parameter FETCH = 0              // 1: an instruction port (req_fetch = 1 by default)
) (
    input  wire        clk,
    output reg         m_req_valid,
    input  wire        m_req_ready,
    output reg  [31:0] m_req_addr,
    output reg         m_req_write,
    output reg  [3:0]  m_req_be,
    output reg  [31:0] m_req_wdata,
    output reg         m_req_fetch,
    input  wire        m_rsp_valid,
    input  wire [31:0] m_rsp_rdata,
    input  wire        m_rsp_err
);
    integer seed       = 1;
    integer max_gap    = 2;
    integer timeout    = 5000;
    integer errors     = 0;
    integer n_xfer     = 0;
    integer n_errrsp   = 0;
    integer n_b2b      = 0;          // requests offered in the same cycle as the previous response
    reg     fetch_flag = (FETCH != 0);
    reg [31:0] rdata;
    reg        err;
    integer    lat;
    integer    wait_acc;
    reg        just_responded = 1'b0;

    initial begin
        m_req_valid = 1'b0;
        m_req_addr  = 32'h0;
        m_req_write = 1'b0;
        m_req_be    = 4'h0;
        m_req_wdata = 32'h0;
        m_req_fetch = (FETCH != 0);
    end

    // one transfer; call it at a falling edge of clk, it returns at the falling edge in which
    // the response was seen (so a following call with no gap offers in the response cycle)
    task xfer;
        input        wr;
        input [31:0] addr;
        input [3:0]  be;
        input [31:0] wdata;
        integer n;
        begin
            if (max_gap > 0 && ($random(seed) & 1)) begin
                n = 1 + ({$random(seed)} % max_gap);
                repeat (n) @(negedge clk);
                just_responded = 1'b0;
            end
            if (just_responded) n_b2b = n_b2b + 1;
            m_req_valid = 1'b1;
            m_req_addr  = addr;
            m_req_write = wr;
            m_req_be    = be;
            m_req_wdata = wdata;
            m_req_fetch = fetch_flag;
            wait_acc    = 0;
            @(posedge clk);
            while (m_req_ready !== 1'b1) begin
                wait_acc = wait_acc + 1;
                if (wait_acc > timeout) $fatal(1, "%0s: request %h never accepted", NAME, addr);
                @(posedge clk);
            end
            n_xfer = n_xfer + 1;
            @(negedge clk);
            m_req_valid = 1'b0;
            m_req_addr  = $random(seed);
            m_req_write = $random(seed);
            m_req_be    = $random(seed);
            m_req_wdata = $random(seed);
            m_req_fetch = $random(seed);
            lat = 1;
            while (m_rsp_valid !== 1'b1) begin
                @(negedge clk);
                lat = lat + 1;
                if (lat > timeout) $fatal(1, "%0s: no response to %h", NAME, addr);
            end
            rdata = m_rsp_rdata;
            err   = m_rsp_err;
            if (err) n_errrsp = n_errrsp + 1;
            just_responded = 1'b1;
        end
    endtask

    task idle;
        input integer n;
        begin
            repeat (n) @(negedge clk);
            just_responded = 1'b0;
        end
    endtask

    task read;                  // no check: the result is in rdata / err
        input [31:0] addr;
        input [3:0]  be;
        begin
            xfer(1'b0, addr, be, 32'h0);
        end
    endtask

    task read_check;            // expects an OK response with (rdata & mask) == (exp & mask)
        input [31:0] addr;
        input [3:0]  be;
        input [31:0] exp;
        input [31:0] mask;
        begin
            xfer(1'b0, addr, be, 32'h0);
            if (err !== 1'b0) begin
                $display("%0s: ERROR read %h be %b: error response", NAME, addr, be);
                errors = errors + 1;
            end else if ((rdata & mask) !== (exp & mask)) begin
                $display("%0s: ERROR read %h be %b: got %h, expected %h (mask %h)", NAME, addr, be,
                         rdata, exp, mask);
                errors = errors + 1;
            end
        end
    endtask

    task read_err;              // expects an error response
        input [31:0] addr;
        input [3:0]  be;
        begin
            xfer(1'b0, addr, be, 32'h0);
            if (err !== 1'b1) begin
                $display("%0s: ERROR read %h be %b: expected an error, got data %h", NAME, addr, be,
                         rdata);
                errors = errors + 1;
            end
        end
    endtask

    task write_ok;              // expects an OK response
        input [31:0] addr;
        input [3:0]  be;
        input [31:0] data;
        begin
            xfer(1'b1, addr, be, data);
            if (err !== 1'b0) begin
                $display("%0s: ERROR write %h be %b data %h: error response", NAME, addr, be, data);
                errors = errors + 1;
            end
        end
    endtask

    task write_err;             // expects an error response
        input [31:0] addr;
        input [3:0]  be;
        input [31:0] data;
        begin
            xfer(1'b1, addr, be, data);
            if (err !== 1'b1) begin
                $display("%0s: ERROR write %h be %b data %h: expected an error", NAME, addr, be, data);
                errors = errors + 1;
            end
        end
    endtask
endmodule
