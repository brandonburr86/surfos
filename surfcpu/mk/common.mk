# Shared settings and the aggregate targets (inside the toolchain container).
# Each area adds its own rules in mk/area-<area>.mk and hangs them on the aggregates below
# (for example `lint: lint-cpu`), so areas never edit each other's makefiles.

BUILD      ?= build
BOARD      ?= arty_a7_100t
V          ?=
Q           = $(if $(V),,@)
PYTHON     ?= python3

# Bare-metal RISC-V: one ISA and ABI for every object (docs/ISA.md). Earlier milestones that
# need another ISA set their own flags next to their rules.
RV_PREFIX  ?= riscv64-unknown-elf-
RV_CC       = $(RV_PREFIX)gcc
RV_OBJCOPY  = $(RV_PREFIX)objcopy
RV_OBJDUMP  = $(RV_PREFIX)objdump
RV_NM       = $(RV_PREFIX)nm
RV_SIZE     = $(RV_PREFIX)size
RV_ARCH     = rv32im_zicsr_zifencei
RV_ABI      = ilp32
RV_CFLAGS   = -march=$(RV_ARCH) -mabi=$(RV_ABI) -mcmodel=medlow -ffreestanding -fno-builtin \
              -nostdlib -fno-pic -fno-common -ffunction-sections -fdata-sections -Wall -Wextra -g
RV_LDFLAGS  = -march=$(RV_ARCH) -mabi=$(RV_ABI) -nostdlib -nostartfiles -Wl,--gc-sections
RV_LIBGCC   = -lgcc

VERILATOR  ?= verilator
IVERILOG   ?= iverilog
YOSYS      ?= yosys
VERILATOR_LINT = $(VERILATOR) --lint-only --quiet -Wall -Wno-DECLFILENAME -Irtl/generated -Irtl/cpu -Irtl/lib

# Build identity (devenv/run.sh passes these in from the Mac).
SURFCPU_BUILD_ID    ?= unknown
SURFCPU_BUILD_DIRTY ?= 1
BUILD_ID_HEX        = $(shell printf '%.8s' '$(SURFCPU_BUILD_ID)' | grep -E '^[0-9a-f]{8}$$' || echo 00000000)

.PHONY: all gen check-generated lint test-cpu test-soc test-fw test-os build-rom build-kernel \
        check clean versions
all: build-rom

gen:
	$(PYTHON) tools/socspec.py gen

check-generated:
	$(PYTHON) tools/socspec.py check

versions:
	@cat /etc/surfcpu-toolchain.txt

# Aggregates: prerequisites come from the area makefiles.
lint:
test-cpu:
test-soc:
test-fw:
test-os:
build-rom:
build-kernel:

# Every automated pre-release check (brief section 9). Areas add theirs with `check: ...`.
check: check-generated lint test-cpu test-soc test-fw
	@echo "check: all automated SurfCPU checks passed"

clean:
	rm -rf $(BUILD)/cpu $(BUILD)/soc $(BUILD)/fw $(BUILD)/sim $(BUILD)/smoke
