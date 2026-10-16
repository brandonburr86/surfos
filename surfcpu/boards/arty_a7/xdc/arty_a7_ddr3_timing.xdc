## SurfCPU on the Arty A7: timing of the DDR3L designs' own pins (surfcpu_arty_a7_top with
## ARTY_HAS_DDR, docs/DDR.md section 10). The crossing between sys_clk and the memory
## controller's clock is bounded by the bridge's scoped constraints (rtl/soc/ddr/
## surf_ddr_bridge.xdc, read last); LiteX's reset synchronizers inside the controller by the
## core's own litedram_core.xdc (generated with it). Vivado derives the controller's clocks
## (100, 400, 400 at 90 degrees and 200 MHz) from clk100 and its PLL by itself.

## The DDR3L pins. The PHY's OSERDESE2, ISERDESE2 and IDELAYE2 sit in the pins' own I/O tiles,
## clocked by the controller's 400 MHz clocks, and the BIOS's read leveling places the read
## capture at run time: there is no static input or output delay to give, and LiteX's own Arty
## designs give none. These false paths say so explicitly (the board's rule: no I/O without a
## timing constraint, tools/check-wiring.py); the fabric paths into and out of the I/O tiles
## stay timed.
set_false_path -to [get_ports {ddr3_addr[*] ddr3_ba[*] ddr3_ras_n ddr3_cas_n ddr3_we_n ddr3_reset_n}]
set_false_path -to [get_ports {ddr3_cke ddr3_cs_n ddr3_odt ddr3_ck_p ddr3_ck_n ddr3_dm[*]}]
set_false_path -to [get_ports {ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*]}]
set_false_path -from [get_ports {ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*]}]

## The calibration console: a serial adapter samples it with its own clock, 8.7 us a bit.
set_false_path -to [get_ports {dram_uart_tx}]

## Bank 34's reference voltage for the SSTL135 inputs: internal, half of its 1.35 V supply
## (Digilent's MIG projects: InternalVref). The open flow sets it by itself.
set_property INTERNAL_VREF 0.675 [get_iobanks 34]
