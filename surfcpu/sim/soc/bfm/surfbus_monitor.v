/*
SurfCPU SurfBus protocol monitor
--------------------
File: surfbus_monitor.v Date: 10/15/26 (SurfCPU)
--------------------
Testbench-only checker for one SurfBus link (docs/SURFBUS.md section 2), sampled at every
rising edge out of reset: no X on req_valid and rsp_valid (nor on req_ready and rsp_err while
they matter); a request offered and not accepted
is offered again, unchanged, in the next cycle (rule 2); every response answers an earlier
accepted request, never one accepted at the same edge (rule 4); a master offers no new
request while one is outstanding, except in the cycle its response arrives (rule 6); every
request is answered within MAX_LAT cycles (rule 7). Prints each violation and counts it in
`errors`; also counts accepted requests, responses, error responses and stall cycles.
*/
`timescale 1ns/1ps
module surfbus_monitor #(
    parameter         NAME    = "mon",
    parameter integer MAX_LAT = 10000
) (
    input wire        clk,
    input wire        rst,
    input wire        req_valid,
    input wire        req_ready,
    input wire [31:0] req_addr,
    input wire        req_write,
    input wire [3:0]  req_be,
    input wire [31:0] req_wdata,
    input wire        req_fetch,
    input wire        rsp_valid,
    input wire [31:0] rsp_rdata,
    input wire        rsp_err
);
    integer errors        = 0;
    integer accepts       = 0;
    integer responses     = 0;
    integer err_responses = 0;
    integer stall_cycles  = 0;
    integer outstanding   = 0;
    integer age           = 0;
    integer max_age       = 0;
    reg        held = 1'b0;
    reg [69:0] held_req;

    wire [69:0] req_now = {req_addr, req_write, req_be, req_wdata, req_fetch};

    task violation;
        input [8*64-1:0] what;
        begin
            $display("%0s: PROTOCOL ERROR at %0t: %0s", NAME, $time, what);
            errors = errors + 1;
        end
    endtask

    always @(posedge clk) begin
        if (rst !== 1'b0) begin
            outstanding = 0;
            held        = 1'b0;
            age         = 0;
        end else begin
            if (req_valid === 1'bx || (req_valid === 1'b1 && req_ready === 1'bx) ||
                rsp_valid === 1'bx || (rsp_valid === 1'b1 && rsp_err === 1'bx))
                violation("X on a handshake signal");
            if (held && req_valid !== 1'b1)
                violation("request withdrawn before acceptance");
            else if (held && req_now !== held_req)
                violation("request changed before acceptance");
            // age = rising edges since the outstanding request was accepted
            if (outstanding > 0) age = age + 1;
            if (rsp_valid === 1'b1) begin
                responses = responses + 1;
                if (rsp_err === 1'b1) err_responses = err_responses + 1;
                if (outstanding == 0) begin
                    violation("response without an outstanding request");
                end else begin
                    outstanding = outstanding - 1;
                    if (age > max_age) max_age = age;
                end
            end else if (outstanding > 0 && age == MAX_LAT) begin
                violation("no response within MAX_LAT cycles");
            end
            if (req_valid === 1'b1 && outstanding > 0)
                violation("new request offered while one is outstanding");
            if (req_valid === 1'b1 && req_ready === 1'b1) begin
                accepts     = accepts + 1;
                outstanding = outstanding + 1;
                age         = 0;
            end
            if (req_valid === 1'b1 && req_ready !== 1'b1) stall_cycles = stall_cycles + 1;
            held     = (req_valid === 1'b1) && (req_ready !== 1'b1);
            held_req = req_now;
        end
    end

    // the data fields are not checked here
    wire unused_ok = &{1'b0, rsp_rdata};
endmodule
