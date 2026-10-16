# SurfCPU: timing constraints for surf_ddr_bridge's clock-domain crossings
# File: surf_ddr_bridge.xdc Date: 10/15/26 (SurfCPU)
#
# Scoped to the module: the board's Vivado script reads it with
#     read_xdc -ref surf_ddr_bridge rtl/soc/ddr/surf_ddr_bridge.xdc
# so the cell names below are relative to each surf_ddr_bridge instance and its two clocks are
# found through the module's ports clk (sys_clk) and ui_clk (the MIG's ui_clk).
#
# sys_clk (the board's PLL) and ui_clk (the MIG's MMCM) both come from the 100 MHz oscillator,
# so Vivado sees them as related clocks and would time these paths as synchronous ones against
# the closest pair of edges, a meaningless requirement. Every path between the two domains
# goes through the crossings below, and each gets set_max_delay -datapath_only instead (no
# hold check, no clock skew): a mailbox's held message must reach the destination's capture
# registers within one destination clock period, which leaves a whole period of margin, since
# the destination takes it two or more of its edges after the toggle that announces it was
# first sampled; the toggles and the calibration status get the same bound into their first
# synchronizer stage (ASYNC_REG is set on the synchronizers in the RTL). Any other path between
# sys_clk and ui_clk would still be timed as a synchronous one and fail, which is the point:
# there must be none (check report_cdc and report_clock_interaction after implementation).
#
# PENDING: never read by Vivado (no build host yet). The register names follow Vivado's
# synthesis naming (<reg>_reg, vectors as [i]); check them with get_cells at the first build.

set sys_period [get_property PERIOD [get_clocks -of_objects [get_ports clk]]]
set ui_period  [get_property PERIOD [get_clocks -of_objects [get_ports ui_clk]]]

# request mailbox, sys -> ui: the held request into u_id/u_write/u_be/u_waddr/u_wdata
set_max_delay -datapath_only -from [get_cells {req_box/hold_reg[*]}] $ui_period
set_max_delay -datapath_only -from [get_cells req_box/req_reg] -to [get_cells {req_box/req_sync_reg[0]}] $ui_period
set_max_delay -datapath_only -from [get_cells req_box/ack_reg] -to [get_cells {req_box/ack_sync_reg[0]}] $sys_period

# response mailbox, ui -> sys: the held response into rs_q
set_max_delay -datapath_only -from [get_cells {rsp_box/hold_reg[*]}] $sys_period
set_max_delay -datapath_only -from [get_cells rsp_box/req_reg] -to [get_cells {rsp_box/req_sync_reg[0]}] $sys_period
set_max_delay -datapath_only -from [get_cells rsp_box/ack_reg] -to [get_cells {rsp_box/ack_sync_reg[0]}] $ui_period

# calibration status, ui -> sys: the MIG's init_calib_complete into the first synchronizer stage
set_max_delay -datapath_only -from [get_clocks -of_objects [get_ports ui_clk]] \
    -to [get_cells {calib_sync/sync_ff_reg[0]}] $sys_period
