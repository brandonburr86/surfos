/*
SurfCPU open-flow clock-tile probe: LiteDRAM's PLL
--------------------
File: pll_sized.v Date: 10/16/26 (SurfCPU)
--------------------
A flow test, not for a board: the PLLE2_ADV of the Arty's LiteDRAM core (ip/litedram/arty.yml)
with its parameters exactly as the generator writes them, sized constants (5'd16, 4'd8, 5'd20,
3'd5, 7'd90), which Yosys's JSON keeps as short bit strings ("10000"). Each output clocks a
counter on an LED. devenv/openxc7/cmtcheck.py read such strings as decimal numbers until
round ddr-3 (tools/openxc7-checks-test.sh, docs/evidence/flow/checkers.txt).
*/
module pll_sized (
    input  wire       clk100,
    output wire [3:0] led
);
    wire fb, c0, c1, c2, c3, locked;
    PLLE2_ADV #(
        .CLKFBOUT_MULT  (5'd16),
        .CLKIN1_PERIOD  (10.0),
        .CLKOUT0_DIVIDE (4'd8),
        .CLKOUT0_PHASE  (1'd0),
        .CLKOUT1_DIVIDE (5'd20),
        .CLKOUT1_PHASE  (1'd0),
        .CLKOUT2_DIVIDE (3'd5),
        .CLKOUT2_PHASE  (1'd0),
        .CLKOUT3_DIVIDE (3'd5),
        .CLKOUT3_PHASE  (7'd90),
        .DIVCLK_DIVIDE  (1'd1),
        .REF_JITTER1    (0.01),
        .STARTUP_WAIT   ("FALSE")
    ) pll (
        .CLKFBIN(fb), .CLKIN1(clk100), .PWRDWN(1'b0), .RST(1'b0),
        .CLKFBOUT(fb), .CLKOUT0(c0), .CLKOUT1(c1), .CLKOUT2(c2), .CLKOUT3(c3), .LOCKED(locked));
    wire [3:0] g;
    BUFG b0 (.I(c0), .O(g[0]));
    BUFG b1 (.I(c1), .O(g[1]));
    BUFG b2 (.I(c2), .O(g[2]));
    BUFG b3 (.I(c3), .O(g[3]));
    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : g_cnt
            reg [3:0] n = 4'h0;
            always @(posedge g[i]) if (locked) n <= n + 4'h1;
            assign led[i] = n[3];
        end
    endgenerate
endmodule
