# verif: reference-model checking (docs/VERIFICATION.md). Spike is the reference; the runner
# (tools/rvrun.py) compares the CPU testbench's retirement traces and signatures with it.
#
#   make test-ref        the checking machinery's own tests (on test-cpu): unit tests,
#                        directed programs through Spike, the CSR probe's known differences
#   make test-cpu-ref    the real comparison: build/cpu/surfcpu_tb against Spike (needs the core)

REF_BUILD   = $(BUILD)/ref
REF_LD      = sim/ref/common/link.ld
REF_CFLAGS  = -march=$(RV_ARCH) -mabi=$(RV_ABI) -nostdlib -nostartfiles -Wl,--no-warn-rwx-segments \
              -Isim/ref/common -T $(REF_LD)

REF_DIRECTED_SRC = $(sort $(wildcard sim/ref/directed/*.S))
REF_DIRECTED_ELF = $(patsubst sim/ref/directed/%.S,$(REF_BUILD)/directed/%.elf,$(REF_DIRECTED_SRC))

.PHONY: test-ref test-ref-unit ref-directed test-cpu-ref
test-cpu: test-ref

$(REF_BUILD)/directed/%.elf: sim/ref/directed/%.S sim/ref/common/ref_env.h $(REF_LD)
	@mkdir -p $(dir $@)
	$(Q)$(RV_CC) $(REF_CFLAGS) -o $@ $<

ref-directed: $(REF_DIRECTED_ELF)

test-ref-unit:
	$(Q)$(PYTHON) -m unittest discover -s sim/ref/tests -t sim/ref/tests $(if $(V),-v,-q)
	@echo "test-ref-unit: PASS"

test-ref: test-ref-unit ref-directed
	@echo "test-ref: PASS"
