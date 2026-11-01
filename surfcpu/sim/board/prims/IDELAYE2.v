/*
SurfCPU simulation model of AMD's IDELAYE2
--------------------
File: IDELAYE2.v Date: 10/16/26 (SurfCPU)
--------------------
Same name, parameters and ports as the 7-series primitive (UG953), for simulating LiteDRAM's
Artix-7 DDR3 PHY (ip/litedram/arty.yml) in the DDR top's RTL and netlist simulations. The tap
count follows the controls as the primitive's does (IDELAY_TYPE "VARIABLE" or "VAR_LOAD": LD
loads IDELAY_VALUE or CNTVALUEIN, CE with INC steps it up or down, wrapping at 31/0, on rising
edges of C) and is on CNTVALUEOUT, but the delay itself is not modelled: DATAOUT is IDATAIN
(DELAY_SRC "IDATAIN") or DATAIN unchanged. Simulation only, never read by synthesis.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module IDELAYE2 #(
    parameter         CINVCTRL_SEL          = "FALSE",
    parameter         DELAY_SRC             = "IDATAIN",
    parameter         HIGH_PERFORMANCE_MODE = "FALSE",
    parameter         IDELAY_TYPE           = "FIXED",
    parameter integer IDELAY_VALUE          = 0,
    parameter [0:0]   IS_C_INVERTED         = 1'b0,
    parameter [0:0]   IS_DATAIN_INVERTED    = 1'b0,
    parameter [0:0]   IS_IDATAIN_INVERTED   = 1'b0,
    parameter         PIPE_SEL              = "FALSE",
    parameter real    REFCLK_FREQUENCY      = 200.0,
    parameter         SIGNAL_PATTERN        = "DATA",
    parameter integer SIM_DELAY_D           = 0
) (
    output wire [4:0] CNTVALUEOUT,
    output wire       DATAOUT,
    input  wire       C,
    input  wire       CE,
    input  wire       CINVCTRL,
    input  wire [4:0] CNTVALUEIN,
    input  wire       DATAIN,
    input  wire       IDATAIN,
    input  wire       INC,
    input  wire       LD,
    input  wire       LDPIPEEN,
    input  wire       REGRST
);
    reg [4:0] taps = IDELAY_VALUE[4:0];
    always @(posedge C) begin
        if (LD)      taps <= (IDELAY_TYPE == "VAR_LOAD") ? CNTVALUEIN : IDELAY_VALUE[4:0];
        else if (CE) taps <= INC ? taps + 5'd1 : taps - 5'd1;
    end
    assign CNTVALUEOUT = taps;
    assign DATAOUT     = (DELAY_SRC == "DATAIN") ? DATAIN : IDATAIN;
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
