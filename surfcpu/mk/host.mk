# Targets that run on the Mac itself: they talk to the board over USB, to a mounted card,
# to a remote build host, or to Docker. Everything else goes into the container (Makefile).

PYTHON_HOST ?= python3
BOARD       ?= arty_a7_100t

.PHONY: help doctor image openxc7-image program-volatile program-flash deploy-sd build-fpga \
        board-test board-test-selftest uart-list uart-console

help:
	@sed -n '3,15p' Makefile | sed 's/^# //'
	@echo
	@echo "Setup: make image (build the toolchain container), make openxc7-image (the open-source"
	@echo "bitstream flow's image, for make build-fpga FLOW=openxc7), make doctor (check everything)."
	@echo "Details: docs/COMMANDS.md"
	@echo "Board tests: make board-test M=m1 (m1 m3 m5 m7 m8 m9; docs/PHYSICAL-TESTS.md), make uart-console."

doctor:
	@$(PYTHON_HOST) tools/doctor.py $(if $(JSON),--json)

image:
	devenv/build-image.sh

# The open-source bitstream flow's image, surfcpu-openxc7 (docs/OPEN-FLOW.md; about 6 minutes,
# 661 MB). devenv/openxc7/build-image.sh --rm-builder afterwards frees its 2.4 GB build cache.
openxc7-image:
	devenv/openxc7/build-image.sh

# Board, card and build-host commands: tools/*.sh and tools/*.py on the Mac (docs/COMMANDS.md).
program-volatile:
	@tools/program.sh volatile $(BOARD) $(BIT)

program-flash:
	@tools/program.sh flash $(BOARD) $(BIT)

deploy-sd:
	@$(PYTHON_HOST) tools/deploy-sd.py $(if $(MOUNT),--mount "$(MOUNT)") $(DEPLOY_ARGS)

build-fpga:
	@tools/fpga-build.sh $(BOARD)

# The physical test kit (docs/PHYSICAL-TESTS.md), with the Mac's own python3 and nothing installed:
#   make board-test M=m1 [PORT=/dev/cu.usbserial-<serial>1] [ARGS="--lines 20"]
#                                  one milestone's acceptance test (m1 m3 m5 m7 m8 m9): it asks for each
#                                  step, checks what the board's serial port shows and writes
#                                  docs/evidence/physical/<M>-<UTC date>.txt; PORT defaults to the only
#                                  Arty UART plugged in
#   make uart-list                 the serial ports, and which one is the Arty's UART
#   make uart-console [PORT=...] [LOG=file]   a terminal on the board at 115200 8N1; Ctrl-] quits
#   make board-test-selftest       the kit against fake boards on this Mac's pseudo-terminals (15 s)
board-test:
ifeq ($(M),)
	@echo "board-test: name the milestone: make board-test M=m1 (m1, m3, m5, m7, m8 or m9; docs/PHYSICAL-TESTS.md)"; exit 2
else
	@$(PYTHON_HOST) tools/board-test.py $(M) --port "$(or $(PORT),auto)" --board $(BOARD) $(ARGS)
endif

uart-list:
	@$(PYTHON_HOST) tools/uart.py list

uart-console:
	@$(PYTHON_HOST) tools/uart.py console "$(or $(PORT),auto)" $(if $(LOG),--log "$(LOG)")

board-test-selftest:
	@$(PYTHON_HOST) sim/soc/tools/kit_test.py --set fake --work build/os/kit-mac
