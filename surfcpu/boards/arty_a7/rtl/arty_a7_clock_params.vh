// Generated from ip/clocking/arty_a7_clocks.toml and spec/soc.toml by
// ip/clocking/clockplan.py gen -- do not edit. Checked against DS181 v1.27.1 Tables 37/38
// (-1 and -1LI) and UG472 v1.14 Tables 3-7/3-8; docs/BOARD.md section 2.
// `include it inside the module body (localparams, no include guard).

/* verilator lint_off UNUSEDPARAM */
localparam real    ARTY_CLKIN_PERIOD_NS = 10.000;  // oscillator, clocks.osc_hz = 100000000

// sys_clk: PLLE2_BASE, VCO 1000 MHz, PFD 100 MHz
localparam integer ARTY_SYS_DIVCLK_DIVIDE  = 1;
localparam integer ARTY_SYS_CLKFBOUT_MULT  = 10;
localparam integer ARTY_SYS_CLKOUT0_DIVIDE = 40;
localparam integer ARTY_SYS_HZ = 25000000;  // exactly 25000000.000 Hz

// pix_clk: MMCME2_BASE, VCO 887.5 MHz, PFD 100 MHz
localparam integer ARTY_PIX_DIVCLK_DIVIDE   = 1;
localparam real    ARTY_PIX_CLKFBOUT_MULT_F  = 8.875;
localparam real    ARTY_PIX_CLKOUT0_DIVIDE_F = 35.250;
localparam integer ARTY_PIX_HZ = 25177305;  // 25177304.965 Hz
/* verilator lint_on UNUSEDPARAM */
