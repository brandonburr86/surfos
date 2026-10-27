# SurfCPU open-source bitstream flow: the step runner, placement and routing, and the checks of a
# routed build
# File: openxc7-steps.sh Date: 10/18/26 (SurfGPU); the checks and the LVS routes 10/27/26 (flow)
#
# Sourced by tools/openxc7-build.sh (and by tools/openxc7-flow-test.sh, which runs place_and_route
# with stand-in nextpnrs and lvs_prove with a stand-in lvs_equiv, and by
# devenv/openxc7/test/lvs_routes_inner.sh, for lvs_verdict). Uses the flow's variables:
# dry, out, top, part, chipdb, threads, xdc, tools, pnr_timeout and min_slack (the design's
# "CLOCK=NS" words; may be empty); the checks also check, device, chipdie, family, db, dbroot,
# lvs, lvs_timeout and lvs_solver, and lvs_inputs_hook (a recheck's: a function run between the
# LVS inputs' extraction and the proof; unset in a build).
#
#   run <step> <log> <command...>   time a step, keep its output in $out/<log>, stop on failure
#   place_and_route <seed>...       nextpnr with each seed in turn until one routes with every
#                                   clock meeting its constraint with the slack the rule wants;
#                                   sets pnr_ok, seed_used, slack_state (PASS or THIN), slack_note
#   post_route_checks               the checks of a routed build (clocks, bitstream, round trip,
#                                   block RAM, LVS), the build's and the recheck's (--recheck):
#                                   sets result_clk, result_bit, result_rt, result_bram,
#                                   result_lvs, and fail=1 on a FAIL
#   lvs_prove                       the LVS proof by the route lvs_solver names (below)
#   lvs_verdict <status> <report>   the LVS result for lvs_equiv's exit status and report
#
# The time limit. Each attempt runs under `timeout --foreground --verbose -k 60 $pnr_timeout`:
# nextpnr's router2 has no iteration limit, and a placement it cannot route can keep it busy for
# hours with its overuse growing (docs/GPU.md section 12.4), so without a limit the next seed would
# never be tried. An attempt stopped there counts as failed ("timeout" in seeds.txt) and the next
# seed is tried. timeout says when it stopped one (--verbose), which tells a timeout from nextpnr
# killed by something else, such as the kernel's out-of-memory killer ("killed", with the time it
# ran). --foreground leaves nextpnr in the terminal's process group, so Ctrl-C stops it at once;
# nextpnr starts no process of its own, so the limit's signals still reach everything it runs.
# Within the limit the command and its outputs are what they were before the limit existed.
#
# The slack rule. nextpnr passes a placement whose clocks meet their constraints by any margin; the
# flow wants each clock's slack at least max(0.5 ns, 4 % of its period), or what the design list
# sets for it (min-slack CLOCK NS), as devenv/openxc7/slackcheck.py judges report.json. A routed
# placement with less is "thin": its outputs are put aside and the next seed is tried. When every
# seed is thin, the one with the most margin is kept and the build is marked THIN, which make
# release refuses without PRERELEASE=1 (docs/OPEN-FLOW.md section 3).

run() {   # run <step> <log> <command...>: time it, keep its output, stop on failure
  step=$1 log=$2; shift 2
  if [ "$dry" = 1 ]; then echo "[$step] $*"; return 0; fi
  t0=$(date +%s.%N)
  if "$@" > "$out/$log" 2>&1; then status=0; else status=$?; fi
  t1=$(date +%s.%N)
  awk -v s="$step" -v a="$t0" -v b="$t1" -v r="$([ $status = 0 ] && echo ok || echo "FAILED ($status)")" \
    'BEGIN { printf "%-12s %8.2f s  %s\n", s, b - a, r }' >> "$out/times.txt"
  if [ $status != 0 ]; then
    echo "openxc7-build.sh: $step failed (exit $status); the end of $out/$log:" >&2
    tail -n 25 "$out/$log" >&2
    [ -z "$(tail -c 1 "$out/$log")" ] || echo >&2   # its last line left open (a proof stopped mid-line)
    return $status
  fi
}

pnr_grace=${pnr_grace:-60}        # how long nextpnr has to end after the limit's TERM, before KILL

# Placement and routing: the settings openXC7/nextpnr's CI uses for hardware-tested designs.
# With a list of seeds, each in turn until nextpnr succeeds with enough slack: it fails when
# routing does not converge or a clock misses its constraint after routing, the same gate for
# every seed, and an attempt that reaches the time limit, or is killed, fails too. Each attempt's
# log is kept (nextpnr-seed<n>.log, slack-seed<n>.txt) and summarized in seeds.txt; nextpnr.log is
# the attempt the flow goes on with.
pnr_summary() {   # SEED STATUS [WORD]: one line on that attempt, from nextpnr.log (WORD: a routed one's)
  awk -v s="$1" -v st="$2" -v w="${3:-pass}" '/Max frequency for clock/ { c = $0; sub(/.*clock +/, "", c); sub(/:.*/, "", c)
        l = $0; sub(/^(Info|ERROR|Warning): */, "", l); sub(/^Max frequency for clock +/, "", l); gsub(/ +/, " ", l)
        last[c] = l; if (!(c in seen)) { seen[c] = 1; order[n++] = c } }
      /Routing complete/ { routed = 1 }
      END { printf "seed %s: %s, %s;", s, st == 0 ? w : "fail (nextpnr exit " st ")", routed ? "routed" : "not routed"
            for (i = 0; i < n; i++) printf " %s%s", last[order[i]], i < n - 1 ? "," : ""
            printf "\n" }' "$out/nextpnr.log"
}

pnr_routed() {    # what nextpnr.log says of routing, for an attempt that was stopped
  if grep -q 'Routing complete' "$out/nextpnr.log"; then
    echo "routed, but stopped before nextpnr finished (its outputs are not used)"
  else
    it=$(grep -E 'iter=[0-9]+ ' "$out/nextpnr.log" | tail -n 1 | sed -n 's/.*\(iter=[0-9]*\) .*\(overused=[0-9]*\).*/\1, \2/p')
    echo "not routed;${it:+ router at $it}"
  fi
}

pnr_timeout_summary() {   # SEED STATUS: the line for an attempt stopped at the time limit
  echo "seed $1: timeout (nextpnr stopped at the limit of ${pnr_timeout} s, exit $2), $(pnr_routed)" | sed 's/;$//'
}

pnr_killed_summary() {    # SEED STATUS SECONDS: nextpnr ended by a signal that was not the limit's
  sig=$(( $2 - 128 ))
  case "$sig" in
    9)  why="SIGKILL, not from the time limit: the out-of-memory killer?" ;;
    11) why="SIGSEGV" ;;
    6)  why="SIGABRT" ;;
    *)  why="signal $sig" ;;
  esac
  echo "seed $1: killed (exit $2: $why) after $3 s, $(pnr_routed)" | sed 's/;$//'
}

slack_args() {    # the design's min-slack words, as slackcheck.py's options
  for w in $min_slack; do printf -- '--min %s ' "$w"; done
}

place_and_route() {   # SEED...: see the top of this file
  pnr_ok=0 seed_used= slack_state= slack_note= best= best_margin= best_note=
  rm -rf "$out"/thin-seed*
  for seed in "$@"; do
    pnr_t0=$(date +%s)
    # shellcheck disable=SC2086
    if run pnr nextpnr.out timeout --foreground --verbose -k "$pnr_grace" "$pnr_timeout" nextpnr-himbaechel \
         --device "$part" --chipdb "$chipdb" --threads "$threads" --json "$out/$top.json" -o xdc="$xdc" \
         -o fasm="$out/$top.fasm" -o placement="$out/placement.json" --router router2 -o hold-fix \
         --placer-heap-timingweight 60 --seed "$seed" --report "$out/report.json" --detailed-timing-report \
         -l "$out/nextpnr.log"
    then st=0; else st=$?; fi
    if [ "$dry" = 1 ]; then
      # shellcheck disable=SC2046
      run slack "slack-seed$seed.txt" python3 -I "$tools/slackcheck.py" --report "$out/report.json" $(slack_args) \
        --out "$out/slack-seed$seed.txt"
      pnr_ok=1 seed_used=$seed slack_state=PASS
      break
    fi
    secs=$(( $(date +%s) - pnr_t0 ))
    [ -f "$out/nextpnr.log" ] || : > "$out/nextpnr.log"
    cp "$out/nextpnr.log" "$out/nextpnr-seed$seed.log"
    verdict=
    if grep -q '^timeout: sending signal' "$out/nextpnr.out" 2>/dev/null; then
      line=$(pnr_timeout_summary "$seed" "$st")
    elif [ "$st" -gt 128 ] && [ "$st" -lt 160 ]; then
      line=$(pnr_killed_summary "$seed" "$st" "$secs")
    elif [ "$st" != 0 ]; then
      line=$(pnr_summary "$seed" "$st")
    else
      # shellcheck disable=SC2046
      # only slackcheck's own "thin" (exit 1 with that verdict) keeps a placement aside as thin; a
      # placement it cannot judge, one that misses a constraint, or a failure of the script, fails
      if sl=$(python3 -I "$tools/slackcheck.py" --report "$out/report.json" $(slack_args) \
                --out "$out/slack-seed$seed.txt" 2> "$out/slack-seed$seed.err")
      then verdict=pass
      else
        st_sl=$?
        if [ "$st_sl" = 1 ] && [ "${sl%% *}" = thin ]; then verdict=thin; else verdict=bad; fi
      fi
      read -r _ sclk sslack swant smargin <<EOF
$sl
EOF
      case "$verdict" in
        pass) line="$(pnr_summary "$seed" 0 pass); slack: least margin $sclk $sslack ns, wants $swant" ;;
        thin) line="$(pnr_summary "$seed" 0 thin); slack: $sclk $sslack ns, wants $swant"
              mkdir -p "$out/thin-seed$seed"
              for f in "$top.fasm" placement.json report.json nextpnr.log; do
                if [ -f "$out/$f" ]; then cp "$out/$f" "$out/thin-seed$seed/"; fi
              done
              if [ -z "$best" ] || awk -v a="$smargin" -v b="$best_margin" 'BEGIN { exit !(a + 0 > b + 0) }'; then
                best=$seed best_margin=$smargin best_note="$sclk $sslack ns, wants $swant"
              fi ;;
        *)    why=$(tail -n 1 "$out/slack-seed$seed.err" 2>/dev/null)
              line=$(pnr_summary "$seed" 0 "fail (the slack rule: ${why:-slackcheck exit $st_sl, no reason given})") ;;
      esac
    fi
    echo "$line" | tee -a "$out/seeds.txt" | sed 's/^/openxc7-build.sh: placement and routing, /'
    if [ "$verdict" = pass ]; then
      pnr_ok=1 seed_used=$seed slack_state=PASS slack_note="least margin $sclk $sslack ns, wants $swant"
      break
    fi
  done
  if [ "$pnr_ok" = 0 ] && [ -n "$best" ]; then
    for f in "$top.fasm" placement.json report.json nextpnr.log; do
      rm -f "$out/$f"
      if [ -f "$out/thin-seed$best/$f" ]; then cp "$out/thin-seed$best/$f" "$out/$f"; fi
    done
    pnr_ok=1 seed_used=$best slack_state=THIN
    slack_note="$best_note; every seed tried was thin or failed, this one has the most margin"
    echo "kept: seed $best, the thin placement with the most margin (THIN)" | tee -a "$out/seeds.txt" |
      sed 's/^/openxc7-build.sh: placement and routing, /'
  fi
  rm -rf "$out"/thin-seed*
  return 0
}

# ------------------------------------------------------------------ the checks of a routed build
decode_bitstream() {   # Project X-Ray's bit2fasm: the finished bitstream back to FASM
  PYTHONWARNINGS=ignore::RuntimeWarning bit2fasm --db-root "$db" --part "$part" \
    --bitread "$(command -v bitread)" "$out/$top.bit" > "$out/$top.decoded.fasm"
}

# The routes of the LVS proof (lvs_solver: --lvs-solver, docs/OPEN-FLOW.md section 3). Both run the
# image's lvs_equiv (fasm2netlist 0948889f) on the same files with the same arguments, so it asks
# the same questions in the same order (a miter for each register, output and memory boundary bit,
# src/lvs/lvs_equiv.cpp); they differ in how a question reaches Z3 (src/lvs/solver.cpp):
#   libz3  lvs_equiv's default, the Z3 library it links: one incremental session for the whole run,
#          which keeps what it learnt from one question for the next and whose memory grows with
#          the run (the DDR+GPU design at bfaac48 passed the 3.5 GB cap at 41 % of its 21,000
#          questions, docs/GPU.md 16.11). The flow's default.
#   z3cmd  lvs_equiv --solver 'python3 -I devenv/openxc7/z3cmd.py' --format smt2: each question
#          written out with the whole network as SMT-LIB 2 and answered by a process of its own
#          (devenv/openxc7/z3cmd.py over the image's libz3), so its memory is a question's and goes
#          when the question is answered; slower (that design: 13 h 52 min, 2.6 GB at most). The
#          front end's self-test (a question it must prove, one it must not, one it must not
#          answer, run as lvs_equiv runs it) comes first: a front end that fails it fails LVS and
#          the proof does not run. Each question's line goes to lvs/questions.log (Z3CMD_LOG),
#          summed up at the end of check-lvs.txt.
lvs_prove() {     # the proof by the route lvs_solver names, under lvs_timeout: sets result_lvs, fail=1 on FAIL
  case "$lvs_solver" in
    libz3)
      if run lvs-prove check-lvs.txt timeout "$lvs_timeout" lvs_equiv --gold "$out/lvs/gold.v" \
           --gold-top "$top" --gate "$out/lvs/fabric.v" --gate-top fabric --placement "$out/placement.json" \
           --gold-json "$out/$top.json" --db "$db" --device "$chipdie"
      then st=0; else st=$?; fi ;;
    z3cmd)
      solver="python3 -I $tools/z3cmd.py"     # relative: lvs_equiv runs it from the flow's directory
      if ! run lvs-selftest lvs/z3cmd-selftest.txt python3 -I "$tools/z3cmd.py" --self-test "$solver"; then
        result_lvs="FAIL (the z3cmd self-test: lvs/z3cmd-selftest.txt)" fail=1
        return 0
      fi
      [ "$dry" = 1 ] || : > "$out/lvs/questions.log"
      if run lvs-prove check-lvs.txt env Z3CMD_LOG="$out/lvs/questions.log" timeout "$lvs_timeout" lvs_equiv \
           --gold "$out/lvs/gold.v" --gold-top "$top" --gate "$out/lvs/fabric.v" --gate-top fabric \
           --placement "$out/placement.json" --gold-json "$out/$top.json" --db "$db" --device "$chipdie" \
           --solver "$solver" --format smt2
      then st=0; else st=$?; fi
      if [ "$dry" = 0 ]; then
        endline "$out/check-lvs.txt"   # a proof stopped in the middle of a line leaves it open
        python3 -I "$tools/z3cmd.py" --summary "$out/lvs/questions.log" >> "$out/check-lvs.txt" 2>&1 || true
      fi ;;
    *)
      result_lvs="FAIL (no LVS solver $lvs_solver)" fail=1
      return 0 ;;
  esac
  if [ "$dry" = 1 ]; then result_lvs=PASS; return 0; fi
  result_lvs=$(lvs_verdict "$st" "$out/check-lvs.txt")
  case "$result_lvs" in FAIL*) fail=1 ;; esac
  return 0
}

endline() {   # FILE: end it with a newline when it has text and does not (what is appended starts a line)
  [ ! -s "$1" ] || [ -z "$(tail -c 1 "$1")" ] || echo >> "$1"
}

lvs_route_note() {   # what the manifest's "lvs solver" line says of the route (no parentheses)
  case "$lvs_solver" in
    libz3) echo "lvs_equiv's linked libz3, one incremental session" ;;
    z3cmd) echo "lvs_equiv --solver, one process a question: devenv/openxc7/z3cmd.py over the image's libz3" ;;
  esac
}

# The verdict. lvs_equiv (src/lvs/lvs_equiv.cpp) prints "  proved  ", "  DIFFER  " or "  unknown "
# and a point's name for each question, then "N proved, D differ, U unknown (Ts)", and ends with:
#   0  every point proved                                  PASS, if its count says 0 differ, 0 unknown
#                                                          and at least 1 proved, and no point's line
#                                                          says DIFFER; else FAIL. A count of 0 proved
#                                                          means nothing was compared: each output of
#                                                          a design is a point, every design the flow
#                                                          builds has outputs, and a proof of none is
#                                                          no proof
#   1  a point differs, or is unknown                      FAIL (D differ) when one differs;
#                                                          BLOCKED (U unknown, none differ) when the
#                                                          solver decided all but U: not proved, but
#                                                          nothing found to differ (a z3cmd process
#                                                          the memory cap kills is read as unknown)
#   2  a setup fault (a netlist it cannot parse, no solver, a database it cannot read): "not a
#      verdict about the design", but the check did not run                       FAIL
# and, through timeout: 124 at the limit, BLOCKED (timeout); 128+N ended by signal N, which is
# BLOCKED (killed) when it came from outside the proof (KILL, as the memory cap's out-of-memory
# killer sends, TERM, INT, HUP, QUIT, XCPU, XFSZ), like the limit, and FAIL for a crash (SEGV,
# ABRT, BUS, ILL, FPE, ...). A proof stopped after it printed a DIFFER is FAIL: a difference was
# found. Anything else is FAIL. BLOCKED is reported, not proved: tools/fpga-build.sh and
# tools/program.sh take only PASS; this script's exit status leaves BLOCKED alone, as it did.
lvs_verdict() {   # STATUS REPORT
  _sum=$(sed -n 's/^\([0-9][0-9]*\) proved, \([0-9][0-9]*\) differ, \([0-9][0-9]*\) unknown.*/\1 \2 \3/p' "$2" 2>/dev/null | tail -n 1)
  _ndiff=$(grep -c '^  DIFFER  ' "$2" 2>/dev/null) || _ndiff=${_ndiff:-0}
  # shellcheck disable=SC2086
  set -- "$1" $_sum
  _st=$1 _proved=${2:-} _differ=${3:-} _unknown=${4:-}
  case "$_st" in
    0) if [ "$_differ" = 0 ] && [ "$_unknown" = 0 ] && [ "$_ndiff" = 0 ]; then
         if [ -n "$_proved" ] && [ "$_proved" != 0 ]; then echo PASS
         else echo "FAIL (lvs_equiv ended 0 having proved no point: nothing was compared)"; fi
       else echo "FAIL (lvs_equiv ended 0 without a count of 0 differ and 0 unknown)"; fi
       return 0 ;;
    1) if [ -n "$_differ" ] && [ "$_differ" != 0 ]; then echo "FAIL ($_differ differ)"
       elif [ "$_ndiff" != 0 ]; then echo "FAIL ($_ndiff differ)"
       elif [ -n "$_unknown" ] && [ "$_unknown" != 0 ]; then echo "BLOCKED ($_unknown unknown, none differ)"
       else echo "FAIL (lvs_equiv ended 1 without a count of points that differ or are unknown)"; fi
       return 0 ;;
  esac
  if [ "$_ndiff" != 0 ]; then echo "FAIL ($_ndiff differ before the proof stopped, exit $_st)"; return 0; fi
  case "$_st" in
    2)   echo "FAIL (lvs_equiv could not run the proof: exit 2, see check-lvs.txt)" ;;
    124) echo "BLOCKED (timeout)" ;;
    129) echo "BLOCKED (killed: SIGHUP, exit 129)" ;;
    130) echo "BLOCKED (killed: SIGINT, exit 130)" ;;
    131) echo "BLOCKED (killed: SIGQUIT, exit 131)" ;;
    137) echo "BLOCKED (killed: SIGKILL, exit 137)" ;;
    143) echo "BLOCKED (killed: SIGTERM, exit 143)" ;;
    152) echo "BLOCKED (killed: SIGXCPU, exit 152)" ;;
    153) echo "BLOCKED (killed: SIGXFSZ, exit 153)" ;;
    *)   if [ "$_st" -gt 128 ] 2>/dev/null && [ "$_st" -lt 192 ]; then
           echo "FAIL (lvs_equiv crashed: signal $((_st - 128)), exit $_st)"
         else
           echo "FAIL (lvs_equiv exit $_st)"
         fi ;;
  esac
  return 0
}

post_route_checks() {   # the checks of a routed build in $out (see the top of this file)
  result_bit=skipped result_rt=skipped result_lvs=skipped result_clk=skipped result_bram=skipped
  [ "$check" = 1 ] || return 0
  # The clock managers' dividers against AMD's XAPP888 (LVS does not model MMCM/PLL).
  if run check-clk check-clocks.txt python3 $tools/cmtcheck.py --json "$out/$top.json" \
       --placement "$out/placement.json" --fasm "$out/$top.fasm"
  then
    if [ "$dry" = 0 ] && grep -q 'no MMCME2/PLLE2' "$out/check-clocks.txt"; then result_clk="none in the design"; else result_clk=PASS; fi
  else result_clk=FAIL; fail=1; fi
  # No ordinary net on the global clock network: no net through an unconfigured BUFGCTRL, no
  # clock line into a data input (docs/OPEN-FLOW.md 6.8). Its report goes into check-clocks.txt.
  if run check-gclk check-gclk.txt python3 -I $tools/gclkcheck.py --fasm "$out/$top.fasm"
  then :
  else result_clk=FAIL; fail=1; fi
  [ "$dry" = 1 ] || { echo; cat "$out/check-gclk.txt"; } >> "$out/check-clocks.txt"
  if run check-bit check-bitstream.txt python3 -I $tools/bitinfo.py --expect-device "$device" "$out/$top.bit"
  then result_bit=PASS; else result_bit=FAIL; fail=1; fi
  if run decode bit2fasm.log decode_bitstream &&
     run roundtrip check-roundtrip.txt python3 $tools/fasmcheck.py --db-root "$db" --part "$part" \
       "$out/$top.fasm" "$out/$top.decoded.fasm"
  then result_rt=PASS; else result_rt=FAIL; fail=1; fi
  # Block RAM contents: fasm2netlist reads them out of the decoded bitstream; they must be
  # the synthesized netlist's, block for block (LVS checks block RAMs only at their pins).
  if [ "$dry" = 1 ] || grep -Eq '"type": "RAMB(18|36)E1"' "$out/$top.json"; then
    if run bram-extract bram-extract.log fasm2netlist --fasm "$out/$top.decoded.fasm" --db "$dbroot" \
         --family "$family" --device "$chipdie" --out "$out/bram-extracted.v" &&
       run check-bram check-bram.txt python3 -I $tools/bramcheck.py --netlist "$out/$top.json" --top "$top" \
         --extracted "$out/bram-extracted.v"
    then result_bram=PASS; else result_bram=FAIL; fail=1; fi
  else
    result_bram="none in the design"
  fi
  # LVS (openXC7's lvs-equiv flow): the routed FASM read back into a fabric netlist by
  # fasm2netlist's tile model and proved equal to the synthesis, register by register, by the
  # route lvs_solver names (lvs_prove, lvs_verdict). A primitive the tile model lacks leaves it
  # BLOCKED, as a proof stopped by its limit or killed from outside does: not proved, which is
  # reported, but not a failure. Between extraction and proof, bramwe.py makes the netlist read
  # each RAMB18E1 byte write enable from the site pins the silicon reads it from (the extractor
  # reads them one to one; docs/OPEN-FLOW.md) and checks every pin it no longer reads: a
  # difference it finds fails LVS like one the proof finds.
  if [ "$lvs" = 1 ]; then
    if run lvs-gold lvs/gold.log yosys -q -p "read_json $out/$top.json; hierarchy -top $top; splitnets; select $top; write_verilog -noattr -norename -selected $out/lvs/gold.v" &&
       run lvs-extract lvs/tileverilog.log tileverilog --fasm "$out/$top.fasm" --db "$db" --device "$chipdie" \
         --xdc "$xdc" --part "$part" --placement "$out/placement.json" --out "$out/lvs/fabric.v" \
         --model-out "$out/lvs/tile_model.v" &&
       run lvs-bramwe lvs/bramwe.log python3 -I $tools/bramwe.py --fabric "$out/lvs/fabric.v" \
         --fasm "$out/$top.fasm" --report "$out/lvs/bramwe.txt"
    then
      # a recheck's: the inputs made again must be the build's before anything is proved of them
      # (tools/openxc7-build.sh --recheck sets lvs_inputs_hook, which stops the script when they are not)
      [ -z "${lvs_inputs_hook:-}" ] || "$lvs_inputs_hook"
      lvs_prove
      if [ "$dry" = 0 ]; then
        endline "$out/check-lvs.txt"
        cat "$out/lvs/bramwe.txt" >> "$out/check-lvs.txt"
        bram_diff=$(sed -n 's/^bramwe: .* \([0-9][0-9]*\) difference(s)$/\1/p' "$out/lvs/bramwe.txt")
        if [ "${bram_diff:-0}" != 0 ]; then result_lvs="FAIL ($bram_diff block RAM write enable difference(s))"; fail=1; fi
      fi
    else
      result_lvs="BLOCKED (extraction: see lvs/tileverilog.log)"
    fi
  fi
  return 0
}
