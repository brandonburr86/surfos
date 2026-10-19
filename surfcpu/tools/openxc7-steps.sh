# SurfCPU open-source bitstream flow: the step runner and placement and routing
# File: openxc7-steps.sh Date: 10/18/26 (SurfGPU)
#
# Sourced by tools/openxc7-build.sh (and by tools/openxc7-flow-test.sh, which runs place_and_route
# with stand-in nextpnrs). Uses the flow's variables: dry, out, top, part, chipdb, threads, xdc,
# tools, pnr_timeout and min_slack (the design's "CLOCK=NS" words; may be empty).
#
#   run <step> <log> <command...>   time a step, keep its output in $out/<log>, stop on failure
#   place_and_route <seed>...       nextpnr with each seed in turn until one routes with every
#                                   clock meeting its constraint with the slack the rule wants;
#                                   sets pnr_ok, seed_used, slack_state (PASS or THIN), slack_note
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
