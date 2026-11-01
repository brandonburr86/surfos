#!/bin/sh
# SurfCPU board track: what the board should print with the DDR SurfBoot design when the DDR3L
# works, SurfBoot through to SurfOS's shell, from simulation with the C++ DDR model (make
# board-uart-expect-ddr, in the toolchain container, from surfcpu/; docs/BOARD.md section 12).
#
#   SOC_SRC=... SOC_INC=... SIM_CPP=... sh sim/board/uart_expect_ddr.sh BOARD
#
# The soc track's surfsim harness (the whole surfcpu_soc in Verilator with the C++ models of the
# DDR memory, the UART, the SD card, the keyboard and the screen) built with the SoC configured
# as surfcpu_arty_a7_top configures it with ARTY_HAS_DDR on BOARD: HAS_DDR=1, DDR_SWINIT=1,
# BOARD's sysinfo BOARD_ID, the spec's clocks, this commit's BUILD_ID and BUILD_FLAGS. The DDR
# model (sim/models/ddr_model.h) stands in for the bridge, the adapter and LiteDRAM's data path,
# and LiteDRAM's register model (firmware/tests/ddrc_model.h, surfsim --ddr-swinit) for its
# control registers: SurfBoot initializes and levels it as on the board, and calibration is
# done when it has (CALIB_FAIL after the bridge's 1.5 s otherwise). It is the only DDR model
# here fast enough for a kernel boot (sim/board/ddr_top_sim.sh runs the DDR top with LiteDRAM's
# own controller, about 100 times slower). SurfBoot (build/fw/
# surfboot.mem) loads the Arty kernel (../build/riscv32/arty_a7/SURFOS.BIN, make build-kernel)
# from a card made by tools/mkcard.py and SurfOS boots to its shell prompt. The transcript goes
# to build/sim/board/uart_expect_ddr/; exit 1 if the prompt does not come.
set -eu

board=${1:-arty_a7_100t}
kernel=../build/riscv32/arty_a7/SURFOS.BIN
out=build/sim/board/uart_expect_ddr
[ -f "$kernel" ] || { echo "uart_expect_ddr: no $kernel (make build-kernel)" >&2; exit 2; }
rm -rf "$out"
mkdir -p "$out"
hdr=firmware/include/surfcpu/soc.h
up=$(echo "$board" | tr 'a-z' 'A-Z')
board_id=$(awk -v n="SURF_BOARD_$up" '$2 == n { sub(/u$/, "", $3); print $3 }' "$hdr")
[ -n "$board_id" ] || { echo "uart_expect_ddr: no SURF_BOARD_$up in $hdr" >&2; exit 2; }
sys_hz=$(awk '$2 == "SURF_SYS_HZ" { print $3 }' "$hdr")
pix_hz=$(awk '$2 == "SURF_PIX_HZ" { print $3 }' "$hdr")
id=$(printf '%.8s' "${SURFCPU_BUILD_ID:-0}" | grep -E '^[0-9a-f]{8}$' || echo 00000000)
flags=$(( ${SURFCPU_BUILD_DIRTY:-1} & 1 ))
echo "uart_expect_ddr: surfcpu_soc as on $board with DDR: BOARD_ID $board_id, HAS_DDR 1, DDR_SWINIT 1, BUILD_ID $id, BUILD_FLAGS $flags"

# shellcheck disable=SC2086
verilator --cc --exe --build -j 4 -O3 --x-assign fast --top-module surfcpu_soc +define+SURFCPU_RVFI \
    $SOC_INC -GHAS_DDR=1 -GDDR_SWINIT=1 "-GBOARD_ID=32'h$(printf '%08x' "$board_id")" "-GBUILD_ID=32'h$id" \
    "-GBUILD_FLAGS=32'h$(printf '%08x' "$flags")" -GBUILD_TIME=0 -GSYS_HZ="$sys_hz" -GPIX_HZ="$pix_hz" \
    -CFLAGS "-std=c++17 -O2 -DSURFCPU_RVFI -I$PWD/sim/models -I$PWD/firmware/include" \
    --Mdir "$out/obj" -o surfsim_board $SOC_SRC $SIM_CPP > "$out/build.txt" 2>&1 ||
    { tail -30 "$out/build.txt"; exit 1; }
cp "$out/obj/surfsim_board" "$out/surfsim_board"
rm -rf "$out/obj"

printf 'ascii\n' > "$out/CMDLINE.TXT"
python3 tools/mkcard.py "$out/card.img" --size 64M --standard-dirs --label SURFOS \
    --file "/BOOT/SURFOS.BIN=$kernel" --file "/BOOT/CMDLINE.TXT=$out/CMDLINE.TXT" > "$out/mkcard.txt"
if "$out/surfsim_board" --rom build/fw/surfboot.mem --sd "$out/card.img" --ddr-swinit --ddr-calib 37500000 \
       --max-cycles 1500000000 --quiet --transcript "$out/surfboot.txt" --vga-dir "$out" \
       --expect 'SURF ~' > "$out/surfboot.log" 2>&1
then echo "uart_expect_ddr: SurfBoot loaded SurfOS, which reached its shell prompt"
else echo "uart_expect_ddr: SurfOS did not reach its shell prompt" >&2; tail -20 "$out/surfboot.log" >&2; exit 1; fi
rm -f "$out/card.img"
grep -h "cycles\|matched" "$out/surfboot.log" | sed 's/^/uart_expect_ddr: /' | head -6
