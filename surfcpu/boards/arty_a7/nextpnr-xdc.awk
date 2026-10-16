# SurfCPU Arty A7: the part of a design's XDC files that nextpnr's XDC reader applies, for
# tools/fpga-build.sh with FLOW=openxc7 (docs/BOARD.md section 8, docs/OPEN-FLOW.md section 3).
#
#   awk -f boards/arty_a7/nextpnr-xdc.awk -v notgiven=FILE a.xdc b.xdc ... > nextpnr.xdc
#
# Given to nextpnr, rewritten the way its reader needs (no ";" or comment after a command,
# brace lists without inner padding):
#   set_property on [get_ports ...] with PACKAGE_PIN, IOSTANDARD, DRIVE and PULLTYPE, each
#     shown to reach the bitstream (FASM) by a probe build. Vivado's PULLUP (PULLDOWN, KEEPER)
#     true becomes PULLTYPE PULLUP (...): nextpnr 1.0.0 sets the pull from PULLTYPE only and
#     drops PULLUP true without a word;
#   create_clock -period on a port, without -name, -add and -waveform (nextpnr ignores them).
# Everything else goes to FILE, one line each, "file:line: command  [why]", because nextpnr
# would ignore it, mostly with a warning, sometimes silently: other pin options (IOB TRUE: the
# register stays in the fabric), create_generated_clock (nextpnr derives and names the clock
# tiles' outputs itself), timing exceptions (set_false_path, set_max_delay, ...), I/O delays,
# properties of [current_design] (configuration and bitstream options) or of cells, and any
# other command. A multi-line Tcl command is not expected in these files and is refused.
BEGIN {
    if (notgiven == "") { print "nextpnr-xdc.awk: -v notgiven=FILE is required" > "/dev/stderr"; exit 2 }
    printf "" > notgiven
    keep["PACKAGE_PIN"] = 1; keep["IOSTANDARD"] = 1; keep["DRIVE"] = 1; keep["PULLTYPE"] = 1
    pull["PULLUP"] = 1; pull["PULLDOWN"] = 1; pull["KEEPER"] = 1
}
function skip(why) {
    printf "%s:%d: %s  [%s]\n", FILENAME, FNR, cmd, why > notgiven
    nskip++
}
{
    line = $0
    sub(/^[ \t]+/, "", line)
    if (line == "" || substr(line, 1, 1) == "#") next
    if ((i = index(line, ";")) > 0) line = substr(line, 1, i - 1)     # "; # comment"
    if ((i = index(line, "#")) > 0) line = substr(line, 1, i - 1)
    gsub(/[ \t]+/, " ", line)
    gsub(/\{ /, "{", line); gsub(/ \}/, "}", line)
    sub(/ +$/, "", line)
    if (line == "") next
    cmd = line
    if (line ~ /\\$/ || gsub(/\{/, "{", line) != gsub(/\}/, "}", line)) {
        print "nextpnr-xdc.awk: " FILENAME ":" FNR ": a command over several lines: " line > "/dev/stderr"
        bad = 1
        next
    }
    n = split(line, w, " ")
    if (w[1] == "create_clock") {
        if (match(line, /-period [0-9.]+/) == 0 || match(line, /\[get_ports (\{[^}]*\}|[^] ]*)\]$/) == 0) {
            skip("create_clock not on a port with -period: not given")
            next
        }
        match(line, /-period [0-9.]+/); period = substr(line, RSTART, RLENGTH)
        match(line, /\[get_ports (\{[^}]*\}|[^] ]*)\]$/); target = substr(line, RSTART, RLENGTH)
        print "create_clock " period " " target
        ngiven++
        next
    }
    if (w[1] != "set_property") {
        why = "not given"
        if (w[1] == "create_generated_clock") why = "nextpnr derives the clock tiles' outputs and names them itself"
        else if (w[1] ~ /^set_(false_path|max_delay|min_delay|multicycle_path|clock_groups)$/) why = "timing exception: not checked"
        else if (w[1] ~ /^set_(input|output)_delay$/) why = "I/O timing: not checked"
        skip(why)
        next
    }
    if (match(line, /\[get_ports (\{[^}]*\}|[^] ]*)\]$/) == 0) {
        if (line ~ /\[current_design\]$/) skip("configuration or bitstream option: not applied, the bitstream keeps its defaults")
        else skip("property of something other than a port: not applied")
        next
    }
    target = substr(line, RSTART, RLENGTH)
    props = substr(line, 1, RSTART - 2)                     # without " [get_ports ...]"
    if (props ~ /^set_property -dict \{/) {
        sub(/^set_property -dict \{/, "", props); sub(/\}$/, "", props)
    } else {
        sub(/^set_property /, "", props)
    }
    np = split(props, p, " ")
    if (np % 2 != 0) { skip("an odd number of property words: not given"); next }
    out = ""
    dropped = ""
    for (k = 1; k + 1 <= np; k += 2) {
        name = toupper(p[k]); value = p[k + 1]
        if (name in pull && toupper(value) == "TRUE") { name = "PULLTYPE"; value = toupper(p[k]) }
        if (name in keep) out = out (out == "" ? "" : " ") name " " value
        else dropped = dropped (dropped == "" ? "" : " ") p[k] " " value
    }
    if (out != "") { print "set_property -dict {" out "} " target; ngiven++ }
    if (dropped != "") {
        cmd = "set_property -dict {" dropped "} " target
        skip("pin option nextpnr does not apply")
    }
}
END {
    if (bad) exit 1
}
