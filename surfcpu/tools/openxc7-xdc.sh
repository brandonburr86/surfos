#!/bin/sh
# Make constraints from Digilent's master XDC: for each port named, take its (commented-out)
# line from the master file, uncomment it and write it in a form both nextpnr's XDC reader
# and Vivado accept. The pins are therefore Digilent's, never typed by hand.
#
#   tools/openxc7-xdc.sh <master.xdc> <port>...     > constraints.xdc
#
# A port is named as in the master file (CLK100MHZ, led[0], uart_rxd_out, ...). The port's
# create_clock line, if the master file has one, comes along. Rewriting: the leading '#'
# goes, the trailing ';' and comment go, and the brace lists lose their inner padding
# ({ PACKAGE_PIN E3    IOSTANDARD LVCMOS33 } -> {PACKAGE_PIN E3 IOSTANDARD LVCMOS33},
# { led[0] } -> {led[0]}), which nextpnr's reader needs. A port that is missing from the
# master file, or listed in it twice, is an error.
set -eu
[ $# -ge 2 ] || { echo "usage: $0 <master.xdc> <port>..." >&2; exit 2; }
master=$1; shift
[ -f "$master" ] || { echo "openxc7-xdc.sh: no such file: $master" >&2; exit 2; }

for port in "$@"; do
  awk -v port="$port" '
    function norm(s) {
      sub(/^#+/, "", s)                       # uncomment
      sub(/;[^;]*$/, "", s)                   # the trailing ";" and its comment
      gsub(/[ \t]+/, " ", s)
      gsub(/\{ /, "{", s); gsub(/ \}/, "}", s)
      sub(/ +$/, "", s)
      return s
    }
    {
      line = $0
      if (match(line, /get_ports \{ *[^ }]+ *\}/) == 0) next
      name = substr(line, RSTART, RLENGTH)
      sub(/^get_ports \{ */, "", name); sub(/ *\}$/, "", name)
      if (name != port) next
      if (line ~ /^#*set_property/) { pins++; out = out norm(line) "\n" }
      else if (line ~ /^#*create_clock/) { clocks++; clk = clk norm(line) "\n" }
    }
    END {
      if (pins != 1) {
        printf "openxc7-xdc.sh: port %s has %d PACKAGE_PIN lines in the master file\n", port, pins > "/dev/stderr"
        exit 1
      }
      printf "%s%s", out, clk
    }' "$master"
done
