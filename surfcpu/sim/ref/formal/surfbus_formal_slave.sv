/*
SurfCPU riscv-formal: a SurfBus slave with unconstrained answers
--------------------
File: surfbus_formal_slave.sv Date: 10/15/26 (SurfCPU)
--------------------
The memory for formal checks: it keeps every rule of docs/SURFBUS.md section 2 a slave must
keep (a response only for an accepted request, exactly one, in order, no earlier than the
cycle after acceptance; one request outstanding) and leaves everything else to the solver:
when it accepts, when it answers, the read data and the error flag. With
SURFCPU_FORMAL_FAIRNESS it accepts within 4 cycles and answers within 4 (for liveness).
It also checks the master side: a request is not withdrawn or changed before it is
accepted, and no new request is offered while one is outstanding.
*/
module surfbus_formal_slave (
    input  wire        clock,
    input  wire        reset,
    input  wire        req_valid,
    output wire        req_ready,
    input  wire [31:0] req_addr,
    input  wire        req_write,
    input  wire [3:0]  req_be,
    input  wire [31:0] req_wdata,
    input  wire        req_fetch,
    output wire        rsp_valid,
    output wire [31:0] rsp_rdata,
    output wire        rsp_err
);
    (* keep *) `rvformal_rand_reg        any_ready;
    (* keep *) `rvformal_rand_reg        any_rsp;
    (* keep *) `rvformal_rand_reg [31:0] any_rdata;
    (* keep *) `rvformal_rand_reg        any_err;

    reg pending = 0;
    assign req_ready = !reset && !pending && any_ready;
    assign rsp_valid = !reset && pending && any_rsp;
    assign rsp_rdata = any_rdata;
    assign rsp_err   = any_err;

    always @(posedge clock) begin
        if (reset)
            pending <= 0;
        else if (req_valid && req_ready)
            pending <= 1;
        else if (rsp_valid)
            pending <= 0;
    end

`ifdef SURFCPU_FORMAL_FAIRNESS
    reg [2:0] wait_req = 0, wait_rsp = 0;
    always @(posedge clock) begin
        wait_req <= (!reset && req_valid && !req_ready) ? wait_req + 1 : 0;
        wait_rsp <= (!reset && pending && !rsp_valid) ? wait_rsp + 1 : 0;
        if (wait_req == 4) assume (any_ready);
        if (wait_rsp == 4) assume (any_rsp);
    end
`endif

`ifdef SURFCPU_FORMAL_BUS_ASSERTS
    reg        held = 0;
    reg [31:0] held_addr, held_wdata;
    reg        held_write, held_fetch;
    reg [3:0]  held_be;
    always @(posedge clock) begin
        held <= !reset && req_valid && !req_ready;
        held_addr <= req_addr;
        held_write <= req_write;
        held_be <= req_be;
        held_wdata <= req_wdata;
        held_fetch <= req_fetch;
        if (!reset && held) begin
            assert (req_valid);                                     // no withdrawal
            assert (req_addr == held_addr && req_write == held_write &&
                    req_be == held_be && req_fetch == held_fetch);
            if (held_write) assert (req_wdata == held_wdata);
        end
        if (!reset && pending && !rsp_valid)
            assert (!req_valid);                                    // one outstanding
    end
`endif
endmodule
