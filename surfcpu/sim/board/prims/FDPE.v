/*
SurfCPU simulation model of AMD's FDPE
--------------------
File: FDPE.v Date: 10/16/26 (SurfCPU)
--------------------
A D flip-flop with clock enable and asynchronous preset, for the RTL simulation of LiteDRAM's
generated core, which instantiates it (its reset synchronizers). Same name, parameters and
ports as the 7-series primitive (UG953); simulation only, never read by synthesis (the netlist
simulation takes Yosys's own cell model instead).
*/
`timescale 1ps/1ps
module FDPE #(
    parameter [0:0] INIT = 1'b1,
    parameter [0:0] IS_C_INVERTED = 1'b0,
    parameter [0:0] IS_D_INVERTED = 1'b0,
    parameter [0:0] IS_PRE_INVERTED = 1'b0
) (
    output reg  Q = INIT,
    input  wire C,
    input  wire CE,
    input  wire PRE,
    input  wire D
);
    always @(posedge C or posedge PRE) begin
        if (PRE)     Q <= 1'b1;
        else if (CE) Q <= D;
    end
endmodule
