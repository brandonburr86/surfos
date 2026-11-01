# SurfCPU: create the MIG 7-series DDR3L controller for the Arty A7 (non-project Vivado Tcl)
# File: mig_ddr3.tcl Date: 10/15/26 (SurfCPU)
#
#   vivado -mode batch -nojournal -source ip/mig/mig_ddr3.tcl -tclargs <board> <out_dir>
#
# board: arty_a7_100t (xc7a100tcsg324-1) or arty_a7_35t (xc7a35ticsg324-1L). Reads
# ip/mig/<board>/mig_ddr3.prj (made by ip/mig/make_prj.py from Digilent's file) and writes
# <out_dir>/mig_ddr3/: the .xci, the generated RTL and constraints (DDR3 pins, I/O standards,
# internal Vref, the MIG's own clock constraints), the instantiation template mig_ddr3.veo,
# the out-of-context checkpoint from synth_ip; and <out_dir>/mig_ddr3_summary.txt with the
# Vivado version and every CONFIG property. Nothing else on disk: an in-memory project.
#
# The board's build then reads the IP with `read_ip <out_dir>/mig_ddr3/mig_ddr3.xci`
# (docs/DDR.md, "The board top").
#
# STATUS: PENDING. Never run: there is no Vivado host yet (docs/BUILD-HOST.md). Written for
# Vivado 2018.3 or later, whose MIG 7 Series is version 4.2, the version of Digilent's file;
# create_ip asks for 4.2 exactly and stops on any other. The first run must:
#   * compare the generated port list (mig_ddr3.veo) with ip/mig/mig_ddr3_stub.v;
#   * check that the generated RTL has ORDERING = "STRICT", nCK_PER_CLK = 4, ADDR_WIDTH = 28,
#     APP_DATA_WIDTH = 128, and a ui_clk of 81.25 MHz (report_clocks after synthesis);
#   * keep mig_ddr3_summary.txt and the log as evidence (docs/evidence/ddr/).

if {[llength $argv] != 2} {
    error "usage: vivado -mode batch -source mig_ddr3.tcl -tclargs <arty_a7_100t|arty_a7_35t> <out_dir>"
}
set board [lindex $argv 0]
set out   [file normalize [lindex $argv 1]]
set here  [file dirname [file normalize [info script]]]
switch -- $board {
    arty_a7_100t { set part xc7a100tcsg324-1 }
    arty_a7_35t  { set part xc7a35ticsg324-1L }
    default      { error "mig_ddr3.tcl: unknown board $board" }
}
set prj [file join $here $board mig_ddr3.prj]
if {![file exists $prj]} { error "mig_ddr3.tcl: missing $prj (run python3 ip/mig/make_prj.py)" }

file mkdir $out
create_project -in_memory -part $part
set_property target_language Verilog [current_project]

create_ip -name mig_7series -vendor xilinx.com -library ip -version 4.2 -module_name mig_ddr3 -dir $out
set ip [get_ips mig_ddr3]

# MIG takes its whole configuration from a project file in the IP's directory.
file copy -force $prj [file join [get_property IP_DIR $ip] mig_ddr3.prj]
set_property -dict [list \
    CONFIG.XML_INPUT_FILE        {mig_ddr3.prj} \
    CONFIG.RESET_BOARD_INTERFACE {Custom} \
    CONFIG.MIG_DONT_TOUCH_PARAM  {Custom} \
    CONFIG.BOARD_MIG_PARAM       {Custom}] $ip

generate_target {instantiation_template synthesis simulation} $ip
synth_ip $ip

set f [open [file join $out mig_ddr3_summary.txt] w]
puts $f "board $board part $part vivado [version -short]"
puts $f "ipdef [get_property IPDEF $ip]"
puts $f "ip_dir [get_property IP_DIR $ip]"
foreach p [lsort [list_property $ip CONFIG.*]] {
    puts $f "$p = [get_property $p $ip]"
}
close $f
puts "mig_ddr3.tcl: mig_ddr3 for $board generated in $out"
