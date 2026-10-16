/*
SurfCPU clock tile models: illegal settings are refused
--------------------
File: tb_prims_bad.v Date: 10/15/26 (SurfCPU)
--------------------
Each CASE instantiates a clock tile model with one setting outside the 7-series limits; the
model must stop the simulation with $fatal naming the limit. mk/area-board.mk runs every case
and expects the failure and its message (a run that reaches "NOT CAUGHT" is a test failure).
  1 MMCM VCO 1300 MHz (> 1200)        4 PLL VCO 700 MHz (< 800)
  2 MMCM multiply 8.8 (not 1/8 steps) 5 PLL phase detector 16.7 MHz (< 19)
  3 MMCM output divide 1.5 (a fraction 6 PLL output divide 129 (> 128)
    below 2)
*/
`timescale 1ps/1ps
module tb_prims_bad;
    parameter integer CASE = 1;

    reg clk = 1'b0;
    always #5000 clk = ~clk;
    wire fb_m, fb_p;
    /* verilator lint_off PINCONNECTEMPTY */
    generate
        if (CASE <= 3) begin : mmcm
            MMCME2_BASE #(
                .CLKIN1_PERIOD(10.0),
                .CLKFBOUT_MULT_F(CASE == 1 ? 13.0 : CASE == 2 ? 8.8 : 8.0),
                .CLKOUT0_DIVIDE_F(CASE == 3 ? 1.5 : 32.0)
            ) u (.CLKIN1(clk), .CLKFBIN(fb_m), .CLKFBOUT(fb_m), .RST(1'b0), .PWRDWN(1'b0),
                 .CLKOUT0(), .CLKOUT0B(), .CLKOUT1(), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(),
                 .CLKOUT3(), .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(), .CLKFBOUTB(),
                 .LOCKED());
        end else begin : pll
            PLLE2_BASE #(
                .CLKIN1_PERIOD(10.0),
                .DIVCLK_DIVIDE(CASE == 5 ? 6 : 1),
                .CLKFBOUT_MULT(CASE == 4 ? 7 : CASE == 5 ? 60 : 10),
                .CLKOUT0_DIVIDE(CASE == 6 ? 129 : 40)
            ) u (.CLKIN1(clk), .CLKFBIN(fb_p), .CLKFBOUT(fb_p), .RST(1'b0), .PWRDWN(1'b0),
                 .CLKOUT0(), .CLKOUT1(), .CLKOUT2(), .CLKOUT3(), .CLKOUT4(), .CLKOUT5(), .LOCKED());
        end
    endgenerate
    /* verilator lint_on PINCONNECTEMPTY */

    initial begin
        #100000;
        $display("tb_prims_bad: case %0d NOT CAUGHT", CASE);
        $finish;
    end
endmodule
