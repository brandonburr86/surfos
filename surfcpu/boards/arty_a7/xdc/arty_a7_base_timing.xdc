## SurfCPU on the Arty A7: clocks and I/O timing for the board's own pins (both tops).
## docs/BOARD.md sections 2-4 explain every line. The oscillator's create_clock is in
## arty_a7_clocks.xdc (generated from spec/soc.toml); the CDC paths are constrained per module
## by the scoped files in cdc/ (read last).

## The clock tiles' outputs. Vivado derives both clocks from clk100 and the tiles' settings by
## itself; these two lines only give the derived clocks their SurfCPU names.
create_generated_clock -name sys_clk [get_pins u_clocks/u_sys_pll/CLKOUT0]
create_generated_clock -name pix_clk [get_pins u_clocks/u_pix_mmcm/CLKOUT0]

## No clock groups: sys_clk and pix_clk are unrelated in practice (25 MHz and 25.18 MHz), and
## every path between them goes through rtl/lib synchronizers whose scoped constraints bound it
## with set_max_delay -datapath_only. A clock group would override those bounds, and any path
## between the domains that is not a synchronizer must show up in report_cdc, not vanish.

## Inputs with no timing relation to any FPGA clock: the switches, the buttons and the UART
## receive line go straight into surf_sync two-flop synchronizers (ASYNC_REG), the RESET
## button into the asynchronous preset of surf_reset_sync. Nanoseconds do not matter for any.
set_false_path -from [get_ports {sw[*] btn[*] ck_rst_n uart_rx}]

## Outputs whose receivers have no relation to an FPGA clock: LEDs (eyes) and the UART
## transmit line, sampled by the FT2232 with its own clock at 8.7 us a bit (115200 baud).
set_false_path -to [get_ports {led[*] rgb[*] uart_tx}]
