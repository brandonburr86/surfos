/*
SurfCPU simulation model of AMD's OSERDESE2
--------------------
File: OSERDESE2.v Date: 10/16/26 (SurfCPU)
--------------------
Same name, parameters and ports as the 7-series primitive (UG953), for simulating LiteDRAM's
Artix-7 DDR3 PHY (ip/litedram/arty.yml) in the DDR top's RTL and netlist simulations, in the
one mode that PHY uses: DATA_RATE_OQ "DDR", DATA_WIDTH 8, MASTER, no cascade. At each rising
edge of CLKDIV it takes D1..D8 (while OCE is 1), and sends them on OQ one per edge of CLK
(rising and falling), D1 first, from the next rising CLK edge on. DATA_RATE_TQ "BUF" passes T1
to TQ unregistered (TRISTATE_WIDTH 1); "SDR" registers T1 on CLKDIV. OFB and TFB follow OQ and
TQ. RST clears it. Simulation only, never read by synthesis; another mode stops the
simulation ($fatal). Not modelled: the primitive's exact latency (UG471), TBYTE, the cascade.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module OSERDESE2 #(
    parameter         DATA_RATE_OQ   = "DDR",
    parameter         DATA_RATE_TQ   = "DDR",
    parameter integer DATA_WIDTH     = 4,
    parameter [0:0]   INIT_OQ        = 1'b0,
    parameter [0:0]   INIT_TQ        = 1'b0,
    parameter         SERDES_MODE    = "MASTER",
    parameter [0:0]   SRVAL_OQ       = 1'b0,
    parameter [0:0]   SRVAL_TQ       = 1'b0,
    parameter         TBYTE_CTL      = "FALSE",
    parameter         TBYTE_SRC      = "FALSE",
    parameter integer TRISTATE_WIDTH = 4,
    parameter [0:0]   IS_CLKDIV_INVERTED = 1'b0,
    parameter [0:0]   IS_CLK_INVERTED    = 1'b0,
    parameter [0:0]   IS_D1_INVERTED = 1'b0, parameter [0:0] IS_D2_INVERTED = 1'b0,
    parameter [0:0]   IS_D3_INVERTED = 1'b0, parameter [0:0] IS_D4_INVERTED = 1'b0,
    parameter [0:0]   IS_D5_INVERTED = 1'b0, parameter [0:0] IS_D6_INVERTED = 1'b0,
    parameter [0:0]   IS_D7_INVERTED = 1'b0, parameter [0:0] IS_D8_INVERTED = 1'b0,
    parameter [0:0]   IS_T1_INVERTED = 1'b0, parameter [0:0] IS_T2_INVERTED = 1'b0,
    parameter [0:0]   IS_T3_INVERTED = 1'b0, parameter [0:0] IS_T4_INVERTED = 1'b0
) (
    output wire OFB,
    output reg  OQ = 1'b0,
    output wire SHIFTOUT1,
    output wire SHIFTOUT2,
    output wire TBYTEOUT,
    output wire TFB,
    output wire TQ,
    input  wire CLK,
    input  wire CLKDIV,
    input  wire D1, input wire D2, input wire D3, input wire D4,
    input  wire D5, input wire D6, input wire D7, input wire D8,
    input  wire OCE,
    input  wire RST,
    input  wire SHIFTIN1,
    input  wire SHIFTIN2,
    input  wire T1, input wire T2, input wire T3, input wire T4,
    input  wire TBYTEIN,
    input  wire TCE
);
    initial begin
        if (DATA_RATE_OQ != "DDR" || DATA_WIDTH != 8 || SERDES_MODE != "MASTER" || TRISTATE_WIDTH != 1 ||
            (DATA_RATE_TQ != "BUF" && DATA_RATE_TQ != "SDR"))
            $fatal(1, "OSERDESE2 %m: only DDR x8 MASTER, TRISTATE_WIDTH 1, TQ BUF or SDR are modelled");
    end

    reg  [7:0] word = 8'h0;            // D8..D1, taken at CLKDIV
    reg        taken = 1'b0;           // toggles when a word is taken (CLKDIV side)
    reg        started = 1'b0;         // follows it when the word starts on CLK
    reg  [7:0] shift = 8'h0;
    reg  [2:0] bit_n = 3'd0;
    reg        t_q = 1'b0;

    always @(posedge CLKDIV or posedge RST) begin
        if (RST) begin
            word <= 8'h0;
            t_q  <= SRVAL_TQ;
        end else begin
            if (OCE) word <= {D8, D7, D6, D5, D4, D3, D2, D1};
            taken <= ~taken;
            if (TCE) t_q <= T1;
        end
    end

    // one bit per CLK edge; a new word starts on the first rising CLK edge after it was taken
    always @(posedge CLK or negedge CLK or posedge RST) begin
        if (RST) begin
            OQ      <= SRVAL_OQ;
            bit_n   <= 3'd0;
            started <= taken;
        end else if (CLK && bit_n == 3'd0 && started != taken) begin
            OQ      <= word[0];
            shift   <= word >> 1;
            bit_n   <= 3'd1;
            started <= taken;
        end else if (bit_n != 3'd0) begin
            OQ    <= shift[0];
            shift <= shift >> 1;
            bit_n <= bit_n + 3'd1;
        end
    end

    assign TQ        = (DATA_RATE_TQ == "BUF") ? T1 : t_q;
    assign OFB       = OQ;
    assign TFB       = TQ;
    assign SHIFTOUT1 = 1'b0;
    assign SHIFTOUT2 = 1'b0;
    assign TBYTEOUT  = 1'b0;
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
