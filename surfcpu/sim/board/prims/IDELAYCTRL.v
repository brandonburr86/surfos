/*
SurfCPU simulation model of AMD's IDELAYCTRL
--------------------
File: IDELAYCTRL.v Date: 10/16/26 (SurfCPU)
--------------------
Same name and ports as the 7-series primitive (UG953), for simulating LiteDRAM's Artix-7 DDR3
PHY in the DDR top's RTL and netlist simulations: RDY rises 64 REFCLK cycles after RST falls
and falls with RST. The calibration of the delay lines it does on silicon is not modelled
(IDELAYE2.v models no delay). Simulation only, never read by synthesis.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
module IDELAYCTRL #(
    parameter SIM_DEVICE = "7SERIES"
) (
    output wire RDY,
    input  wire REFCLK,
    input  wire RST
);
    reg [6:0] count = 7'd0;
    always @(posedge REFCLK or posedge RST) begin
        if (RST)                count <= 7'd0;
        else if (!count[6])     count <= count + 7'd1;
    end
    assign RDY = count[6];
endmodule
/* verilator lint_on UNUSEDPARAM */
