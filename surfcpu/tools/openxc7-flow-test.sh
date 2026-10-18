#!/bin/sh
# Tests of the open flow's scripts that need neither the openXC7 image nor the network
# (part of `make test-flow`): tools/openxc7-xdc.sh on a fixture in the format of Digilent's
# master XDC, tools/openxc7-build.sh's argument handling through --dry-run, and the synthesis
# check (devenv/openxc7/synthcheck.py) on the Yosys DSP-packing reproducer with this image's
# Yosys, which must fail with DSP packing and pass with the safe mode and without DSPs.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
cd "$here"
t=build/flow/test
rm -rf "$t"
mkdir -p "$t"
fails=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fails=$((fails + 1)); }

# ---------------------------------------------------------------- openxc7-xdc.sh
cat > "$t/master.xdc" <<'EOF'
## Clock signal
#set_property -dict { PACKAGE_PIN E3    IOSTANDARD LVCMOS33 } [get_ports { CLK100MHZ }]; #IO_L12P_T1_MRCC_35 Sch=gclk[100]
#create_clock -add -name sys_clk_pin -period 10.00 -waveform {0 5} [get_ports { CLK100MHZ }];
## LEDs
#set_property -dict { PACKAGE_PIN H5    IOSTANDARD LVCMOS33 } [get_ports { led[0] }]; #IO_L24N_T3_35 Sch=led[4]
#set_property -dict { PACKAGE_PIN J5    IOSTANDARD LVCMOS33 } [get_ports { led[10] }]; #a made-up neighbour
#set_property -dict { PACKAGE_PIN A1    IOSTANDARD LVCMOS33 } [get_ports { twice }];
#set_property -dict { PACKAGE_PIN A2    IOSTANDARD LVCMOS33 } [get_ports { twice }];
EOF
cat > "$t/expected.xdc" <<'EOF'
set_property -dict {PACKAGE_PIN E3 IOSTANDARD LVCMOS33} [get_ports {CLK100MHZ}]
create_clock -add -name sys_clk_pin -period 10.00 -waveform {0 5} [get_ports {CLK100MHZ}]
set_property -dict {PACKAGE_PIN H5 IOSTANDARD LVCMOS33} [get_ports {led[0]}]
EOF
if sh tools/openxc7-xdc.sh "$t/master.xdc" CLK100MHZ 'led[0]' > "$t/got.xdc" && cmp -s "$t/got.xdc" "$t/expected.xdc"
then ok "xdc: lines uncommented and normalized, the clock comes along"; else bad "xdc: output differs"; diff "$t/expected.xdc" "$t/got.xdc" || true; fi
if sh tools/openxc7-xdc.sh "$t/master.xdc" 'led[1]' > /dev/null 2>&1
then bad "xdc: a missing port (led[1], with led[10] present) was accepted"; else ok "xdc: a missing port is an error, led[10] does not match led[1]"; fi
if sh tools/openxc7-xdc.sh "$t/master.xdc" twice > /dev/null 2>&1
then bad "xdc: a port listed twice was accepted"; else ok "xdc: a port listed twice is an error"; fi

# ---------------------------------------------------------------- openxc7-build.sh --dry-run
dry() { sh tools/openxc7-build.sh --dry-run --top t --xdc x.xdc "$@" a.v b.v; }
expect_in() {   # expect_in <what> <text> <pattern>...
  what=$1 text=$2; shift 2
  for p in "$@"; do
    if ! printf '%s\n' "$text" | grep -qF -- "$p"; then bad "$what: no '$p'"; return; fi
  done
  ok "$what"
}
o=$(dry --board arty_a7_35t)
expect_in "build: the 35T is placed with the 50T die's database and checked as a 35T" "$o" \
  "--device xc7a35tcsg324-1 --chipdb build/openxc7/chipdb/2026-10-15/chipdb-xc7a50t.bin" \
  "--expect-device xc7a35t" "--part_file /opt/openxc7/share/prjxray-db/artix7/xc7a35tcsg324-1/part.yaml" \
  "tileverilog --fasm build/openxc7/t-arty_a7_35t/t.fasm" "--device xc7a50t" "read_verilog a.v b.v" \
  "cmtcheck.py --json build/openxc7/t-arty_a7_35t/t.json" \
  "scratchpad -set xilinx_dsp.multonly 1" "setundef -zero" "synthcheck.py --top t --pre build/openxc7/t-arty_a7_35t/pre.json" \
  "srlcheck.py --json build/openxc7/t-arty_a7_35t/t.json --top t" \
  "fasm2netlist --fasm build/openxc7/t-arty_a7_35t/t.decoded.fasm" "bramcheck.py --netlist"
o=$(dry --board arty_a7_100t)
expect_in "build: the 100T uses its own database" "$o" \
  "--device xc7a100tcsg324-1 --chipdb build/openxc7/chipdb/2026-10-15/chipdb-xc7a100t.bin" \
  "--expect-device xc7a100t" "--device xc7a100t"
expect_in "build: the chip database is the image's generator's without BUFGCTRL route-throughs, and the clock check reads the FASM for nets on the clock network" "$o" \
  "chipdb-gen.py --xray /opt/openxc7/share/prjxray-db/artix7 --device xc7a100t" \
  "bbasm -l /tmp/chipdb-xc7a100t.bba build/openxc7/chipdb/2026-10-15/chipdb-xc7a100t.bin" \
  "gclkcheck.py --fasm build/openxc7/t-arty_a7_100t/t.fasm"
o=$(dry --part xc7a50tcsg324-1 -D FOO=1 -DBAR -I inc --seed 7 --out build/x)
expect_in "build: part, defines, includes, seed and output directory" "$o" \
  "read_verilog -DFOO=1 -DBAR -Iinc a.v b.v" "--seed 7" "--expect-device xc7a50t" "build/x/t.bit"
o=$(dry --board arty_a7_35t --no-lvs)
if printf '%s\n' "$o" | grep -q lvs_equiv; then bad "build: --no-lvs still runs LVS"; else ok "build: --no-lvs"; fi
o=$(dry --board arty_a7_35t --no-check)
if printf '%s\n' "$o" | grep -qE 'bitinfo|fasmcheck|lvs_equiv|cmtcheck|gclkcheck|synthcheck|bramcheck|srlcheck'; then bad "build: --no-check still checks"; else ok "build: --no-check"; fi
o=$(dry --board arty_a7_35t --dsp none)
if printf '%s\n' "$o" | grep -q 'synth_xilinx .*-nodsp' && ! printf '%s\n' "$o" | grep -q multonly
then ok "build: --dsp none synthesizes with -nodsp"; else bad "build: --dsp none"; fi
o=$(dry --board arty_a7_35t --dsp full)
if printf '%s\n' "$o" | grep -qE 'multonly|-nodsp'; then bad "build: --dsp full"; else ok "build: --dsp full is plain synth_xilinx"; fi
o=$(sh tools/openxc7-build.sh --dry-run --top t --board arty_a7_35t --synth-only a.v 2>&1) || true
if printf '%s\n' "$o" | grep -q synthcheck && ! printf '%s\n' "$o" | grep -q nextpnr
then ok "build: --synth-only needs no XDC and stops after the synthesis check"; else bad "build: --synth-only"; fi
for args in "--board arty_s7_50" "--part xc7a200tfbg484-1" "" "--board arty_a7_35t --seed x" "--board arty_a7_35t --dsp some"; do
  # shellcheck disable=SC2086
  if dry $args > /dev/null 2>&1; then bad "build: accepted '$args'"; else ok "build: refuses '${args:-no board or part}'"; fi
done
if sh tools/openxc7-build.sh --dry-run --xdc x.xdc --board arty_a7_35t a.v > /dev/null 2>&1
then bad "build: accepted a missing --top"; else ok "build: refuses a missing --top"; fi
o=$(dry --board arty_a7_35t --lvs-timeout 7200)
expect_in "build: --lvs-timeout bounds the LVS proof" "$o" "timeout 7200 lvs_equiv"
if dry --board arty_a7_35t --lvs-timeout 2h > /dev/null 2>&1
then bad "build: accepted --lvs-timeout 2h"; else ok "build: refuses a --lvs-timeout that is not seconds"; fi

# ---------------------------------------------------------------- bramcheck.py: a RAMB36E1 as two RAMB18 halves
# fasm2netlist decodes a RAMB36E1 as the two RAMB18 of its tile, bits interleaved (even bits
# in the lower Y). A made-up block: woven back it must match; halves swapped or one bit
# changed must not.
cat > "$t/bram_fixture.py" <<'PYEOF'
import json, random, sys
out, mode = sys.argv[1], sys.argv[2]
rnd = random.Random(7)
data = rnd.getrandbits(128 * 256)
par = rnd.getrandbits(16 * 256)
cell = {"type": "RAMB36E1", "parameters": {}, "connections": {}}
for i in range(128):
    cell["parameters"][f"INIT_{i:02X}"] = format((data >> (256 * i)) & ((1 << 256) - 1), "0256b")
for i in range(16):
    cell["parameters"][f"INITP_{i:02X}"] = format((par >> (256 * i)) & ((1 << 256) - 1), "0256b")
json.dump({"modules": {"t": {"cells": {"u_rom.0.0": cell}}}}, open(out + ".json", "w"))
def half(x, nbits, h):
    v = 0
    for k in range(nbits // 2):
        v |= ((x >> (2 * k + h)) & 1) << k
    return v
halves = [(half(data, 128 * 256, h), half(par, 16 * 256, h)) for h in (0, 1)]
if mode == "swapped":
    halves.reverse()
if mode == "flipped":
    halves[1] = (halves[1][0] ^ (1 << 1000), halves[1][1])
with open(out + ".v", "w") as f:
    for y, (d, p) in zip((40, 41), halves):
        params = ", ".join([f".INIT_{i:02X}(256'h{(d >> (256 * i)) & ((1 << 256) - 1):064x})" for i in range(64)]
                           + [f".INITP_{i:02X}(256'h{(p >> (256 * i)) & ((1 << 256) - 1):064x})" for i in range(8)])
        f.write(f"  RAMB18E1 #({params}) \\bram18_BRAM_L_X38Y100_RAMB18_X1Y{y} (.CLKARDCLK(c));\n")
PYEOF
for mode in right swapped flipped; do
  python3 -I "$t/bram_fixture.py" "$t/bram_$mode" "$mode"
  if python3 -I devenv/openxc7/bramcheck.py --netlist "$t/bram_$mode.json" --top t --extracted "$t/bram_$mode.v" > "$t/bram_$mode.txt"
  then got=PASS; else got=FAIL; fi
  want=FAIL; [ "$mode" = right ] && want=PASS
  if [ "$got" = "$want" ]; then ok "bramcheck: a RAMB36E1 decoded as two halves, $mode: $got as expected"
  else bad "bramcheck: $mode: $got, expected $want"; cat "$t/bram_$mode.txt"; fi
done

# ---------------------------------------------------------------- synthcheck.py on the reproducer
r=devenv/openxc7/test/dsp_counter.v
if ! command -v yosys > /dev/null 2>&1; then
  echo "  skip  synthcheck on the reproducer: no Yosys here (make test-flow runs it in the container)"
else
yosys -q -p "read_verilog $r; hierarchy -top dsp_counter; proc; flatten; opt -full; wreduce; opt_clean; memory -nomap; opt_clean; write_json $t/pre.json" > "$t/pre.log" 2>&1
for mode in full safe none; do
  case $mode in
    full) cmd="synth_xilinx -top dsp_counter -flatten -abc9 -arch xc7" ;;
    safe) cmd="scratchpad -set xilinx_dsp.multonly 1; synth_xilinx -top dsp_counter -flatten -abc9 -arch xc7" ;;
    none) cmd="synth_xilinx -top dsp_counter -flatten -abc9 -arch xc7 -nodsp" ;;
  esac
  yosys -q -p "read_verilog $r; $cmd; write_json $t/$mode.json" > "$t/$mode.log" 2>&1
  if python3 -I devenv/openxc7/synthcheck.py --top dsp_counter --pre "$t/pre.json" --post "$t/$mode.json" > "$t/$mode.txt"
  then got=PASS; else got=FAIL; fi
  want=PASS; [ $mode = full ] && want=FAIL
  v=$(yosys -V | cut -d' ' -f2)
  if [ "$got" = "$want" ]; then ok "synthcheck: Yosys $v, DSP mode $mode: $got as expected"
  else bad "synthcheck: Yosys $v, DSP mode $mode: $got, expected $want"; cat "$t/$mode.txt"; fi
done
fi

# ---------------------------------------------------------------- synthcheck.py: registers that drive output ports
if command -v yosys > /dev/null 2>&1; then
  r=devenv/openxc7/test/port_register.v
  yosys -q -p "read_verilog $r; hierarchy -top port_register; proc; flatten; opt -full; wreduce; opt_clean; memory -nomap; opt_clean; write_json $t/pr_pre.json" > "$t/pr_pre.log" 2>&1
  yosys -q -p "read_verilog $r; synth_xilinx -top port_register -flatten -abc9 -arch xc7; write_json $t/pr_post.json" > "$t/pr_post.log" 2>&1
  if python3 -I devenv/openxc7/synthcheck.py --top port_register --pre "$t/pr_pre.json" --post "$t/pr_post.json" > "$t/pr.txt" &&
     grep -Eq '^u_sub +32 +32 ' "$t/pr.txt"
  then ok "synthcheck: an instance's register that drives an output port counts in its instance, RTL and netlist"
  else bad "synthcheck: the output register's flip-flops"; cat "$t/pr.txt"; fi
fi

[ "$fails" = 0 ] || { echo "openxc7-flow-test.sh: $fails failures"; exit 1; }
echo "openxc7-flow-test.sh: PASS"
