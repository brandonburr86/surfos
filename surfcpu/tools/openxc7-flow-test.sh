#!/bin/sh
# Tests of the open flow's scripts that need neither the openXC7 image nor the network
# (part of `make test-flow`): tools/openxc7-xdc.sh on a fixture in the format of Digilent's
# master XDC, tools/openxc7-build.sh's argument handling through --dry-run, its seed loop
# (tools/openxc7-steps.sh) with stand-in nextpnrs that hang, are killed, ignore TERM, route too
# thinly or well, and one Ctrl-C on a terminal stops, and the synthesis check
# (devenv/openxc7/synthcheck.py) on the Yosys DSP-packing reproducer with this image's Yosys,
# which must fail with DSP packing and pass with the safe mode and without DSPs. Since 10/27/26:
# the LVS proof's two routes (their command lines, the z3cmd front end's self-test first, the
# self-test of devenv/openxc7/z3cmd.py on right and broken commands), and the verdict on
# every way a stand-in lvs_equiv can end (proved, a difference, unknowns, killed by SIGKILL or
# TERM, stopped at the limit, a setup fault, a crash, a difference before it was killed).
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
o=$(dry --board arty_a7_35t)
expect_in "build: each seed's placement and routing runs under a time limit, 1800 s by default, in the terminal's process group, saying when it stops one; then the slack rule" "$o" \
  "[pnr] timeout --foreground --verbose -k 60 1800 nextpnr-himbaechel --device xc7a35tcsg324-1" \
  "[slack] python3 -I devenv/openxc7/slackcheck.py --report build/openxc7/t-arty_a7_35t/report.json --out"
o=$(dry --board arty_a7_35t --pnr-timeout 600 --seed 3,4 --min-slack dram_clk=0.25 --min-slack sys_clk=2)
expect_in "build: --pnr-timeout sets it, --min-slack a clock's least slack" "$o" \
  "timeout --foreground --verbose -k 60 600 nextpnr-himbaechel" "--seed 3 " "--min dram_clk=0.25 --min sys_clk=2 "
for v in dram_clk dram_clk= =0.5 dram_clk=x dram_clk=0.5.1 a=1=2; do
  if dry --board arty_a7_35t --min-slack "$v" > /dev/null 2>&1
  then bad "build: accepted --min-slack '$v'"; else ok "build: refuses --min-slack '$v'"; fi
done
if dry --board arty_a7_35t --min-slack dram_clk=0.5 --min-slack dram_clk=0.05 > /dev/null 2>&1
then bad "build: accepted --min-slack naming dram_clk twice"; else ok "build: refuses --min-slack naming a clock twice"; fi
for v in 0 00 000 2h ""; do
  if dry --board arty_a7_35t --pnr-timeout "$v" > /dev/null 2>&1
  then bad "build: accepted --pnr-timeout '$v'"; else ok "build: refuses --pnr-timeout '$v'"; fi
done
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

# ---------------------------------------------------------------- place_and_route: the seed loop
# tools/openxc7-steps.sh's seed loop (what openxc7-build.sh runs) with a stand-in nextpnr whose every
# seed does what PLAN says: hang (as router2 on a placement it cannot route), be killed (as by the
# out-of-memory killer), ignore TERM, hang after routing, or route with dram_clk's slack above the
# rule (pass, +1.389 ns) or below it (thin, +0.087 ns; thinner, +0.031 ns). A 2 s limit, 1 s grace.
if command -v timeout > /dev/null 2>&1; then
  p=$t/pnr
  mkdir -p "$p/bin" "$p/out"
  cat > "$p/bin/nextpnr-himbaechel" <<'EOF'
#!/bin/sh
seed= log= rep= fasm= place=
while [ $# -gt 0 ]; do
  case "$1" in --seed) seed=$2; shift 2 ;; -l) log=$2; shift 2 ;; --report) rep=$2; shift 2 ;;
    -o) case "$2" in fasm=*) fasm=${2#fasm=} ;; placement=*) place=${2#placement=} ;; esac; shift 2 ;; *) shift ;; esac
done
routes() {   # DRAM_MHZ: routed, sys_clk 45.61 MHz and dram_clk at that, both met
  printf '{"fmax": {"sys_clk": {"achieved": 45.61, "constraint": 25}, "dram_clk": {"achieved": %s, "constraint": 80}}}\n' "$1" > "$rep"
  { echo "Info: Max frequency for clock 'sys_clk': 45.61 MHz (PASS at 25.00 MHz)"
    echo "Info: Max frequency for clock 'dram_clk': $1 MHz (PASS at 80.00 MHz)"; echo "Info: Routing complete."; } > "$log"
  echo "seed$seed" > "$fasm"; echo "seed$seed" > "$place"
}
case " $PLAN " in
  *" $seed:pass "*) routes 90.00 ;;
  *" $seed:thin "*) routes 80.56 ;;
  *" $seed:thinner "*) routes 80.20 ;;
  *" $seed:zero "*) routes 0 ;;
  *" $seed:late "*) routes 79.00 ;;
  *" $seed:noclock "*) echo '{"fmax": {}}' > "$rep"; echo "Info: Routing complete." > "$log"; echo "seed$seed" > "$fasm" ;;
  *" $seed:hang "*) echo "Info:     iter=11 wires=427863 overused=2358 overuse=2373 archfail=NA" > "$log"; echo partial > "$fasm"; exec sleep 600 ;;
  *" $seed:routed-hang "*) echo "Info: Routing complete." > "$log"; exec sleep 600 ;;
  *" $seed:kill "*) echo "Info: placing" > "$log"; kill -KILL $$ ;;
  *" $seed:deaf "*) echo "Info: placing" > "$log"; trap '' TERM; exec sleep 600 ;;
  *) echo "no plan for seed $seed" >&2; exit 3 ;;
esac
EOF
  chmod +x "$p/bin/nextpnr-himbaechel"
  pnr_try() {   # PLAN SEEDS...: place_and_route as openxc7-build.sh calls it, a 2 s limit, the stand-in first on PATH
    plan=$1; shift
    rm -f "$p/out/"*
    ( PATH="$p/bin:$PATH"; PLAN=$plan; export PLAN
      dry=0 out=$p/out top=t part=xc7a35tcsg324-1 chipdb=none threads=1 xdc=none tools=devenv/openxc7 pnr_timeout=2 pnr_grace=1
      min_slack=${MIN_SLACK:-}
      . tools/openxc7-steps.sh; place_and_route "$@"
      echo "pnr_ok=$pnr_ok seed_used=$seed_used slack_state=$slack_state"; echo "slack_note=$slack_note" ) > "$p/run.txt" 2>&1
  }
  has() { grep -q -- "$1" "$p/$2"; }
  s0=$(date +%s); pnr_try "1:hang 2:pass" 1 2; s1=$(date +%s)
  if has '^pnr_ok=1 seed_used=2 slack_state=PASS$' run.txt && [ "$(grep -c '^seed ' "$p/out/seeds.txt")" = 2 ] &&
     has '^seed 1: timeout (nextpnr stopped at the limit of 2 s, exit 124), not routed; router at iter=11, overused=2358$' out/seeds.txt &&
     has "^seed 2: pass, routed; 'sys_clk': 45.61 MHz (PASS at 25.00 MHz), 'dram_clk': 90.00 MHz (PASS at 80.00 MHz); slack: least margin dram_clk +1.389 ns, wants 0.500$" out/seeds.txt &&
     [ $((s1 - s0)) -lt 30 ] && [ -f "$p/out/nextpnr-seed1.log" ] && [ "$(cat "$p/out/t.fasm")" = seed2 ]
  then ok "pnr: a seed whose nextpnr hangs is stopped at the limit ($((s1 - s0)) s), recorded as a timeout, and the next seed routes"
  else bad "pnr: the hanging seed"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:hang 3:hang" 1 3
  if has '^pnr_ok=0 ' run.txt && [ "$(grep -c '^seed [13]: timeout' "$p/out/seeds.txt")" = 2 ]
  then ok "pnr: when every seed times out, none routed (pnr_ok 0) and each is recorded"
  else bad "pnr: every seed timing out"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:kill 2:pass" 1 2
  if has '^pnr_ok=1 seed_used=2 ' run.txt &&
     has '^seed 1: killed (exit 137: SIGKILL, not from the time limit: the out-of-memory killer?) after [0-9]* s, not routed$' out/seeds.txt
  then ok "pnr: nextpnr killed by SIGKILL inside the limit is recorded as killed, not as a timeout, and the next seed routes"
  else bad "pnr: the killed seed"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:deaf 2:pass" 1 2
  if has '^pnr_ok=1 seed_used=2 ' run.txt && has '^seed 1: timeout (nextpnr stopped at the limit of 2 s, exit 137), not routed$' out/seeds.txt
  then ok "pnr: a nextpnr that ignores the limit's TERM is killed after the grace and recorded as a timeout"
  else bad "pnr: the seed that ignores TERM"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:routed-hang 2:pass" 1 2
  if has '^seed 1: timeout (nextpnr stopped at the limit of 2 s, exit 124), routed, but stopped before nextpnr finished (its outputs are not used)$' out/seeds.txt
  then ok "pnr: an attempt stopped at the limit after routing is recorded as routed, and not used"
  else bad "pnr: the seed that routed before the limit"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:thin 2:pass" 1 2
  if has '^pnr_ok=1 seed_used=2 slack_state=PASS$' run.txt &&
     has "^seed 1: thin, routed; 'sys_clk': 45.61 MHz (PASS at 25.00 MHz), 'dram_clk': 80.56 MHz (PASS at 80.00 MHz); slack: dram_clk +0.087 ns, wants 0.500$" out/seeds.txt &&
     [ "$(cat "$p/out/t.fasm")" = seed2 ] && [ -f "$p/out/slack-seed1.txt" ] && ! ls -d "$p"/out/thin-seed* > /dev/null 2>&1
  then ok "pnr: a placement thinner than the rule (dram_clk +0.087 ns of 0.5) is recorded as thin and the next seed is tried"
  else bad "pnr: the thin seed"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:thinner 2:hang 3:thin 4:thinner" 1 2 3 4
  if has '^pnr_ok=1 seed_used=3 slack_state=THIN$' run.txt && has '^kept: seed 3, the thin placement with the most margin (THIN)$' out/seeds.txt &&
     has '^slack_note=dram_clk +0.087 ns, wants 0.500; every seed tried was thin or failed, this one has the most margin$' run.txt &&
     [ "$(cat "$p/out/t.fasm")" = seed3 ] && grep -q "dram_clk': 80.56 MHz" "$p/out/nextpnr.log" && grep -q '80.56' "$p/out/report.json"
  then ok "pnr: when every seed is thin or failed, the thin placement with the most margin is kept (its outputs back in place), THIN"
  else bad "pnr: every seed thin"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:thinner 3:thin" 1 3; note_all=$(grep '^slack_note=' "$p/run.txt")
  pnr_try "3:thin" 3; note_one=$(grep '^slack_note=' "$p/run.txt")
  if [ -n "$note_all" ] && [ "$note_all" = "$note_one" ] && has '^pnr_ok=1 seed_used=3 slack_state=THIN$' run.txt
  then ok "pnr: a THIN build's note does not depend on the seeds tried (all of a list, or the kept seed pinned, as release-verify does)"
  else bad "pnr: the THIN note depends on the seeds tried: '$note_all' against '$note_one'"; fi
  MIN_SLACK="dram_clk=0.05" pnr_try "1:thin 2:pass" 1 2
  if has '^pnr_ok=1 seed_used=1 slack_state=PASS$' run.txt && has 'slack: least margin dram_clk +0.087 ns, wants 0.050$' out/seeds.txt
  then ok "pnr: a design's min-slack (dram_clk 0.05 ns) takes the placement the rule calls thin"
  else bad "pnr: the design's min-slack"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  pnr_try "1:zero 2:late 3:noclock 4:thin" 1 2 3 4
  if has '^pnr_ok=1 seed_used=4 slack_state=THIN$' run.txt &&
     has '^seed 1: fail (the slack rule: slackcheck: clock dram_clk: achieved 0.0 MHz is not a positive number), routed;' out/seeds.txt &&
     has '^seed 2: fail (the slack rule: slackcheck: clock dram_clk misses its constraint (79.00 MHz of 80.00, slack -0.158 ns)' out/seeds.txt &&
     has '^seed 3: fail (the slack rule: slackcheck: the report has no clock' out/seeds.txt && [ "$(cat "$p/out/t.fasm")" = seed4 ]
  then ok "pnr: placements the rule cannot judge (a clock at 0 MHz, one that misses its constraint, no clock) fail, saying why; never kept as thin"
  else bad "pnr: placements the rule cannot judge"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  mkdir -p "$p/broken"
  printf 'raise SystemExit("slackcheck broken on purpose")\n' > "$p/broken/slackcheck.py"
  ( PATH="$p/bin:$PATH"; PLAN="1:thin 2:pass"; export PLAN
    dry=0 out=$p/out top=t part=xc7a35tcsg324-1 chipdb=none threads=1 xdc=none tools=$p/broken pnr_timeout=2 pnr_grace=1 min_slack=
    rm -f "$p/out/"*; . tools/openxc7-steps.sh; place_and_route 1 2
    echo "pnr_ok=$pnr_ok seed_used=$seed_used slack_state=$slack_state" ) > "$p/run.txt" 2>&1
  if has '^pnr_ok=0 ' run.txt && has '^seed 1: fail (the slack rule: slackcheck broken on purpose)' out/seeds.txt
  then ok "pnr: a slackcheck that fails (exit 1 without its verdict) fails the attempt; nothing is kept as thin"
  else bad "pnr: a failing slackcheck"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  MIN_SLACK="ddr_clk=0.5" pnr_try "1:pass" 1
  if has '^pnr_ok=0 ' run.txt && has 'fail (the slack rule: slackcheck: min-slack names ddr_clk, which the design does not have' out/seeds.txt
  then ok "pnr: a min-slack naming a clock the design does not have fails the attempt, saying so"
  else bad "pnr: a min-slack naming no clock"; cat "$p/run.txt" "$p/out/seeds.txt" 2>/dev/null || true; fi
  # Ctrl-C on a terminal (docker run -it, as devenv/run.sh gives on one) while nextpnr runs: the
  # build stops at once, as it did before the limit, and no other seed is tried
  cat > "$p/ctrlc.py" <<'EOF'
import os, pty, sys, time
p = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.environ["PATH"] = p + "/bin:" + os.environ["PATH"]
    os.environ["PLAN"] = "1:hang 2:pass"
    os.execvp("sh", ["sh", "-c", "dry=0 out=%s/out top=t part=x chipdb=none threads=1 xdc=none tools=devenv/openxc7 "
                     "pnr_timeout=1800 min_slack=; . tools/openxc7-steps.sh; place_and_route 1 2; echo the loop went on" % p])
time.sleep(1.5)
t0 = time.time()
os.write(fd, b"\x03")
out, end = b"", t0 + 20
while time.time() < end:
    try:
        d = os.read(fd, 1024)
    except OSError:
        break
    if not d:
        break
    out += d
dt = time.time() - t0
os.waitpid(pid, 0)
went_on = b"the loop went on" in out or b"seed 2" in out
print("    Ctrl-C: the build ended %.1f s after it%s" % (dt, ", but the loop went on" if went_on else ""))
sys.exit(0 if dt < 5 and not went_on else 1)
EOF
  rm -f "$p/out/"*
  if python3 "$p/ctrlc.py" "$p"
  then ok "pnr: Ctrl-C on a terminal stops placement and routing at once (timeout --foreground)"
  else bad "pnr: Ctrl-C did not stop placement and routing at once"; fi
else
  bad "pnr: no timeout command here (coreutils)"
fi

# ---------------------------------------------------------------- the LVS routes (--lvs-solver)
o=$(dry --board arty_a7_100t --lvs-timeout 7200)
if printf '%s\n' "$o" | grep -qE -- '--solver|z3cmd'
then bad "lvs: the default route (libz3) still gives lvs_equiv a --solver"
else expect_in "lvs: the default route (libz3) runs lvs_equiv as before: its own session, no --solver" "$o" \
       "[lvs-prove] timeout 7200 lvs_equiv --gold build/openxc7/t-arty_a7_100t/lvs/gold.v --gold-top t --gate"; fi
o=$(dry --board arty_a7_100t --lvs-solver z3cmd --lvs-timeout 86400)
expect_in "lvs: --lvs-solver z3cmd: the front end's self-test, then lvs_equiv --solver it --format smt2 within the limit, a line a question" "$o" \
  "[lvs-selftest] python3 -I devenv/openxc7/z3cmd.py --self-test python3 -I devenv/openxc7/z3cmd.py" \
  "[lvs-prove] env Z3CMD_LOG=build/openxc7/t-arty_a7_100t/lvs/questions.log timeout 86400 lvs_equiv --gold build/openxc7/t-arty_a7_100t/lvs/gold.v" \
  "--device xc7a100t --solver python3 -I devenv/openxc7/z3cmd.py --format smt2"
if [ "$(printf '%s\n' "$o" | grep -n -e '^\[lvs-selftest\]' -e '^\[lvs-prove\]' | cut -d: -f2 | cut -c1-14 | tr '\n' ' ')" = "[lvs-selftest] [lvs-prove] en " ]
then ok "lvs: z3cmd: the self-test comes before the proof"; else bad "lvs: z3cmd: the self-test is not before the proof"; fi
o=$(OPENXC7_LVS_SOLVER=z3cmd sh tools/openxc7-build.sh --dry-run --top t --xdc x.xdc --board arty_a7_100t a.v)
expect_in "lvs: OPENXC7_LVS_SOLVER=z3cmd (inside the image) chooses it too" "$o" "--solver python3 -I devenv/openxc7/z3cmd.py --format smt2"
for v in cvc5 Z3CMD ""; do
  if dry --board arty_a7_100t --lvs-solver "$v" > /dev/null 2>&1
  then bad "lvs: accepted --lvs-solver '$v'"; else ok "lvs: refuses --lvs-solver '$v'"; fi
done

# ---------------------------------------------------------------- devenv/openxc7/z3cmd.py
# Its self-test (run as lvs_equiv runs a solver) must pass a command that answers the three questions
# right and fail commands that answer nothing, prove anything or find a difference anywhere. With
# the library (the openxc7 image) it passes itself; without it (this toolchain image: its Debian z3
# is a program linked statically, no libz3.so.4) it answers nothing, so it can never prove, and its
# own self-test fails. tools/openxc7-lvs-test.sh runs it on the openxc7 image's libz3.
for a in right unsat sat; do      # stand-in solvers: no answer to the malformed question, and then
  cat > "$t/solver-$a.sh" <<EOF
#!/bin/sh
if grep -q 'n9' "\$1"; then exit 1; fi
case $a in right) if grep -q '(and n1 (not n1))' "\$1"; then echo unsat; else echo sat; fi ;; *) echo $a ;; esac
EOF
done                              # the right answers, unsat to all, sat to all
if python3 -I devenv/openxc7/z3cmd.py --self-test "sh $t/solver-right.sh" > "$t/z3cmd-good.txt" 2>&1 &&
   grep -q '^z3cmd self-test: PASS$' "$t/z3cmd-good.txt"
then ok "z3cmd: its self-test passes a command that answers the three questions right"
else bad "z3cmd: its self-test on a right command"; cat "$t/z3cmd-good.txt"; fi
for c in "sh $t/solver-unsat.sh" "sh $t/solver-sat.sh" "false" "echo unsat; true" "echo unknown; true"; do
  if python3 -I devenv/openxc7/z3cmd.py --self-test "$c" > "$t/z3cmd-bad.txt" 2>&1
  then bad "z3cmd: its self-test passed the command '$c'"; cat "$t/z3cmd-bad.txt"
  else ok "z3cmd: its self-test fails the command '$c' ($(grep -c 'WRONG$' "$t/z3cmd-bad.txt") of 3 wrong)"; fi
done
printf '(set-logic QF_UF)\n(declare-const n1 Bool)\n(define-fun n2 () Bool (and n1 (not n1)))\n(assert n2)\n(check-sat)\n' > "$t/q-unsat.smt2"
if python3 -I -c 'import ctypes; ctypes.CDLL("libz3.so.4")' 2>/dev/null; then
  if python3 -I devenv/openxc7/z3cmd.py --self-test > "$t/z3cmd-self.txt" 2>&1 && grep -q '^z3cmd self-test: PASS$' "$t/z3cmd-self.txt"
  then ok "z3cmd: its own self-test passes on this image's libz3 ($(sed -n 's/^\(libz3 [0-9.]*\).*/\1/p' "$t/z3cmd-self.txt"))"
  else bad "z3cmd: its own self-test on this image's libz3"; cat "$t/z3cmd-self.txt"; fi
else
  if a=$(python3 -I devenv/openxc7/z3cmd.py "$t/q-unsat.smt2" 2>/dev/null); then st=0; else st=$?; fi
  if [ "$st" = 1 ] && [ -z "$a" ] && ! python3 -I devenv/openxc7/z3cmd.py --self-test > "$t/z3cmd-self.txt" 2>&1 &&
     grep -q '^libz3: not found' "$t/z3cmd-self.txt"
  then ok "z3cmd: without libz3.so.4 it answers nothing (exit 1: unknown, never proved), and its own self-test fails"
  else bad "z3cmd: without its library (answer '$a', exit $st)"; cat "$t/z3cmd-self.txt"; fi
fi
printf '3224975 2.104 0 unsat 812345\n48347140 15.980 0 unsat 2599464\n100 0.050 0 sat 20000\n10 0.010 3 - 15000\n' > "$t/q.log"
o=$(python3 -I devenv/openxc7/z3cmd.py --summary "$t/q.log")
expect_in "z3cmd: --summary sums the questions' log up" "$o" "z3cmd: 4 questions (1 error 3, 1 sat, 2 unsat)" \
  "question files of 10 to 48347140 bytes" "the longest 15.98 s (question 2)" "the largest solver process 2539 MiB (question 2)"

# ---------------------------------------------------------------- the LVS proof's verdict (lvs_prove, lvs_verdict)
# tools/openxc7-steps.sh's lvs_prove as post_route_checks calls it, with a stand-in lvs_equiv that
# ends as PLAN says, a stand-in front end in a tools directory of its own, and a 2 s limit.
if command -v timeout > /dev/null 2>&1; then
  l=$t/lvs
  mkdir -p "$l/bin" "$l/tools" "$l/out/lvs"
  cat > "$l/bin/lvs_equiv" <<'EOF'
#!/bin/sh
echo "lvs_equiv $* Z3CMD_LOG=${Z3CMD_LOG:-}" > "$LVS_ARGS"
echo "registers: 2 gold, 2 gate, 2 matched by name"
ulimit -c 0
case "$PLAN" in
  proved)     printf '  proved  r1\n  proved  r2\n\n2 proved, 0 differ, 0 unknown   (0.1s)\n'; exit 0 ;;
  differ)     printf '  proved  r1\n  DIFFER  r2\n\n1 proved, 1 differ, 0 unknown   (0.1s)\n'; exit 1 ;;
  unknown)    printf '  proved  r1\n  unknown r2\n\n1 proved, 0 differ, 1 unknown   (0.1s)\n'; exit 1 ;;
  both)       printf '  DIFFER  r1\n  unknown r2\n\n0 proved, 1 differ, 1 unknown   (0.1s)\n'; exit 1 ;;
  killed)     printf '  proved  r1\n'; kill -KILL $$ ;;
  term)       printf '  proved  r1\n'; kill -TERM $$ ;;
  diffkilled) printf '  DIFFER  r1\n'; kill -KILL $$ ;;
  hang)       printf '  proved  r1\n'; exec sleep 600 ;;
  setup)      echo "parse error: line 1" >&2; exit 2 ;;
  segv)       kill -SEGV $$ ;;
  quiet0)     printf '  proved  r1\n'; exit 0 ;;
  *)          echo "no plan $PLAN" >&2; exit 9 ;;
esac
EOF
  chmod +x "$l/bin/lvs_equiv"
  cat > "$l/tools/z3cmd.py" <<'EOF'
import os, sys
if sys.argv[1] == "--self-test":
    print("stand-in self-test of %r" % sys.argv[2:])
    open(os.environ["SELFTEST_RAN"], "w").write("ran\n")
    sys.exit(0 if os.environ.get("SELFTEST", "pass") == "pass" else 1)
if sys.argv[1] == "--summary":
    print("z3cmd: stand-in summary of " + sys.argv[2])
EOF
  lvs_try() {   # PLAN SOLVER [SELFTEST]: one proof; its result and fail flag in $l/run.txt
    rm -f "$l/args.txt" "$l/selftest-ran" "$l/out/check-lvs.txt" "$l/out/lvs/z3cmd-selftest.txt"
    ( PATH="$l/bin:$PATH" PLAN=$1 SELFTEST=${3:-pass} LVS_ARGS=$l/args.txt SELFTEST_RAN=$l/selftest-ran
      export PATH PLAN SELFTEST LVS_ARGS SELFTEST_RAN
      dry=0 out=$l/out top=t chipdie=xc7a100t db=db tools=$l/tools lvs_timeout=2 lvs_solver=$2 fail=0
      . tools/openxc7-steps.sh; lvs_prove; echo "result_lvs=$result_lvs"; echo "fail=$fail" ) > "$l/run.txt" 2>&1
  }
  lvs_is() {    # WHAT PLAN SOLVER RESULT FAIL: lvs_prove's verdict on that end
    lvs_try "$2" "$3"
    if grep -qxF "result_lvs=$4" "$l/run.txt" && grep -qx "fail=$5" "$l/run.txt"
    then ok "lvs: $1: $4$( [ "$5" = 1 ] && echo ', a failure' || echo ', not a failure of the build script')"
    else bad "lvs: $1: wanted '$4' and fail=$5"; cat "$l/run.txt"; fi
  }
  lvs_is "every point proved" proved libz3 "PASS" 0
  lvs_is "a point differs" differ libz3 "FAIL (1 differ)" 1
  lvs_is "a point unknown, none differs" unknown libz3 "BLOCKED (1 unknown, none differ)" 0
  lvs_is "a point differs, one unknown" both libz3 "FAIL (1 differ)" 1
  lvs_is "the proof killed by SIGKILL, as the memory cap's out-of-memory killer does" killed libz3 "BLOCKED (killed: SIGKILL, exit 137)" 0
  lvs_is "the proof killed by SIGTERM" term libz3 "BLOCKED (killed: SIGTERM, exit 143)" 0
  lvs_is "a difference printed before the proof was killed" diffkilled libz3 "FAIL (1 differ before the proof stopped, exit 137)" 1
  lvs_is "the proof stopped at its limit" hang libz3 "BLOCKED (timeout)" 0
  lvs_is "a setup fault, lvs_equiv's exit 2" setup libz3 "FAIL (lvs_equiv could not run the proof: exit 2, see check-lvs.txt)" 1
  lvs_is "a crash (SIGSEGV)" segv libz3 "FAIL (lvs_equiv crashed: signal 11, exit 139)" 1
  lvs_is "exit 0 without its count" quiet0 libz3 "FAIL (lvs_equiv ended 0 without a count of 0 differ and 0 unknown)" 1
  lvs_try proved libz3
  if grep -q -- '--device xc7a100t Z3CMD_LOG=$' "$l/args.txt" && ! grep -q -- '--solver' "$l/args.txt" && [ ! -f "$l/selftest-ran" ]
  then ok "lvs: libz3: lvs_equiv with its own session (no --solver), no front end, no self-test"
  else bad "lvs: libz3's command line"; cat "$l/args.txt" "$l/run.txt"; fi
  lvs_try proved z3cmd
  if grep -q -- "--device xc7a100t --solver python3 -I $l/tools/z3cmd.py --format smt2 Z3CMD_LOG=$l/out/lvs/questions.log\$" "$l/args.txt" &&
     [ -f "$l/selftest-ran" ] && grep -qF "stand-in self-test of ['python3 -I $l/tools/z3cmd.py']" "$l/out/lvs/z3cmd-selftest.txt" &&
     grep -q '^z3cmd: stand-in summary' "$l/out/check-lvs.txt" && grep -qx 'result_lvs=PASS' "$l/run.txt"
  then ok "lvs: z3cmd: the self-test of the very command lvs_equiv gets, then lvs_equiv --solver it --format smt2, a line a question, summed up"
  else bad "lvs: z3cmd's command line"; cat "$l/args.txt" "$l/run.txt"; fi
  lvs_try proved z3cmd fail
  if grep -qx 'result_lvs=FAIL (the z3cmd self-test: lvs/z3cmd-selftest.txt)' "$l/run.txt" && grep -qx 'fail=1' "$l/run.txt" && [ ! -f "$l/args.txt" ]
  then ok "lvs: z3cmd: a front end that fails its self-test fails LVS, and the proof does not run"
  else bad "lvs: z3cmd's failing self-test"; cat "$l/run.txt"; fi
  lvs_is "z3cmd: a point differs" differ z3cmd "FAIL (1 differ)" 1
  lvs_is "z3cmd: the proof killed by SIGKILL" killed z3cmd "BLOCKED (killed: SIGKILL, exit 137)" 0
  lvs_is "z3cmd: points the solver did not decide (a front end killed)" unknown z3cmd "BLOCKED (1 unknown, none differ)" 0
else
  bad "lvs: no timeout command here (coreutils)"
fi

[ "$fails" = 0 ] || { echo "openxc7-flow-test.sh: $fails failures"; exit 1; }
echo "openxc7-flow-test.sh: PASS"
