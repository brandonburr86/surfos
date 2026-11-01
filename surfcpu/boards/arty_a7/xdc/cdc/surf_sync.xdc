# SurfCPU: timing of every surf_sync instance (rtl/lib/surf_sync.v).
# Read with: read_xdc -ref surf_sync -unmanaged, PROCESSING_ORDER LATE; names below are
# relative to each instance. docs/BOARD.md section 4.
#
# A level crosses into a two-flop synchronizer (ASYNC_REG). When it comes from a register in
# another clock domain, the path into the first stage gets a datapath-only maximum delay of
# this domain's clock period. When it comes from a pin, the pin's false path in the board's
# timing XDC already covers it (a false path takes precedence over this bound).
# The synchronizer flops are ASYNC_REG in the RTL already; repeated here so placement keeps
# them together even if an attribute were lost in synthesis.
set_property ASYNC_REG TRUE [get_cells {sync_ff_reg[*]}]

set clk [get_clocks -quiet -of_objects [get_ports clk]]
if {[llength $clk] == 1} {
    set others [get_clocks -quiet -filter "NAME != [get_property NAME $clk]"]
    if {[llength $others] > 0} {
        set_max_delay -datapath_only -from $others -to [get_cells {sync_ff_reg[0]}] [get_property PERIOD $clk]
    }
}
