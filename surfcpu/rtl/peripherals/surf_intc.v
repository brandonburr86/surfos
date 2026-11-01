/*
SurfCPU interrupt controller
--------------------
File: surf_intc.v Date: 10/15/26 (SurfCPU)
--------------------
intc at 0x1000_2000 (docs/MEMORY-MAP.md): 32 level sources, a per-source enable, and
irq_external (mip.MEIP) = OR of (sources & ENABLE). There is no claim/complete state: a
source stays pending until software clears its cause in the peripheral. Registers: PENDING
(raw levels), ENABLE (rw, reset 0), ACTIVE (PENDING & ENABLE), NEXT (lowest active source,
or all ones). A SurfBus slave (docs/SURFBUS.md) that accepts every request and answers in
the next cycle; an unknown offset, a non-word access or a fetch gets an error and has no
effect. Writes to the read-only registers are ignored. irq_external follows a write to
ENABLE by the time the write's response arrives.
*/
module surf_intc (
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
    output wire        s_rsp_err,
    input  wire [31:0] sources,
    output wire        irq_external
);
    `include "surfcpu_soc.vh"

    reg  [31:0] enable_q;
    wire [31:0] active = sources & enable_q;

    // NEXT: the number of the lowest active source, or all ones when none is active
    reg [31:0] next_src;
    integer i;
    always @* begin
        next_src = 32'hFFFF_FFFF;
        for (i = 31; i >= 0; i = i - 1)
            if (active[i]) next_src = i;
    end

    // ---------------------------------------------------------------- SurfBus slave
    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    reg        known;
    reg [31:0] rd_mux;
    always @* begin
        known  = 1'b1;
        rd_mux = 32'h0;
        case (woff)
            SURF_INTC_PENDING[11:2]: rd_mux = sources;
            SURF_INTC_ENABLE[11:2]:  rd_mux = enable_q;
            SURF_INTC_ACTIVE[11:2]:  rd_mux = active;
            SURF_INTC_NEXT[11:2]:    rd_mux = next_src;
            default:                 known  = 1'b0;
        endcase
    end

    wire ok    = s_req_valid && word_acc && known;
    wire do_wr = ok && s_req_write;

    reg        rsp_valid_q, rsp_err_q;
    reg [31:0] rsp_rdata_q;
    always @(posedge clk) begin
        if (rst) begin
            enable_q    <= 32'h0;
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'h0;
        end else begin
            rsp_valid_q <= s_req_valid;
            rsp_err_q   <= s_req_valid && !(word_acc && known);
            rsp_rdata_q <= (ok && !s_req_write) ? rd_mux : 32'h0;
            if (do_wr && woff == SURF_INTC_ENABLE[11:2]) enable_q <= s_req_wdata;
        end
    end

    assign s_req_ready  = 1'b1;
    assign s_rsp_valid  = rsp_valid_q;
    assign s_rsp_err    = rsp_err_q;
    assign s_rsp_rdata  = rsp_rdata_q;
    assign irq_external = |active;

    // the decoder has already matched the window (address bits 31:12)
    wire unused_ok = &{1'b0, s_req_addr[31:12]};
endmodule
