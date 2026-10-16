# Targets that run on the Mac itself: they talk to the board over USB, to a mounted card,
# to a remote build host, or to Docker. Everything else goes into the container (Makefile).
#
# Paths may hold spaces (the project lives in "SurfOS Restoration Project"): BIT, MOUNT, PORT,
# LOG, RELEASE, REPORT, REFERENCE, EVIDENCE and COMMIT each reach their tool as one argument, so
# quote them on the command line as usual: make program-flash BIT="/Users/me/My Releases/x.bit".
# ARGS and DEPLOY_ARGS are lists of arguments, split at spaces; a path with spaces inside them
# needs quotes of its own: make deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS='--image "/Users/me/My
# Releases/SURFOS.BIN"'. No value may hold a double quote, a backslash, a dollar sign or a backquote.

PYTHON_HOST ?= python3
BOARD       ?= arty_a7_100t

.PHONY: help doctor image openxc7-image program-volatile program-flash deploy-sd build-fpga \
        board-test board-test-selftest uart-list uart-console release release-verify release-check \
        fresh-checkout release-selftest

help:
	@sed -n '3,15p' Makefile | sed 's/^# //'
	@echo
	@echo "Setup: make image (build the toolchain container), make openxc7-image (the open-source"
	@echo "bitstream flow's image, for make build-fpga FLOW=openxc7), make doctor (check everything)."
	@echo "Details: docs/COMMANDS.md"
	@echo "Board tests: make board-test M=m1 (m1 m3 m5 m7 m8 m9; docs/PHYSICAL-TESTS.md), make uart-console."
	@echo "Release: make release [PRERELEASE=1], make release-verify RELEASE=build/release/<name>,"
	@echo "make fresh-checkout REFERENCE=build/release/<name> (docs/RELEASE.md)."

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
	@tools/program.sh volatile "$(BOARD)" "$(BIT)"

program-flash:
	@tools/program.sh flash "$(BOARD)" "$(BIT)"

deploy-sd:
	@$(PYTHON_HOST) tools/deploy-sd.py $(if $(MOUNT),--mount "$(MOUNT)") $(DEPLOY_ARGS)

build-fpga:
	@tools/fpga-build.sh "$(BOARD)"

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
	@$(PYTHON_HOST) tools/board-test.py $(M) --port "$(or $(PORT),auto)" --board "$(BOARD)" $(ARGS)
endif

uart-list:
	@$(PYTHON_HOST) tools/uart.py list

uart-console:
	@$(PYTHON_HOST) tools/uart.py console "$(or $(PORT),auto)" $(if $(LOG),--log "$(LOG)")

board-test-selftest:
	@$(PYTHON_HOST) sim/soc/tools/kit_test.py --set fake --work build/os/kit-mac

# The reproducible release (milestone 9, docs/RELEASE.md), on the Mac; the builds run in the images.
#   make release [PRERELEASE=1] [SKIP_DDR=1] [DESIGNS=a,b] [BOARDS=a,b] [FORCE=1]
#                    from a clean tree only, SOURCE_DATE_EPOCH = the commit's time: every artifact,
#                    SHA256SUMS and MANIFEST in build/release/<version>-<commit>/ (a pre-release, named
#                    <version>-pre-<commit>, only with PRERELEASE=1)
#   make release-verify RELEASE=build/release/<name> [DESIGNS=..] [BOARDS=..] [KEEP=1] [REPORT=file]
#                    rebuild from a pristine copy of the release's commit and compare byte for byte
#   make release-check RELEASE=build/release/<name>   its files against SHA256SUMS and MANIFEST
#   make fresh-checkout [COMMIT=..] [REFERENCE=build/release/<name>] [EVIDENCE=file] [KEEP=1]
#                       [ARGS="PRERELEASE=1 SKIP_DDR=1"]
#                    clone, follow docs/RELEASE.md, make the release, compare it with REFERENCE
#   make release-selftest   tools/release-test.py with the Mac's own python3 (make check runs it in the
#                    container; the release itself runs on the Mac, whose paths and Python differ)
release:
	@$(PYTHON_HOST) tools/release.py make $(if $(filter 1,$(PRERELEASE)),--prerelease) \
	    $(if $(filter 1,$(SKIP_DDR)),--skip-ddr) $(if $(DESIGNS),--designs "$(DESIGNS)") \
	    $(if $(BOARDS),--boards "$(BOARDS)") $(if $(filter 1,$(FORCE)),--force)

release-verify:
ifeq ($(RELEASE),)
	@echo "release-verify: name the release: make release-verify RELEASE=build/release/<version>-<commit>"; exit 2
else
	@$(PYTHON_HOST) tools/release.py verify "$(RELEASE)" $(if $(DESIGNS),--designs "$(DESIGNS)") \
	    $(if $(BOARDS),--boards "$(BOARDS)") $(if $(filter 1,$(KEEP)),--keep) $(if $(REPORT),--report "$(REPORT)")
endif

release-check:
ifeq ($(RELEASE),)
	@echo "release-check: name the release: make release-check RELEASE=build/release/<version>-<commit>"; exit 2
else
	@$(PYTHON_HOST) tools/release.py check "$(RELEASE)"
endif

fresh-checkout:
	@tools/fresh-checkout.sh $(if $(COMMIT),--commit "$(COMMIT)") $(if $(REFERENCE),--reference "$(REFERENCE)") \
	    $(if $(EVIDENCE),--evidence "$(EVIDENCE)") $(if $(filter 1,$(KEEP)),--keep) -- $(ARGS)

release-selftest:
	@mkdir -p build/release-test; $(PYTHON_HOST) tools/release-test.py > build/release-test/mac.txt 2>&1; s=$$?; \
	    grep -E '^(Ran|OK|FAILED|FAIL:|ERROR:)' build/release-test/mac.txt; exit $$s
