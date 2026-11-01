/*
SurfCPU simulation model of AMD's PLLE2_BASE
--------------------
File: PLLE2_BASE.v Date: 10/15/26 (SurfCPU)
--------------------
Same name, parameters and ports as the 7-series primitive (UG953), so the board tops simulate
in Icarus and Verilator (--timing) unchanged. Simulation only: synthesis uses AMD's primitive,
and nothing under rtl/ or boards/ may read this file into a build.

At time 0 it checks its settings against UG472 v1.14 Table 3-8 and against DS181 v1.27.1
Table 38 for the Arty parts (the -1 and -1LI columns are equal): CLKIN 19..800 MHz,
PFD 19..450 MHz, VCO 800..1600 MHz, outputs 6.25..800 MHz, and $fatal on a violation. The
behaviour (lock, exact output periods) is surf_sim_cmt's. An output whose divide is left at 1
(the VCO frequency, never used on this board) is treated as unused and stays low; phase and
duty cycle are not modelled.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module PLLE2_BASE #(
    parameter         BANDWIDTH          = "OPTIMIZED",
    parameter integer CLKFBOUT_MULT      = 5,
    parameter real    CLKFBOUT_PHASE     = 0.000,
    parameter real    CLKIN1_PERIOD      = 0.000,
    parameter integer CLKOUT0_DIVIDE     = 1,
    parameter integer CLKOUT1_DIVIDE     = 1,
    parameter integer CLKOUT2_DIVIDE     = 1,
    parameter integer CLKOUT3_DIVIDE     = 1,
    parameter integer CLKOUT4_DIVIDE     = 1,
    parameter integer CLKOUT5_DIVIDE     = 1,
    parameter real    CLKOUT0_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT1_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT2_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT3_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT4_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT5_DUTY_CYCLE = 0.500,
    parameter real    CLKOUT0_PHASE      = 0.000,
    parameter real    CLKOUT1_PHASE      = 0.000,
    parameter real    CLKOUT2_PHASE      = 0.000,
    parameter real    CLKOUT3_PHASE      = 0.000,
    parameter real    CLKOUT4_PHASE      = 0.000,
    parameter real    CLKOUT5_PHASE      = 0.000,
    parameter integer DIVCLK_DIVIDE      = 1,
    parameter real    REF_JITTER1        = 0.010,
    parameter         STARTUP_WAIT       = "FALSE"
) (
    output wire CLKOUT0,
    output wire CLKOUT1,
    output wire CLKOUT2,
    output wire CLKOUT3,
    output wire CLKOUT4,
    output wire CLKOUT5,
    output wire CLKFBOUT,
    output wire LOCKED,
    input  wire CLKIN1,
    input  wire PWRDWN,
    input  wire RST,
    input  wire CLKFBIN
);
    localparam real IN_PS = CLKIN1_PERIOD * 1000.0;
    localparam real VCO_PS = IN_PS * DIVCLK_DIVIDE / CLKFBOUT_MULT;

    function real half(input integer divide);
        half = (divide == 1) ? 0.0 : VCO_PS * divide / 2.0;
    endfunction

    task check_out(input integer n, input integer divide);
        real f;
        begin
            if (divide < 1 || divide > 128)
                $fatal(1, "PLLE2_BASE %m: CLKOUT%0d_DIVIDE %0d outside 1..128 (UG472)", n, divide);
            if (divide != 1) begin
                f = 1.0e6 / (VCO_PS * divide);
                if (f < 6.25 || f > 800.0)
                    $fatal(1, "PLLE2_BASE %m: CLKOUT%0d %f MHz outside 6.25..800 MHz (DS181 Table 38)", n, f);
            end
        end
    endtask

    initial begin : check
        real fin, fpfd, fvco;
        if (CLKIN1_PERIOD < 0.938 || CLKIN1_PERIOD > 52.631)
            $fatal(1, "PLLE2_BASE %m: CLKIN1_PERIOD %f ns outside 0.938..52.631 (UG472)", CLKIN1_PERIOD);
        if (DIVCLK_DIVIDE < 1 || DIVCLK_DIVIDE > 56)
            $fatal(1, "PLLE2_BASE %m: DIVCLK_DIVIDE %0d outside 1..56 (UG472)", DIVCLK_DIVIDE);
        if (CLKFBOUT_MULT < 2 || CLKFBOUT_MULT > 64)
            $fatal(1, "PLLE2_BASE %m: CLKFBOUT_MULT %0d outside 2..64 (UG472)", CLKFBOUT_MULT);
        fin  = 1000.0 / CLKIN1_PERIOD;
        fpfd = fin / DIVCLK_DIVIDE;
        fvco = fpfd * CLKFBOUT_MULT;
        if (fin < 19.0 || fin > 800.0)
            $fatal(1, "PLLE2_BASE %m: CLKIN1 %f MHz outside 19..800 MHz (DS181 Table 38)", fin);
        if (fpfd < 19.0 || fpfd > 450.0)
            $fatal(1, "PLLE2_BASE %m: PFD %f MHz outside 19..450 MHz (DS181 Table 38)", fpfd);
        if (fvco < 800.0 || fvco > 1600.0)
            $fatal(1, "PLLE2_BASE %m: VCO %f MHz outside 800..1600 MHz (DS181 Table 38, -1/-1LI)", fvco);
        check_out(0, CLKOUT0_DIVIDE);
        check_out(1, CLKOUT1_DIVIDE);
        check_out(2, CLKOUT2_DIVIDE);
        check_out(3, CLKOUT3_DIVIDE);
        check_out(4, CLKOUT4_DIVIDE);
        check_out(5, CLKOUT5_DIVIDE);
    end

    wire [6:0] out;
    surf_sim_cmt #(
        .IN_PERIOD_PS(IN_PS), .FB_HALF_PS(IN_PS * DIVCLK_DIVIDE / 2.0),
        .HALF0_PS(half(CLKOUT0_DIVIDE)), .HALF1_PS(half(CLKOUT1_DIVIDE)),
        .HALF2_PS(half(CLKOUT2_DIVIDE)), .HALF3_PS(half(CLKOUT3_DIVIDE)),
        .HALF4_PS(half(CLKOUT4_DIVIDE)), .HALF5_PS(half(CLKOUT5_DIVIDE)), .HALF6_PS(0.0)
    ) cmt (
        .clkin(CLKIN1), .rst(RST || PWRDWN), .clkfbin(CLKFBIN), .clkfbout(CLKFBOUT),
        .clkout(out), .locked(LOCKED)
    );

    assign {CLKOUT5, CLKOUT4, CLKOUT3, CLKOUT2, CLKOUT1, CLKOUT0} = out[5:0];
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
