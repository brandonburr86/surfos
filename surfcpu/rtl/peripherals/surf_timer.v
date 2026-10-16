/*
SurfCPU machine timer
--------------------
File: surf_timer.v Date: 10/15/26 (SurfCPU)
--------------------
timer at 0x1000_1000 (docs/MEMORY-MAP.md): the 64-bit mtime counting at TIMEBASE_HZ from
reset, the 64-bit mtimecmp (reset all ones), MSIP and TIMEBASE. irq_timer (mip.MTIP) is
mtime >= mtimecmp, unsigned 64-bit, and irq_software (mip.MSIP) is MSIP bit 0; both are
straight from the registers, so a write that clears them has taken effect by the time its
response arrives. mtime also goes to the core for the time/timeh CSRs.
The rate comes from a fractional accumulator: each sys_clk cycle adds TIMEBASE_HZ, and a tick
is due whenever the sum reaches SYS_HZ, so mtime advances exactly TIMEBASE_HZ times per
SYS_HZ cycles for any SYS_HZ >= TIMEBASE_HZ (every 25 cycles at 25 MHz and 1 MHz).
Software reads 64-bit values hi/lo/hi and writes mtimecmp lo = all ones, hi, lo; the
hardware simply has a 32-bit register per half. A write to MTIME_LO or MTIME_HI replaces
that half and keeps the other; a tick that falls in the same cycle as such a write is lost
(the same rule as mcycle in docs/ISA.md). A SurfBus slave (docs/SURFBUS.md) that accepts
every request and answers in the next cycle; an unknown offset, a non-word access or a fetch
gets an error and has no effect; a write to TIMEBASE is ignored.
*/
module surf_timer #(
    parameter integer SYS_HZ      = 25000000,
    parameter integer TIMEBASE_HZ = 1000000
) (
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
    output wire [63:0] mtime,
    output wire        irq_timer,
    output wire        irq_software
);
    `include "surfcpu_soc.vh"

    localparam [31:0] TB_INC   = TIMEBASE_HZ;
    localparam [31:0] TB_LIMIT = SYS_HZ - TIMEBASE_HZ;   // a tick is due once acc + TB_INC >= SYS_HZ

    // ---------------------------------------------------------------- the time base
    reg  [31:0] acc_q;
    wire        tick = (acc_q >= TB_LIMIT);

    reg [63:0] mtime_q;
    reg [63:0] mtimecmp_q;
    reg        msip_q;

    // ---------------------------------------------------------------- SurfBus slave
    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    reg        known;
    reg [31:0] rd_mux;
    always @* begin
        known  = 1'b1;
        rd_mux = 32'h0;
        case (woff)
            SURF_TIMER_MTIME_LO[11:2]:    rd_mux = mtime_q[31:0];
            SURF_TIMER_MTIME_HI[11:2]:    rd_mux = mtime_q[63:32];
            SURF_TIMER_MTIMECMP_LO[11:2]: rd_mux = mtimecmp_q[31:0];
            SURF_TIMER_MTIMECMP_HI[11:2]: rd_mux = mtimecmp_q[63:32];
            SURF_TIMER_MSIP[11:2]:        rd_mux[SURF_TIMER_MSIP_MSIP_LSB] = msip_q;
            SURF_TIMER_TIMEBASE[11:2]:    rd_mux = TB_INC;
            default:                      known  = 1'b0;
        endcase
    end

    wire ok      = s_req_valid && word_acc && known;
    wire do_wr   = ok && s_req_write;
    wire wr_tlo  = do_wr && (woff == SURF_TIMER_MTIME_LO[11:2]);
    wire wr_thi  = do_wr && (woff == SURF_TIMER_MTIME_HI[11:2]);
    wire wr_clo  = do_wr && (woff == SURF_TIMER_MTIMECMP_LO[11:2]);
    wire wr_chi  = do_wr && (woff == SURF_TIMER_MTIMECMP_HI[11:2]);
    wire wr_msip = do_wr && (woff == SURF_TIMER_MSIP[11:2]);

    reg        rsp_valid_q, rsp_err_q;
    reg [31:0] rsp_rdata_q;
    always @(posedge clk) begin
        if (rst) begin
            acc_q       <= 32'h0;
            mtime_q     <= 64'h0;
            mtimecmp_q  <= 64'hFFFF_FFFF_FFFF_FFFF;
            msip_q      <= 1'b0;
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'h0;
        end else begin
            acc_q <= tick ? acc_q - TB_LIMIT : acc_q + TB_INC;

            if (wr_tlo)      mtime_q[31:0]  <= s_req_wdata;
            else if (wr_thi) mtime_q[63:32] <= s_req_wdata;
            else if (tick)   mtime_q        <= mtime_q + 64'd1;

            if (wr_clo)  mtimecmp_q[31:0]  <= s_req_wdata;
            if (wr_chi)  mtimecmp_q[63:32] <= s_req_wdata;
            if (wr_msip) msip_q            <= s_req_wdata[SURF_TIMER_MSIP_MSIP_LSB];

            rsp_valid_q <= s_req_valid;
            rsp_err_q   <= s_req_valid && !(word_acc && known);
            rsp_rdata_q <= (ok && !s_req_write) ? rd_mux : 32'h0;
        end
    end

    assign s_req_ready  = 1'b1;
    assign s_rsp_valid  = rsp_valid_q;
    assign s_rsp_err    = rsp_err_q;
    assign s_rsp_rdata  = rsp_rdata_q;
    assign mtime        = mtime_q;
    assign irq_timer    = (mtime_q >= mtimecmp_q);
    assign irq_software = msip_q;

    // the decoder has already matched the window (address bits 31:12)
    wire unused_ok = &{1'b0, s_req_addr[31:12]};
endmodule
