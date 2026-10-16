# Touchpad progress

## Resumed 2026-10-15 (Claude, at the owner's request)

The owner asked Claude to take over the desktop branch from Codex and continue.
The next step below ("set InputMode directly") is implemented:

- `hid_pointer_mode_report()` builds the whole InputMode feature report when
  InputMode is its report's only Data field. Constant padding is zeroed, and
  other report types or IDs don't count. Mixed reports keep GET-and-patch.
- `driver/touchpad.c` sends SET_REPORT(Feature 4, mode 0) without a GET. A
  refused mode transfer is recorded (`touchpad info` mode line with its abort
  source) but no longer stops start-up: after the host reset the device reports
  through its default collection, which Microsoft says may be the mouse one.
- Input: up to 3 reports per 10 ms wake while the line is low, stopping at the
  first non-data read. A zero-length input (a device reset, or a read with
  nothing waiting) is counted (`resets=`) and input continues, as in Linux. It
  no longer stops the touchpad, and it doesn't release a held button.
  `touchpad info` also prints the HID registers.
- `pointer_devices()` counted only sources below POINTER_KEYS, so the I2C
  touchpad (source 10) never showed in the desktop's or pointerinfo's count. Fixed.
- After an independent review, these changes followed:
  - `i2cdw` no longer asks an idle master to abort after a NACK. Linux aborts
    only a master on hold; a stuck abort used to retire the controller.
  - `transfer()` sets up a retired controller again before the next transfer.
  - `touchpad info` shows `bus=ready|down`, `reinits`, `recovered`, and the
    interrupt line before and after the mode transfer.
  - `touchpad up` also restarts a working touchpad.
  - Decoded axes outside the logical range are clamped, or mean no motion with
    a Null State, as in Linux. A second InputMode is skipped (the ELAN quirk).
- Tests: the laptop's 665-byte descriptor is a fixture (`tests/host/touchpad_06cb_ce2d.h`).
  `test_touchpad` runs `hid_start()` and the input path against a simulated
  Synaptics that NACKs that GET. Putting back the old read-first code makes 6 of
  its checks fail.

Validation of this code and the laptop result: see "Status" at the end.

# Earlier: touchpad work paused at the owner's request

Saved 2026-10-12 (local); do not resume changes, deployment or reboot until asked.
(Superseded by the resumption above.)

## Source and integration

Worktree: `/Users/brandonburr/.codex/worktrees/surfos-desktop/06-restoration`
Branch: `codex/surfos-desktop`
Desktop checkpoint commit: `7a269b053261` (GUI, Calculator, Sketch, PS/2 and USB input).
The ACPI export and native I2C touchpad follow-up are saved as uncommitted files.
They have not been merged into `ai-dev` or pushed. Main remains at `4ac143b`;
its unrelated Undertow plan/spec files are untouched.

## Installed laptop build

Laptop: `10.0.0.164`, telnet enabled. It came back online after deployment.
Build ID: `7a269b053261-dirty 2026-10-13T03:50:06Z`.
Frozen pair: `build/validation/touchpad-n2-O0/`.
Kernel SHA256: `10173ff28794a15c2fde6b6ace0d580b8e279493d55f4c72c0bfb30f272371f2`.
Initrd SHA256: `5d575b3d71ccfa85768d8c4bb1c6e114f50d042d0a35c9638cea05e167f9009b`.
Previous build backup on data USB: `/surfos-dev-backup-20261013-035057`.
The normal protected boot USB was not changed. Do not assume usb0/usb1 identities;
use devloop's protected/data-stick selection. No further laptop actions after pause.

## Native result and precise next issue

`build/validation/laptop-touchpad-n2/after.txt` records the live result.
The native driver powers Intel I2C1 to D0, assigns BAR0 `0x65400000`, resets the
controller and touchpad, and reads/parses the actual 665-byte HID report descriptor.
The device is **Synaptics 06cb:ce2d**, version0102, firmware variant2, I2C address
`0x2c`, HID descriptor register `0x20`, maximum input64 bytes.
GPIO `0xfd6e0780` initially `80100102/00000030`, current `80100100`.
Actual raw descriptor: `build/validation/laptop-touchpad-n2/report-descriptor.bin`.

Startup stops at **HID read input mode**: I2C error8 (abort), abort source
`03000008`, tx6/rx0. No pointer reports have been accepted and the touchpad is
**not working yet**. This is not a controller discovery failure.

The captured descriptor has mouse input report ID2 (two buttons, relative8-bit
X/Y) and an InputMode feature report ID4 containing only one8-bit field at
payload offset0. Current `hid_start()` insists on GET_REPORT before SET_REPORT.
The device NACKs that GET. Microsoft's configuration-collection specification
says the host sends InputMode0 to select the mouse collection; reading this
setting is not required for that one-field report. Next hypothesis to test:
construct the entire known one-field mode report and send SET_REPORT directly.
For mixed-field feature reports retain GET-and-patch to preserve other fields.
No implementation of this change has been made yet.
Reference: https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/touchpad-configuration-collection

Recommended resumption: add the captured descriptor as a regression fixture;
test a bounded helper that initializes a mode report only when the InputMode
field covers its entire payload; integrate direct SET in `driver/touchpad.c`;
review/build/test; deploy only after owner resumes. Then verify real movement
and clicks with the owner. Current GUI itself was previously confirmed working.

## Validation completed

- Desktop checkpoint: full host, O0/O2 smoke286, Files51, full USB/UEFI307 each.
- Diagnostic R2: full USB/UEFI311 each, actual DSDT171067 bytes +9 SSDTs exported
  with checked lengths/checksums (`build/validation/laptop-acpi-r2/`).
- New pure helpers: I2C57, I2C HID267, HID pointer107; AML5135 with native fixtures
  (5120 without fixtures); platform281. Native sanitizer and32-bit checks pass.
- New platform regression test is integrated in `make unittest-touchpad` and
  passed in Docker. It exercises actual range/AML guards and descriptor snapshots.
- N1 focused UEFI serial+GUI:86/86 passed. N2 serial boot checks passed.
- Review fixed ELAN post-reset wake quirk, descriptor snapshot bounds, mixed
  partial SSDT window pairing, full LFCN containment and mutable AML name refs.
- The earlier diagnostic Files fragmented-arrow timing failure did not reproduce:
  an independent rerun passed51/51. Native follow-up O0 Files also passed51/51.

## Incomplete validation stopped for pause

The N2 full O0/O2 USB/UEFI runs were still at GUI when interrupted on request.
`make unittest test-all` had completed host tests and O0 Files, and was running
O0 smoke; it was stopped. Do not claim those full suites passed. Rerun required
full tests on the final code before committing/merging. Logs are preserved under
`build/validation/paused-touchpad/`; normal per-scenario evidence remains under
`build/validation/touchpad-n2-usb-O0` and `touchpad-n2-usb-O2`.

Build volumes: `surfos-acpidump-test` for current native builds;
`surfos-desktop-build` for the interrupted full validation; do not touch other
projects' volumes. Frozen O0/O2 images are in `build/validation/touchpad-n2-O0`
and `touchpad-n2-O2`. Source and build outputs remain on disk.

## Status (2026-10-15)

Commits on `codex/surfos-desktop` (not pushed, not merged into `ai-dev`):
`b88d8b7` (the read-only ACPI export, verified on its own) and the touchpad
commit that carries this file.

Verified on the touchpad commit's tree, with the host's QEMU against Docker builds:
- unittest: every suite, including i2cdw 59, i2chid 267, hidpointer 127,
  amlfield 5120, touchpad 454 and pointer 14.
- smoke 297 and files 51 at O0 and O2.
- USB/UEFI 313/313 at O0 and O2.

Logs are under `build/claude-O0*` and `build/claude-O2*`; the ACPI commit's are
in `build/validation/commit-b88d8b7/`. Known flake, not touchpad code:
`tools/qemu-files.py`'s "fragmented arrow is parsed" fails now and then,
because `keys()` reads the screen after a fixed 250 ms. A rerun passes.

Laptop (2026-10-15, evening): **the touchpad works.** `build/validation/touchpad-n3-O0/`
(build `784b294ab7d9 2026-10-15T22:47:22Z`) was pushed with devloop; the
evidence is in `build/validation/laptop-touchpad-n3/` (before, deploy,
after-boot, live, after, dmesg).
1. Before, the 2026-10-13 build still stopped at "HID read input mode".
2. After: "ready (I2C HID mouse reports)", "mode: mouse (InputMode 0 set)",
   no transfer error, no re-init.
3. The owner's test in `gui`: 2,374 reports, all decoded; 0 errors, 0
   zero-length, 0 queue drops; left button (16 presses, drags) and right
   button (3 presses) seen by a telnet sampler. Details in the hardware record.

Deploying over Wi-Fi: devloop's 180 s wait after the reboot ran out before the
laptop was back on the network (autonet allows 90 s to join); a telnet check
once the owner had joined confirmed the build id.

Next candidates: the Precision Touchpad collection (report 3) for two-finger
scrolling and tap-to-click; a steady Files "fragmented arrow" check; merging
into `ai-dev` when the owner says.
