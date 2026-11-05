# SurfOS desktop

Boot the UEFI USB image, then type `gui` at the active local console. The desktop
uses the existing firmware framebuffer. It keeps the normal shell boot and does
not start a graphics session over telnet, in VGA text mode, or on another console.

*10/17/26:* the desktop has Notepad and Files, and menus that hold every command
with its shortcut. Windows can be minimized, and Notepad, Files and Help can also be
maximized and resized. Together Files and Notepad let you open a file, edit it and
save it, on a mounted disk or a USB stick. The initrd's files can be opened too, and
kept elsewhere with Save as, since the initrd is read-only.

*10/17/26, later:* eight more applications. The Terminal runs the shell in a window,
and there are System Monitor, Disks, Log, Clock, Character Map, Image Viewer and Hex.
Sketch saves its drawings as BMP pictures and opens them. Files opens a picture in the
Image Viewer, and a file Notepad cannot open in Hex. See
[The applications](#the-applications).

## The desktop

The menu bar has six menus. Greyed-out items do not apply to the window in front,
and each item shows its shortcut.

| Menu | Items |
| --- | --- |
| **≋ SurfOS** | About SurfOS, Pointer & desktop, Ocean / dusk colors, Return to shell |
| **File** | New note, Open…, Save, Save as…, Show in Files, Close window. With Files in front: Open selected, New file…, New folder…, Rename…, Delete…, Copy path, Mount, Eject, Refresh, then New note, Open…, Close window. With Sketch in front: Open BMP…, Save as BMP…, then New note, Open…, Close window |
| **Edit** | Notepad's Undo, Redo, Cut, Copy, Paste, Delete, Select all, Find…, Find next, Go to line…. Every application but the Image Viewer has its own while it is in front: the same items, those it does on and the rest greyed, the last one its own Go to (pid, code or offset) |
| **Apps** | Notepad, Files, Calculator, Sketch, Clear sketch, then Terminal, System Monitor, Disks, Log, Clock, Image Viewer, Hex and Character Map |
| **Window** | Next window, Minimize, Maximize (or Restore size), Close, then every open window |
| **Help** | Keyboard and mouse (the Help window), About SurfOS |

*10/26/26:* a right-click opens one of these menus where the pointer is, kept on the
screen. Over a window, which comes to the front first so that the items act on it, it
is the window's Edit menu when the window edits (Notepad, and the applications with an
Edit menu of their own), File for Files, and Window on a title bar and anywhere else;
over the desktop it is Apps; in Files the row under the pointer is selected first. Its
items are chosen and run as from the bar: a click, or the arrows and Enter, and Esc or a
click elsewhere closes it (a right-click elsewhere too; in the menu it does nothing). It
belongs to its window: when the focus leaves that window (a program's window comes up
with the focus, or the window goes) it closes, and its items never act on another.
Nothing happens on the menu bar and the taskbar, and over a program's own content the
right button stays the program's.

Drag a title bar to move a window. Notepad, Files, Help and the applications (all
but Clock and Character Map) also fill the screen (double-click the title bar, or
press its middle button) and resize from their dotted corner, never narrower than
their toolbar. The `_` button tucks a window into the taskbar, and `x` closes it. A
window whose application is busy (a Terminal session, a running timer) asks before it
closes, as leaving the desktop does. The taskbar at the
bottom has a button for each open window. Click a button to bring its window to
the front, or click it again to tuck the window away. The taskbar also shows the
time from the PC's clock. The tiles on the desktop open the applications: the four
of the desktop's own first, then the others in the columns beside them.

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
  offers Save as, under another name) and Files deletes and renames neither, nor the
  folder they are in. The desktop takes the same care whenever it finds a `.tmp`,
  `.N.tmp` or `.part` beside a file it opens, saves over, deletes or renames, of the
  file's size and time, which is how such a pair usually looks. It remembers four such
  pairs; after more, it saves over, deletes and renames no file at all until it closes,
  and saves only under new names. Check the stick with SurfOS's own `fsck`, or on another
  computer (First Aid on the Mac, `fsck` elsewhere), before you change them, also with the shell's `rm`, `mv`
  or `cp`, which take no such care. Once it is checked, delete the copy you no longer
  want with `rm` in the shell (or on the Mac), and start the desktop again: while the
  two look alike, and until it closes, the desktop keeps taking the care. Only
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
  and Left to close it. Double-click a file, or press Enter, to open it:
  * a picture (`.bmp`) opens in the Image Viewer;
  * a file that Notepad would refuse (one of more than 256 KB, or with a NUL byte in its
    first 512 bytes) opens in Hex;
  * any other file opens in Notepad. Notepad asks about its own unsaved changes only
    for a file it opens itself.

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
swatch erases. Clear, or Ctrl+N, erases everything.

* **Save as BMP.** Ctrl+S, File > Save as BMP… while Sketch is in front, or the Save
  button saves the drawing as a 400 x 180, 24-bit BMP (216,054 bytes). The Image
  Viewer, Sketch itself and other computers can open it.
  * It asks for the name and the folder. The chooser starts in the folder of the
    picture Sketch saved or opened last, or else in the last chooser's.
  * It asks before it replaces a file that is there already, as Notepad's Save as
    does, except the file Sketch itself saved last.
  * Like Notepad, it never saves over a file the desktop is taking care of (one that
    may share its space with a `.tmp` beside it, see Notepad's Saving above).
  * Unsaved sketches are lost when the desktop exits.
* **Open BMP.** File > Open BMP…, or the Open button, reads a BMP of up to 8 MB into the
  canvas. A 1920 x 1080 screenshot is 6 MB; a larger file is refused, and the Image
  Viewer shows it.
  * A picture larger than the canvas is shrunk to fit it, keeping its proportions;
    a smaller one is put in the middle.
  * Each colour becomes the nearest of Sketch's six.
  * It replaces the drawing, as Clear does. A picture that cannot be read leaves the
    drawing as it was.
* The line under the canvas says what Save and Open did, until the drawing changes (a
  stroke, Clear, Ctrl+N).

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
| Ctrl+T | The Terminal; in it every key but Ctrl+G and Ctrl+Space is the program's |
| Ctrl+N, Ctrl+O, Ctrl+S | New note, open a file, save it (with Sketch in front, Ctrl+N clears it and Ctrl+S saves it as a BMP) |
| Ctrl+Z, Ctrl+Y, Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+A | Undo, redo, cut, copy, paste, select all: Notepad's, or those of the application in front that has them |
| Ctrl+F, Ctrl+L | Find, go to: a line in Notepad; a pid, a code or an offset in System Monitor, Character Map and Hex |
| Ctrl+R, Del, Ctrl+C | Read the folders again, delete, copy the path (in Files) |
| Ctrl+W | Close the window in front |
| Ctrl+G | The menus (the arrows move, Enter chooses, Esc closes them) |
| Tab | The next window (in Notepad: a tab; in the Terminal: the program's) |
| Esc | Close a menu or dialog; in Notepad, close the find bar or drop the selection; in the Image Viewer and Hex, stop a reading or a search first; close any other window. With no window open at all (not merely tucked away), return to the shell. In the Terminal it is the program's |
| Ctrl+Q | Return to the shell, asking first about unsaved changes and about busy applications (after a dialog in front is answered) |
| Ctrl+C elsewhere | Copy, in an application with an Edit menu of its own (in the Terminal, the program's). Elsewhere it returns to the shell, as before, asking first about Notepad's unsaved changes |

While Notepad's find bar has the keys, they are its own: Ctrl+V and Ctrl+A work in
its box, and Ctrl+X, Ctrl+Z and Ctrl+Y do nothing until Esc, the bar's x or a click
in the text gives the keys back.

In Notepad, Files and Help the arrow keys, Home, End, Page Up, Page Down and Delete
work as in any editor. In the applications they do what each one's section below
says, and the Terminal sends them to the program. Elsewhere (Calculator, Sketch,
About and Pointer & desktop, the Image Viewer with no picture, Hex with no file) the
arrow keys move the pointer, and in Sketch Space puts the pen down or lifts it, so
drawing also works without a mouse.

**Consoles.** F1-F4 switch consoles as before. Since 10/17/26 switching away no
longer ends the desktop. It waits on its own console and draws itself again when
you come back, with Notepad's text as it was. Another console can run a desktop of
its own meanwhile. The shell's text is restored when the desktop exits.

## The applications

*10/17/26.* Each of these is in the Apps menu and has a tile on the desktop. Only the
Terminal has a shortcut of its own (Ctrl+T). None of them takes the desktop's other
shortcuts, except the Terminal, whose keys are the program's. Each has its own lines
in Help. Every one fits a 640x400 screen.

### Terminal

The **Terminal** runs surfsh, the SurfOS shell, in a window, on a pseudo-terminal of
its own. Every command works as on F1-F4. The prompt shows `≋@>`, as surfsh shows on
any pseudo-terminal, a telnet session's too. Open it with Ctrl+T, its tile, or Apps >
Terminal. Each Terminal window has one session, started when it opens, and a desktop
has one Terminal window.

* **Keys.** Every key is the shell's except Ctrl+G and Ctrl+Space, which open the menus.
  * Ctrl+C stops the program that runs (`sleep 30000`, say).
  * Ctrl+Q, Ctrl+W, Ctrl+T, Tab, Esc and the arrows are the program's too (`edit`
    quits with Ctrl+Q).
  * The arrows, Home, End, Page Up and Down, Delete and Insert reach the program as the
    escape sequences a terminal sends. Enter, Backspace, Tab and Esc reach it as the
    bytes they are.
  * To reach the desktop with the Terminal in front, use Ctrl+G and the menus (Return
    to shell is in the ≋ SurfOS menu), or the mouse.
* **The size.** The window's size is the terminal's, and the status bar shows it. On a
  1920x1080 screen it opens at 77 by 21 characters and is 117 by 26 maximized; on a
  1280x800 screen (a desktop of 640 by 400 pixels) it opens at 71 by 16 and is 77 by 18
  maximized.
  * A new size reaches the shell when the window is maximized or restored, or when its
    corner is let go.
  * A program sees it when it asks (`pagetest size`; `edit` with Ctrl+L).
  * Lines are not rewrapped: they are cut or padded.
* **Scrolling back.** About 1,000 lines are kept above the screen.
  * Use the scroll bar (click above or below the thumb for a page, or drag it), or the
    wheel of a mouse that reports one (SurfOS's mouse drivers report none yet).
  * While the view is scrolled back, new output does not move it, and the status bar
    says how many lines came below.
  * Any key goes back to the end, and to the program.
* **Copy and paste.** Drag to select (past the top or bottom edge the text scrolls), or
  double-click a word (also one that a line's end broke in two).
  * Edit > Copy copies the selection, or the whole screen as it shows when nothing is
    selected.
  * A line that wrapped is copied as one, also after the window got wider or when it is
    in the scrollback. One that a narrower window cut on the screen is copied as it
    shows. Of a line in the scrollback from when the window was wider, a copy of the
    screen takes what shows, and a selection all of it.
  * A selection of nothing but blanks leaves the clipboard as it was, and the status bar
    says "Nothing copied".
  * Edit > Select all selects everything kept, the scrollback too.
  * Edit > Paste types the clipboard into the session, each line end as Enter. What the
    shell does not take at once follows a moment later.
  * As on every SurfOS terminal, a pasted block of several lines runs its first line
    only: the rest of a paste is skipped until the input pauses. A paste into `edit`
    arrives whole.
  * The clipboard is the desktop's, so text goes both ways with Notepad and the other
    windows.
  * Ctrl+C and Ctrl+V are the program's, so copy and paste are in the Edit menu (Ctrl+G,
    Right, Right).
* **The end of a session.** When the shell exits (`exit`, or Ctrl+D at an empty prompt),
  the session ends at once, and a line in the window and the status bar say so. A
  program the shell left running in the background (`pagetest hold 300000 &`, say) ends
  with it, as when the window closes. Enter, or the New session button in the status
  bar, starts a new session below; Esc or Ctrl+W then closes the window.
* **Closing.** Closing the window while a session runs, or leaving the desktop, asks
  first.
  * Close (or Leave) ends the session and every program it runs; Cancel keeps it.
  * The session goes on while the window is tucked into the taskbar, and while another
    console has the screen.
  * Nothing the Terminal shows or is typed in it is logged. The kernel log only says
    when a session starts and ends.
* **Limits.**
  * There is no keyboard path to the scrollback: the keyboard driver sends the special
    keys without their modifiers, so there is no Shift+Page Up to take for it.
  * Ctrl+G and Ctrl+Space (NUL) cannot be typed into a program (edit's "go to line"
    needs another key).
  * There is no mouse reporting to programs, no bracketed paste, and no bold font
    (bright colours instead, as on the console).
  * The font is code page 437, as on the console, so a character it lacks shows as `?`.

### System Monitor

**System Monitor** shows what the machine is doing. Its toolbar has three tabs,
Processes, Memory and System, and End task; Left and Right also change the page. It
reads the system again every second while it is open. Tucked away in the taskbar it
waits, and reads again as soon as it is back. A reading that fails keeps what was read
before, and the status bar says so.

* **Processes** lists every task as `ps` does: its pid, name, state (running, ready,
  blocked, sleeping, zombie), console (F1-F4, `pty n` for a terminal's session, `-` for
  none) and whether it is a program or a kernel thread. A kernel thread's console is that
  of the task that started it: the boot's threads are on F1, a `tcpecho` started from F2
  is on F2, and one started from a terminal's session is on F1.
  * **Sorting.** Click a heading to sort by it, and again to reverse the order (a ▲ or ▼
    beside it says which). S sorts by the next column and R reverses it. Tasks that sort
    alike stay in ascending pid order, also when the order is reversed.
  * **Selecting.** Click a task, or move with the arrows, Page Up, Page Down, Home and
    End. The wheel and the scroll bar move the list. The selection follows its task while
    the list changes. When the task is not in a reading (it ended), nothing is selected
    until you move: the status bar says its pid is not in the list now, Down goes on from
    its row, and if it is listed again it is selected again.
  * **End task** (or Del) asks first ("End edit (pid 9) on F2?", then "Ends now or after
    a system call; unsaved work is lost."), with Cancel as the default, so Enter alone
    ends nothing.
    * It ends programs only: for a kernel thread (init and the idle task are kernel
      threads) the button is greyed, and Del says why in the status bar.
    * A program running its own code ends at once. One inside a system call or a kernel
      command ends when that call does, however long it takes ("edit (pid 9) ends when its
      system call does"), so that nothing it holds, such as a volume's lock, is left held.
      Once it is gone from the list, the status bar says so ("edit (pid 9) has ended").
    * A task whose system call never ends is never ended by End task. The shell's `kill`,
      which kills at once after 2 seconds, is the way out.
    * The kernel refuses this desktop's own task, the shell that ran `gui` ("surfsh (pid
      12) runs this desktop: Ctrl+Q leaves it"), and the shells above it ("surfsh (pid 4)
      runs the shell this desktop runs in"), since a new shell on the desktop's console
      would take its keys. The status bar says so.
    * Any other console's shell may be ended. init then starts a new one on that console,
      with a new pid, as when the shell's `kill` ends it. Three such ends within 30
      seconds leave that console with the recovery shell.
  * The status bar describes the selected task, and counts the tasks at its right. Up to
    128 tasks are listed; with more, the newest 128, and the count says "the newest 128
    of N".
* **Memory** shows physical memory, the kernel heap and the DMA heap, each with a bar of
  what is used (teal; coral from 90%) and its numbers.
  * Physical memory is the 4 KB pages SurfOS manages, the RAM the firmware reported, and
    any above 4 GB, which a 32-bit kernel leaves unused.
  * A heap grows when it needs to, up to a limit: the kernel heap's 256 MB of address
    space, and the DMA heap's fixed arena. Its bar is of that limit: "at most 256 MB".
* **System** shows:
  * the processor and the firmware (UEFI or BIOS);
  * the profile (laptop or standard);
  * the display: the framebuffer's mode, and the desktop's size in its own pixels;
  * the build (/initrd/etc/build-id) and the uptime (h:mm:ss);
  * how many tasks and kernel timers there are.
* **Short windows.** In a short window the Memory and System pages scroll: the arrows,
  Page Up and Down, Home and End, the wheel, or their scroll bar.
* **The Edit menu** is System Monitor's while it is in front.
  * Copy (Ctrl+C) copies the selected task's line, or the Memory or System page as text,
    for Notepad or a bug report. A page that could never be read copies nothing, and the
    status bar says so.
  * Find (Ctrl+F) selects the next task whose name holds what you type, in either case,
    after the selected one (or from the row of the one that ended), going round past the
    end, and Find next does it again.
  * Go to pid (Ctrl+L) selects a task by its pid.
* Limits: no processor time per task; at most 128 tasks are listed, the newest; names
  are cut at 15 characters.

### Disks

**Disks** shows the block devices as `lsblk` lists them, each disk with its partitions
under it. For each it shows, in lsblk's own words:
* its name, size and bus (usb, ata, ram);
* its type (`disk gpt`, `part mbr FAT32 LBA`) and its model or partition name;
* its status (`mounted`, `boot drive: protected`, ...).

The columns are as wide as what they hold, and the model gives way first when the window
is narrow, as in lsblk. Disks reads the devices again about every two seconds while it
is open, so a stick that is plugged in appears by itself. A reading that fails keeps the
list read before, and the status bar says so.

* **Details.** Below the list are the selected device's details: what it is (a USB disk
  and its model, or a partition of which table). For a mounted volume they add where it
  is mounted, its file system, read-only or read-write, its volume label, and a bar of
  the space used, with the numbers (or why its file system could not say).
* **Mount** (M, or Enter) mounts the selected USB volume at `/<device>`. On a stick's own
  row it mounts the stick's first partition that is not mounted, and selects it,
  scrolling to it.
* **Eject** (E) unmounts every volume of the selected stick and flushes it. The status
  bar then says `usb1: safe to remove`.
* **The rules of `mount` and `eject`.** Mount and Eject are the shell's own commands, and
  what they say goes to the status bar. So their rules hold:
  * the SurfOS boot drive mounts read-only (the details say so beforehand);
  * a busy volume is not ejected;
  * nothing is mounted unless you ask;
  * Eject also refuses, with a message in the status bar, while Notepad has unsaved
    changes to a file on that stick, as Files does.
* **Show in Files** (or Enter on a mounted volume) opens Files at the mount point. Files,
  when it is open, shows a mount or an eject made in Disks at once.
* **Refresh** (Ctrl+R) reads the devices again now.
  * A stick pulled out while mounted stays in the list, `unplugged: eject it`, until
    Eject unmounts it.
  * A device that is not in a reading (pulled out) stays followed: nothing is selected
    (M, E and Enter say so), the status bar says it is not there any more, Down goes on
    from its row, and if it comes back it is selected again.
* Ctrl+C copies the selected device's line. A double-click on a row is Enter.
* Limits: Mount and Eject are for USB volumes; the initrd and internal disks are only
  shown.
  * Disks reads nothing from a device itself, so an unmounted volume's own label is not
    shown (lsblk does not show it either).
  * How full a mounted volume is comes from its file system, which answers once another
    task's write to that volume is done. So Disks can pause while a long copy to that
    volume runs. FAT counts a read-only volume's free space once, when it is mounted or
    first asked.
  * A file system that keeps no count of its space (the initrd's tarfs) has no bar.
  * Up to 32 rows are listed. With more devices, the sticks pulled out while mounted are
    kept, so that they can still be ejected.

### Log

**Log** shows the kernel log, as `dmesg` does. It reads it again twice a second while
its window shows; a window tucked into the taskbar reads it again when it comes back.

* **Follow.** When the window opens, Follow is on, and the newest lines stay in view as
  they come.
  * Scrolling up, or selecting lines (a click, a drag or Select all), stops it.
  * End, Follow in the toolbar, or scrolling down to the end starts it again: Down, Page
    Down or the wheel, also when the view is there already.
  * Follow is lit in the toolbar while it is on, and the status bar says "Follow on" or
    "Follow off: End".
* **Reading.** Up, Down, Page Up, Page Down, Home, End, the wheel, and the scroll bar
  (click above or below its thumb to page, or drag the thumb).
  * Long lines wrap at the window's edge, their later rows two cells in, so every line
    can be read whole. A wider window (maximize, or the corner) wraps less.
  * Characters are shown as on the console: UTF-8 as code page 437, and a tab to the next
    multiple of 8 columns. Other control characters and colour codes are not shown.
  * While the find bar is open, Home, End, Left and Right move in its box; Up, Down, Page
    Up and Page Down still scroll the log.
* **Find.** Ctrl+F (or Edit > Find…) opens a find bar under the log, as in Notepad.
  * Enter finds the next match, ASCII letters in either case. It starts after the last match
    (or from the selected lines, or the top of the view).
  * At the end it starts again from the top ("Found it again from the top").
  * The match's line is selected and the word marked in yellow. Edit > Find next finds
    the next one with the bar closed.
  * Esc closes the bar; the next Esc closes the window.
* **Copy.** Click a line to select it, or drag over several. Past the log's top or bottom
  edge the drag scrolls a row each time the pointer moves, and goes on while the pointer
  is held there.
  * Ctrl+C, Copy in the toolbar or Edit > Copy puts the lines on the desktop's
    clipboard, each with its line end, ready for Notepad or the Terminal. The newest
    line, which the kernel may not have finished yet, is copied as it is, without one.
  * Ctrl+A (Select all) selects the whole log.
  * The status bar counts the lines and the selected ones.
* **Limits.** The kernel log keeps its last 16 KB. When more has been written, the oldest
  lines are gone, and the status bar says "the newest 16 KB".
  * The first line shown may be the end of a line whose start has gone.
  * A selection keeps the lines it still has whole, and goes when none is left. A match on
    a line that went is dropped.

### Clock

**Clock** has four pages, chosen in its toolbar or with the keys 1 to 4. *10/26/26:* it is a
program now, `wclock` (`user/wclock/`), out of the kernel: Apps > Clock and its tile start it,
or bring its window back when it runs already, and the window opens where the desktop's Clock
stood, looking the same. A fault in it costs only its window.

* **Clock.** The time, large, from the PC's clock (the CMOS clock, as the taskbar's
  clock), the weekday and the date ("Saturday", "October 17, 2026") and the day of the
  year.
  * The weekday is worked out from the date.
  * There is no time zone: the clock is whatever the PC's clock keeps (under QEMU, UTC;
    on a PC that also runs Windows, usually local time).
* **Calendar.** A month, weeks from Sunday, today marked.
  * `<` and `>` (or Left and Right, Page Up and Page Down) go a month back or on, and Up
    and Down a year. Today (or Home) goes back to this month.
  * A click on a day names it in the status bar ("Thursday, October 1, 2026: day 274").
* **Stopwatch.** Start and Stop (Space), Lap (L) and Reset (R).
  * It shows tenths of a second, and hours from the first hour on.
  * The newest five laps are listed, each with its time since the lap before; eight are
    kept.
* **Timer.** Set 0:00 to 99:59 with the buttons (one minute, ten seconds) or the keys:
  Up and Down a minute, Page Up and Page Down ten seconds, Left and Right one, Home 0:00.
  * Start, Pause and Resume with Space or the button. Reset (R) sets it back to the time
    set.
  * While it runs, the title counts down ("Clock - 04:59"), in the taskbar too.
  * At zero the title says "Time's up!", in the taskbar too, and the timer's page shows it
    in coral. *10/26/26:* the window comes back in front of the others, even when it was
    tucked away, but without the focus, its taskbar button marked: what you type elsewhere
    stays where it goes (the desktop's message in front of everything is gone with the
    move out of the kernel).
  * A key or a click in the window answers it: the timer is set back to its time.
* **In the background.** The stopwatch and the timer keep running while the window is
  minimized or behind another window, and the status bar of the other pages shows them
  (the stopwatch in whole seconds there).
  Closing the window ends both. While one is running, closing it (the close button, Esc or
  Ctrl+W) asks first, in the window, Cancel first; leaving the desktop asks, as it does
  about every program's window.
* **Copy.** Ctrl+C (Edit > Copy) copies what the page shows: the date and time, the month
  as `cal` prints it (its title centred over the weeks), the stopwatch's time and the laps
  it keeps (up to eight), or the timer's time. On the clock's page with no date known,
  nothing is copied (the status bar says so) and the clipboard is left as it was.

### Character Map

**Character Map** shows the console's 256 characters (code page 437, with SurfOS's ▸ ≋
…) in a grid of 16 by 16. The code's last hexadecimal digit is along the top, and its
first down the side. *10/26/26:* it is a program now, `wcharmap` (`user/wcharmap/`), out of
the kernel: Apps > Character Map and its tile start it, or bring its window back when it
runs already, and the window opens where the desktop's Character Map stood, looking the
same. A fault in it costs only its window.

* **Choosing.** Click a character, or move with the arrows.
  * Home and End go to the start and the end of the row, Page Up and Page Down to the top
    and the bottom of the column.
  * A key typed chooses its own character (A, say).
  * Ctrl+L (Go to code) opens a band at the bottom of the window, with the chosen code in
    it. It takes a code (247, three digits at most, or 0xF7), a Unicode code point
    (U+224B, six hexadecimal digits at most), or a character other than a digit or a
    space (é, typed on the serial line, say). The first key typed replaces what was put
    there; Enter or Go goes there, Esc or Cancel leaves it. Anything else, and a
    character the console does not have (€), is refused in the status bar. (*10/26/26:*
    this was the desktop's Edit > Go to code… dialog, which a program's window does not
    have; a character can no longer be pasted into it.)
* **What it shows.** The chosen character large, with its code in decimal and hexadecimal,
  its Unicode code point and its UTF-8 bytes.
* **Copy.** Enter, Ctrl+C, Edit > Copy, a double-click or the Copy button puts the
  character on the desktop's clipboard as UTF-8, and the status bar says so.
  * Ctrl+V pastes it in Notepad: one character, saved as its UTF-8 bytes. Edit > Paste
    pastes it in the Terminal.
  * The laptop's keyboard types nothing beyond ASCII, so this is how ≋, é, ½ or █ get
    into a note.
* **Small screens.** On a 640x400 screen the window is shorter (as tall as the room
  between the menu bar and the taskbar lets it be) and its cells a pixel lower, so that
  the whole grid and the Copy button stay above the status bar.
* **Codes 0-31 and 127.** On the console these are symbols (☺, ♥, ♪, ▸, ← and the like,
  and ⌂), not control characters, and those symbols are what is copied (☺ is U+263A).
  0 is the console's empty cell, and copies nothing.

### Image Viewer

**Image Viewer** shows BMP pictures. Double-click a `.bmp` in Files, or use the
toolbar's Open or press O: the chooser lists a folder as Notepad's does.

* **What it reads.** Uncompressed BMP files:
  * of 1, 4 and 8 bits with a palette, and of 24 and 32 bits;
  * stored bottom-up or top-down;
  * with any of the usual headers: BITMAPINFOHEADER and its V2 to V5 versions, and 32-bit
    BI_BITFIELDS with the usual masks.

  Other files are refused, and the message says why: RLE compression, 16-bit pixels, OS/2
  bitmaps, a PNG, JPEG or GIF (or a BMP with a PNG or JPEG inside), a file cut short. A
  picture is drawn opaque: its alpha, if any, is not used.
* **Size.** Pictures of up to 16384 x 16384 pixels are read.
  * A picture of more than 1,048,576 pixels (1024 x 1024) is reduced while it is read,
    keeping its proportions: 1920 x 1080 is kept at 1365 x 768, and the status bar says
    `kept at 1365 x 768`.
  * A picture is read a piece at a time, one piece in each frame the desktop draws (at
    most 128 reads, and 256 KB plus one 64 KB chunk, a frame, and a single read when it
    opens). So the desktop goes on
    answering while a big file is read. The window and the status bar count the percent
    (`Reading sea.bmp... 45%`), and Esc stops the reading.
  * When a file cannot be read to its end (it was cut short, or the stick was pulled
    out), the window and its status bar say so. No message box interrupts what you are
    doing meanwhile.
* **The views.** Fit is the default: a picture larger than the window is shrunk to fit
  it, and a smaller one is enlarged by a whole number (2 times, 3 times, ...), as far as
  it fits, so that its pixels stay square.
  * Press 1 (or the toolbar's Actual size) for the actual size.
  * + and - (or Zoom in, Zoom out) zoom between 1/64 and 16 times. Zooming keeps the point
    in the middle of the window where it is.
  * F or 0 fits again. Fit follows the window as it is resized or maximized.
  * A double-click anywhere in the picture area (the picture or the dark space around it)
    switches between Fit and the actual size.
* **Moving a zoomed picture.** Drag it with the mouse, or use:
  * the arrows (32 pixels a press);
  * Page Up and Page Down (the picture area's height less 32 pixels);
  * Home (the top left) and End (the bottom right);
  * the wheel: up and down, or sideways when the picture is no taller than the window.
* **The status bar** shows the name, the picture's size and bits per pixel, and the zoom
  (`Fit 1000%`, `150%`). The title bar shows the file's name. Closing the window forgets
  the picture.

### Hex

**Hex** shows any file's bytes, read-only. Files opens here a file that Notepad would
refuse; Hex's own Open (or O) opens any file.

* **A row** is the offset (8 hexadecimal digits), 16 bytes in hexadecimal, and the same
  bytes as characters: printable ASCII as it is, and a grey dot for every other byte (a
  real `.` is dark). A header row numbers the columns.
* **Any size.** Only what the window shows is read, so a file of any size up to 4 GB
  opens at once.
* **Moving.** The arrows move the cursor a byte or a row, Page Up and Page Down a
  window's rows, Home and End to the file's first and last byte.
  * The wheel and the scroll bar (drag the thumb, or click above or below it) move the
    view.
  * Click a byte, in either column, to put the cursor there, or drag over several to
    select them. The view scrolls when the pointer passes its edge.
* **Copy.** Ctrl+C (Edit > Copy, or the toolbar's Copy) puts the selected bytes on the
  clipboard in hexadecimal, `DE AD BE EF`, up to 43,690 bytes.
  * Edit > Select all selects the whole file.
  * The toolbar's Copy offset puts the cursor's offset there, `0x0000001F`.
* **Go to offset.** Ctrl+L (Edit > Go to offset…, or the toolbar's Go to) takes `0x1F00`
  in hexadecimal or `7936` in decimal, and the row goes to the top of the window. An
  offset past the end is refused.
* **Find.** Ctrl+F (Edit > Find…, or the toolbar's Find) finds bytes from the start of the
  selection on (the cursor's byte when only one is selected).
  * Type text as it is, or bytes in hexadecimal: two or more pairs with spaces between
    them (`DE AD BE EF`), or after `0x` (`0xDEADBEEF`).
  * A single pair such as `FF` is text (the two characters F and F): one byte is written
    `0xFF`.
  * Text in double quotes is always text: `"DE AD"` finds the five characters.
  * Up to 128 bytes, matched exactly (letters in their case). The match is selected.
  * N, Edit > Find next or the toolbar's Find next finds the next one. At the end of the
    file the status bar says `Not found from 0x… to the end`, and says it again at the
    next N; Home, then Find next, searches from the start. A Find whose text is not one
    (`0xZZ`) stops a search that was running.
  * A long search runs a piece at a time (the status bar counts the percent), and Esc
    stops it.
* **The status bar** shows the cursor's offset in hexadecimal and decimal, how many bytes
  are selected, the last message, and the file's size. The title bar shows the file's
  name. Hex opens files, not devices. Closing the window forgets the file.

### The desktop's side of the applications

*11/05/26* (`docs/superpowers/plans/2026-11-05-desktop-apps.md`, track desk). What the desktop does
for the programs of the desktop applications, and for any other program with a window.

* **Apps > Programs.** The programs of `/initrd/apps/` (`rootfs/apps/<name>.app`, the format in
  `rootfs/apps/README`) are in a menu of their own, the last item of Apps (after Run program...,
  which keeps its place), by their titles; Right or Enter opens it, Left goes back to Apps. A menu
  taller than the screen scrolls: the arrows follow the selection, the wheel turns it, and strips of
  arrows at its ends move it a row (Apps does on a 640x400 screen once Programs is in it). Those
  with `desktop = yes` also have tiles, after the desktop's own and the suite's, as many as the
  screen has room for. Choosing one starts the program; choosing it again while its window is
  there brings that window to the front. A program's window shows its entry's icon in its title
  bar and on its taskbar button. An entry the desktop cannot take is left out, and the kernel log
  says why (`dmesg`: `gui: apps/...`).
* **Files opens a file in its program.** Enter or a double-click on a file whose type (its
  extension, or its whole name for a `=Makefile` type) an entry's `types` names starts that program
  with the file's path as its argument (spaces and all). What no entry names keeps the old rules:
  a `.bmp` in the Image Viewer, a file
  Notepad refuses in Hex, the rest in Notepad. **File > Open with** (Files in front, a file
  selected) lists every program whose `types` or `open-with` name the file's type, then the Image
  Viewer for a picture it reads, Notepad and Hex.
* **A program's menus.** While a program's window that has menus of its own (`WIN_MENU`) has the
  focus, the menu bar reads `≋ SurfOS`, the program's menus, Apps, Window and Help: its menus take
  the place of File and Edit. Ctrl+G opens the bar as always, and the arrows, Enter, Esc and the
  mouse work in its menus as in the desktop's. A greyed item cannot be chosen; a checked one has
  `√` before it, one of a group `•`. An item with `▸` opens a submenu in its menu's place, as Apps >
  Programs does (Right, Enter or a click; Left goes back). A program's Help items come first in the
  desktop's Help, above a line and the desktop's own. A full bar fits a 640x400 screen; "Ride the
  wave." gives it room.
* **Its choosers.** A program asks for the desktop's Open, Save as or Choose folder chooser
  (`WIN_DIALOG`), only while its window has the focus and after you did something in it (a key, a
  click or a menu's item since its last chooser, so that a program cannot keep one up); the title
  bar names the window it is for ("Paint: Save as"). It lists the types it names (and the
  folders), and a Save as name without one of them gets the first. Save as asks before it replaces
  a file and never takes one the desktop takes care of (a `.tmp` beside it, as Notepad's Saving
  says). Choose folder lists the folders: Right or a double-click goes into one, Enter or Choose
  takes the one selected (the one shown while `..` is).
* **Keys and the clipboard.** A program's window gets every key with its modifiers, Ctrl+Shift+Left
  and Alt+X and F5 to F12 included, Ctrl+Tab apart from Ctrl+I and Ctrl+0 at all, and a key held
  down says when it repeats (`include/surfos/win.h`, "The keys", says exactly what comes: the
  keyboard driver gives the desktop each key with its modifiers, `TTY_RAW_KEYS` in
  `include/surfos/tty.h`), and the modifiers held with a click or a turn of the wheel. An Alt key
  is no longer an Esc and the key (that Esc closed the Calculator, the Clock and the Character Map);
  the Terminal still sends the Esc to its shell, as a terminal does. While its window has the focus it may
  read the desktop's clipboard (`WIN_PASTE`), as it may write it (`WIN_CLIP`). It reads the
  desktop's theme's colours (`WIN_THEME`) and hears when they change.
* **Screenshots.** Print Screen, or SurfOS > Screenshot for a keyboard without the key, captures the
  screen as it is, then the Save as chooser asks where to put it, offering `Screenshot YYYY-MM-DD
  HHMMSS.bmp` in the first volume mounted that can be written (a USB stick's), or `/`. It is saved
  as a 24-bit BMP of the desktop's own pixels (960 x 540 on the laptop's 1920x1080 screen, each
  shown there as 2 x 2), the way Notepad saves (a new file beside it, renamed over), with the same
  care. One screenshot at a time.
* **Pointer & desktop is kept.** Its colours and pointer speed are `desktop.theme` (`ocean` or
  `dusk`) and `pointer.speed` (`slow`, `normal` or `fast`: the window's 1, 2 and 3) in
  `surfos-config/settings.conf` on the stick SurfOS was started from (the `settings` command lists
  them). The desktop reads them when it starts and each second after, so a change made by Settings
  or the `settings` command applies at once; a change made in Pointer & desktop is written there
  when there is a settings stick, and its last line says whether it was.

### Paint

*11/05/26* (track paint of `docs/superpowers/plans/2026-11-05-desktop-apps.md`): **Paint**, `wpaint`
(`user/wpaint/`), draws pictures, opens BMP, PNG, GIF and JPEG files and saves BMP and PNG ones. Apps >
Programs and its tile start it; Files' File > Open with offers it for a picture (a double-click
still shows a picture), and starts it with the file's path. The tool palette is at the left, the
canvas in the middle with its scroll bars, the colours under it, and the status bar says the last
message or the tool's hint, then where the pointer is, the selection, the canvas's size and the
zoom.

* **The canvas.** 32-bit colour, up to 4096 pixels each way and 16,000,000 in all (4000 x 4000), so
  that every picture Paint makes can be saved and opened again. A new one is 640 x 400, white;
  File > New asks its size, Image > Canvas size changes it from its top left (the new part the
  second colour). Paint has no transparency: a picture's transparent pixels open onto white.
* **The tools** (the palette, the Tools menu, or a key): pencil P, brush B, eraser E, line L,
  rectangle R, ellipse O, fill F, text T, colour picker K, select S. The left button draws in the
  first colour, the right in the second, and the eraser paints the second. Under the tools are the
  chosen one's options: the brush's sizes (1 to 24 pixels; [ and ] too) for the brush, the eraser,
  lines and outlines, outlined or filled for rectangles and ellipses, 1 to 8 times for text.
  * A line, a rectangle or an ellipse shows while it is dragged and is drawn when the button is
    let go; Shift held keeps a line at 45 degrees and makes squares and circles. Esc while it is
    dragged draws nothing.
  * The fill fills the area of the colour clicked, 4-connected, however large and twisted.
  * Text: a click starts it, keys type it in the console's characters (UTF-8 as code page 437, `?`
    for one it lacks), Enter starts a line, Ctrl+V pastes the desktop's clipboard's text into it. A
    click elsewhere or another tool draws it; Esc or Undo drops it.
  * The colour picker takes the colour clicked (the right button: as the second colour) and gives
    the tool before it back.
* **The selection.** A drag with the selection tool marks a rectangle. A drag inside it, or the
  arrows (ten pixels with Shift), lifts its pixels: they float, and what they leave is the second
  colour; a click outside, Enter, another tool or anything that draws puts them down, Esc puts them
  down and selects none, and Undo before that puts them back. Cut, Copy and Paste use Paint's own
  clipboard (Paste floats the pixels at the top left of the view); Delete fills the selection with
  the second colour.
* **Undo and Redo** (Ctrl+Z, Ctrl+Y) go over the last 100 changes, 32 MB of them: Paint keeps the
  64 x 64 pieces of the canvas a change touched, run-length coded, so a stroke on a large blank
  canvas costs a few bytes. A change larger than that is made but cannot be undone, and the status
  bar says so. Undoing back to what was saved clears the title's `*`.
* **The view.** Zoom from 12% to 3200%: + and - (Ctrl++, Ctrl+-), Ctrl+0 for 100%, Ctrl+F to fit,
  Image > Zoom for each level, and Ctrl with the wheel about the pointer. The wheel scrolls (across
  with Shift), the middle button drags the view, and the arrows move it when nothing is selected.
  A picture opened that is larger than the view is fitted to it.
* **The colours.** The first colour over the second (a click there swaps them, as X does), the 24
  of the palette and 8 custom colours: the left button makes one the first colour, the right one the
  second. Edit... (Colours > Edit colour...) takes `#RRGGBB` or `red, green, blue` and makes it the
  first colour and the newest custom one. The first colour's code shows beside them.
* **Files.** Open and Save as are the desktop's choosers (their bars name Paint), which ask before
  a file is replaced. Save writes the picture as its name says, `.png` (8 bits a sample) or `.bmp`
  (24 bits); a picture opened from a GIF or JPEG, or never saved, goes through Save as, `.png`
  offered. A file you have is replaced as `edit` saves one (`user/bin/saveutil.h`: a new file beside
  it, fsynced and renamed over it), never one that may share its space with a `.tmp` beside it, and
  a file Paint makes is removed again if the save fails. New, Open and Close ask first about
  changes not saved (Save, Don't save, Cancel). A file of more than 72 MB, or one that is no picture,
  is refused, and the canvas stays as it was.
* **Limits.** No transparency, layers or anti-aliasing; no GIF or JPEG saved. Its clipboard is
  Paint's own (the desktop's holds text). Everything it keeps comes to at most 126 MB, twice the
  largest canvas and 4 MB: the history gives way first, and what does not fit then is refused,
  saying so.

## Programs' windows

*10/25/26* (milestone M12, `docs/superpowers/plans/2026-10-25-app-windows.md`): an ordinary
program can open a window on the desktop, draw in it and take its keys and its pointer.
Type `wintest show &` in the Terminal: a window of four coloured quadrants opens in front,
and its title says what it was last sent.

* **Which programs.** Programs started in the desktop's Terminal, or on the console the
  desktop runs on, may open windows on it. Programs on other consoles and in telnet
  sessions cannot: their window calls say ENODEV. A window appears when its program first
  draws in it, or after a quarter of a second, in front of the others, in a cascade from
  the top left, but without the focus: what you type still goes where it went (a program
  in the background never catches the rest of a password typed in the Terminal). Its
  taskbar button is marked until a click on the window, on that button, or Next window
  (the Window menu) gives it the focus. A window you asked for takes the focus as it
  appears: that of a program typed at the Terminal's prompt without `&` while the
  Terminal has the focus, and the first of a program started from Run program....
* **The pointer and the keys.** Everything the pointer does over a program's window is
  the program's: moving over it, all three buttons, the wheel. A press in it brings the
  window to the front, and while a button is held the program follows the pointer
  wherever it goes. While its window has the focus every key is the program's, but Ctrl+G
  and Ctrl+Space, which open the menus, as in the Terminal.
* **Moving and resizing.** Drag the title bar to move the window. A program that allows
  it can be resized from the dotted corner and maximized: it is told the new size when the
  corner is let go, and draws itself again at that size. The taskbar and the Window menu
  list programs' windows with the others.
* **Closing.** The close button asks the program to close its window. A program that does
  not is asked about the second time: "End the program?", with Cancel first, and End stops
  it and takes its window away. A program that ends or crashes takes only its own window
  with it. Leaving the desktop with programs' windows open asks first; their windows close
  with the desktop, and the programs are told so (they are not stopped).
* **The clipboard and asking to be seen** (*10/26/26*, for the applications moving out of
  the kernel). While its window has the focus, a program may put text on the desktop's
  clipboard (`gw_clip()`, up to the clipboard's 128 KB, to a NUL if one comes first; no
  text is refused), as the desktop's own Copy does: Notepad and the Terminal paste it.
  Without the focus it may not, so nothing in the background plants text to be pasted, and
  text a window set as it lost the focus is dropped. A program may ask to be seen
  (`gw_alert()`: a timer that ran out), once in five seconds at most, whichever of its
  windows asks: its window comes to the front, shown again if it was tucked into the
  taskbar, without the focus, its taskbar button marked until it has it, as a new
  window's is. Nothing you type elsewhere reaches it. It never comes up under a click: a
  click already on its way lands where it was aimed, the window waits while a mouse button
  is down or a drag goes on, and for half a second after it came up a click on it does
  nothing at all (click again to use it). In `wintest show`, x copies "echo wintest-clip"
  and y asks to be seen once the window is tucked away.
* **Limits.** 8 windows on a desktop, 32 in all. A window's content is at most the room
  below the menu bar (958 by 457 on a 1920x1080 screen), and a desktop keeps at most 8 MB
  of programs' pixels.
* **Writing one.** libgui (`user/include/gui.h`, in libsurf) gives a program a window with
  an image to draw in: `gw_open()`, then rectangles, lines, UTF-8 text in the console's
  font, images and buttons, drawn as the desktop draws its own windows (its drawing kit,
  `include/sys/guidraw.h`, is built into libsurf); `gw_present()` shows what changed, and
  `gw_next()` waits for the next event and remakes the image at a new size by itself.
  `wdemo` is the example: colour bars, its size and the last event it got (`wdemo hang`
  pays no heed to the close button, `wdemo crash` faults after it first draws). Until the
  kernel keeps floating point across task switches, such a program must not use it.
* **The Calculator as a program.** `wcalc` is the desktop's Calculator in a window of its
  own, looking and counting the same (they share their arithmetic, `shell/guicalc.c`): the
  first application to move out of the kernel, so that a fault in it costs only its window.
* **The web browser.** `wundertow` (10/25/26) shows web pages and this computer's HTML files
  in a window: Back, Forward, Reload, the address field, links to click and the page to
  scroll (`docs/UNDERTOW.md`, "In a window: wundertow"). 11/01/26: with their style sheets, in
  proportional text and real sizes.
  11/1/26: it loads in helper processes, so the window answers meanwhile and Stop ends a load; it shows PNG, JPEG, GIF
  and BMP pictures, finds words (Ctrl+F), goes from link to link by Tab, and uses forms that send by GET. Windows
  of programs get a key the desktop's own apps do not, Shift+Tab (`WIN_KEY_BACKTAB`), and Enter typed with Shift says so
  in the event's `mods` (`WIN_MOD_SHIFT`): to a program that does not look, it is Enter.
* **Run program...** (in the Apps menu) lists the programs of `/initrd/bin` above a line
  to run: a program picked in the list takes the line, and arguments may be typed after
  it (`wcalc` is offered, selected in the list). Run starts it on the desktop's own
  console, its input from `/dev/null`, so that it never takes the desktop's keys; its
  first window takes the focus when it appears. It is
  nobody's child: it outlives the Terminal. It ends with the desktop, whose end hangs its
  windows up (libgui's `gw_next()` then fails with `EPIPE`, and the program returns). A
  program that cannot start is named in a message, with the reason. (The brief's
  Apps > Programs list is this dialog's list: the Apps menu has room for one item more
  on a 640x400 desktop.)

## Mouse support

`pointerinfo` reports connected pointing devices and the latest position/buttons.
The driver supports three-button PS/2 relative mice and PS/2-compatible touchpads,
and USB HID **boot-protocol** mice connected directly to a supported xHCI root
port. USB attach/unplug is supported; unplug releases that device's buttons.
The existing USB stack does not enumerate devices behind hubs. USB
report-protocol-only devices and multi-touch gestures are not implemented.
`mouse=off` on the kernel command line disables PS/2 mouse probing for diagnostics.
*10/26/26:* the wheel. A PS/2 mouse that answers the IntelliMouse handshake (sample
rates 200, 100 and 80, then ID 3) sends four-byte packets, the fourth its wheel's, and
`pointerinfo` calls it a wheel mouse; a USB boot mouse whose report descriptor puts its
Wheel in the boot report's fourth byte (an 8-bit field at bit 24, with no report IDs) has
it there, and lsusb says "boot mouse with a wheel". A notch scrolls the window under the
pointer three rows (Notepad, Files, Help, the suite's applications), and a program's window
gets it as WIN_EV_WHEEL (rows, + down: -3 for a notch away from the user). A turn that
moves nothing else, as the mice report it, is the wheel's alone: no pointer event follows
it, so the wheel is a program's last event. With a menu open the wheel does nothing, and
while a program holds the pointer (a button pressed in its window and not let go) the
wheel is that program's, wherever the pointer is.

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
* The applications of 10/17/26 each fill an entry of the table `struct gui_app`
  (`shell/guipriv.h`). The entry holds the title, icon, size and Help lines, and the
  hooks that the window manager, the menus, the taskbar and the tick call: render, key,
  pointer, tick, busy, dialog answers, and the Edit menu's items.
  * `shell/guiterm.c` and `shell/guivt.c`: the Terminal and its terminal emulator.
  * `shell/guimon.c`, `shell/guidisks.c` and `shell/guimeter.c`: System Monitor and
    Disks.
  * `shell/guilog.c`, `shell/guiclock.c` and `shell/guichar.c`: Log, Clock and Character
    Map.
  * `shell/guiview.c`, `shell/guihex.c` and `shell/guibmp.c`: the Image Viewer, Hex, and
    the BMP reader and writer that the viewer and Sketch share.

  Their state is in `gui_state` too (fixed buffers: the viewer's picture of at most
  3 MB, the Terminal's 1,024 lines), so the desktop needs about 4.6 MB of kernel heap.

The model reaches files only through `struct gui_fs`, and the system only through
`struct gui_sys`. It calls them only from its key and pointer handlers and the tick,
never while drawing. `shell/guisys.c`, `guisys_disk.c` and `guisys_term.c` implement
`struct gui_sys` in the kernel:
* the tasks, memory and machine for System Monitor, and End task's kill;
* the kernel log;
* the block devices, as lsblk lists them (`bdev_walk()`, `bdev_describe()`);
* the Terminal's sessions, on `kernel/pty.c`'s pseudo-terminals. The read never waits. `shell/guifs.c` implements it on the VFS: Mount
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

The applications of 10/17/26 have host tests of their own, against the in-memory file
system and system of `tests/host/gui_fake.c`: `test_guiterm.c`, `test_guisys.c`,
`test_guitools.c` and `test_guimedia.c`. `test_guisuite.c` tests them together: Files'
Open taking each kind of file to its application, a character copied into Notepad and
the Terminal, and every window open at once inside guard pixels at three sizes.
`tools/qemu-usb.py --test --scenario guiapps` runs each application on the screen of a
USB-booted SurfOS, with a FAT32 data stick of the test's own (APPSTICK). The checks for
each application are in `tools/gui_apps_<track>.py`. Among them:
* the Terminal runs `ansi`, `edit`, the pager and Ctrl+C;
* End task ends a console's shell;
* Disks mounts and ejects the stick;
* the Log follows a USB mouse plugged in and pulled out;
* the timer rings;
* the Image Viewer shows the stick's pictures;
* Sketch saves one that the host reads back;
* Files opens a picture in the Image Viewer and a file that is not text in Hex
  (`tools/gui_apps_suite.py`, after the applications' own checks);
* 10/25/26: programs' windows (`tools/gui_apps_win.py`, before the suite's checks):
  `wintest show` and `wdemo` typed in the Terminal open windows at 2x without the focus
  (a key typed meanwhile stays the Terminal's), take it at a click on their title bar, and
  then take keys, a press and a release (echoed with where they were), keep their content
  across F2 and F1,
  answer a corner resize and close by their close button (`ps` then finds none); `wdemo`
  typed without `&` opens with the focus; `wdemo hang` is ended through "End the
  program?", and `wdemo crash` loses only its window;
  `wintest pty` is refused; Run program... starts wcalc, with the focus, which counts by
  keys and clicks,
  and a wcalc started so outlives the Terminal and ends with the desktop (the check leaves
  the desktop, runs `ps` on the serial console and starts the desktop again); and once the
  Terminal has closed, `wintest pty` started from Run program... takes the Terminal's pty
  number for a pty of its own and is refused there.

Programs' windows (10/25/26) are `shell/guiclient.c` in the model (host-tested by
`test_guiclient.c` against fake windows: appearing, drawing and clipping, the pointer and its
grab, sizes, keys, focus, closing, a program that has gone, the launcher, every window inside
guard pixels at three sizes), `shell/guisys_win.c` on the kernel's side, and the windows
themselves in `kernel/win.c` (host-tested by `test_win.c`). libgui (`user/libsurf/gui.c`,
`guikit.c`) builds the desktop's own drawing kit, so the screen checks of wdemo and wcalc
test both.

Each part leaves the desktop as it found it, and the host runs `fsck` on the stick
afterwards.

The 2026-10-12 desktop validation passed the host suite, 286 smoke checks and
51 Files checks at both O0 and O2, plus all 15 USB/UEFI scenarios at both levels
(307 checks each, including 48 GUI checks). The deployed O0 desktop was confirmed
working by the laptop owner. With the touchpad (2026-10-15) the host suite,
297 smoke checks and 51 Files checks passed at O0 and O2, and 313 USB/UEFI
checks at both levels; the owner then used the touchpad in the desktop.

The applications (2026-10-18, gui/suite 3923ce1, from a clean tree) passed the whole
host suite, the desktop's 3,249 checks among it (test_gui 545, test_guiterm 544,
test_guisys 364, test_guitools 505, test_guimedia 1,086, test_guisuite 191,
test_pointer 14); the image builder's 15 tests; 311 smoke checks and 51 Files checks
at O0 and O2; and all 18 USB/UEFI scenarios at both levels, 570 checks each (the gui
and guiapps scenarios among them). Each track was reviewed independently until a review
found nothing more. These are emulator results only: the applications have not yet
run on the laptop.
