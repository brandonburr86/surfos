# SurfCPU board track: runs the scoped CDC constraint files (boards/arty_a7/xdc/cdc/*.xdc) with
# stand-in Vivado queries, as Vivado would for one instance, and checks the constraints they
# issue: with two different clocks every crossing gets set_max_delay -datapath_only with the
# smaller period, with one clock nothing is constrained, and the reset synchronizer's presets
# get a false path. Run from surfcpu/: yosys -q -c sim/board/tools/cdc_xdc_test.tcl
# (the Tcl logic of the files is what is tested; the cell names are checked by
# tools/check-wiring.py against the RTL).

set issued [list]
set clock_of [dict create]
proc get_ports {name} { return "port:$name" }
proc get_clocks {args} {
    global clock_of
    set i [lsearch -exact $args -of_objects]
    if {$i >= 0} {
        set p [lindex $args [expr {$i + 1}]]
        return [expr {[dict exists $clock_of $p] ? [dict get $clock_of $p] : ""}]
    }
    set i [lsearch -exact $args -filter]
    set all [lsort -unique [dict values $clock_of]]
    if {$i >= 0 && [regexp {NAME != (\S+)} [lindex $args [expr {$i + 1}]] -> not]} {
        return [lsearch -all -inline -not -exact $all $not]
    }
    return $all
}
proc get_property {prop obj} {
    if {$prop eq "NAME"} { return $obj }
    if {$prop eq "PERIOD"} { return [dict get {sys_clk 40.000 pix_clk 39.718 clk100 10.000} $obj] }
    error "unexpected property $prop"
}
proc get_cells {args} { return "cells:[lindex $args end]" }
proc get_pins {args} { return "pins:[lindex $args end]" }
proc set_max_delay {args} { global issued; lappend issued [concat set_max_delay $args] }
proc set_false_path {args} { global issued; lappend issued [concat set_false_path $args] }

set failures 0
proc expect {name cond} {
    global failures
    if {[uplevel 1 [list expr $cond]]} {
        puts "ok   $name"
    } else {
        puts "FAIL $name"
        incr failures
    }
}
proc run_xdc {file clocks} {
    global issued clock_of
    set issued [list]
    set clock_of [dict create]
    foreach {port clk} $clocks { dict set clock_of "port:$port" $clk }
    source $file
    return $issued
}

set dir boards/arty_a7/xdc/cdc

# surf_cdc_word across sys_clk (40 ns) and pix_clk (39.718 ns)
set r [run_xdc $dir/surf_cdc_word.xdc {src_clk pix_clk dst_clk sys_clk}]
expect "cdc_word: three crossings constrained" {[llength $r] == 3}
expect "cdc_word: data bus hold -> dst_data" {[lindex $r 0] eq {set_max_delay -datapath_only -from {cells:hold_reg[*]} -to {cells:dst_data_reg[*]} 39.718}}
expect "cdc_word: request into the destination synchronizer" {[lindex $r 1] eq {set_max_delay -datapath_only -from cells:req_reg -to {cells:req_sync_reg[0]} 39.718}}
expect "cdc_word: acknowledge into the source synchronizer" {[lindex $r 2] eq {set_max_delay -datapath_only -from cells:ack_reg -to {cells:ack_sync_reg[0]} 39.718}}
set r [run_xdc $dir/surf_cdc_word.xdc {src_clk sys_clk dst_clk sys_clk}]
expect "cdc_word: one clock, nothing constrained" {[llength $r] == 0}
set r [run_xdc $dir/surf_cdc_word.xdc {src_clk sys_clk}]
expect "cdc_word: an unclocked end, nothing constrained" {[llength $r] == 0}

# surf_pulse_sync
set r [run_xdc $dir/surf_pulse_sync.xdc {src_clk sys_clk dst_clk pix_clk}]
expect "pulse_sync: the toggle into the synchronizer" {[llength $r] == 1 && [lindex $r 0] eq {set_max_delay -datapath_only -from cells:src_toggle_reg -to {cells:dst_sync_reg[0]} 39.718}}
set r [run_xdc $dir/surf_pulse_sync.xdc {src_clk pix_clk dst_clk pix_clk}]
expect "pulse_sync: one clock, nothing constrained" {[llength $r] == 0}

# surf_sync in the sys domain, with pix_clk and clk100 in the design
set r [run_xdc $dir/surf_sync.xdc {clk sys_clk other1 pix_clk other2 clk100}]
expect "sync: paths from every other clock into the first stage, sys period" {[llength $r] == 1 && [lindex $r 0] eq {set_max_delay -datapath_only -from {clk100 pix_clk} -to {cells:sync_ff_reg[0]} 40.000}}
set r [run_xdc $dir/surf_sync.xdc {clk sys_clk}]
expect "sync: a design with one clock, nothing constrained" {[llength $r] == 0}

# surf_reset_sync
set r [run_xdc $dir/surf_reset_sync.xdc {clk sys_clk}]
expect "reset_sync: false path to the presets" {[llength $r] == 1 && [lindex $r 0] eq {set_false_path -to {pins:cells:rst_ff_reg[*]}}}

puts "cdc_xdc_test: [expr {$failures ? "FAIL" : "PASS"}]"
exit [expr {$failures ? 1 : 0}]
