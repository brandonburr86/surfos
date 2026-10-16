/*
SurfCPU simulation model of AMD's IOBUF
--------------------
File: IOBUF.v Date: 10/15/26 (SurfCPU)
--------------------
A bidirectional pad: T = 1 releases the pin (high impedance), T = 0 drives I onto it; O is the
pin's level. Same name, parameters and ports as the 7-series primitive (UG953); simulation
only, never read by synthesis. SurfCPU uses it for the PS/2 lines as open drain (I = 0).
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
module IOBUF #(
    parameter DRIVE        = 12,
    parameter IBUF_LOW_PWR = "TRUE",
    parameter IOSTANDARD   = "DEFAULT",
    parameter SLEW         = "SLOW"
) (
    output wire O,
    inout  wire IO,
    input  wire I,
    input  wire T
);
    assign IO = T ? 1'bz : I;
    assign O  = IO;
endmodule
/* verilator lint_on UNUSEDPARAM */
