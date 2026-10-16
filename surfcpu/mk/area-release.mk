# The release tooling (milestone 9, docs/RELEASE.md): its tests, without Docker or a bitstream build.
# make release, release-verify and fresh-checkout themselves run on the Mac (mk/host.mk).
#
#   make test-release   tools/release-test.py: tools/bitfile.py (header stamp, QSPI flash image,
#                       configuration options), tools/release.py (its comparison and the causes it
#                       names; make in a scratch repository with stand-ins for the builds) and
#                       tools/program.sh's flash path on a release's bitstream (the fake openFPGALoader)

.PHONY: test-release
check: test-release

test-release:
	$(Q)mkdir -p $(BUILD)/release-test
	$(Q)$(PYTHON) tools/release-test.py > $(BUILD)/release-test/test.txt 2>&1 || { cat $(BUILD)/release-test/test.txt; exit 1; }
	@grep -E '^(Ran|OK)' $(BUILD)/release-test/test.txt | tr '\n' ' ' | sed 's/^/test-release: /'; echo
