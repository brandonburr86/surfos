/*
SurfCPU simulation model of AMD's PLLE2_ADV
--------------------
File: PLLE2_ADV.v Date: 10/16/26 (SurfCPU)
--------------------
Same name, parameters and ports as the 7-series primitive (UG953), for simulating LiteDRAM's
generated core (its CRG's PLL: 100 MHz in, VCO 1600 MHz, 200, 100, 400 and 400 MHz at 90
degrees out) in the DDR top's RTL and netlist simulations. Simulation only: synthesis uses
AMD's primitive, and nothing under rtl/ or boards/ may read this file into a build.

At time 0 it checks its settings against UG472 and DS181 for the Arty parts, as PLLE2_BASE.v
does (CLKIN 19..800 MHz, PFD 19..450 MHz, VCO 800..1600 MHz, outputs 6.25..800 MHz), and
$fatal on a violation. Lock and the outputs' periods are surf_sim_cmt's. Not modelled: the
outputs' phases (LiteDRAM's 400 MHz at 90 degrees runs in phase with its 400 MHz here; a delay
on the output would cost Verilator's scheduler memory without end), CLKIN2 and CLKINSEL
(CLKIN1 is the input), the dynamic reconfiguration port (DO 0, DRDY 0), duty cycles other than
50 %, jitter.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module PLLE2_ADV #(
    parameter         BANDWIDTH          = "OPTIMIZED",
    parameter         COMPENSATION       = "ZHOLD",
    parameter integer CLKFBOUT_MULT      = 5,
    parameter real    CLKFBOUT_PHASE     = 0.000,
    parameter real    CLKIN1_PERIOD      = 0.000,
    parameter real    CLKIN2_PERIOD      = 0.000,
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
    parameter real    REF_JITTER2        = 0.010,
    parameter         STARTUP_WAIT       = "FALSE",
    parameter [0:0]   IS_CLKINSEL_INVERTED = 1'b0,
    parameter [0:0]   IS_PWRDWN_INVERTED   = 1'b0,
    parameter [0:0]   IS_RST_INVERTED      = 1'b0
) (
    output wire        CLKOUT0,
    output wire        CLKOUT1,
    output wire        CLKOUT2,
    output wire        CLKOUT3,
    output wire        CLKOUT4,
    output wire        CLKOUT5,
    output wire        CLKFBOUT,
    output wire        LOCKED,
    output wire [15:0] DO,
    output wire        DRDY,
    input  wire        CLKIN1,
    input  wire        CLKIN2,
    input  wire        CLKINSEL,
    input  wire        CLKFBIN,
    input  wire        RST,
    input  wire        PWRDWN,
    input  wire [6:0]  DADDR,
    input  wire [15:0] DI,
    input  wire        DCLK,
    input  wire        DEN,
    input  wire        DWE
);
    localparam real IN_PS  = CLKIN1_PERIOD * 1000.0;
    localparam real VCO_PS = IN_PS * DIVCLK_DIVIDE / CLKFBOUT_MULT;

    function real half(input integer divide);
        half = (divide == 1) ? 0.0 : VCO_PS * divide / 2.0;
    endfunction

    task check_out(input integer n, input integer divide);
        real f;
        begin
            if (divide < 1 || divide > 128)
                $fatal(1, "PLLE2_ADV %m: CLKOUT%0d_DIVIDE %0d outside 1..128 (UG472)", n, divide);
            if (divide != 1) begin
                f = 1.0e6 / (VCO_PS * divide);
                if (f < 6.25 || f > 800.0)
                    $fatal(1, "PLLE2_ADV %m: CLKOUT%0d %f MHz outside 6.25..800 MHz (DS181)", n, f);
            end
        end
    endtask

    initial begin : check
        real fin, fpfd, fvco;
        if (CLKIN1_PERIOD < 0.938 || CLKIN1_PERIOD > 52.631)
            $fatal(1, "PLLE2_ADV %m: CLKIN1_PERIOD %f ns outside 0.938..52.631 (UG472)", CLKIN1_PERIOD);
        if (DIVCLK_DIVIDE < 1 || DIVCLK_DIVIDE > 56)
            $fatal(1, "PLLE2_ADV %m: DIVCLK_DIVIDE %0d outside 1..56 (UG472)", DIVCLK_DIVIDE);
        if (CLKFBOUT_MULT < 2 || CLKFBOUT_MULT > 64)
            $fatal(1, "PLLE2_ADV %m: CLKFBOUT_MULT %0d outside 2..64 (UG472)", CLKFBOUT_MULT);
        fin  = 1000.0 / CLKIN1_PERIOD;
        fpfd = fin / DIVCLK_DIVIDE;
        fvco = fpfd * CLKFBOUT_MULT;
        if (fin < 19.0 || fin > 800.0)
            $fatal(1, "PLLE2_ADV %m: CLKIN1 %f MHz outside 19..800 MHz (DS181)", fin);
        if (fpfd < 19.0 || fpfd > 450.0)
            $fatal(1, "PLLE2_ADV %m: PFD %f MHz outside 19..450 MHz (DS181)", fpfd);
        if (fvco < 800.0 || fvco > 1600.0)
            $fatal(1, "PLLE2_ADV %m: VCO %f MHz outside 800..1600 MHz (DS181, -1/-1LI)", fvco);
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

    assign CLKOUT0 = out[0];
    assign CLKOUT1 = out[1];
    assign CLKOUT2 = out[2];
    assign CLKOUT3 = out[3];
    assign CLKOUT4 = out[4];
    assign CLKOUT5 = out[5];
    assign DO   = 16'h0;
    assign DRDY = 1'b0;
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
