# Constraints for flow_features (devenv/openxc7/demo/flow_features.v) on the Arty A7-35T and A7-100T.
# Generated, do not edit: the lines of Digilent's master XDC (github.com/Digilent/digilent-xdc,
# commit c8644a9fe31cc803789643124545d56b3bef4600, Arty-A7-35-Master.xdc and
# Arty-A7-100-Master.xdc, identical for these ports; MIT licence) for the ports below,
# uncommented by tools/openxc7-xdc.sh. `make flow-xdc-check` regenerates and compares it.
#   ports: CLK100MHZ ck_rst sw[0] sw[1] led[0] led[1] led[2] led[3] uart_rxd_out ck_ss
set_property -dict {PACKAGE_PIN E3 IOSTANDARD LVCMOS33} [get_ports {CLK100MHZ}]
create_clock -add -name sys_clk_pin -period 10.00 -waveform {0 5} [get_ports {CLK100MHZ}]
set_property -dict {PACKAGE_PIN C2 IOSTANDARD LVCMOS33} [get_ports {ck_rst}]
set_property -dict {PACKAGE_PIN A8 IOSTANDARD LVCMOS33} [get_ports {sw[0]}]
set_property -dict {PACKAGE_PIN C11 IOSTANDARD LVCMOS33} [get_ports {sw[1]}]
set_property -dict {PACKAGE_PIN H5 IOSTANDARD LVCMOS33} [get_ports {led[0]}]
set_property -dict {PACKAGE_PIN J5 IOSTANDARD LVCMOS33} [get_ports {led[1]}]
set_property -dict {PACKAGE_PIN T9 IOSTANDARD LVCMOS33} [get_ports {led[2]}]
set_property -dict {PACKAGE_PIN T10 IOSTANDARD LVCMOS33} [get_ports {led[3]}]
set_property -dict {PACKAGE_PIN D10 IOSTANDARD LVCMOS33} [get_ports {uart_rxd_out}]
set_property -dict {PACKAGE_PIN C1 IOSTANDARD LVCMOS33} [get_ports {ck_ss}]
