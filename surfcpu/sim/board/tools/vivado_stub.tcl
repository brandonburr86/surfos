# SurfCPU: stand-ins for the Vivado Tcl commands boards/arty_a7/vivado/build.tcl uses, so the
# real build.tcl can run without Vivado (in Yosys's Tcl interpreter) for the wrapper tests.
# It checks what Vivado would check of the inputs (files exist, the part and top are set, the
# generics are well formed), logs every call to $FAKE_CALLS, writes small reports in the
# formats build.tcl parses, and makes the results the test asks for through the environment:
#   FAKE_WNS, FAKE_WHS      slack in ns (default 10.0 / 0.05)
#   FAKE_DRC                number of DRC errors (default 0)
#   FAKE_CRIT               number of critical warnings (default 0)
#   FAKE_CDC                number of critical CDC findings (default 0)
#   FAKE_NO_INPUT_DELAY     ports with no input delay and no false path (default 0)
# Run as: yosys -c vivado_stub.tcl, with FAKE_SOURCE (build.tcl) and FAKE_TCLARGS (params.tcl).
# This is a test aid: it proves the script's logic, not Vivado's behaviour.

proc env_or {name default} {
    if {[info exists ::env($name)]} { return $::env($name) }
    return $default
}
proc log_call {args} {
    set f [open $::env(FAKE_CALLS) a]
    puts $f [join $args " "]
    close $f
}
proc need_file {cmd path} {
    if {![file exists $path]} {
        puts "ERROR: \[$cmd\] file not found: $path"
        exit 1
    }
}
proc write_text {path text} {
    file mkdir [file dirname $path]
    set f [open $path w]
    puts -nonewline $f $text
    close $f
}
proc opt_value {args name} {
    set i [lsearch -exact $args $name]
    if {$i < 0} { return "" }
    return [lindex $args [expr {$i + 1}]]
}

set ::fake_part ""
proc version {args} { log_call version {*}$args; return "2025.1" }
proc set_part {part} { log_call set_part $part; set ::fake_part $part }
proc read_verilog {args} { log_call read_verilog {*}$args; need_file read_verilog [lindex $args end] }
proc read_mem {path} { log_call read_mem $path; need_file read_mem $path }
proc read_xdc {args} { log_call read_xdc {*}$args; need_file read_xdc [lindex $args end] }
proc get_files {args} { log_call get_files {*}$args; return [lindex $args end] }
proc set_property {args} { log_call set_property {*}$args }
proc synth_design {args} {
    log_call synth_design {*}$args
    if {[opt_value $args -top] eq "" || [opt_value $args -part] ne $::fake_part} {
        puts "ERROR: \[synth_design\] -top missing or -part differs from set_part"
        exit 1
    }
    foreach d [opt_value $args -include_dirs] { need_file synth_design $d }
    set i 0
    while {[set i [lsearch -start $i -exact $args -generic]] >= 0} {
        set g [lindex $args [incr i]]
        if {![regexp {^[A-Z_]+=(\"[^\"]*\"|32'h[0-9a-f]{8}|[0-9]+)$} $g]} {
            puts "ERROR: \[synth_design\] malformed -generic $g"
            exit 1
        }
    }
}
proc opt_design {args} { log_call opt_design {*}$args }
proc place_design {args} { log_call place_design {*}$args }
proc phys_opt_design {args} { log_call phys_opt_design {*}$args }
proc route_design {args} { log_call route_design {*}$args }
proc write_checkpoint {args} { log_call write_checkpoint {*}$args; write_text [lindex $args end] "fake checkpoint\n" }
proc report_timing_summary {args} { log_call report_timing_summary {*}$args; write_text [opt_value $args -file] "fake timing summary\n" }
proc report_utilization {args} {
    log_call report_utilization {*}$args
    set text "| Site Type | Used | Fixed | Prohibited | Available | Util% |\n"
    append text "| Slice LUTs | 312 | 0 | 0 | 63400 | 0.49 |\n"
    append text "| Slice Registers | 420 | 0 | 0 | 126800 | 0.33 |\n"
    append text "| Block RAM Tile | 0 | 0 | 0 | 135 | 0.00 |\n"
    append text "| DSPs | 0 | 0 | 0 | 240 | 0.00 |\n"
    write_text [opt_value $args -file] $text
}
proc report_drc {args} { log_call report_drc {*}$args; write_text [opt_value $args -file] "fake drc\n" }
proc report_clock_interaction {args} { log_call report_clock_interaction {*}$args; write_text [opt_value $args -file] "fake\n" }
proc report_cdc {args} {
    log_call report_cdc {*}$args
    set n [env_or FAKE_CDC 0]
    set text "| Severity | ID | Count | Description |\n"
    append text "| Info | CDC-15 | 1 | Clock enable controlled CDC structure detected |\n"
    if {$n > 0} { append text "| Critical | CDC-1 | $n | 1-bit unknown CDC circuitry |\n" }
    write_text [opt_value $args -file] $text
}
proc report_methodology {args} { log_call report_methodology {*}$args; write_text [opt_value $args -file] "fake\n" }
proc report_clocks {args} { log_call report_clocks {*}$args; write_text [opt_value $args -file] "fake\n" }
proc report_io {args} { log_call report_io {*}$args; write_text [opt_value $args -file] "fake\n" }
proc report_exceptions {args} { log_call report_exceptions {*}$args; write_text [opt_value $args -file] "fake\n" }
proc report_power {args} { log_call report_power {*}$args; write_text [opt_value $args -file] "fake\n" }
proc check_timing {args} {
    log_call check_timing {*}$args
    set n [env_or FAKE_NO_INPUT_DELAY 0]
    set text "1. checking no_clock (0)\n  There are 0 register/latch pins with no clock.\n"
    append text "  There are 0 unconstrained internal endpoints.\n"
    append text "  There are 11 input ports with no input delay but user has a false path constraint. (MEDIUM)\n"
    append text "  There are $n input ports with no input delay specified. (HIGH)\n"
    write_text [opt_value $args -file] $text
}
proc get_timing_paths {args} { log_call get_timing_paths {*}$args; return [expr {"-hold" in $args ? "hold" : "setup"}] }
proc get_property {name object} {
    if {$name eq "SLACK"} {
        return [expr {$object eq "hold" ? [env_or FAKE_WHS 0.05] : [env_or FAKE_WNS 10.0]}]
    }
    return ""
}
proc get_drc_violations {args} {
    log_call get_drc_violations {*}$args
    set out [list]
    for {set i 0} {$i < [env_or FAKE_DRC 0]} {incr i} { lappend out "DRC-$i" }
    return $out
}
proc get_msg_config {args} { log_call get_msg_config {*}$args; return [env_or FAKE_CRIT 0] }
proc write_bitstream {args} {
    log_call write_bitstream {*}$args
    # a .bit header starts with a fixed preamble and the design and part fields
    set part [string range [regsub {^xc} $::fake_part ""] 0 [expr {[string first - [regsub {^xc} $::fake_part ""]] - 1}]]
    write_text [lindex $args end] "\x00\x09\x0f\xf0\x0f\xf0\x0f\xf0\x0f\xf0\x00\x00\x01a\x00\x05fake\x00b\x00\x0d$part\x00 fake bitstream\n"
}

set argv [list $::env(FAKE_TCLARGS)]
set argc 1
source $::env(FAKE_SOURCE)
