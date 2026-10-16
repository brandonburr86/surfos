# SurfCPU: notes for AI sessions

SurfCPU is a custom RV32IM RISC-V CPU in Verilog plus its SoC, boot firmware (SurfBoot) and a
RISC-V port of SurfOS, for the Digilent Arty A7. It lives on branch `surfcpu` (worktree
`06-restoration-surfcpu`), apart from `ai-dev`. The owner's brief is
`docs/AGENT-INSTRUCTIONS.md`; the running record is `docs/PROGRESS.md`. The repository's
root `CLAUDE.md` still applies to the x86 kernel, which must keep building and passing its tests.

## Working rules

* Tools run in the `surfcpu-toolchain` container: `make -C surfcpu <target>` from the Mac, or
  `surfcpu/devenv/run.sh <cmd>`. Install nothing on the Mac. `make doctor` shows the state.
* The contracts are `spec/soc.toml` (generated into `rtl/generated/`, `firmware/include/surfcpu/soc.h`,
  `firmware/ld/`, `docs/MEMORY-MAP.md`: never edit those by hand; `make gen`), `docs/SURFBUS.md`,
  `docs/ISA.md`, `docs/BOOT-ABI.md`, `docs/MODULES.md`. Change them deliberately, bump the
  version they carry, and update every user in the same change.
* Report simulation, synthesis, programming and physical results separately; never call a
  milestone done while its physical test is outstanding; never invent tool output.
* Never write a disk device or format a card; card tests use image files. `deploy-sd` needs an
  explicit mount point and never formats. Never program the configuration flash without the
  owner's explicit go-ahead. Never put credentials in files.
* Keep evidence short, as `docs/evidence/<area>/*.txt` (the repository ignores `*.log`).
* Commits: small, one topic, plain messages, `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  Do not push, and do not merge `surfcpu` into `ai-dev` without the owner's word.
