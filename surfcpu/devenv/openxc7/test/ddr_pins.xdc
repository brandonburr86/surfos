# Constraints for devenv/openxc7/test/ddr_pins.v, a flow experiment only (not for a board). DDR3
# pins as in LiteX's digilent_arty platform (unverified here); the clock line is Digilent's.
set_property -dict {PACKAGE_PIN E3 IOSTANDARD LVCMOS33} [get_ports {CLK100MHZ}]
create_clock -period 10.00 [get_ports {CLK100MHZ}]
set_property -dict {PACKAGE_PIN K6 IOSTANDARD SSTL135} [get_ports {ddr[0]}]
set_property -dict {PACKAGE_PIN U8 IOSTANDARD SSTL135} [get_ports {ddr[1]}]
set_property -dict {PACKAGE_PIN R2 IOSTANDARD SSTL135} [get_ports {ddr[2]}]
