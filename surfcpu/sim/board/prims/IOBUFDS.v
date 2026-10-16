/*
SurfCPU simulation model of AMD's IOBUFDS
--------------------
File: IOBUFDS.v Date: 10/16/26 (SurfCPU)
--------------------
A differential bidirectional pad pair: T = 1 releases both pins, T = 0 drives I on IO and its
complement on IOB; O is IO's level (the receiver's difference, IO - IOB, read as IO).
Same name, parameters and ports as the 7-series primitive (UG953); simulation only, never
read by synthesis. LiteDRAM's DDR3 PHY uses it for the data strobes.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
module IOBUFDS #(
    parameter DIFF_TERM    = "FALSE",
    parameter DQS_BIAS     = "FALSE",
    parameter IBUF_LOW_PWR = "TRUE",
    parameter IOSTANDARD   = "DEFAULT",
    parameter SLEW         = "SLOW"
) (
    output wire O,
    inout  wire IO,
    inout  wire IOB,
    input  wire I,
    input  wire T
);
    assign IO  = T ? 1'bz : I;
    assign IOB = T ? 1'bz : ~I;
    assign O   = IO;
endmodule
/* verilator lint_on UNUSEDPARAM */
