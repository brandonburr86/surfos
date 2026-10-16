#!/bin/sh
# SurfCPU board track: what the board should print on its serial port with the two SoC
# designs, from RTL simulation (make board-uart-expect, in the toolchain container, from
# surfcpu/; docs/BOARD.md section 12).
#
#   SOC_SRC=... SOC_INC=... SIM_CPP=... sh sim/board/uart_expect.sh BOARD
#
# The soc track's surfsim harness (the whole surfcpu_soc in Verilator with the C++ models of the
# UART, the SD card, the keyboard and the screen) is built here with the SoC configured as
# surfcpu_arty_a7_top configures it on BOARD: HAS_DDR=0, BOARD's sysinfo BOARD_ID, the spec's
# clocks, this commit's BUILD_ID and BUILD_FLAGS without the simulation bit (the bitstream's).
# Then:
#   1. the BRAM diagnostics (build/fw/bramdiag.mem) run to "BRAMDIAG: PASS" or FAIL;
#   2. SurfBoot (build/fw/surfboot.mem) with a card from tools/mkcard.py in the socket runs to
#      its monitor (no DDR: "SurfBoot stopped"), where info, sd and ls / are typed.
# The transcripts go to build/sim/board/uart_expect/ (and, for SurfBoot, vga/screen.ppm and
# vga/screen.txt: the last frame and its text decoded from the pixels); exit 1 if either run does
# not end as described.
set -eu

board=${1:-arty_a7_100t}
out=build/sim/board/uart_expect
rm -rf "$out"
mkdir -p "$out"
hdr=firmware/include/surfcpu/soc.h
up=$(echo "$board" | tr 'a-z' 'A-Z')
board_id=$(awk -v n="SURF_BOARD_$up" '$2 == n { sub(/u$/, "", $3); print $3 }' "$hdr")
[ -n "$board_id" ] || { echo "uart_expect: no SURF_BOARD_$up in $hdr" >&2; exit 2; }
sys_hz=$(awk '$2 == "SURF_SYS_HZ" { print $3 }' "$hdr")
pix_hz=$(awk '$2 == "SURF_PIX_HZ" { print $3 }' "$hdr")
id=$(printf '%.8s' "${SURFCPU_BUILD_ID:-0}" | grep -E '^[0-9a-f]{8}$' || echo 00000000)
flags=$(( ${SURFCPU_BUILD_DIRTY:-1} & 1 ))
echo "uart_expect: surfcpu_soc as on $board: BOARD_ID $board_id, HAS_DDR 0, BUILD_ID $id, BUILD_FLAGS $flags"

# shellcheck disable=SC2086
verilator --cc --exe --build -j 4 -O3 --x-assign fast --top-module surfcpu_soc +define+SURFCPU_RVFI \
    $SOC_INC -GHAS_DDR=0 "-GBOARD_ID=32'h$(printf '%08x' "$board_id")" "-GBUILD_ID=32'h$id" \
    "-GBUILD_FLAGS=32'h$(printf '%08x' "$flags")" -GBUILD_TIME=0 -GSYS_HZ="$sys_hz" -GPIX_HZ="$pix_hz" \
    -CFLAGS "-std=c++17 -O2 -DSURFCPU_RVFI -I$PWD/sim/models -I$PWD/firmware/include" \
    --Mdir "$out/obj" -o surfsim_board $SOC_SRC $SIM_CPP > "$out/build.txt" 2>&1 ||
    { tail -30 "$out/build.txt"; exit 1; }
cp "$out/obj/surfsim_board" "$out/surfsim_board"
rm -rf "$out/obj"
sim=$out/surfsim_board

# 1. the BRAM diagnostics
if "$sim" --rom build/fw/bramdiag.mem --max-cycles 1000000000 --quiet --transcript "$out/bramdiag.txt" \
       --expect 'BRAMDIAG: (PASS|FAIL)[^\n]*\n' > "$out/bramdiag.log" 2>&1 && grep -q 'BRAMDIAG: PASS' "$out/bramdiag.txt"
then echo "uart_expect: bramdiag: $(grep -o 'BRAMDIAG: PASS[^\r]*' "$out/bramdiag.txt")"
else echo "uart_expect: bramdiag did not pass" >&2; tail -20 "$out/bramdiag.log" >&2; exit 1; fi

# 2. SurfBoot with a card, to its monitor and through info, sd and ls /
printf 'A card made by tools/mkcard.py for the expected-output run.\n' > "$out/readme.txt"
mkdir -p "$out/vga"
python3 tools/mkcard.py "$out/card.img" --size 64M --standard-dirs --label SURFOS \
    --file "/BOOT/README.TXT=$out/readme.txt" > "$out/mkcard.txt"
if "$sim" --rom build/fw/surfboot.mem --sd "$out/card.img" --max-cycles 600000000 --quiet \
       --transcript "$out/surfboot.txt" --vga-dir "$out/vga" --settle 2000000 \
       --send 'surfboot> =info\r' --send 'surfboot> =sd\r' --send 'surfboot> =ls /\r' \
       --expect 'SurfBoot stopped[^\n]*\n' --expect 'ls /[\s\S]*surfboot> ' > "$out/surfboot.log" 2>&1
then echo "uart_expect: surfboot: $(grep -m 1 '^SurfBoot stopped' "$out/surfboot.txt" | tr -d '\r')"
else echo "uart_expect: SurfBoot did not reach its monitor and answer" >&2; tail -20 "$out/surfboot.log" >&2; exit 1; fi
grep -h "cycles\|matched" "$out/bramdiag.log" "$out/surfboot.log" | sed 's/^/uart_expect: /' | head -12
