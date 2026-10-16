# Targets that run on the Mac itself: they talk to the board over USB, to a mounted card,
# to a remote build host, or to Docker. Everything else goes into the container (Makefile).

PYTHON_HOST ?= python3
BOARD       ?= arty_a7_100t

.PHONY: help doctor image program-volatile program-flash deploy-sd build-fpga

help:
	@sed -n '3,15p' Makefile | sed 's/^# //'
	@echo
	@echo "Setup: make image (build the toolchain container), make doctor (check everything)."
	@echo "Details: docs/COMMANDS.md"

doctor:
	@$(PYTHON_HOST) tools/doctor.py $(if $(JSON),--json)

image:
	devenv/build-image.sh

# Board, card and build-host commands: tools/*.sh and tools/*.py on the Mac (docs/COMMANDS.md).
program-volatile:
	@tools/program.sh volatile $(BOARD) $(BIT)

program-flash:
	@tools/program.sh flash $(BOARD) $(BIT)

deploy-sd:
	@$(PYTHON_HOST) tools/deploy-sd.py $(if $(MOUNT),--mount "$(MOUNT)") $(DEPLOY_ARGS)

build-fpga:
	@tools/fpga-build.sh $(BOARD)
