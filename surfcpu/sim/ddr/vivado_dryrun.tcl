# SurfCPU: dry runs of the DDR track's Vivado Tcl without Vivado
# File: vivado_dryrun.tcl Date: 10/15/26 (SurfCPU)
#
# Runs ip/mig/mig_ddr3.tcl and reads rtl/soc/ddr/surf_ddr_bridge.xdc in a plain Tcl 8.6
# interpreter (the one inside Yosys: `yosys -q -c sim/ddr/vivado_dryrun.tcl`, from surfcpu/),
# with the Vivado commands they use replaced by stubs that record each call. It checks that the
# scripts parse, take their arguments, find and copy their files, call the commands expected
# with the values expected, refuse an unknown board; and that every cell the XDC constrains is
# a real register of the RTL (instance and register names checked against the sources). It
# cannot check what Vivado itself would do: that is pending a build host.

set errors 0
proc fail {msg} { global errors; incr errors; puts "vivado_dryrun: FAIL $msg" }

# ---------------------------------------------------------------- stubs
set calls {}
set props [dict create]
proc record {args} { global calls; lappend calls $args }
proc create_project {args} { record create_project {*}$args }
proc current_project {args} { return proj }
proc create_ip {args} {
    record create_ip {*}$args
    global ipdir
    set i [lsearch $args -dir]
    set ipdir [file join [lindex $args [expr {$i + 1}]] mig_ddr3]
    file mkdir $ipdir
}
proc get_ips {name} { return $name }
proc get_property {name obj} {
    global ipdir sys_period ui_period
    switch -- $name {
        IP_DIR  { return $ipdir }
        IPDEF   { return xilinx.com:ip:mig_7series:4.2 }
        PERIOD  { if {[string match *ui_clk* $obj]} { return $ui_period } else { return $sys_period } }
        default { return "<$name>" }
    }
}
proc set_property {args} { record set_property {*}$args }
proc generate_target {args} { record generate_target {*}$args }
proc synth_ip {args} { record synth_ip {*}$args }
proc version {args} { return "dry-run" }
proc list_property {obj pattern} { return {CONFIG.XML_INPUT_FILE CONFIG.BOARD_MIG_PARAM} }

proc called {cmd {with ""}} {
    global calls
    foreach c $calls {
        if {[lindex $c 0] eq $cmd && ($with eq "" || [lsearch -exact $c $with] >= 0)} { return $c }
    }
    return ""
}

# ---------------------------------------------------------------- ip/mig/mig_ddr3.tcl
set tmp [file normalize build/ddr/dryrun]
file delete -force $tmp
foreach {board part} {arty_a7_100t xc7a100tcsg324-1 arty_a7_35t xc7a35ticsg324-1L} {
    set calls {}
    set argv [list $board [file join $tmp $board]]
    if {[catch {source ip/mig/mig_ddr3.tcl} msg]} { fail "mig_ddr3.tcl $board: $msg"; continue }
    set c [called create_project]
    if {[lsearch $c -in_memory] < 0 || [lindex $c [expr {[lsearch $c -part] + 1}]] ne $part} {
        fail "$board: create_project is '$c', expected -in_memory -part $part"
    }
    set c [called create_ip]
    foreach {opt want} {-name mig_7series -vendor xilinx.com -library ip -version 4.2 -module_name mig_ddr3} {
        set got [lindex $c [expr {[lsearch $c $opt] + 1}]]
        if {$got ne $want} { fail "$board: create_ip $opt is '$got', expected '$want'" }
    }
    set copied [file join $ipdir mig_ddr3.prj]
    if {![file exists $copied]} { fail "$board: the project file was not copied into the IP directory" } else {
        set f [open $copied]; set a [read $f]; close $f
        set f [open ip/mig/$board/mig_ddr3.prj]; set b [read $f]; close $f
        if {$a ne $b} { fail "$board: copied project file differs from ip/mig/$board/mig_ddr3.prj" }
    }
    set c [called set_property -dict]
    set d [lindex $c [expr {[lsearch -exact $c -dict] + 1}]]
    foreach {k v} {CONFIG.XML_INPUT_FILE mig_ddr3.prj CONFIG.RESET_BOARD_INTERFACE Custom
                   CONFIG.MIG_DONT_TOUCH_PARAM Custom CONFIG.BOARD_MIG_PARAM Custom} {
        set i [lsearch -exact $d $k]
        if {$i < 0 || [lindex $d [expr {$i + 1}]] ne $v} { fail "$board: set_property $k is not $v ('$d')" }
    }
    if {[called generate_target] eq "" || [called synth_ip] eq ""} { fail "$board: no generate_target/synth_ip" }
    if {![file exists [file join $tmp $board mig_ddr3_summary.txt]]} { fail "$board: no summary file" }
}
set argv [list arty_s7_50 [file join $tmp bad]]
if {![catch {source ip/mig/mig_ddr3.tcl} msg]} { fail "mig_ddr3.tcl accepted an unknown board" }
set argv {}
if {![catch {source ip/mig/mig_ddr3.tcl} msg]} { fail "mig_ddr3.tcl ran without arguments" }

# ---------------------------------------------------------------- rtl/soc/ddr/surf_ddr_bridge.xdc
proc slurp {path} { set f [open $path]; set t [read $f]; close $f; return $t }
set src(surf_ddr_bridge)  [slurp rtl/soc/ddr/surf_ddr_bridge.v]
set src(surf_ddr_mailbox) [slurp rtl/soc/ddr/surf_ddr_mailbox.v]
set src(surf_sync)        [slurp rtl/lib/surf_sync.v]
set constrained {}

proc get_ports {name} {
    global src
    if {![regexp "input\\s+wire\\s+$name," $src(surf_ddr_bridge)]} { fail "xdc: no input port $name" }
    return port:$name
}
proc get_clocks {args} { return clock:[lindex $args end] }
proc get_cells {pattern} {
    # inst/reg_reg or inst/reg_reg[i]: inst must be an instance in the bridge, reg a register of its module
    global src constrained
    if {![regexp {^([a-z_]+)/([a-z_]+)_reg(\[[0-9*]+\])?$} $pattern -> inst reg idx]} {
        fail "xdc: cannot read the cell name $pattern"; return ""
    }
    if {![regexp "(\[a-z_]+)\\s+(#\\(\[^;]*?\\)\\s+)?$inst\\s*\\(" $src(surf_ddr_bridge) -> module]} {
        fail "xdc: no instance $inst in surf_ddr_bridge"; return ""
    }
    if {![info exists src($module)]} { fail "xdc: instance $inst is a $module, not checked"; return "" }
    if {![regexp "reg\\s+(\\\[\[^\\]]+\\\]\\s+)?$reg\\s*=" $src($module)]} {
        fail "xdc: $module has no register $reg (from $pattern)"; return ""
    }
    lappend constrained $pattern
    return cell:$pattern
}
proc set_max_delay {args} {
    global constrained
    if {[lsearch $args -datapath_only] < 0} { fail "xdc: set_max_delay without -datapath_only: $args" }
    if {[lsearch $args -from] < 0} { fail "xdc: set_max_delay -datapath_only needs -from: $args" }
    if {![string is double -strict [lindex $args end]]} { fail "xdc: no delay value: $args" }
    record set_max_delay {*}$args
}
set sys_period 40.000
set ui_period 12.308
set calls {}
if {[catch {source rtl/soc/ddr/surf_ddr_bridge.xdc} msg]} { fail "surf_ddr_bridge.xdc: $msg" }
set n 0
foreach c $calls { if {[lindex $c 0] eq "set_max_delay"} { incr n } }
foreach want {req_box/hold_reg req_box/req_sync_reg req_box/ack_sync_reg rsp_box/hold_reg
              rsp_box/req_sync_reg rsp_box/ack_sync_reg calib_sync/sync_ff_reg} {
    if {[lsearch -glob $constrained "$want*"] < 0} { fail "xdc: nothing constrains $want" }
}
if {$n != 7} { fail "xdc: $n set_max_delay commands, expected 7" }

if {$errors} { puts "vivado_dryrun: $errors failures"; exit 1 }
puts "vivado_dryrun: PASS (mig_ddr3.tcl for both boards and its refusals; surf_ddr_bridge.xdc: $n crossings, [llength $constrained] cell patterns, all real registers)"
