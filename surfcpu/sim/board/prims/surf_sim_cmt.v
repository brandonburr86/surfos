/*
SurfCPU clock tile behaviour (simulation only)
--------------------
File: surf_sim_cmt.v Date: 10/15/26 (SurfCPU)
--------------------
The common behaviour of the MMCME2_BASE and PLLE2_BASE simulation models in this directory
(they check their parameters and instantiate this). Never read by synthesis: Vivado uses AMD's
own primitives. What is modelled is what the SurfCPU tops rely on:
 * clkin's period is measured on every rising edge and must be within 1 % of IN_PERIOD_PS;
 * clkfbout runs at the phase detector frequency whenever rst is low and clkin is good;
 * locked rises LOCK_CYCLES clkin cycles after rst is low, clkin is good and clkfbin has
   toggled (no feedback, no lock); it falls at once on rst, a wrong clkin period or a clkin
   that stops for more than 1.5 periods, and the outputs stop (low) while it is low;
 * each output toggles every HALFn_PS picoseconds from the moment of lock, scheduled from
   absolute times so the frequency is exact over any run length (each edge within 0.5 ps).
   HALFn_PS = 0 means the output is not used and stays low.
Not modelled: the outputs' phase against clkin (they start at lock), jitter, duty cycles
other than 50 %, phase shift. +cmt_lock_cycles=N overrides LOCK_CYCLES for every tile.
*/
`timescale 1ps/1ps
// A behavioural model: blocking assignments order the measurement and the lock decision
// inside one process on purpose, and two processes share the stopped flag.
/* verilator lint_off MULTIDRIVEN */
/* verilator lint_off BLKSEQ */
module surf_sim_cmt #(
    parameter real    IN_PERIOD_PS = 10000.0,
    parameter real    FB_HALF_PS   = 5000.0,
    parameter real    HALF0_PS     = 0.0,
    parameter real    HALF1_PS     = 0.0,
    parameter real    HALF2_PS     = 0.0,
    parameter real    HALF3_PS     = 0.0,
    parameter real    HALF4_PS     = 0.0,
    parameter real    HALF5_PS     = 0.0,
    parameter real    HALF6_PS     = 0.0,
    parameter integer LOCK_CYCLES  = 100
) (
    input  wire       clkin,
    input  wire       rst,
    input  wire       clkfbin,
    output reg        clkfbout = 1'b0,
    output reg  [6:0] clkout   = 7'b0,
    output wire       locked
);
    localparam [6:0] USED = {HALF6_PS > 0.0, HALF5_PS > 0.0, HALF4_PS > 0.0, HALF3_PS > 0.0,
                             HALF2_PS > 0.0, HALF1_PS > 0.0, HALF0_PS > 0.0};

    function real half_of(input integer i);
        case (i)
            0: half_of = HALF0_PS;
            1: half_of = HALF1_PS;
            2: half_of = HALF2_PS;
            3: half_of = HALF3_PS;
            4: half_of = HALF4_PS;
            5: half_of = HALF5_PS;
            default: half_of = HALF6_PS;
        endcase
    endfunction

    integer lock_cycles = LOCK_CYCLES;
    initial begin
        if ($value$plusargs("cmt_lock_cycles=%d", lock_cycles)) begin end
    end

    // ---------------------------------------------------------------- input clock and lock
    realtime last_edge = 0.0;
    reg      have_edge = 1'b0;
    reg      period_ok = 1'b0;   // the last clkin period was right
    reg      stopped   = 1'b1;   // no clkin edge for 1.5 periods (or none yet)
    integer  fb_edges  = 0;      // clkfbin rising edges since rst (saturates)
    integer  cycles    = 0;
    reg      locked_q  = 1'b0;

    always @(posedge clkin or posedge rst) begin : tick
        realtime p;
        if (rst) begin                           // (a clkin edge during rst is not measured)
            cycles    = 0;
            locked_q  = 1'b0;
            have_edge = 1'b0;
        end else begin
            p         = $realtime - last_edge;
            period_ok = have_edge && (p > 0.99 * IN_PERIOD_PS) && (p < 1.01 * IN_PERIOD_PS);
            have_edge = 1'b1;
            last_edge = $realtime;
            stopped   = 1'b0;
            if (!period_ok) begin
                cycles   = 0;
                locked_q = 1'b0;
            end else if (fb_edges >= 4) begin
                if (cycles < lock_cycles) cycles = cycles + 1;
                else                      locked_q = 1'b1;
            end
        end
    end

    always begin : watchdog
        #(IN_PERIOD_PS / 2.0);
        if (have_edge && $realtime - last_edge > 1.5 * IN_PERIOD_PS) stopped = 1'b1;
    end

    always @(posedge clkfbin or posedge rst) begin
        if (rst)               fb_edges = 0;
        else if (fb_edges < 8) fb_edges = fb_edges + 1;
    end

    wire running = !rst && period_ok && !stopped;
    assign locked = locked_q && running;

    // ---------------------------------------------------------------- outputs
    always begin : feedback
        realtime t0;
        integer  n;
        wait (running === 1'b1);
        t0 = $realtime;
        n  = 0;
        while (running === 1'b1) begin
            n = n + 1;
            #(t0 + n * FB_HALF_PS - $realtime);
            if (running === 1'b1) clkfbout = ~clkfbout;
        end
        clkfbout = 1'b0;
    end

    genvar g;
    generate
        for (g = 0; g < 7; g = g + 1) begin : out
            if (USED[g]) begin : used
                always begin : run
                    realtime t0;
                    integer  n;
                    wait (locked === 1'b1);
                    t0 = $realtime;
                    n  = 0;
                    while (locked === 1'b1) begin
                        n = n + 1;
                        #(t0 + n * half_of(g) - $realtime);
                        if (locked === 1'b1) clkout[g] = ~clkout[g];
                    end
                    clkout[g] = 1'b0;
                end
            end
        end
    endgenerate
endmodule
/* verilator lint_on BLKSEQ */
/* verilator lint_on MULTIDRIVEN */
