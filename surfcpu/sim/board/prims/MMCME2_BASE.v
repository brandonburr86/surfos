/*
SurfCPU simulation model of AMD's MMCME2_BASE
--------------------
File: MMCME2_BASE.v Date: 10/15/26 (SurfCPU)
--------------------
Same name, parameters and ports as the 7-series primitive (UG953), so the board tops simulate
in Icarus and Verilator (--timing) unchanged. Simulation only: synthesis uses AMD's primitive,
and nothing under rtl/ or boards/ may read this file into a build.

At time 0 it checks its settings against UG472 v1.14 Table 3-7 and against DS181 v1.27.1
Table 37 for the Arty parts (the -1 and -1LI columns are equal): CLKIN 10..800 MHz,
PFD 10..450 MHz, VCO 600..1200 MHz, outputs 4.69..800 MHz, and $fatal on a violation. The
behaviour (lock, exact output periods) is surf_sim_cmt's. An output whose divide is left at 1
(the VCO frequency, never used on this board) is treated as unused and stays low; CLKOUTnB,
CLKFBOUTB, phase, duty cycle and CLKOUT4_CASCADE are not modelled (the B outputs stay low).
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module MMCME2_BASE #(
    parameter         BANDWIDTH          = "OPTIMIZED",
    parameter real    CLKFBOUT_MULT_F    = 5.000,
    parameter real    CLKFBOUT_PHASE     = 0.000,
    parameter real    CLKIN1_PERIOD      = 0.000,
    parameter real    CLKOUT0_DIVIDE_F   = 1.000,
    parameter integer CLKOUT1_DIVIDE     = 1,
    parameter integer CLKOUT2_DIVIDE     = 1,
    parameter integer CLKOUT3_DIVIDE     = 1,
    parameter integer CLKOUT4_DIVIDE     = 1,
    parameter integer CLKOUT5_DIVIDE     = 1,
    parameter integer CLKOUT6_DIVIDE     = 1,
    parameter real    CLKOUT0_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT1_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT2_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT3_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT4_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT5_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT6_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT0_PHASE      = 0.000,
    parameter real    CLKOUT1_PHASE      = 0.000,
    parameter real    CLKOUT2_PHASE      = 0.000,
    parameter real    CLKOUT3_PHASE      = 0.000,
    parameter real    CLKOUT4_PHASE      = 0.000,
    parameter real    CLKOUT5_PHASE      = 0.000,
    parameter real    CLKOUT6_PHASE      = 0.000,
    parameter         CLKOUT4_CASCADE    = "FALSE",
    parameter integer DIVCLK_DIVIDE      = 1,
    parameter real    REF_JITTER1        = 0.010,
    parameter         STARTUP_WAIT       = "FALSE"
) (
    output wire CLKOUT0,
    output wire CLKOUT0B,
    output wire CLKOUT1,
    output wire CLKOUT1B,
    output wire CLKOUT2,
    output wire CLKOUT2B,
    output wire CLKOUT3,
    output wire CLKOUT3B,
    output wire CLKOUT4,
    output wire CLKOUT5,
    output wire CLKOUT6,
    output wire CLKFBOUT,
    output wire CLKFBOUTB,
    output wire LOCKED,
    input  wire CLKIN1,
    input  wire PWRDWN,
    input  wire RST,
    input  wire CLKFBIN
);
    localparam real IN_PS = CLKIN1_PERIOD * 1000.0;
    localparam real VCO_PS = IN_PS * DIVCLK_DIVIDE / CLKFBOUT_MULT_F;   // VCO period

    function real half(input real divide);
        half = (divide == 1.0) ? 0.0 : VCO_PS * divide / 2.0;
    endfunction

    function eighths(input real v);            // v is a whole number of 1/8 steps
        eighths = (v * 8.0 - $floor(v * 8.0 + 0.5)) < 1.0e-9 && (v * 8.0 - $floor(v * 8.0 + 0.5)) > -1.0e-9;
    endfunction

    task check_out(input integer n, input real divide);
        real f;
        begin
            if (divide != 1.0) begin
                f = 1.0e6 / (VCO_PS * divide);
                if (f < 4.69 || f > 800.0)
                    $fatal(1, "MMCME2_BASE %m: CLKOUT%0d %f MHz outside 4.69..800 MHz (DS181 Table 37)", n, f);
            end
        end
    endtask

    initial begin : check
        real fin, fpfd, fvco;
        if (CLKIN1_PERIOD < 0.938 || CLKIN1_PERIOD > 100.0)
            $fatal(1, "MMCME2_BASE %m: CLKIN1_PERIOD %f ns outside 0.938..100 (UG472)", CLKIN1_PERIOD);
        if (DIVCLK_DIVIDE < 1 || DIVCLK_DIVIDE > 106)
            $fatal(1, "MMCME2_BASE %m: DIVCLK_DIVIDE %0d outside 1..106 (UG472)", DIVCLK_DIVIDE);
        if (CLKFBOUT_MULT_F < 2.0 || CLKFBOUT_MULT_F > 64.0 || !eighths(CLKFBOUT_MULT_F))
            $fatal(1, "MMCME2_BASE %m: CLKFBOUT_MULT_F %f not 2.000..64.000 in steps of 0.125 (UG472)", CLKFBOUT_MULT_F);
        if (CLKOUT0_DIVIDE_F < 1.0 || CLKOUT0_DIVIDE_F > 128.0 || !eighths(CLKOUT0_DIVIDE_F) ||
            (CLKOUT0_DIVIDE_F < 2.0 && CLKOUT0_DIVIDE_F != 1.0))
            $fatal(1, "MMCME2_BASE %m: CLKOUT0_DIVIDE_F %f not 1 or 2.000..128.000 in steps of 0.125 (UG472)", CLKOUT0_DIVIDE_F);
        if (CLKOUT1_DIVIDE < 1 || CLKOUT1_DIVIDE > 128 || CLKOUT2_DIVIDE < 1 || CLKOUT2_DIVIDE > 128 ||
            CLKOUT3_DIVIDE < 1 || CLKOUT3_DIVIDE > 128 || CLKOUT4_DIVIDE < 1 || CLKOUT4_DIVIDE > 128 ||
            CLKOUT5_DIVIDE < 1 || CLKOUT5_DIVIDE > 128 || CLKOUT6_DIVIDE < 1 || CLKOUT6_DIVIDE > 128)
            $fatal(1, "MMCME2_BASE %m: a CLKOUT1..6_DIVIDE is outside 1..128 (UG472)");
        fin  = 1000.0 / CLKIN1_PERIOD;
        fpfd = fin / DIVCLK_DIVIDE;
        fvco = fpfd * CLKFBOUT_MULT_F;
        if (fin < 10.0 || fin > 800.0)
            $fatal(1, "MMCME2_BASE %m: CLKIN1 %f MHz outside 10..800 MHz (DS181 Table 37)", fin);
        if (fpfd < 10.0 || fpfd > 450.0)
            $fatal(1, "MMCME2_BASE %m: PFD %f MHz outside 10..450 MHz (DS181 Table 37)", fpfd);
        if (fvco < 600.0 || fvco > 1200.0)
            $fatal(1, "MMCME2_BASE %m: VCO %f MHz outside 600..1200 MHz (DS181 Table 37, -1/-1LI)", fvco);
        check_out(0, CLKOUT0_DIVIDE_F);
        check_out(1, CLKOUT1_DIVIDE);
        check_out(2, CLKOUT2_DIVIDE);
        check_out(3, CLKOUT3_DIVIDE);
        check_out(4, CLKOUT4_DIVIDE);
        check_out(5, CLKOUT5_DIVIDE);
        check_out(6, CLKOUT6_DIVIDE);
    end

    wire [6:0] out;
    surf_sim_cmt #(
        .IN_PERIOD_PS(IN_PS), .FB_HALF_PS(IN_PS * DIVCLK_DIVIDE / 2.0),
        .HALF0_PS(half(CLKOUT0_DIVIDE_F)), .HALF1_PS(half(CLKOUT1_DIVIDE)),
        .HALF2_PS(half(CLKOUT2_DIVIDE)), .HALF3_PS(half(CLKOUT3_DIVIDE)),
        .HALF4_PS(half(CLKOUT4_DIVIDE)), .HALF5_PS(half(CLKOUT5_DIVIDE)),
        .HALF6_PS(half(CLKOUT6_DIVIDE))
    ) cmt (
        .clkin(CLKIN1), .rst(RST || PWRDWN), .clkfbin(CLKFBIN), .clkfbout(CLKFBOUT),
        .clkout(out), .locked(LOCKED)
    );

    assign {CLKOUT6, CLKOUT5, CLKOUT4, CLKOUT3, CLKOUT2, CLKOUT1, CLKOUT0} = out;
    assign {CLKOUT0B, CLKOUT1B, CLKOUT2B, CLKOUT3B, CLKFBOUTB} = 5'b0;
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
