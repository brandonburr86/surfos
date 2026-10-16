# SurfCPU: timing of every surf_pulse_sync instance (rtl/lib/surf_pulse_sync.v).
# Read with: read_xdc -ref surf_pulse_sync -unmanaged, PROCESSING_ORDER LATE; names below are
# relative to each instance. docs/BOARD.md section 4.
#
# A pulse crosses as a toggle into a two-flop synchronizer (ASYNC_REG). The toggle-to-first-
# stage path gets a datapath-only maximum delay of the smaller clock period, so the toggle is
# never seen later than one destination cycle after a stable value would have been.
# The synchronizer flops are ASYNC_REG in the RTL already; repeated here so placement keeps
# the pair together even if an attribute were lost in synthesis.
set_property ASYNC_REG TRUE [get_cells {dst_sync_reg[*]}]

set src_clk [get_clocks -quiet -of_objects [get_ports src_clk]]
set dst_clk [get_clocks -quiet -of_objects [get_ports dst_clk]]
if {[llength $src_clk] == 1 && [llength $dst_clk] == 1 &&
    [get_property NAME $src_clk] ne [get_property NAME $dst_clk]} {
    set limit [expr {min([get_property PERIOD $src_clk], [get_property PERIOD $dst_clk])}]
    set_max_delay -datapath_only -from [get_cells src_toggle_reg] -to [get_cells {dst_sync_reg[0]}] $limit
}
