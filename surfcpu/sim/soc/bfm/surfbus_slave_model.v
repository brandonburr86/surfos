/*
SurfCPU SurfBus test slave
--------------------
File: surfbus_slave_model.v Date: 10/15/26 (SurfCPU)
--------------------
Testbench-only SurfBus slave for interconnect tests. While idle it raises req_ready on a
random ready_pct percent of cycles (independent of req_valid, which the protocol allows);
an accepted request is answered after a random latency of lat_min..lat_max cycles (at least
1), during which req_ready stays low. Read data identify the slave and the request:
{ID, fetch, 3'b0, addr[23:0]} (rsp_data_for). Writes must carry wdata_for(addr, be) and the
slave counts a mismatch as an error, so a write that reaches the wrong slave, or arrives with
wrong fields, is caught even when two masters run at once. ALLOW_FETCH / ALLOW_WRITE say which
requests may reach this slave at all. inject_err makes every response an error; force_ready
(0 or 1) overrides the random ready. Counters: n_fetch, n_data (by req_fetch), n_write.
*/
`timescale 1ns/1ps
module surfbus_slave_model #(
    parameter         NAME        = "slave",
    parameter [3:0]   ID          = 4'h0,
    parameter         ALLOW_FETCH = 1,
    parameter         ALLOW_WRITE = 1
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        s_req_valid,
    output reg         s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output reg         s_rsp_valid,
    output reg  [31:0] s_rsp_rdata,
    output reg         s_rsp_err
);
    integer seed       = 1000 + ID;
    integer ready_pct  = 70;
    integer lat_min    = 1;
    integer lat_max    = 4;
    integer force_ready = -1;       // -1 random, 0 always low, 1 always high (while idle)
    reg     inject_err = 1'b0;
    integer errors     = 0;
    integer n_fetch    = 0, n_data = 0, n_write = 0;
    reg [31:0] last_addr;
    reg        last_fetch;

    function [31:0] wdata_for;
        input [31:0] addr;
        input [3:0]  be;
        begin
            wdata_for = {addr[15:0] ^ 16'hA5C3, 8'h5A, 4'h0, be};
        end
    endfunction

    function [31:0] rsp_data_for;
        input [3:0]  id;
        input [31:0] addr;
        input        fetch;
        begin
            rsp_data_for = {id, fetch, 3'b000, addr[23:0]};
        end
    endfunction

    reg        pend;
    integer    left;
    reg [31:0] pend_addr;
    reg        pend_fetch;
    integer    lat;

    initial begin
        s_req_ready = 1'b0;
        s_rsp_valid = 1'b0;
        s_rsp_rdata = 32'h0;
        s_rsp_err   = 1'b0;
        pend        = 1'b0;
        left        = 0;
    end

    function next_ready;
        input integer dummy;
        begin
            if (force_ready == 0)      next_ready = 1'b0;
            else if (force_ready == 1) next_ready = 1'b1;
            else                       next_ready = ({$random(seed)} % 100) < ready_pct;
        end
    endfunction

    always @(posedge clk) begin
        if (rst) begin
            s_req_ready <= 1'b0;
            s_rsp_valid <= 1'b0;
            pend        <= 1'b0;
        end else begin
            s_rsp_valid <= 1'b0;
            if (pend) begin
                if (left == 1) begin
                    s_rsp_valid <= 1'b1;
                    s_rsp_rdata <= rsp_data_for(ID, pend_addr, pend_fetch);
                    s_rsp_err   <= inject_err;
                    pend        <= 1'b0;
                    s_req_ready <= next_ready(0);
                end else begin
                    left <= left - 1;
                end
            end else if (s_req_valid && s_req_ready) begin
                // accepted at this edge
                last_addr  <= s_req_addr;
                last_fetch <= s_req_fetch;
                if (s_req_fetch) n_fetch = n_fetch + 1;
                else             n_data  = n_data + 1;
                if (s_req_write) n_write = n_write + 1;
                if (s_req_fetch && !ALLOW_FETCH) begin
                    $display("%0s: ERROR a fetch reached this slave (addr %h)", NAME, s_req_addr);
                    errors = errors + 1;
                end
                if (s_req_write && !ALLOW_WRITE) begin
                    $display("%0s: ERROR a write reached this slave (addr %h)", NAME, s_req_addr);
                    errors = errors + 1;
                end
                if (s_req_write && s_req_wdata !== wdata_for(s_req_addr, s_req_be)) begin
                    $display("%0s: ERROR write %h be %b data %h, expected %h", NAME, s_req_addr,
                             s_req_be, s_req_wdata, wdata_for(s_req_addr, s_req_be));
                    errors = errors + 1;
                end
                lat = lat_min + ({$random(seed)} % (lat_max - lat_min + 1));
                if (lat < 1) lat = 1;
                if (lat == 1) begin
                    s_rsp_valid <= 1'b1;
                    s_rsp_rdata <= rsp_data_for(ID, s_req_addr, s_req_fetch);
                    s_rsp_err   <= inject_err;
                    s_req_ready <= next_ready(0);
                end else begin
                    pend        <= 1'b1;
                    left        <= lat - 1;
                    pend_addr   <= s_req_addr;
                    pend_fetch  <= s_req_fetch;
                    s_req_ready <= 1'b0;
                end
            end else begin
                s_req_ready <= next_ready(0);
            end
        end
    end
endmodule
