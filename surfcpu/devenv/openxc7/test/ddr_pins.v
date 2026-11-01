/*
SurfCPU open-flow DDR3 pin probe
--------------------
File: ddr_pins.v Date: 10/15/26 (SurfCPU)
--------------------
A flow experiment, not for a board: one OSERDESE2 with CLK and CLKDIV (400 and 100 MHz from an
MMCM), as LiteDRAM's 7-series DDR3 PHY drives every DDR output, on three Arty A7 DDR3 pins,
two of them in the SING I/O tiles at the ends of bank 34 (K6, U8) and one ordinary (R2).
openXC7's CI records LiteX's DDR3 design for the Arty S7 failing on exactly such a pin
(an unfuzzed divided-clock pip in a Spartan-7 SING tile); this shows whether the Artix-7
database and nextpnr place and encode it (docs/evidence/flow/ddr-pins.txt). Pins from LiteX's
digilent_arty platform; the ddr track owns the real DDR3 pinout.
*/
module ddr_pins (input wire CLK100MHZ, output wire [2:0] ddr);
    wire fb, c400, c100, locked, clk400, clk100;
    MMCME2_BASE #(.CLKIN1_PERIOD(10.0), .CLKFBOUT_MULT_F(8.0), .CLKOUT0_DIVIDE_F(2.0), .CLKOUT1_DIVIDE(8))
        mmcm (.CLKIN1(CLK100MHZ), .CLKFBIN(fb), .CLKFBOUT(fb), .RST(1'b0), .PWRDWN(1'b0),
              .CLKOUT0(c400), .CLKOUT1(c100), .LOCKED(locked));
    BUFG b400 (.I(c400), .O(clk400));
    BUFG b100 (.I(c100), .O(clk100));
    reg [3:0] pat = 4'd0;
    reg rst = 1'b1;
    always @(posedge clk100) begin rst <= !locked; pat <= pat + 4'd1; end
    genvar i;
    generate for (i = 0; i < 3; i = i + 1) begin : lane
        wire oq;
        OSERDESE2 #(.DATA_RATE_OQ("DDR"), .DATA_RATE_TQ("BUF"), .DATA_WIDTH(4), .SERDES_MODE("MASTER"),
                    .TRISTATE_WIDTH(1)) ser (
            .CLK(clk400), .CLKDIV(clk100), .D1(pat[0] ^ i[0]), .D2(pat[1]), .D3(pat[2]), .D4(pat[3]),
            .D5(1'b0), .D6(1'b0), .D7(1'b0), .D8(1'b0), .OCE(1'b1), .RST(rst), .OQ(oq),
            .T1(1'b0), .T2(1'b0), .T3(1'b0), .T4(1'b0), .TCE(1'b0), .TBYTEIN(1'b0),
            .SHIFTIN1(1'b0), .SHIFTIN2(1'b0));
        OBUF ob (.I(oq), .O(ddr[i]));
    end endgenerate
endmodule
