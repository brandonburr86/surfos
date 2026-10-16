/*
SurfCPU simulation model of AMD's BUFG
--------------------
File: BUFG.v Date: 10/15/26 (SurfCPU)
--------------------
A global clock buffer is a wire in simulation. Same name and ports as the 7-series primitive
(UG953); simulation only, never read by synthesis.
*/
`timescale 1ps/1ps
module BUFG (
    output wire O,
    input  wire I
);
    assign O = I;
endmodule
