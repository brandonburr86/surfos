# SurfCPU Arty A7: the Vivado non-project build (docs/BOARD.md section 8, docs/BUILD-HOST.md).
#
#   vivado -mode batch -nojournal -log out/vivado.log -source <this file> -tclargs params.tcl
#
# Run from the staged tree that tools/fpga-build.sh makes: the current directory holds
# params.tcl, the sources under surfcpu/ and an out/ directory for the results. params.tcl sets
#   design top board part      names (the board's part from boards/arty_a7/boards.txt)
#   sources incdirs mems xdcs  paths relative to surfcpu/
#   cdcs                       {module xdc} pairs: constraints scoped to every instance of module
#   generics                   {NAME value ...} top-level parameters (BOARD, the build identity, ...)
#   rom                        a $readmemh file for BOOTROM_INIT, relative to surfcpu/ ("" = none)
#
# The flow: read, synthesize, optimize, place, route, write the reports (timing summary,
# utilization, DRC, clock interaction, CDC, methodology, I/O, check_timing, exceptions), then
# check them: setup and hold met, no DRC error or critical warning, no critical CDC finding,
# no unconstrained clock, endpoint or I/O, no critical warning anywhere in the run. Only then
# is the bitstream written. out/build-results.txt (key = value) feeds the build manifest;
# result = pass only with a bitstream. Nothing is waived here: a failed check fails the build.

set params [lindex $argv 0]
if {$params eq "" || ![file exists $params]} {
    puts "surfcpu-build: usage: vivado -mode batch -source build.tcl -tclargs params.tcl"
    exit 2
}
source $params
set here [pwd]
set outdir [file join $here out]
set rptdir [file join $outdir reports]
file mkdir $rptdir
set results [list]
# A result line is TOML: numbers bare, everything else (and anything asked for with -string)
# a quoted string.
proc result {key value {kind number}} {
    global results
    if {$kind eq "-string" || ![string is double -strict $value]} {
        set value "\"[string map {\\ \\\\ \" \\\"} $value]\""
    }
    lappend results [list $key $value]
}
proc write_results {} {
    global results outdir
    set f [open [file join $outdir build-results.txt] w]
    foreach kv $results {
        puts $f "[lindex $kv 0] = [lindex $kv 1]"
    }
    close $f
}
proc fail {why} {
    result result fail
    result reason $why
    write_results
    puts "surfcpu-build: FAIL: $why"
    exit 1
}

result design $design -string
result top $top -string
result board $board -string
result part $part -string
result vivado_version [version -short] -string
result build_host [info hostname] -string

# ---------------------------------------------------------------------------- read
cd [file join $here surfcpu]
set_part $part
set incs [list]
foreach d $incdirs {
    lappend incs [file normalize $d]
}
foreach f $sources {
    read_verilog -library work $f
}
foreach m $mems {
    read_mem $m
}
foreach x $xdcs {
    read_xdc $x
}
foreach c $cdcs {
    lassign $c module file
    read_xdc -ref $module -unmanaged $file
    set_property PROCESSING_ORDER LATE [get_files [file normalize $file]]
}
set gen [list]
foreach {name value} $generics {
    lappend gen -generic "$name=$value"
}
if {$rom ne ""} {
    lappend gen -generic "BOOTROM_INIT=\"[file normalize $rom]\""
}

# ---------------------------------------------------------------------------- implement
synth_design -top $top -part $part -include_dirs $incs -flatten_hierarchy rebuilt {*}$gen
report_utilization -file [file join $rptdir post_synth_utilization.rpt]
opt_design
place_design
phys_opt_design
route_design
write_checkpoint -force [file join $outdir post_route.dcp]

# ---------------------------------------------------------------------------- reports
report_timing_summary -max_paths 20 -report_unconstrained -warn_on_violation -file [file join $rptdir timing_summary.rpt]
report_utilization -file [file join $rptdir utilization.rpt]
report_utilization -hierarchical -file [file join $rptdir utilization_hierarchical.rpt]
report_drc -file [file join $rptdir drc.rpt]
report_clock_interaction -file [file join $rptdir clock_interaction.rpt]
report_cdc -details -file [file join $rptdir cdc.rpt]
report_methodology -file [file join $rptdir methodology.rpt]
report_clocks -file [file join $rptdir clocks.rpt]
report_io -file [file join $rptdir io.rpt]
report_exceptions -file [file join $rptdir exceptions.rpt]
report_power -file [file join $rptdir power.rpt]
check_timing -verbose -file [file join $rptdir check_timing.rpt]

# ---------------------------------------------------------------------------- checks
proc read_file {path} {
    set f [open $path r]
    set text [read $f]
    close $f
    return $text
}
set wns [get_property SLACK [get_timing_paths -setup -max_paths 1 -nworst 1]]
set whs [get_property SLACK [get_timing_paths -hold -max_paths 1 -nworst 1]]
result wns_ns $wns
result whs_ns $whs

# Utilization (the main resources; utilization.rpt has the rest): used and available are the
# first and the next-to-last numbers of the row (the columns vary between Vivado versions).
set util [read_file [file join $rptdir utilization.rpt]]
foreach {key label} {lut "Slice LUTs" ff "Slice Registers" bram_tiles "Block RAM Tile" dsp "DSPs" bufg "BUFGCTRL" mmcm "MMCME2_ADV" pll "PLLE2_ADV" bonded_io "Bonded IOB"} {
    if {[regexp -line "^\\|\\s*${label}\\*?\\s*\\|(.*)\$" $util -> rest]} {
        set nums [list]
        foreach cell [split $rest "|"] {
            set cell [string trim $cell]
            if {[string is double -strict $cell]} { lappend nums $cell }
        }
        if {[llength $nums] >= 3} {
            result $key "[lindex $nums 0] / [lindex $nums end-1]"
        }
    }
}

set problems [list]
if {$wns eq "" || $wns < 0} { lappend problems "setup timing not met (WNS $wns ns)" }
if {$whs eq "" || $whs < 0} { lappend problems "hold timing not met (WHS $whs ns)" }

set drc_bad [get_drc_violations -quiet -filter {SEVERITY == "Error" || SEVERITY == "Critical Warning"}]
result drc_errors [llength $drc_bad]
if {[llength $drc_bad] > 0} { lappend problems "[llength $drc_bad] DRC errors or critical warnings (drc.rpt)" }

# check_timing: every "There are N ..." line with N > 0 is a problem, except input or output
# ports without a delay that carry a deliberate false path (the asynchronous I/O of the
# timing XDC files, docs/BOARD.md section 4).
set ct [read_file [file join $rptdir check_timing.rpt]]
set ct_issues 0
foreach line [split $ct "\n"] {
    if {[regexp {There (?:is|are) ([0-9]+) } $line -> n] && $n > 0 && ![string match "*false path*" $line]} {
        incr ct_issues
        puts "surfcpu-build: check_timing: [string trim $line]"
    }
}
result check_timing_issues $ct_issues
if {$ct_issues > 0} { lappend problems "check_timing found unconstrained or ambiguous timing (check_timing.rpt)" }

set cdc [read_file [file join $rptdir cdc.rpt]]
set cdc_critical 0
foreach line [split $cdc "\n"] {
    if {[regexp {^\s*\|?\s*Critical\s*\|?\s*(CDC-[0-9]+)\s*\|?\s*([0-9]+)} $line -> id n]} {
        incr cdc_critical $n
        puts "surfcpu-build: report_cdc: $n critical $id"
    }
}
result cdc_critical $cdc_critical
if {$cdc_critical > 0} { lappend problems "$cdc_critical critical CDC findings (cdc.rpt)" }

set crit [get_msg_config -count -severity {CRITICAL WARNING}]
result critical_warnings $crit
if {$crit > 0} { lappend problems "$crit critical warnings in the run (vivado.log)" }

if {[llength $problems] > 0} {
    foreach p $problems { puts "surfcpu-build: $p" }
    fail [join $problems "; "]
}

# ---------------------------------------------------------------------------- bitstream
write_bitstream -force [file join $outdir $design.bit]
result bitstream $design.bit
result result pass
write_results
puts "surfcpu-build: PASS: $outdir/$design.bit (WNS $wns ns, WHS $whs ns)"
exit 0
