# The SurfCPU release (milestone 9)

Brief section 10, milestone 9: "Persistent QSPI configuration, recovery image selection, final
documentation and clean rebuild." This page is the procedure: making a release, checking that it
reproduces, rebuilding it from a fresh checkout, writing the configuration flash and recovering
from a bad one. Evidence: [`evidence/release/`](evidence/release/).

**Status (2026-10-17).** The tooling is written and tested without a board. The release of the
commit it was written at, `1.0.0-pre-bb066932` (18 artifacts), was made from a fresh checkout and
reproduces byte for byte from a pristine copy ([evidence](evidence/release/)). It is a
**pre-release**, because the DDR designs did not build then (their generated LiteDRAM core failed
its own hash check on a fresh checkout, section 4, and past that they missed nextpnr's timing),
so it has no bitstream that boots SurfOS from the card, and so no flash image to boot from. Both
causes are gone: LiteDRAM's generated files are the same in every checkout since d6f3ef8
(section 4), and both DDR designs pass every gate at 80 MHz (BOARD.md section 8). Built with the
fixed open flow they pass on the owner's A7-100T, where SurfBoot boots SurfOS from the card
(BOARD.md section 12); no release has been made with them yet. The owner's A7-100T has run
designs loaded over JTAG (milestones 1, 3 and 4, and SurfOS from the card; PROGRESS.md), but
**nothing has been written to its configuration flash and no release has been on it**: the board
steps of section 10 are pending. A review of the tooling followed (`review-fixes.txt`): the
release now builds in a directory of its own, ships only what it checked, and its checks and
comparisons refuse damaged releases; a reduced release made with it, `1.0.0-pre-147fe813` (the
baseline design on the 100T, 11 artifacts), reproduces byte for byte from a pristine copy too. A
release made before that fails today's `release-check` on one point (its `MANIFEST` names a boot
design it does not have), and its bitstreams predate the open flow's fix for BUFGCTRL
route-throughs (OPEN-FLOW.md section 6.8): make it again. Until the third GPU review (2026-10-18)
every `MANIFEST` said placer seed 1 for every design, whatever seed its bitstream was placed with
(`tools/release.py` read the seed at the top of a build record; `tools/fpga-build.sh` writes it in
the `[openxc7]` table): `1.0.0-f791b8a1`'s says 1 for the 100T `surfcpu_arty_a7_ddr_bramdiag`, which
was placed with seed 3 (its own build record says so). That release's files stay as they are;
releases since (`manifest_format = 2`) record each bitstream's seed, and `release-verify` rebuilds
each with it (section 5).

## 1. What a release is

`make -C surfcpu release` writes `surfcpu/build/release/<version>-<commit>/` (a pre-release:
`<version>-pre-<commit>`), where the version comes from `release/release.txt` and the commit is
the first 8 hex digits, the build id every artifact carries:

| Path | What |
|---|---|
| `fpga/<board>/<design>/<design>.bit` | the bitstreams: every design of `release/release.txt` for `arty_a7_100t` and `arty_a7_35t`, from the open flow with every gate of BOARD.md section 8 |
| `fpga/<board>/<design>/manifest.txt` | each bitstream's build record (`tools/fpga-build.sh`), which `tools/program.sh` reads before loading or flashing it. It holds the build's own times, so it is not one of the reproducible artifacts |
| `flash/<board>/<design>.bin` | the QSPI flash image of each design meant for the flash (section 6) |
| `SURFOS.BIN`, `DDRDIAG.BIN` | the kernel package and the DDR diagnostic package (CARD.md) |
| `card/BOOT/SURFOS.BIN`, `card/BOOT/CMDLINE.TXT` | the card's `/BOOT` files exactly as `tools/deploy-sd.py` installs them on a new card, with `sd=rw` on the command line (CARD.md section 8) |
| `rom/surfboot.mem`, `rom/bramdiag.mem` | the boot ROM images inside the bitstreams |
| `debug/*.elf` | SurfBoot, the diagnostics and the kernel with their symbols, for reading a fault address |
| `SHA256SUMS` | the SHA-256 of every artifact above (not the build records): `shasum -a 256 -c SHA256SUMS` |
| `MANIFEST` | TOML: the release's name and status, the commit and its time, that the tree was clean, the designs whose flash images it holds (`flash_boot_design`, `flash_test_design`: empty when the release does not have one), each image with its id and tool versions, the contract versions (`spec/soc.toml`, SURFBUS.md, ISA.md, BOOT-ABI.md, MODULES.md, MEMORY-MAP.md, each with its SHA-256), every design with its gate results, timing and use (or why it is missing), and every artifact with its size and SHA-256 |

`release/release.txt` lists the boards and the designs:

| Design | Flash | Needs | Use |
|---|---|---|---|
| `arty_a7_baseline` | | | milestone 1: no CPU, the board's clocks, LEDs and UART |
| `surfcpu_arty_a7_bramdiag` | | | milestone 3: the CPU and its BRAM diagnostics |
| `surfcpu_arty_a7_top` | test | | SurfBoot without DDR: its flash image tries the flash path on its own (it stops in SurfBoot's monitor, DDR_ABSENT) |
| `surfcpu_arty_a7_ddr_bramdiag` | | DDR | milestone 4: the DDR3L diagnostics |
| `surfcpu_arty_a7_ddr` | **boot** | DDR | the standalone design: SurfBoot loads SurfOS from the card into DDR |

A design that needs DDR and does not build makes the release a **pre-release**, and so does a
bitstream whose build is THIN (every placer seed of its design list met timing with less slack
than the open flow's rule wants, OPEN-FLOW.md section 3). That needs `PRERELEASE=1`, so it is
never mistaken for a complete one: the directory name says `-pre`, `MANIFEST` says
`status = "prerelease"`, `complete = false` and lists what is missing and why (`missing`) and
which bitstreams are THIN (`thin`). Any other design that does not build stops the release. No gate
is relaxed for a release: a design is in it only if `tools/fpga-build.sh` passed it as it is.

## 2. Before you start

* Docker Desktop running, with the two images of `devenv/` (`make -C surfcpu image openxc7-image`
  once; section 8). `make release` refuses to start without them.
* A clean tree: everything committed, nothing untracked that the repository's own `.gitignore`
  files do not ignore. The release is the commit, so anything else is refused, whatever your git
  configuration hides: `status.showUntrackedFiles=no`, a global excludes file or
  `.git/info/exclude` (a Finder `.DS_Store` counts: delete it), and files marked
  `git update-index --assume-unchanged` or `--skip-worktree`.
* Nothing set that would change what the builds make: `DRY_RUN`, `SURFCPU_FPGA_OUT`,
  `SURFCPU_IMAGE`, `SURFCPU_OPENXC7_IMAGE`, `SURFCPU_OPENXC7_BUILD`, `SURFCPU_ROMCHECK`, `ROM`,
  `SURFCPU_BUILD_ID`, `SURFCPU_BUILD_DIRTY` or `PLACER_SEEDS` (release-verify gives each rebuild
  the seed its release recorded itself, section 5) in the environment is refused. Everything else is
  simply not passed on: the builds get `PATH`, `HOME`, `USER`, `LOGNAME`, `SHELL`, `TMPDIR`, `TERM`
  and Docker's own settings from your shell, and what the release sets.
* About an hour for the six bitstreams without DDR (the SoC ones take 10 to 17 minutes each, most
  of it the LVS proof), plus about 22 minutes for each of the four DDR builds (21 to 23 minutes at
  92a2b510, BOARD.md section 8), and about 1 GB of disk (the bitstream builds about 0.5 GB,
  LiteDRAM's sources 200 MB, the release 27 MB).
* The machine to itself, ideally: a bitstream build waits while three or more `surfcpu-openxc7`
  containers run (other builds), and builds one design at a time. Other builds cannot get into the
  release (it builds in its own directory, step 2), but they slow it down. One release at a time
  in a checkout: a second one is refused while `build/release-work.lock` names a running process.

Nothing here touches the board or a card: they come in sections 6, 7 and 10.

## 3. Making the release

```sh
make -C surfcpu release                   # a complete release, or nothing
make -C surfcpu release PRERELEASE=1      # or a pre-release, should a DDR design not build
```

Variables: `PRERELEASE=1` allows a pre-release, a release without some designs (when every design
builds it is complete anyway). `SKIP_DDR=1` does not even try the DDR designs, and `DESIGNS=a,b` and
`BOARDS=a,b` build only some (for trials): each leaves designs out, so each needs `PRERELEASE=1`
as well (`make release SKIP_DDR=1` alone is refused). `FORCE=1` replaces an existing release
directory of the same name, and nothing else (a directory holding anything a release does not is
never replaced). The version is `release/release.txt`'s, and `SURFOS.BIN` must carry the same one
(`arch/riscv32/kernel.mk` packs it with `--version`): a release whose two disagree is refused, so
change both together. The steps and what they print:

1. **Checks.** The tree must be clean; `SOURCE_DATE_EPOCH` becomes the commit's time (a different
   value in the environment is refused); both images must exist.

   ```
   release: SurfCPU 1.0.0 at bb066932 (commit time 2026-10-16T14:33:34Z = SOURCE_DATE_EPOCH 1792161214), clean tree
   release: images: surfcpu-toolchain:2026-10-15 (sha256:e4389c3c6d93), surfcpu-openxc7:2026-10-15 (sha256:b802ed1bac37)
   release: plan: 10 bitstreams; built in .../build/release-work, staged in .../build/release/.1.0.0-pre-bb066932.stage; logs in .../build/release/1.0.0-pre-bb066932.logs
   ```

2. **The software**, in the release's own directory `build/release-work/`, emptied first; nothing
   else writes there, so another session's `make build-rom` or `make build-fpga` meanwhile cannot
   get into the release: `make build-rom BUILD=build/release-work` and `arch/riscv32/kernel.mk
   KBOARD=arty_a7 OUT=surfcpu/build/release-work/riscv32/arty_a7` in the toolchain image.
   `SOURCE_DATE_EPOCH` goes into the container (`devenv/run.sh`); `LC_ALL=C` and `TZ=UTC` hold on
   the Mac only: the containers run with their images' own environment, which sets no locale and
   no time zone (so the POSIX locale and UTC), and MANIFEST's `[tools] locale` records what each
   image sets. Checked: the packages carry the commit's build id and `release.txt`'s version, the
   firmware was not built dirty. Each file is read once, checked, and copied at once into the
   stage directory (`build/release/.<name>.stage`), with its SHA-256 kept.

   ```
   release: [1/11] the software (make build-rom, the kernel)
   release: software: SurfBoot, the BRAM diagnostics, DDRDIAG.BIN and SURFOS.BIN, build bb066932 (12 s)
   ```

3. **The bitstreams**, one design and board at a time, the DDR designs last:
   `FLOW=openxc7 tools/fpga-build.sh --design <design> <board>`, as `make build-fpga` runs it, but
   into `build/release-work/fpga/<board>/<design>/` (`SURFCPU_FPGA_OUT`, emptied first), with the
   release's own boot ROM (`ROM=`) and kernel (`SURFCPU_KERNEL=`, recorded only). A design counts
   only if the script passed it and its build record is from this build (its `run_id`), says the
   commit, a clean tree, the commit's time, the commit's openXC7 image, the generated files'
   recorded hashes (still true after the build), the release's boot ROM and kernel, and describes
   the bitstream's bytes; then the bitstream, its build record and its flash image (section 6) are
   staged at once. The DDR designs first run `make litedram-gen` (it fetches pinned sources the
   first time; the core goes to `build/litedram/`, shared, and is checked against
   `ip/litedram/arty.sha256` before and after each DDR build).

   ```
   release: [2/11] arty_a7_baseline for arty_a7_100t (tools/fpga-build.sh, FLOW=openxc7)
   release:     PASS (21 s): wns 32.43 ns, sha256 95390240c610c9b8...
   ...
   ```

4. **The card files** (`tools/deploy-sd.py --new-card --cmdline sd=rw` on the checked
   `SURFOS.BIN`, against a directory standing in for the card, as its tests run it: the files it
   installed are staged, and `card/BOOT/SURFOS.BIN` must be `SURFOS.BIN`), then a last check before
   anything is written: `HEAD` is still the commit, the tree is still clean, both images still have
   the IDs they started with, every staged file still has the SHA-256 it had when it was checked
   (and nothing else is there), both packages carry the commit's build id and every bitstream's
   record names the staged ROM and kernel. Then `SHA256SUMS` and `MANIFEST`, the release's own
   `release-check` on the stage, and the stage becomes the release directory: it appears only at
   the end, complete. A failed release leaves no stage behind; its logs and `build/release-work/`
   stay for reading (the next release empties them).

   ```
   release: written .../build/release/1.0.0-pre-bb066932: PRE-RELEASE, without:
       surfcpu_arty_a7_ddr_bramdiag for arty_a7_100t: did not build: make litedram-gen: ...
       ...
   release: 18 artifacts in SHA256SUMS (check: cd ... && shasum -a 256 -c SHA256SUMS); 53.3 min in all
   ```

`make release-check RELEASE=build/release/<name>` checks a release later (after a copy, for
example): `MANIFEST`, `SHA256SUMS` and the files on disk list the same artifacts (nothing more,
nothing missing, nothing empty) with the listed SHA-256 and size; the directory's name, the
version and pre-release or not agree with `MANIFEST`; both packages are the commit's and the
release's version, the card files are what `MANIFEST` says; every built design has its bitstream,
its build record (pass, clean, this commit and time, these bytes, the release's boot ROM) and its
flash image exactly when it has one, which must be the bitstream's stream; nothing belongs to a
design that was not built. It exits 1 on any problem.

The tools delete only what they made. The logs directory, `build/release-work/` and the stage carry
a mark (`.surfcpu-release-logs`, `-work`, `-stage`); one without it that holds anything is refused,
never emptied. `FORCE=1` replaces only a release (a directory holding nothing but what a release
holds). `release-verify` and `fresh-checkout` keep a work directory you gave them and remove only
what they put in it.

**Using a release.** Its files go where the usual commands take them (paths relative to
`surfcpu/`; `R=build/release/<name>`):

```sh
make -C surfcpu program-volatile BOARD=arty_a7_100t BIT=$R/fpga/arty_a7_100t/surfcpu_arty_a7_bramdiag/surfcpu_arty_a7_bramdiag.bit
make -C surfcpu program-flash    BOARD=arty_a7_100t BIT=$R/fpga/arty_a7_100t/surfcpu_arty_a7_ddr/surfcpu_arty_a7_ddr.bit   # section 6
make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS="--image $R/SURFOS.BIN --cmdline sd=rw"
make -C surfcpu board-test M=m9 ARGS="--manifest $R/fpga/arty_a7_100t/surfcpu_arty_a7_ddr/manifest.txt --kernel $R/SURFOS.BIN"
```

`tools/program.sh` reads the build record next to the bitstream, so a release's bitstreams load
and flash as they are; `deploy-sd` checks and installs the release's package exactly as it
installs a fresh build (the release's `card/BOOT` files are what it writes on a new card).

## 4. Reproducibility

A release is reproducible: the same commit gives the same bytes, in any directory, on any day.
What made two builds of one commit differ, and what was done about it:

| Where | What differed | Fix |
|---|---|---|
| `SURFOS.BIN`, `debug/surfos.elf` | the kernel compiles its build time in (`SURFOS_BUILD_TIME` in `arch/riscv32/kernel.mk`, from `SOURCE_DATE_EPOCH` or the date), but `devenv/run.sh` did not pass `SOURCE_DATE_EPOCH` into the container: two builds differed in the payload CRC, the header CRC and two words of `time_now` | `devenv/run.sh` passes it when it is set (`dad4607`); the release sets it to the commit's time |
| every `.bit` | `xc7frames2bit` writes the time of the build into the `.bit` header (fields c and d); the FASM, frames and netlists of two builds were already the same | `tools/openxc7-build.sh` runs `tools/bitfile.py stamp` with `SOURCE_DATE_EPOCH` right after `xc7frames2bit`, before the checks (`b3741f2`) |
| every `.bit` | the header's design field names the frames file, so the output directory is in it | the release always builds in its own `build/release-work/fpga/<board>/<design>` (the same relative path in every checkout) and refuses `SURFCPU_FPGA_OUT`; so a release's bitstreams differ from `make build-fpga`'s in that field |
| the bitstreams' build identity | `BUILD_TIME` is a generic of the tops (sysinfo's build time) | `tools/fpga-build.sh` already took `SOURCE_DATE_EPOCH`; the release sets it |
| build paths | none found: the containers mount the tree at `/work` wherever it is, so the debug information and the `.bit` header's relative path are the same from any checkout (`-ffile-prefix-map` is not needed) | the verify runs build in other directories and compare the ELF files too |
| file order | none found: GNU Make 4.4.1 in the image sorts `$(wildcard)`; `tools/fpga-build.sh` sorts its source lists, and the Mac's `en_US.UTF-8` order equals `C` order for every file there | the release runs the Mac-side scripts with `LC_ALL=C`; the images set no locale, so the containers use the POSIX locale (`MANIFEST` records it) |
| the generated LiteDRAM core (DDR designs) | LiteX lists the core's instances in a block comment of `litedram_core.v` in an order that follows the Python interpreter's memory: a fresh checkout's first `make litedram-gen` wrote a file that failed its own check against `ip/litedram/arty.sha256`, while every line of Verilog outside that comment was the same in the five different files 16 runs produced (evidence `determinism.txt` section 6) | fixed by the ddr track (d6f3ef8): `ip/litedram/run_gen.py` replaces that comment with a fixed note after the generator has run, stopping unless it finds exactly one; `make litedram-repro` generated both cores in eight differently set-up Python processes and got one set of files, the locked one (evidence `../ddr/litedram.txt`) |
| threads and seeds | none found: nextpnr runs with 4 threads (`OPENXC7_THREADS` inside the image; `devenv/run.sh` does not pass it, so the release always gets 4), placer seeds come from the design lists, and two SoC builds gave the same netlist, placement, FASM and frames. But which seed of a list a build ends with can depend on the machine's load since each seed has a time limit (and, since the slack rule, on a seed being thin) | recorded in `MANIFEST` (`nextpnr_threads`, and each design's `placer_seed`, the one its build record names, since `manifest_format = 2`); `release-verify` rebuilds each bitstream with exactly that seed (`release.py make --seed BOARD/DESIGN=SEED`, which gives that build `PLACER_SEEDS`) |

Check a release with:

```sh
make -C surfcpu release-verify RELEASE=build/release/<name> [DESIGNS=a,b] [BOARDS=a,b] [KEEP=1] [REPORT=file]
```

It first checks the release as `release-check` does (a damaged one is refused, exit 2), then
clones its commit into `build/release-verify/<name>-<time>/src` (`git clone --shared`: a checkout
of its own that borrows this repository's objects), runs the same release there for the designs
the release has, and compares every artifact of the release's `SHA256SUMS` byte for byte, hashing
both sides' files. Each bitstream is rebuilt with the placer seed the release's `MANIFEST` records
for it (`manifest_format = 2`; an older `MANIFEST`, whose seeds all say 1, is rebuilt with the
design lists' seeds, as its release was, and the report says so). `DESIGNS` and `BOARDS` name
some of them: a name the release's `MANIFEST` does not know, or a design it did not build, is
refused (exit 2) before anything is cloned. Each
artifact is `match`, `DIFFERS`, `MISSING`, `DAMAGED` (a file that is not the one its own
`SHA256SUMS` lists, on either side), `EXTRA` (only the rebuild has it) or `not rebuilt` (its
design was not asked for). A difference names the first differing byte and its likely cause:

* a `.bit`: the header field (the date and time: `SOURCE_DATE_EPOCH` missing; the design path: the
  output directory), or the configuration packet (an option) or frame (the placed design or a
  block RAM) where the streams part, how many frames differ, and the build inputs that differ in
  the two build records (sources, constraints, ROM, build id and time, seed, image and tool
  versions), or that they are the same, which points at the tools themselves;
* `SURFOS.BIN`, `DDRDIAG.BIN`: the header fields that differ, and the payload's first differing
  address with the function it is in (from `debug/*.elf`);
* a ROM image: the word address and function; an ELF file: the section (debug information points
  at a build path or the compiler).

The `MANIFEST`s are compared too (commit, tools, contracts, every design's gates and timing), and
the rebuild must pass `release-check`. The last line is the verdict: `PASS` (every artifact
rebuilt and the same), `PARTIAL` (every rebuilt one the same, but `DESIGNS`/`BOARDS` left some
out: the line says how many were NOT verified; exit 0, since everything asked for matched) or
`FAIL` (exit 1). The work directory is removed at the end unless `KEEP=1`; the report is in it and
with `REPORT=file` where you say. `tools/release.py compare A B` compares two release directories
the same way, without building; there `PARTIAL` (the new release did not build some of the
reference's designs) exits 1.

## 5. A fresh checkout

```sh
make -C surfcpu fresh-checkout REFERENCE=build/release/<name> ARGS="PRERELEASE=1" [EVIDENCE=file] [KEEP=1]
```

`tools/fresh-checkout.sh` clones the repository (`git clone`, a repository of its own) at
`COMMIT` (default HEAD) into `build/fresh-checkout/run.XXXXXX/` and does there what this page
says, nothing else: checks the two images (with `--build-images` it builds missing ones with
`make image openxc7-image`; otherwise it stops), runs `make -C surfcpu release` with `ARGS`, and
compares the result with `REFERENCE` (a release directory, or a `SHA256SUMS` file) artifact by
artifact. Designs the new release did not attempt (`DESIGNS`, `BOARDS`, `SKIP_DDR`) are listed as
not rebuilt, and the comparison is then `PARTIAL`, not a match (exit 1). `EVIDENCE=file` keeps a
summary; the clone is removed unless `KEEP=1` or something failed (with `--work DIR`, a new or
empty directory, only what the script put there).

The release `1.0.0-pre-bb066932` was made this way, with no reference yet
(`tools/fresh-checkout.sh --commit bb06693 --keep -- PRERELEASE=1`, evidence `release.txt`): from
the clone to the written release in 53 minutes, the images used as they were, nothing else set up.
`make release-verify` then rebuilt it in another clone and compared it (section 4, `verify.txt`).
The first complete release, `1.0.0-f791b8a1` with all ten bitstreams, was made the same way
without `PRERELEASE` (`tools/fresh-checkout.sh --commit f791b8a --keep`, evidence
`release-1.0.0-f791b8a1.txt`) in 141 minutes, and `make release-verify` rebuilt all 24 of its
artifacts byte for byte in 144 minutes (`verify-1.0.0-f791b8a1.txt`).

## 6. The configuration flash

**The part.** Every Arty A7 revision has a 128 Mbit (16 MiB) quad-SPI flash, but the part
changed: Micron N25Q128A13ESF40 (Rev. C and earlier), Spansion S25FL128SAG[M|N]FI00, or
S25FL127SABMFx00 on a sticker (INVENTORY.md gap G7). openFPGALoader reads the flash's JEDEC id
before writing and prints it, with the maker and model for a part it knows (`Detected: ...`,
`src/spiFlash.cpp`); note it in INVENTORY.md the first time. If it says the flash's block
protection is set, it refuses to erase: openFPGALoader's `--unprotect-flash` clears it, which
`tools/program.sh` does not pass (a board-side decision to make then).

**The image.** `flash/<board>/<design>.bin` is the bitstream's configuration stream without its
`.bit` header: the bytes openFPGALoader writes for `-f <design>.bit` (its BitParser drops the
header and reverses no bits for an SPI flash, `src/bitparser.cpp`, `src/xilinx.cpp`), so it is
also what a read-back of the flash must show. `tools/bitfile.py flash` makes it and refuses a
`.bit` for another part, a stream without the board's IDCODE or one longer than 4 MiB. The open
flow writes uncompressed streams of exactly the standard length (UG470 table 1-1):

| | arty_a7_35t | arty_a7_100t |
|---|---|---|
| configuration stream | 2,192,012 bytes = 17,536,096 bits | 3,825,788 bytes = 30,606,304 bits |
| at | 0x000000 to 0x21728B | 0x000000 to 0x3A607B |
| erased with it: openFPGALoader erases 64 KiB blocks (a part with 256 KiB sectors erases those) | up to 0x21FFFF (0x23FFFF) | up to 0x3AFFFF (0x3BFFFF) |

**The address map** of the 16 MiB flash:

| Range | Use |
|---|---|
| `0x000000`-`0x3FFFFF` (4 MiB) | slot 0: the configuration stream; the FPGA reads from address 0 at power-on |
| `0x400000`-`0xFFFFFF` (12 MiB) | free: nothing of SurfCPU's is there. A second slot at `0x400000` would hold a MultiBoot update image (section 7); SurfOS lives on the card |

**Writing it** (the owner's step, with the board; never part of a test or a release):

```sh
make -C surfcpu program-flash BOARD=arty_a7_100t \
    BIT=build/release/<name>/fpga/arty_a7_100t/surfcpu_arty_a7_ddr/surfcpu_arty_a7_ddr.bit
```

`tools/program.sh flash` (BOARD.md section 9) checks the cable, the IDCODE on the JTAG chain, the
part in the `.bit` header and the build record next to the bitstream (it must say pass and carry
this file's SHA-256, this board and this part), asks for the board's name (or takes
`SURFCPU_FLASH_CONFIRM=write-flash-<board>`), then runs `openFPGALoader -b <board> -f --verify
<bit>`: openFPGALoader loads its SPI bridge over JTAG, erases and writes from address 0, reads it
back. It never takes the `.bin` (no header names its part) or a bitstream without its build
record. Expected output, as the fake openFPGALoader of the tests gives it (the real one adds its
progress lines, the flash's JEDEC id among them):

```
program: JTAG: IDCODE 0x13631093 = BOARD arty_a7_100t (xc7a100tcsg324-1)
program: this overwrites the board's configuration flash (it boots from it at power-on).
program: check the flash part first (INVENTORY.md G7) and keep a known-good bitstream.
program: type the board name (arty_a7_100t) to write its flash: arty_a7_100t
program: writing .../surfcpu_arty_a7_ddr.bit to the configuration flash of arty_a7_100t
program: flash written and verified; set the MODE jumper (JP1) to boot from it
```

Then put the Arty's JP1 (MODE) jumper on and power-cycle: the FPGA configures itself from the
flash, the DONE LED lights, and SurfBoot's banner comes on the UART. The boot image is
`surfcpu_arty_a7_ddr`'s; the test image, `surfcpu_arty_a7_top`'s (SurfBoot without DDR: it stops
in its monitor with DDR_ABSENT), shows the board started from the flash by itself without
needing the DDR3L or the card.

**How long the board takes to start.** The FPGA reads the stream at the configuration clock
(CCLK) with the bus width the stream sets. `tools/bitfile.py info <bit>` reads them from the
stream: the open flow's bitstreams set COR0 = 0x02003FE5 (OSCFSEL 0: CCLK stays at its start-up
rate, "3 MHz typical", UG470 v1.17 figure 2-13 note 2) and write no BSPI register (so SPI x1: one bit per
clock). Computed from the stream lengths, plus a few tens of milliseconds of power-on reset and
initialization (the internal oscillator's tolerance, DS181 F_MCCKTOL, can stretch the 3 MHz):

| Setting | arty_a7_35t | arty_a7_100t |
|---|---|---|
| **x1 at the start-up rate, about 3 MHz: the open flow's bitstreams** | **5.8 s** | **10.2 s** |
| x1 at 33 MHz | 0.53 s | 0.93 s |
| x2 at 33 MHz | 0.27 s | 0.46 s |
| x4 at 33 MHz | 0.13 s | 0.23 s |
| x4 at 50 MHz (the Arty's limit for quad SPI, reference manual section 4.2) | 0.09 s | 0.15 s |

So a 100T running the open flow's bitstream starts SurfBoot about 10 seconds after power-on
(then, as seen on the board with the design loaded over JTAG, about 0.2 s of DDR calibration and
2 s to load the kernel). That is slow but works; the board test of milestone 9 waits up to 60 s
for the kernel load.

**What the open flow cannot set.** Project X-Ray's `xc7frames2bit` has no options for the
configuration (its only flags are the part, the frames and the output file), and nextpnr's XDC
reader does not apply `BITSTREAM.*` or `CONFIG_*` properties (every build lists them as not
checked). Vivado writes them from `BITSTREAM.CONFIG.SPI_BUSWIDTH` and `CONFIGRATE`: a write to the
BSPI register (bus width and read opcode, UG470 v1.10 table 5-41) with a BSPI_READ command (v1.17 table 5-25), and
COR0's OSCFSEL. Patching those into the open flow's streams by hand is possible in principle, but
the OSCFSEL value for each rate is not documented (UG470 says only "2 MHz - 60 MHz"), a quad read
needs the flash's quad-enable bit on the Spansion parts, and only the board can show the result.
It was not done: the open flow's streams load at x1, about 3 MHz. With Vivado (BUILD-HOST.md
options A or B), `arty_a7_config.xdc` should then set `SPI_BUSWIDTH 4` and `CONFIGRATE 33` once
the flash part is known (it says so already). Two more differences from Vivado's bitstreams:
they are not compressed (the XDC's `COMPRESS` is not applied), and they carry **no CRC check**
(prjxray resets the CRC instead, OPEN-FLOW.md 6.5), so the FPGA cannot detect a stream damaged in
the flash; `program-flash`'s `--verify` read-back is what checks it.

## 7. Recovery

**The kernel** (SurfBoot, CARD.md section 5): when `/BOOT/SURFOS.BIN` fails any check, SurfBoot
boots `/BOOT/BACKUP.BIN` by itself (deploy-sd keeps the previous good package there); **hold BTN0**
at power-on or RESET to boot `BACKUP.BIN` on purpose; **hold BTN1** to stop in SurfBoot's monitor
(`help`, `boot /BOOT/<file>`). A card can always be fixed on the Mac with `make deploy-sd`.

**The configuration**: a bad bitstream in the flash (one that does not start, or a design that
misbehaves) is recovered over JTAG, which works whatever the flash holds:

1. Unplug the USB cable and **take the Arty's JP1 (MODE) jumper off**. The FPGA then does not
   configure itself from the flash at power-on; it waits, unconfigured, with its pins released
   (BOARD.md section 12, reference manual section 4).
2. Plug the cable back in. `make -C surfcpu program-volatile BOARD=<board> BIT=<a known-good .bit>`
   (for example the previous release's `surfcpu_arty_a7_ddr.bit`, or the baseline) shows the board
   works; it is gone at power-off.
3. Write the known-good bitstream to the flash: `make -C surfcpu program-flash BOARD=<board>
   BIT=<it>` (section 6). openFPGALoader reaches the flash through its own bridge design loaded
   over JTAG, so a broken image in the flash does not stand in the way.
4. Put JP1 back on and power-cycle: the DONE LED and SurfBoot's banner (about 10 s on a 100T).

Keep the previous release directory until the new one has passed milestone 9's board test: its
bitstream is the known-good one.

**No golden and update images (MultiBoot).** The 7-series FPGAs can keep a known-good "golden"
image at address 0 that jumps (WBSTAR and IPROG) to an update image, and fall back to the golden
one when the update fails its IDCODE or CRC check or the configuration watchdog runs out (UG470
chapter 7). The open flow's streams keep the placeholders Xilinx leaves for this (a TIMER write,
a WBSTAR write and a NULL command before the frames, UG470 table 5-19), so a golden image could
be made by changing three words, and the update image would need its CTL0 changed too: the open
flow writes CTL0 = 0x00000501, whose bit 10 disables fallback (UG470 v1.17 table 5-27;
`tools/bitfile.py info` shows it). It was not implemented, because:

* the open flow's streams carry no CRC check, so a damaged update image (as opposed to a missing
  or cut-short one, which the watchdog catches) would configure the FPGA without falling back:
  the layout would promise more safety than it gives;
* every one of those words is a hand patch of the tools' output, outside the flow's checks (the
  round trip and LVS compare frames, not these packets);
* the fallback is the FPGA's configuration logic: nothing here simulates it, and the only test is
  to write a damaged update image into a real board's flash on purpose, which is a board step;
* recovery exists without it: the kernel has `BACKUP.BIN`, and the configuration has JTAG with JP1
  off, whatever the flash holds.

The address map leaves `0x400000` onwards free, so a later golden and update layout (with Vivado's
CRC-checked bitstreams, `BITSTREAM.CONFIG.NEXT_CONFIG_ADDR`, `CONFIGFALLBACK` and the watchdog)
fits without moving anything.

## 8. The images, rebuilt from pinned sources

Both images are built on the Mac from pinned sources, and the release records each one's id and
tool versions (`MANIFEST [tools]`). A tag is not a fixed image: a rebuild gives a new image id (the
timestamps in its layers), and the tag then names the new one; the tool versions stay.

| Image | Command | Pinned | Rebuilt with no cache, 2026-10-16 |
|---|---|---|---|
| `surfcpu-toolchain:2026-10-15` | `make -C surfcpu image` (`devenv/build-image.sh`) | Debian trixie-slim by digest, every package from snapshot.debian.org at 2026-10-05, Spike at a commit, SymbiYosys at its tag | 218 s: packages 65 s, Spike 79 s, the export 72 s; 6.1 GB |
| `surfcpu-openxc7:2026-10-15` | `make -C surfcpu openxc7-image` (`devenv/openxc7/build-image.sh`) | the same base and snapshot; Yosys 0.69 by tarball SHA-256, openXC7 nextpnr, prjxray and prjxray-db at commits, textX by wheel SHA-256; the chip databases generated in the build | 413 s with 4 compile jobs (331 s with 6, `evidence/flow/image.txt`), Yosys the longest stage; 661 MB; the chip databases need 3.1 GB of memory |

So about 10 minutes for both, with the network (Debian's snapshot archive, GitHub, PyPI). Both
were rebuilt that way under tags of their own (evidence `images.txt`): new image ids, the same
version records line for line (the chip databases' SHA-256 included), and the release's software
built with the rebuilt toolchain image was byte for byte the release's. After a rebuild of the
images in use, `make release-verify` against an older release shows whether the artifacts stay
the same; its `MANIFEST` comparison names the new image ids.

## 9. Tests and evidence

| | What |
|---|---|
| `make test-release` (on `make check`, about a minute), and `make release-selftest` (the same with the Mac's own Python) | `tools/release-test.py`: `tools/bitfile.py`'s self-test, flash image and header rule (shared with `tools/program.sh`); `tools/release.py`'s TOML subset, its comparison and the causes it names for planted differences (the `.bit` header's time, a frame, a kernel word with its symbol, a missing and an extra file); `make` in a scratch repository with stand-ins for the builds: a dirty tree (also one git's configuration hides), a foreign `SOURCE_DATE_EPOCH`, settings that would change the builds and missing designs without `PRERELEASE` refused; a complete release; a DDR design that fails gives a pre-release only with `PRERELEASE`; another design that fails stops it; two releases of one commit are the same bytes; other builds, a new commit or a changed staged file during the release; build records from another run or image; the builds' environment; a damaged, renamed, padded or cut release refused by `check` and `compare`; `verify` with names it does not know, and a partial one; the placer seed each bitstream was placed with recorded (from its build record) and given back by `verify`, an older `MANIFEST` rebuilt with the design lists' seeds, a THIN bitstream refused without `PRERELEASE` and listed in a pre-release; the tools never deleting a directory they did not make; `tools/program.sh flash` on a release's bitstream with the board tests' fake openFPGALoader, and the host targets with paths that hold spaces |
| `evidence/release/release.txt` | the pre-release `1.0.0-pre-bb066932` made from a fresh checkout (`tools/fresh-checkout.sh`): the output, `SHA256SUMS`, the gates of every bitstream, the missing DDR designs and why |
| `evidence/release/verify.txt` | `make release-verify` of it: every artifact, with what it found |
| `evidence/release/release-1.0.0-f791b8a1.txt` | the first complete release, made from a fresh checkout: the output, `SHA256SUMS`, the gates of all ten bitstreams |
| `evidence/release/verify-1.0.0-f791b8a1.txt` | `make release-verify` of it: all 24 artifacts byte for byte the same |
| `evidence/release/tests.txt` | `make test-release` in the container and `make release-selftest` with both of the Mac's Pythons |
| `evidence/release/review-fixes.txt` | the review's findings: each one's tests failing on the code it saw and passing after its fix, the reduced release `1.0.0-pre-147fe813` with its check and verify, and verify's refusals |
| `evidence/release/determinism.txt` | the nondeterminism found (section 4), before and after, and the generated LiteDRAM core's |
| `evidence/release/images.txt` | both images rebuilt from their pinned sources with no cache, and the artifacts built with them |
| `evidence/release/flash.txt` | the flash images, the configuration options and times, the sources behind them, and `program.sh flash` with the fake programmer |

## 10. What only the board can show

All pending (PHYSICAL-TESTS.md). The flash part is known: on the owner's Rev. E board a Spansion/Infineon
128 Mbit part (JEDEC id 0x012018) with no block protection (INVENTORY.md G7). Its factory contents
(Digilent's image, about 1 MB) are read back and kept off the repository
(`evidence/physical/flash-backup-2026-10-17.txt`). Release `1.0.0-f791b8a1`'s DDR boot design,
loaded over JTAG, booted SurfOS from the owner's card twice
(`evidence/physical/release-1.0.0-f791b8a1-jtag-2026-10-17.txt`). Still to show:

* that the flash part takes the image (`program-flash` with the real openFPGALoader, its JEDEC id,
  the erase, write and read-back);
* that the board starts by itself from the flash with JP1 on, and how long it really takes;
* the recovery procedure of section 7 on the real board;
* milestone 9's acceptance test, with a release's DDR boot design: `make -C surfcpu board-test M=m9
  ARGS="--manifest build/release/<name>/fpga/<board>/surfcpu_arty_a7_ddr/manifest.txt --kernel
  build/release/<name>/SURFOS.BIN"` with that design in the flash and the release's card files on
  the card: 10 cold boots from the flash and an hour's soak.
