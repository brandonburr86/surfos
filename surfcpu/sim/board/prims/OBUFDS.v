/*
SurfCPU simulation model of AMD's OBUFDS
--------------------
File: OBUFDS.v Date: 10/16/26 (SurfCPU)
--------------------
A differential output: O = I, OB = not I. Same name, parameters and ports as the 7-series
primitive (UG953); simulation only, never read by synthesis. LiteDRAM's DDR3 PHY uses it for
the memory's clock.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
module OBUFDS #(
    parameter CAPACITANCE = "DONT_CARE",
    parameter IOSTANDARD  = "DEFAULT",
    parameter SLEW        = "SLOW"
) (
    output wire O,
    output wire OB,
    input  wire I
);
    assign O  = I;
    assign OB = ~I;
endmodule
/* verilator lint_on UNUSEDPARAM */
