# SurfCPU: the command interface (docs/COMMANDS.md).
#
#   make help                 the list of commands
#   make doctor               what this Mac, Docker, the board and the card look like (Mac)
#   make lint                 Verilator lint of all RTL
#   make test-cpu             CPU unit, ISA, trap, pipeline and reference-model tests
#   make test-soc             SoC, peripheral and whole-system simulations
#   make test-fw              SurfBoot and tool tests on the host (card images, packer, CRC)
#   make build-rom            SurfBoot -> build/fw/surfboot.{elf,bin,mem}
#   make build-kernel         SurfOS for riscv32/arty_a7 -> ../build/riscv32/arty_a7/SURFOS.BIN
#   make build-fpga           bitstream on the configured build host (Mac wrapper; docs/BUILD-HOST.md)
#   make program-volatile     load the bitstream into the FPGA over USB/JTAG (lost at power-off)
#   make deploy-sd MOUNT=/Volumes/<card>   install SURFOS.BIN and system files on a mounted card
#   make check                every automated pre-release check
#
# Run it from surfcpu/ or as `make -C surfcpu <target>` from the repository root. On the Mac,
# targets that need the toolchain re-run themselves in the surfcpu-toolchain container
# (devenv/run.sh); targets that touch the board, the card or a remote build host run on the Mac.
# Command-line variables (BOARD=..., V=1, ...) are passed through.

.DEFAULT_GOAL := help

ifeq ($(SURFCPU_IN_CONTAINER),1)
include mk/common.mk
include $(sort $(wildcard mk/area-*.mk))
else
include mk/host.mk
# Anything mk/host.mk does not define runs inside the container, one goal at a time.
.DEFAULT:
	@devenv/run.sh make --no-print-directory $@ $(MAKEOVERRIDES)
endif
