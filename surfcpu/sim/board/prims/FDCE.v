/*
SurfCPU simulation model of AMD's FDCE
--------------------
File: FDCE.v Date: 10/16/26 (SurfCPU)
--------------------
A D flip-flop with clock enable and asynchronous clear, for the RTL simulation of LiteDRAM's
generated core, which instantiates it (its reset synchronizers). Same name, parameters and
ports as the 7-series primitive (UG953); simulation only, never read by synthesis (the netlist
simulation takes Yosys's own cell model instead).
*/
`timescale 1ps/1ps
module FDCE #(
    parameter [0:0] INIT = 1'b0,
    parameter [0:0] IS_C_INVERTED = 1'b0,
    parameter [0:0] IS_D_INVERTED = 1'b0,
    parameter [0:0] IS_CLR_INVERTED = 1'b0
) (
    output reg  Q = INIT,
    input  wire C,
    input  wire CE,
    input  wire CLR,
    input  wire D
);
    always @(posedge C or posedge CLR) begin
        if (CLR)     Q <= 1'b0;
        else if (CE) Q <= D;
    end
endmodule
