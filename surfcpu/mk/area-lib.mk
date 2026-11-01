# rtl/lib: shared building blocks (integrator-owned). Lint, and the Icarus testbench.

LIB_RTL = $(wildcard rtl/lib/*.v)

.PHONY: lint-lib test-lib
lint: lint-lib
test-soc: test-lib

lint-lib:
	$(Q)for f in $(LIB_RTL); do \
	  $(VERILATOR_LINT) --top-module $$(basename $$f .v) $$f || exit 1; done
	@echo "lint-lib: $(words $(LIB_RTL)) modules clean"

$(BUILD)/sim/lib/tb_lib.vvp: sim/lib/tb_lib.v $(LIB_RTL)
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -o $@ $^

test-lib: $(BUILD)/sim/lib/tb_lib.vvp
	$(Q)for s in 1 7 123; do vvp -n $< +seed=$$s > $(BUILD)/sim/lib/tb_lib.$$s.txt 2>&1; \
	  grep -q PASS $(BUILD)/sim/lib/tb_lib.$$s.txt || { cat $(BUILD)/sim/lib/tb_lib.$$s.txt; exit 1; }; done
	@echo "test-lib: PASS (seeds 1 7 123)"
