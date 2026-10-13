# SurfOS desktop and pointer input

Implement the requested small GUI on the existing revocable framebuffer session.
`gui` opens a SurfOS ocean-themed desktop with a menu bar, Calculator, Sketch,
About and pointer settings. Windows can be selected, moved and closed; both apps
retain their in-session state. Sketch is explicitly temporary. The menu returns
to the shell; Ctrl+C and console switching also restore text. Keyboard shortcuts
make it usable when a pointing device is absent. Do not change boot defaults.

Use a bounded, interrupt-safe shared pointer queue. PS/2 and USB boot-protocol
mice supply relative movement and button states, with per-device state so an
unplug or packet from another mouse cannot leave or clear unrelated held buttons.
Bound controller waits, demultiplex keyboard and auxiliary bytes, and recover
truncated packets. USB hotplug follows the existing xHCI driver lifecycle.

The GUI is a kernel command using the existing framebuffer ownership contract,
not a new userspace graphics ABI. Keep drawing/model code host-testable, prepare
a RAM scene outside the framebuffer lock, and present only changed regions.
Scale logical coordinates by integers, retain the existing physical mode, and
clip all drawing. Use the existing font with a custom navy/teal/seafoam palette.

The laptop record identifies a Precision TouchPad but does not supply its actual
ACPI identity, I2C controller resource, device address or HID descriptor address.
PS/2 compatibility will work when exposed. Native I2C HID support requires those
facts; do not guess bus addresses or claim touchpad compatibility from QEMU.
Ask for that evidence while implementing and verifying the independent GUI and
mouse paths. Record the resulting hardware limitation explicitly if unavailable.

Acceptance: pure host tests for pointer bounds, queue/button behavior and app
logic; QEMU interaction tests for visible menus, calculator, sketch, pointer
movement/click/drag, hotplug, missing devices, keyboard controls, console recovery
and remote rejection. Run the repository host and O0/O2 boot/UEFI suites.
