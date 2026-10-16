# Generated from spec/soc.toml (clocks.osc_hz) by ip/clocking/clockplan.py gen -- do not edit.
# The board oscillator. Vivado derives the MMCM and PLL output clocks from it by itself;
# boards/arty_a7/xdc/arty_a7_timing.xdc names them sys_clk and pix_clk.
create_clock -name clk100 -period 10.000 [get_ports clk100]
