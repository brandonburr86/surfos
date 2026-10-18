# SurfOS desktop

Boot the UEFI USB image, then type `gui` at the active local console. The desktop
uses the existing firmware framebuffer. It keeps the normal shell boot and does
not start a graphics session over telnet, in VGA text mode, or on another console.

*10/17/26:* the desktop has Notepad and Files, and menus that hold every command
with its shortcut. Windows can be minimized, and Notepad, Files and Help can also be
maximized and resized. Together Files and Notepad let you open a file, edit it and
save it, on a mounted disk or a USB stick. The initrd's files can be opened too, and
kept elsewhere with Save as, since the initrd is read-only.

## The desktop

The menu bar has six menus. Greyed-out items do not apply to the window in front,
and each item shows its shortcut.

| Menu | Items |
| --- | --- |
| **≋ SurfOS** | About SurfOS, Pointer & desktop, Ocean / dusk colors, Return to shell |
| **File** | New note, Open…, Save, Save as…, Show in Files, Close window. With Files in front: Open selected, New file…, New folder…, Rename…, Delete…, Copy path, Mount, Eject, Refresh, then New note, Open…, Close window |
| **Edit** | Notepad's Undo, Redo, Cut, Copy, Paste, Delete, Select all, Find…, Find next, Go to line… |
| **Apps** | Notepad, Files, Calculator, Sketch, Clear sketch |
| **Window** | Next window, Minimize, Maximize (or Restore size), Close, then every open window |
| **Help** | Keyboard and mouse (the Help window), About SurfOS |

Drag a title bar to move a window. Notepad, Files and Help also fill the screen
(double-click the title bar, or press its middle button) and resize from their dotted
corner, never narrower than their toolbar. The `_` button tucks a window into the
taskbar, and `x` closes it. The taskbar at the
bottom has a button for each open window. Click a button to bring its window to
the front, or click it again to tuck the window away. The taskbar also shows the
time from the PC's clock. The tiles on the desktop open the four applications.

**Notepad** edits one text file at a time, of up to 256 KB.

* **Editing.** The toolbar has New, Open, Save, Save as, Find, Undo, Copy and Paste.
  Ctrl+Z undoes and Ctrl+Y redoes. Typing in a row is one step. Up to 512 steps and
  64 KB of changed text are kept, and a single change bigger than that (pasting 100
  KB, say) clears the history. Ctrl+X, Ctrl+C and Ctrl+V cut, copy and paste. The
  clipboard holds 128 KB and belongs to the desktop. Ctrl+A selects everything.
* **Find and go to.** Ctrl+F opens a find bar under the text. Enter finds the next
  match in either case, starting again from the top at the end, and Esc closes the
  bar. Ctrl+L goes to a line.
* **The mouse.** Click to place the cursor, drag to select (the text scrolls when you
  pass its edge), double-click a word, or click a line number to take the whole
  line. Drag the scroll bar's thumb, or click above or below it.
* **What it shows.** A tab reaches the next multiple of 8 columns, as in `edit`.
  Lines do not wrap: the view follows the cursor sideways. The status bar shows the
  line and column, the selection, the size and the line ends.
* **The bytes are kept.** A file is shown with the console's characters (UTF-8 as
  code page 437, `?` for a character it lacks, and a byte that is not UTF-8 as the
  437 character it is). A file whose every line ends in CRLF is saved with CRLF
  again, and any other file keeps its bytes (a lone CR shows as ♪). The 256 KB
  count includes the CRs such a file is saved with, so whatever Notepad saves it
  can open again. A file with a NUL byte is not text, and Notepad does not open it.
* **Saving.** Saving writes the text to a new file beside it, then renames that over
  the file, as `edit` does. The new file is `<file>.tmp`, or `<file>.1.tmp` and so on
  when you have a file of that name; Notepad never overwrites it. If the disk fails
  during the rename, the text stays in that new file, and the message names it. The
  two may then share their space on the disk, and deleting or saving over either can
  damage the other, so until the desktop closes Notepad saves over neither (Save
  offers Save as) and Files deletes neither; check the stick on another computer
  (First Aid on the Mac, `fsck` elsewhere) before you change them. Only
  where a file system cannot rename, or a name is too long to take `.tmp`, is the file
  written in place. On a read-only volume (the initrd, or the flashed SURFOSUSB
  stick) Save says so and offers Save as.
* **Unsaved changes.** Notepad asks first, with Save, Don't save and Cancel, before
  it closes, opens or starts another file, or leaves the desktop.

**Files** shows the file system as a tree from `/`.

* **The tree.** The mount points are at the top: `/initrd`, `/dev`, and each mounted
  disk or stick. Folders come first, then files, by name, with their sizes and the
  times the file system recorded.
* **Keys and the mouse.** Double-click a folder, or press Enter or Right, to open it,
  and Left to close it. Double-click a file, or press Enter, to open it in Notepad.
  Ctrl+R reads everything again, Del deletes, and Ctrl+C copies the selected row's
  path, for Notepad or a dialog's text box.
* **The toolbar** has Open, New file, New folder, Rename, Delete, Refresh, Mount and
  Eject. The File menu has all of them too, so the keyboard reaches every command
  (Ctrl+G, then Right to File). Delete asks first, with Cancel as the default, and
  deletes only a file or an empty folder. There is nothing to change at `/`, in
  `/dev` or in the initrd.
* **USB sticks.** A USB stick that is not mounted is listed at the top as
  `usb1p1 FAT32, not mounted`. Mount, or a double-click, mounts it at `/usb1p1`.
  **Eject** unmounts it and flushes it; the status bar then says
  `usb1: safe to remove`.
  * Mount and Eject are the shell's own `mount` and `eject`, so their rules apply.
    The SurfOS boot drive mounts read-only, except the volume of a one-stick setup
    ([USB-BOOT.md](USB-BOOT.md) section 14). Eject takes every partition of the
    stick along. A busy stick is not ejected, and Eject refuses while Notepad has
    unsaved changes to a file on any partition of that stick.
  * A stick pulled out without Eject stays mounted: Files marks it `unplugged: eject
    it`, and Eject unmounts it.
  * Nothing is mounted until you ask.
* **Large folders.** Up to 256 entries of a folder, and 400 rows in all, are shown.
  A last row says how many entries did not fit. When the list is full, a folder stays
  closed and the status bar says so.
* **Long paths.** A path longer than SurfOS takes (255 bytes) is never cut short:
  nothing is done with it, and Files' status bar (or the chooser's own line) says why.

**Calculator** does whole-number `+`, `-`, `*`, `/`. Click its buttons, or type the
numbers and operators. Enter calculates, C clears, and Backspace removes a digit.
Division truncates toward zero. Division by zero and values outside ±999,999,999
show an error, which C resets.

**Sketch** draws with the left button held down. Choose a colour first; the first
swatch erases. Clear, or Ctrl+N, erases everything. Sketches are temporary: they
are lost when the desktop exits, and Sketch does not write files.

**Pointer & desktop** has three pointer speeds and two color themes. They last for
this session.

### Opening, editing and saving a file

1. Press Ctrl+O, choose Open in Notepad's toolbar, or choose File > Open…. The
   chooser lists a folder. Double-click a folder (or `..`, or Up) to move, and
   double-click a file to open it. Or find the file in Files and double-click it
   there.
2. Edit the file. Ctrl+S saves it. A note you have not saved yet asks for a name
   and a folder (Save as). If a file of that name is there already, you are
   asked before it is replaced.
3. To use a USB stick, open Files and double-click the stick, which is listed as
   not mounted. Its files are then under `/usb1p1` (the device's name). Press
   Eject in Files before you pull the stick out.

### Keys

| Keys | Action |
| --- | --- |
| Ctrl+E, Ctrl+B, Ctrl+K, Ctrl+D, Ctrl+P | Notepad, Files, Calculator, Sketch, pointer and desktop settings |
| Ctrl+N, Ctrl+O, Ctrl+S | New note, open a file, save it (Ctrl+N clears Sketch when Sketch is in front) |
| Ctrl+Z, Ctrl+Y, Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+A | Undo, redo, cut, copy, paste, select all (in Notepad) |
| Ctrl+F, Ctrl+L | Find, go to a line (in Notepad) |
| Ctrl+R, Del, Ctrl+C | Read the folders again, delete, copy the path (in Files) |
| Ctrl+W | Close the window in front |
| Ctrl+G | The menus (the arrows move, Enter chooses, Esc closes them) |
| Tab | The next window (in Notepad: a tab) |
| Esc | Close a menu or dialog; in Notepad, close the find bar or drop the selection; close any other window. With no window open at all (not merely tucked away), return to the shell |
| Ctrl+Q | Return to the shell, asking first about unsaved changes (after a dialog in front is answered) |
| Ctrl+C elsewhere | Return to the shell, as before, but asking first about Notepad's unsaved changes |

While Notepad's find bar has the keys, they are its own: Ctrl+V and Ctrl+A work in
its box, and Ctrl+X, Ctrl+Z and Ctrl+Y do nothing until Esc, the bar's x or a click
in the text gives the keys back.

In Notepad, Files and Help the arrow keys, Home, End, Page Up, Page Down and Delete
work as in any editor. Elsewhere the arrow keys move the pointer, and in Sketch
Space puts the pen down or lifts it, so drawing also works without a mouse.

**Consoles.** F1-F4 switch consoles as before. Since 10/17/26 switching away no
longer ends the desktop. It waits on its own console and draws itself again when
you come back, with Notepad's text as it was. Another console can run a desktop of
its own meanwhile. The shell's text is restored when the desktop exits.

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
The 2026-10-15 build sets the mode directly, and the touchpad now drives the
pointer on the laptop as a two-button mouse: during the owner's test in `gui`
every report was decoded, both buttons and drags registered, and nothing failed.
Multi-touch gestures (two-finger scrolling) would need the Precision Touchpad
collection. See the [hardware record](hardware/ideapad-3-15iil05.md) for the
collected firmware facts and validation evidence.

## Implementation and checks

`driver/pointer.c` merges per-device buttons and queues position events.
`driver/ps2mouse.c` parses PS/2 packets; `kernel/keyboard.c` owns the shared 8042.
`driver/usb/mousedesc.c` validates boot-mouse interfaces and reports;
`driver/usb/mouse.c` owns USB transfers and lifecycle.
`driver/i2cdw.c`, `driver/i2chid.c`, `driver/hidpointer.c` and `driver/amlfield.c`
keep the I2C engine, wire framing, HID parsing and static firmware layouts
independently testable. `shell/acpidump.c` and `tools/acpi-fetch.py` provide
checked read-only firmware export for diagnosis.

`include/sys/gui.h` is the desktop's model: no hardware access, no allocation, and
nothing shared between consoles (it all lives in the `gui_state` that `gui`
allocates). The model's files share `shell/guipriv.h`:

* `shell/guicore.c`: the window manager, the menus (tables of items), the taskbar,
  Calculator, Sketch, About, Settings and Help, and the actions that menus, toolbars,
  keys and dialogs share.
* `shell/guinote.c` and `shell/guifiles.c`: Notepad and Files.
* `shell/guidialog.c`: the dialogs, and the Open and Save as choosers.
* `shell/guidraw.c`: the drawing kit, clipped to rectangles, with the icons.

The model reaches files only through `struct gui_fs`, and only from its key and
pointer handlers, never while drawing. `shell/guifs.c` implements it on the VFS: Mount
and Eject run the shell's `mount` and `eject` with their output kept
(`console_capture_begin()`), and show their last line.

`shell/gui.c` owns a revocable graphics session, renders to ordinary RAM, and
presents short batches of rows. Since 10/17/26 it presents only the rows and
columns that changed since the last frame, against a copy of what the screen shows.
Pointer-only moves restore the old cursor's rectangle from RAM. Full scene drawing
does not run inside the framebuffer lock. The existing task death/panic/console-
switch graphics revocation remains in force. A revoked desktop waits for its console
and begins a new session there.

Run `make unittest` and `make test-all`, and the complete USB suite at both
optimization levels. `tests/host/test_gui.c` drives Notepad, Files, the dialogs and the
window manager against an in-memory file system (with a two-partition stick, a stick
pulled out while mounted, folders of 300 files and failing writes). It checks that
every window (also maximized), menu and dialog fits the screen at 640x400, 960x540 and
1280x800, and draws them all inside guard pixels.
`tools/qemu-usb.py --test --scenario gui` checks actual screen pixels, applications,
native mouse input, hotplug and text recovery. Since 10/17/26 it also runs Notepad
and Files on a FAT32 stick that the test makes: Files mounts it, a double-click opens
a CRLF file in Notepad, Save and Save as write it, Eject flushes it, and the host
reads the files back and runs `fsck` on the stick. Emulator results do not establish
laptop touchpad compatibility.

The 2026-10-12 desktop validation passed the host suite, 286 smoke checks and
51 Files checks at both O0 and O2, plus all 15 USB/UEFI scenarios at both levels
(307 checks each, including 48 GUI checks). The deployed O0 desktop was confirmed
working by the laptop owner. With the touchpad (2026-10-15) the host suite,
297 smoke checks and 51 Files checks passed at O0 and O2, and 313 USB/UEFI
checks at both levels; the owner then used the touchpad in the desktop.
