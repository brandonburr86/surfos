# SurfCPU: timing of every surf_cdc_word instance (rtl/lib/surf_cdc_word.v).
# Read with: read_xdc -ref surf_cdc_word -unmanaged, PROCESSING_ORDER LATE (after the clocks
# exist); names below are relative to each instance. docs/BOARD.md section 4.
#
# The word crosses by a request/acknowledge toggle handshake: `hold` is copied into `dst_data`
# only after `req` has passed the destination's two-flop synchronizer, so the data bus needs
# no synchronizer of its own, only a delay shorter than the handshake. Each crossing (data,
# request, acknowledge) gets a datapath-only maximum delay of the smaller clock period. When
# both ends run on the same clock there is no crossing and ordinary timing applies.
# The synchronizer flops are ASYNC_REG in the RTL already; repeated here so placement keeps
# each pair together even if an attribute were lost in synthesis.
set_property ASYNC_REG TRUE [get_cells {req_sync_reg[*] ack_sync_reg[*]}]

set src_clk [get_clocks -quiet -of_objects [get_ports src_clk]]
set dst_clk [get_clocks -quiet -of_objects [get_ports dst_clk]]
if {[llength $src_clk] == 1 && [llength $dst_clk] == 1 &&
    [get_property NAME $src_clk] ne [get_property NAME $dst_clk]} {
    set limit [expr {min([get_property PERIOD $src_clk], [get_property PERIOD $dst_clk])}]
    set_max_delay -datapath_only -from [get_cells {hold_reg[*]}] -to [get_cells {dst_data_reg[*]}] $limit
    set_max_delay -datapath_only -from [get_cells req_reg] -to [get_cells {req_sync_reg[0]}] $limit
    set_max_delay -datapath_only -from [get_cells ack_reg] -to [get_cells {ack_sync_reg[0]}] $limit
}
