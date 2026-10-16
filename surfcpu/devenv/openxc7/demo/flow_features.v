/*
SurfCPU openXC7 feature probe, Arty A7 top
--------------------
File: flow_features.v Date: 10/15/26 (SurfCPU)
--------------------
flow_features_core on the Arty A7 with the vendor primitives the SoC will use, for testing
the open flow (docs/OPEN-FLOW.md). A PLLE2_BASE makes the 25 MHz system clock (100 MHz x 10
/ 40) and an MMCME2_BASE a pixel clock with a fractional multiplier, each on a BUFG: 100 MHz
x 18.125 / 2 / 36 = 25.1736 MHz by default, or with -D FLOW_PIX_PLANNED the planned 100 MHz
x 8.875 / 35.25 = 25.1773 MHz, whose odd fractional divider nextpnr 1.0.0 encodes unlike
AMD's XAPP888 (devenv/openxc7/cmtcheck.py fails it). An IOBUF makes ck_ss (the shield's SPI
select pin, J6, bank 35, no adapter planned on it) open-drain like the PS/2 lines. RESET
(ck_rst) and the clocks' LOCKED outputs reset both domains through rtl/lib/surf_reset_sync.
Port names are the ones in Digilent's master XDC; SW0 pulls ck_ss low, SW1 inverts LD6. Not
part of the SoC: the board track owns the real clocking (boards/arty_a7/).
*/
module flow_features (
    input  wire       CLK100MHZ,
    input  wire       ck_rst,
    input  wire [1:0] sw,           // two, because LVS cannot match a one-bit vector port's name
    output wire [3:0] led,
    output wire       uart_rxd_out,
    inout  wire       ck_ss
);
    // ---------------------------------------------------------------- clocks
    wire pll_fb, pll_out, pll_locked, mmcm_fb, mmcm_out, mmcm_locked;
    wire sys_clk, pix_clk;

    PLLE2_BASE #(
        .BANDWIDTH("OPTIMIZED"), .CLKIN1_PERIOD(10.0), .DIVCLK_DIVIDE(1),
        .CLKFBOUT_MULT(10), .CLKFBOUT_PHASE(0.0),                   // VCO 1000 MHz
        .CLKOUT0_DIVIDE(40), .CLKOUT0_PHASE(0.0), .CLKOUT0_DUTY_CYCLE(0.5),
        .STARTUP_WAIT("FALSE")
    ) pll (
        .CLKIN1(CLK100MHZ), .CLKFBIN(pll_fb), .CLKFBOUT(pll_fb), .RST(1'b0), .PWRDWN(1'b0),
        .CLKOUT0(pll_out), .CLKOUT1(), .CLKOUT2(), .CLKOUT3(), .CLKOUT4(), .CLKOUT5(),
        .LOCKED(pll_locked)
    );

`ifdef FLOW_PIX_PLANNED
    localparam integer PIX_DIVCLK = 1;
    localparam real    PIX_MULT   = 8.875;                         // VCO 887.5 MHz
    localparam real    PIX_DIV    = 35.25;
`else
    localparam integer PIX_DIVCLK = 2;
    localparam real    PIX_MULT   = 18.125;                        // VCO 906.25 MHz
    localparam real    PIX_DIV    = 36.0;
`endif
    MMCME2_BASE #(
        .BANDWIDTH("OPTIMIZED"), .CLKIN1_PERIOD(10.0), .DIVCLK_DIVIDE(PIX_DIVCLK),
        .CLKFBOUT_MULT_F(PIX_MULT), .CLKFBOUT_PHASE(0.0),
        .CLKOUT0_DIVIDE_F(PIX_DIV), .CLKOUT0_PHASE(0.0), .CLKOUT0_DUTY_CYCLE(0.5),
        .REF_JITTER1(0.010), .STARTUP_WAIT("FALSE")
    ) mmcm (
        .CLKIN1(CLK100MHZ), .CLKFBIN(mmcm_fb), .CLKFBOUT(mmcm_fb), .CLKFBOUTB(),
        .RST(1'b0), .PWRDWN(1'b0),
        .CLKOUT0(mmcm_out), .CLKOUT0B(), .CLKOUT1(), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(),
        .CLKOUT3(), .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(),
        .LOCKED(mmcm_locked)
    );

    BUFG sys_bufg (.I(pll_out),  .O(sys_clk));
    BUFG pix_bufg (.I(mmcm_out), .O(pix_clk));

    wire sys_rst, pix_rst;
    surf_reset_sync sys_reset (.clk(sys_clk), .arst(~ck_rst | ~pll_locked),  .rst(sys_rst));
    surf_reset_sync pix_reset (.clk(pix_clk), .arst(~ck_rst | ~mmcm_locked), .rst(pix_rst));

    // ---------------------------------------------------------------- the open-drain pin
    wire od_in, od_pull_low;
    IOBUF od_buf (.IO(ck_ss), .I(1'b0), .T(~od_pull_low), .O(od_in));

    wire [3:0] core_led;
    flow_features_core core (
        .sys_clk(sys_clk), .sys_rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .locked(pll_locked & mmcm_locked), .sw0(sw[0]), .od_in(od_in), .od_pull_low(od_pull_low),
        .led(core_led), .uart_tx(uart_rxd_out)
    );
    assign led = core_led ^ {1'b0, sw[1], 2'b00};   // SW1 inverts LD6 (straight from the pin)
endmodule
