/*
SurfCPU simulation model of AMD's ISERDESE2
--------------------
File: ISERDESE2.v Date: 10/16/26 (SurfCPU)
--------------------
Same name, parameters and ports as the 7-series primitive (UG953), for simulating LiteDRAM's
Artix-7 DDR3 PHY (ip/litedram/arty.yml) in the DDR top's RTL and netlist simulations, in the
one mode that PHY uses: NETWORKING, DATA_RATE "DDR", DATA_WIDTH 8, MASTER, one clock enable.
It samples its input (DDLY with IOBDELAY "IFD" or "BOTH", else D) at every rising edge of CLK
and of CLKB, and at each rising edge of CLKDIV puts the last eight bits on Q8..Q1, Q8 the
oldest (UG471). BITSLIP rotates the word by one bit per pulse. RST clears it; CE1 low holds it.
Simulation only, never read by synthesis; another mode stops the simulation ($fatal). Not
modelled: the primitive's exact latency, the OCLK/memory modes, the cascade, O.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module ISERDESE2 #(
    parameter         DATA_RATE         = "DDR",
    parameter integer DATA_WIDTH        = 4,
    parameter         DYN_CLKDIV_INV_EN = "FALSE",
    parameter         DYN_CLK_INV_EN    = "FALSE",
    parameter [0:0]   INIT_Q1 = 1'b0, parameter [0:0] INIT_Q2 = 1'b0,
    parameter [0:0]   INIT_Q3 = 1'b0, parameter [0:0] INIT_Q4 = 1'b0,
    parameter         INTERFACE_TYPE    = "MEMORY",
    parameter         IOBDELAY          = "NONE",
    parameter integer NUM_CE            = 2,
    parameter         OFB_USED          = "FALSE",
    parameter         SERDES_MODE       = "MASTER",
    parameter [0:0]   SRVAL_Q1 = 1'b0, parameter [0:0] SRVAL_Q2 = 1'b0,
    parameter [0:0]   SRVAL_Q3 = 1'b0, parameter [0:0] SRVAL_Q4 = 1'b0,
    parameter [0:0]   IS_CLKB_INVERTED = 1'b0, parameter [0:0] IS_CLKDIVP_INVERTED = 1'b0,
    parameter [0:0]   IS_CLKDIV_INVERTED = 1'b0, parameter [0:0] IS_CLK_INVERTED = 1'b0,
    parameter [0:0]   IS_D_INVERTED = 1'b0, parameter [0:0] IS_OCLKB_INVERTED = 1'b0,
    parameter [0:0]   IS_OCLK_INVERTED = 1'b0
) (
    output wire O,
    output wire Q1, output wire Q2, output wire Q3, output wire Q4,
    output wire Q5, output wire Q6, output wire Q7, output wire Q8,
    output wire SHIFTOUT1,
    output wire SHIFTOUT2,
    input  wire BITSLIP,
    input  wire CE1,
    input  wire CE2,
    input  wire CLK,
    input  wire CLKB,
    input  wire CLKDIV,
    input  wire CLKDIVP,
    input  wire D,
    input  wire DDLY,
    input  wire DYNCLKDIVSEL,
    input  wire DYNCLKSEL,
    input  wire OCLK,
    input  wire OCLKB,
    input  wire OFB,
    input  wire RST,
    input  wire SHIFTIN1,
    input  wire SHIFTIN2
);
    initial begin
        if (INTERFACE_TYPE != "NETWORKING" || DATA_RATE != "DDR" || DATA_WIDTH != 8 || SERDES_MODE != "MASTER")
            $fatal(1, "ISERDESE2 %m: only NETWORKING DDR x8 MASTER is modelled");
    end

    wire       din = (IOBDELAY == "IFD" || IOBDELAY == "BOTH") ? DDLY : D;
    reg  [7:0] sr = 8'h0;              // the last eight samples, sr[0] the newest
    reg  [7:0] q  = 8'h0;              // q[0] = Q1 (the newest) .. q[7] = Q8 (the oldest)
    reg  [2:0] slip = 3'd0;

    always @(posedge CLK or posedge CLKB or posedge RST) begin
        if (RST)      sr <= 8'h0;
        else if (CE1) sr <= {sr[6:0], din};
    end
    always @(posedge CLKDIV or posedge RST) begin
        if (RST) begin
            q    <= 8'h0;
            slip <= 3'd0;
        end else begin
            if (BITSLIP) slip <= slip + 3'd1;
            if (CE1)     q <= (sr << slip) | (sr >> (4'd8 - {1'b0, slip}));
        end
    end

    assign {Q8, Q7, Q6, Q5, Q4, Q3, Q2, Q1} = q;
    assign O         = din;
    assign SHIFTOUT1 = 1'b0;
    assign SHIFTOUT2 = 1'b0;
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
