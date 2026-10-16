# SurfCPU: timing of every surf_reset_sync instance (rtl/lib/surf_reset_sync.v).
# Read with: read_xdc -ref surf_reset_sync -unmanaged, PROCESSING_ORDER LATE; names below are
# relative to each instance. docs/BOARD.md section 3.
#
# The reset asserts asynchronously through the flops' presets (a clock tile losing lock, the
# RESET button) and releases synchronously through the chain. The assertion has no timing
# relation to any clock; the release path inside the chain and from its last stage to the
# domain's logic is ordinary same-clock timing and stays timed.
# The chain is ASYNC_REG in the RTL already; repeated here so placement keeps it together.
set_property ASYNC_REG TRUE [get_cells {rst_ff_reg[*]}]
set_false_path -to [get_pins -quiet -filter {REF_PIN_NAME == PRE} -of_objects [get_cells {rst_ff_reg[*]}]]
