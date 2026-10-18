# SurfCPU open-source bitstream flow: the step runner and placement and routing
# File: openxc7-steps.sh Date: 10/18/26 (SurfGPU)
#
# Sourced by tools/openxc7-build.sh (and by tools/openxc7-flow-test.sh, which runs place_and_route
# with a stand-in nextpnr). Uses the flow's variables: dry, out, top, part, chipdb, threads, xdc
# and pnr_timeout.
#
#   run <step> <log> <command...>   time a step, keep its output in $out/<log>, stop on failure
#   place_and_route <seed>...       nextpnr with each seed in turn until one routes with every
#                                   clock meeting its constraint; pnr_ok=1 then
#
# Each attempt runs under `timeout -k 60 $pnr_timeout`: nextpnr's router2 has no iteration limit,
# and a placement it cannot route can keep it busy for hours with its overuse growing (docs/GPU.md
# section 12.4), so without a limit the next seed would never be tried. An attempt stopped there
# counts as failed, "timeout" in seeds.txt, and the next seed is tried. Within the limit the
# command and its outputs are what they were before the limit existed.

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

# Placement and routing: the settings openXC7/nextpnr's CI uses for hardware-tested designs.
# With a list of seeds, each in turn until nextpnr succeeds: it fails when routing does not
# converge or a clock misses its constraint after routing, the same gate for every seed, and an
# attempt that reaches the time limit fails too. Each attempt's log is kept (nextpnr-seed<n>.log)
# and summarized in seeds.txt; nextpnr.log is the last attempt's, the one that succeeded if any did.
pnr_summary() {   # SEED STATUS: one line on that attempt, from nextpnr.log
  awk -v s="$1" -v st="$2" '/Max frequency for clock/ { c = $0; sub(/.*clock +/, "", c); sub(/:.*/, "", c)
        l = $0; sub(/^(Info|ERROR|Warning): */, "", l); sub(/^Max frequency for clock +/, "", l); gsub(/ +/, " ", l)
        last[c] = l; if (!(c in seen)) { seen[c] = 1; order[n++] = c } }
      /Routing complete/ { routed = 1 }
      END { printf "seed %s: %s, %s;", s, st == 0 ? "pass" : "fail (nextpnr exit " st ")", routed ? "routed" : "not routed"
            for (i = 0; i < n; i++) printf " %s%s", last[order[i]], i < n - 1 ? "," : ""
            printf "\n" }' "$out/nextpnr.log"
}

pnr_timeout_summary() {   # SEED STATUS: the line for an attempt stopped at the time limit
  it=$(grep -E 'iter=[0-9]+ ' "$out/nextpnr.log" | tail -n 1 | sed -n 's/.*\(iter=[0-9]*\) .*\(overused=[0-9]*\).*/\1, \2/p')
  echo "seed $1: timeout (nextpnr stopped after ${pnr_timeout} s, exit $2), not routed;${it:+ router at $it}"
}

place_and_route() {   # SEED...: see the top of this file; sets pnr_ok
  pnr_ok=0
  for seed in "$@"; do
    if run pnr nextpnr.out timeout -k 60 "$pnr_timeout" nextpnr-himbaechel --device "$part" --chipdb "$chipdb" \
         --threads "$threads" --json "$out/$top.json" -o xdc="$xdc" -o fasm="$out/$top.fasm" \
         -o placement="$out/placement.json" --router router2 -o hold-fix --placer-heap-timingweight 60 \
         --seed "$seed" --report "$out/report.json" --detailed-timing-report -l "$out/nextpnr.log"
    then st=0; else st=$?; fi
    [ "$dry" = 1 ] && { pnr_ok=1; break; }
    [ -f "$out/nextpnr.log" ] || : > "$out/nextpnr.log"
    cp "$out/nextpnr.log" "$out/nextpnr-seed$seed.log"
    case "$st" in
      124|137) pnr_timeout_summary "$seed" "$st" ;;
      *) pnr_summary "$seed" "$st" ;;
    esac | tee -a "$out/seeds.txt" | sed 's/^/openxc7-build.sh: placement and routing, /'
    [ "$st" = 0 ] && { pnr_ok=1; break; }
  done
  return 0
}
