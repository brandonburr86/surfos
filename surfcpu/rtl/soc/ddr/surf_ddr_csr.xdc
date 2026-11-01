# SurfCPU: timing constraints for surf_ddr_csr's clock-domain crossings
# File: surf_ddr_csr.xdc Date: 10/16/26 (SurfCPU)
#
# Scoped to the module: the board's Vivado script reads it with
#     read_xdc -ref surf_ddr_csr rtl/soc/ddr/surf_ddr_csr.xdc
# so the cell names below are relative to each surf_ddr_csr instance and its two clocks are
# found through the module's ports clk (sys_clk) and wb_clk (the memory controller's clock).
#
# As in surf_ddr_bridge.xdc: sys_clk and the controller's clock both come from the 100 MHz
# oscillator, so Vivado would time the paths between them as synchronous ones. Every path
# between the two domains goes through the two mailboxes below, and each gets
# set_max_delay -datapath_only instead: a held message must reach the destination's capture
# registers within one destination clock period (the destination takes it two or more of its
# edges after the toggle that announces it was first sampled), and each toggle gets the same
# bound into its first synchronizer stage (ASYNC_REG is set on the synchronizers in the RTL).
# The open flow cannot check any of these (nextpnr ignores set_max_delay); they are listed as
# not checked in every build's manifest.
#
# PENDING: never read by Vivado (no build host yet). The register names follow Vivado's
# synthesis naming (<reg>_reg, vectors as [i]); check them with get_cells at the first build.

set sys_period [get_property PERIOD [get_clocks -of_objects [get_ports clk]]]
set wb_period  [get_property PERIOD [get_clocks -of_objects [get_ports wb_clk]]]

# request mailbox, sys -> wb: the held request into w_we/w_adr/w_dat
set_max_delay -datapath_only -from [get_cells {req_box/hold_reg[*]}] $wb_period
set_max_delay -datapath_only -from [get_cells req_box/req_reg] -to [get_cells {req_box/req_sync_reg[0]}] $wb_period
set_max_delay -datapath_only -from [get_cells req_box/ack_reg] -to [get_cells {req_box/ack_sync_reg[0]}] $sys_period

# response mailbox, wb -> sys: the held answer into rs_q
set_max_delay -datapath_only -from [get_cells {rsp_box/hold_reg[*]}] $sys_period
set_max_delay -datapath_only -from [get_cells rsp_box/req_reg] -to [get_cells {rsp_box/req_sync_reg[0]}] $sys_period
set_max_delay -datapath_only -from [get_cells rsp_box/ack_reg] -to [get_cells {rsp_box/ack_sync_reg[0]}] $wb_period
