# SurfOS desktop

Boot the UEFI USB image, then type `gui` at the active local console. The desktop
uses the existing firmware framebuffer. It keeps the normal shell boot and does
not start a graphics session over telnet, in VGA text mode, or on another console.

The **SurfOS** menu opens About, closes the current window, or returns to the
shell. **Apps** opens Calculator and Sketch. **Options** opens pointer settings,
changes ocean/dusk colors, or clears Sketch. Click a title bar and drag to move a
window; click its x to close it. Opening an app again retains its current state
until the desktop exits.

- **Calculator:** whole-number `+`, `-`, `*`, `/`; click buttons or type numbers
  and operators. Enter calculates, C clears, Backspace removes a digit. Division
  truncates toward zero. Division by zero and values outside ±999,999,999 show an
  error; C resets it.
- **Sketch:** choose a color, hold the left button and draw, or Clear/Ctrl+N to
  erase. The first swatch erases. Sketches are temporary and are lost when the
  desktop exits. It does not write files.
- **Pointer & desktop:** three speeds and two color themes, for this session.

Keyboard shortcuts: Ctrl+K Calculator, Ctrl+D Sketch, Ctrl+P settings, Tab next
open window, Ctrl+G menu (arrows and Enter operate it), Esc closes a menu or
window, Ctrl+Q or Ctrl+C returns to the shell. F1–F4 switches consoles and cancels
the desktop, just like the graphics test. The shell's text is restored on exit.
With the menu closed, arrow keys move the pointer. In Sketch, Space toggles
the pen down/up, so drawing also works without a pointing device.

## Mouse support

`pointerinfo` reports connected pointing devices and the latest position/buttons.
The driver supports three-button PS/2 relative mice and PS/2-compatible touchpads,
and USB HID **boot-protocol** mice connected directly to a supported xHCI root
port. USB attach/unplug is supported; unplug releases that device's buttons.
The existing USB stack does not enumerate devices behind hubs. Wheel reports,
USB report-protocol-only devices and multi-touch gestures are not implemented.
`mouse=off` on the kernel command line disables PS/2 mouse probing for diagnostics.

The PS/2 controller routes keyboard and auxiliary bytes separately. Its reset,
ACK and self-test waits are bounded; an absent mouse leaves the keyboard usable.
Laptop-profile timer polling is a fallback for the legacy IRQ path. All native
transports feed a bounded event queue; on overload it drops the oldest events
and retains the newest position and button state.

**Built-in IdeaPad touchpad:** `driver/touchpad.c` adds a narrow native I2C HID
binding for the observed Lenovo 81WE firmware and Intel 8086:34e9 controller.
It reads checked static AML fields and firmware variables, restores the powered-
down controller's resources, and polls the firmware-selected input GPIO. Relative
mouse reports and the HID InputMode feature feed the same pointer queue as USB
and PS/2. It does not implement general AML execution or multitouch gestures.

The laptop's touchpad is a Synaptics 06cb:ce2d. Its descriptor has a mouse
collection (report 2: two buttons, relative X/Y) beside the Precision Touchpad
contacts, and SurfOS reads the mouse collection. The device refuses GET_REPORT
for its InputMode feature, so the driver builds the one-byte report and sets mode
0 (mouse) directly, as Linux and Microsoft's configuration collection do. A
refused mode request is recorded, not fatal: after a host reset the device
reports through its default collection anyway. Reports from any other collection
are counted as ignored and never move the pointer.

`touchpad info` shows startup stage, controller/transport state, the HID
registers, how the mouse mode was selected (with the interrupt line before and
after) and report counts.
`touchpad report` shows the last input bytes, `touchpad descriptor` exports the
captured HID report descriptor, and `touchpad up` starts it again in the
dedicated input task: after a failure, or to reset a working touchpad.
Unsupported machines are left alone. All bus waits are bounded and failure
leaves the keyboard and other pointing devices available. An I2C controller
whose error cleanup failed is set up again before the next transfer.
While the touchpad holds its interrupt line low, each 10 ms wake reads up to
three reports. A zero-length read (a device-initiated reset, or nothing
waiting) is counted and input continues, as in Linux. As in Linux and Windows,
a movement value outside the descriptor's range is clamped rather than
dropping the report.

The owner confirmed the desktop works on the laptop with the original GUI
build, but its touchpad did not. The first native build (2026-10-13) found the
device and read its report descriptor, then stopped at the refused GET_REPORT.
The 2026-10-15 build sets the mode directly; physical movement/click acceptance
is still pending. See the [hardware record](hardware/ideapad-3-15iil05.md) for
the collected firmware facts and current validation evidence.

## Implementation and checks

`driver/pointer.c` merges per-device buttons and queues position events.
`driver/ps2mouse.c` parses PS/2 packets; `kernel/keyboard.c` owns the shared 8042.
`driver/usb/mousedesc.c` validates boot-mouse interfaces and reports;
`driver/usb/mouse.c` owns USB transfers and lifecycle.
`driver/i2cdw.c`, `driver/i2chid.c`, `driver/hidpointer.c` and `driver/amlfield.c`
keep the I2C engine, wire framing, HID parsing and static firmware layouts
independently testable. `shell/acpidump.c` and `tools/acpi-fetch.py` provide
checked read-only firmware export for diagnosis.

`include/sys/gui.h` and `shell/guicore.c` contain the host-testable desktop and
apps. `shell/gui.c` owns a revocable graphics session, renders to ordinary RAM,
and presents short batches of rows. Pointer-only moves restore the old cursor's
rectangle from RAM. Full scene drawing does not run inside the framebuffer lock.
The existing task death/panic/console-switch graphics revocation remains in force.

Run `make unittest` and `make test-all`, and the complete USB suite at both
optimization levels. `tools/qemu-usb.py --test --scenario gui` checks actual
screen pixels, applications, native mouse input, hotplug and text recovery.
Emulator results do not establish laptop touchpad compatibility.

The 2026-10-12 desktop validation passed the host suite, 286 smoke checks and
51 Files checks at both O0 and O2, plus all 15 USB/UEFI scenarios at both levels
(307 checks each, including 48 GUI checks). The deployed O0 desktop was confirmed
working by the laptop owner; native touchpad input remains under development.
