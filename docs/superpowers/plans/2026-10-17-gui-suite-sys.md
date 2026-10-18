# Track SYS: System Monitor and Disks (2026-10-17)

Branch `gui/sys`, from the foundation (7b44434) of the brief,
[2026-10-17-gui-suite.md](2026-10-17-gui-suite.md). What docs/GUI.md needs for the two
applications, then the decisions the track made and what it leaves open.

## For the user guide (docs/GUI.md)

Both are in the Apps menu and have a tile on the desktop. Neither has a shortcut of its own,
and neither takes any of the desktop's (Ctrl+Q, G, E, B, K, D, P, O, W, N, T). Both can be
maximized and resized, and both fit a 640x400 screen.

**System Monitor** shows what the machine is doing. Its toolbar has three tabs, Processes,
Memory and System, and End task. It reads the system again every second while it is open;
tucked away in the taskbar it waits, and reads again as soon as it is back.

* **Processes** lists every task as `ps` does: its pid, name, state (running, ready, blocked,
  sleeping, zombie), console (F1-F4, `pty n` for a terminal's session, `-` for none) and
  whether it is a program or a kernel thread. Kernel threads print on F1, so their console
  is F1.
  * Click a heading to sort by it, and again to reverse the order (a ▲ or ▼ beside it says
    which). S sorts by the next column and R reverses it. Tasks that sort alike stay in pid
    order.
  * Click a task, or move with the arrows, Page Up, Page Down, Home and End. The selection
    stays on its task while the list changes; when the task ends, it stays on its row. The
    wheel and the scroll bar move the list.
  * **End task** (or Del) asks first ("End edit (pid 9) on F2?"), with Cancel as the default,
    so Enter alone ends nothing. It ends programs only: for a kernel thread the button is
    greyed, and Del says why in the status bar. The kernel then refuses init, the idle task
    and this desktop's own task, the shell that ran `gui` ("surfsh (pid 4) runs this
    desktop: Ctrl+Q leaves it"), and the status bar says so. A console's shell may be ended:
    init starts a new one on that console, with a new pid, as when the shell's `kill` ends
    it (three such ends within 30 seconds leave that console with the recovery shell).
  * The status bar describes the selected task, and counts the tasks at its right.
* **Memory** shows physical memory (the 4 KB pages SurfOS manages, the RAM the firmware
  reported, and any above 4 GB, which a 32-bit kernel leaves unused), the kernel heap and the
  DMA heap. Each has a bar of what is used (teal; coral from 90%) and its numbers. A heap
  grows when it needs to, up to a limit (the kernel heap's 256 MB of address space, the DMA
  heap's fixed arena), and its bar is of that limit: "at most 256 MB".
* **System** shows the processor, the firmware (UEFI or BIOS), the profile (laptop or
  standard), the display (the framebuffer's mode, and the desktop's size in its own pixels),
  the build (/initrd/etc/build-id), the uptime (h:mm:ss) and how many tasks and kernel timers
  there are.
* **The Edit menu** is System Monitor's while it is in front: Copy (Ctrl+C) copies the
  selected task's line, or the Memory or System page as text (for Notepad, or a bug report);
  Find (Ctrl+F) selects the next task whose name holds what you type, in either case, and
  Find next does it again; Go to pid (Ctrl+L) selects a task by its pid.
* Left and Right change the page. Esc closes the window.

**Disks** shows the block devices as `lsblk` lists them: each disk with its partitions under
it, and for each its name, size, bus (usb, ata, ram), type (`disk gpt`, `part mbr FAT32
LBA`), model or partition name, and status (`mounted`, `boot drive: protected`, ...), in
lsblk's own words. The columns are as wide as what they hold, and the model gives way first
when the window is narrow, as in lsblk. It reads the devices again about every two seconds
while it is open, so a stick that is plugged in appears by itself.

* Below the list are the selected device's details: what it is (a USB disk and its model, or
  a partition of which table), and for a mounted volume where it is mounted, its file system,
  read-only or read-write, its volume label, and a bar of the space used, with the numbers.
* **Mount** (M, or Enter) mounts the selected USB volume at `/<device>`; on a stick's own row
  it mounts the stick's first partition that is not mounted. **Eject** (E) unmounts every
  volume of the selected stick and flushes it; the status bar then says `usb1: safe to
  remove`. Both are the shell's own `mount` and `eject`, so their rules hold: the SurfOS boot
  drive mounts read-only (the details say so beforehand), a busy volume is not ejected, and
  nothing is mounted unless you ask. What they say goes to the status bar. Eject also waits
  while Notepad has unsaved changes to a file on that stick, as in Files.
* **Show in Files** (or Enter on a mounted volume) opens Files at the mount point. Files, when
  it is open, shows a mount or an eject made in Disks at once.
* **Refresh** (Ctrl+R) reads the devices again now. A stick pulled out while mounted stays in
  the list, `unplugged: eject it`, until Eject unmounts it; a device that is gone from the
  list leaves the selection on its row, and the status bar says it is not there any more.
* Ctrl+C copies the selected device's line. A double-click on a row is Enter.

Its limits: Mount and Eject are for USB volumes (the initrd and internal disks only show);
an unmounted volume's own label is not shown (it is on the device, and reading it would
mean waiting for the stick; lsblk does not show it either); a file system that keeps no
count of its space (the initrd's tarfs) has no bar. Up to 32 devices are listed.

Keys, for the table in docs/GUI.md:

| Keys | Action |
| --- | --- |
| Left, Right | System Monitor's previous and next page |
| S, R | System Monitor: sort by the next column, reverse the order |
| Del | System Monitor: End task (asks first) |
| Ctrl+C, Ctrl+F, Ctrl+L | System Monitor: copy the task's line or the page, find a task by name, go to a pid |
| Enter, M, E | Disks: mount (or show a mounted volume in Files), mount, eject |
| Ctrl+R, Ctrl+C | Disks: read the devices again, copy the device's line |

## Decisions

* **End task asks, then the kernel decides.** The question is a dialog with End task and
  Cancel, Cancel the default (Files' Delete does the same). For a kernel thread, or a task
  that has ended, the button is greyed and Del explains in the status bar without asking;
  for every program the question is asked and struct gui_sys's `kill` decides, so its
  refusals (init, the idle task, the desktop's own task) come from the kernel and are shown
  as it words them. The question says when a task is a console's shell that init will start
  again.
* **kill** (shell/guisys.c) finds and kills the task with interrupts off and copies its name
  first: the foundation's version read `t->name` after `kill_task()`, when the zombie may
  already have been collected and freed by its parent. Each refusal now says why ("runs this
  desktop: Ctrl+Q leaves it", "starts the consoles' shells: it is never ended", "is a kernel
  thread: the shell's kill command ends one"); a zombie is "had ended already".
* **What a column sorts by.** State: running, ready, blocked, sleeping, zombie. Console: F1-F4,
  then the ptys, then none. Kind: programs first. Ties: pid order, also reversed. The console
  shown is the one the kernel reports (task_info()), so kernel threads show F1, where they
  print.
* **When they read.** System Monitor every second, Disks every two seconds, from the tick, and
  at once when the window opens or after End task, Mount, Eject and Refresh. Nothing is read
  while the window is tucked away; the next tick after it comes back reads at once. Reading
  sets `dirty` (the uptime changes every second anyway).
* **Selections** follow a pid (System Monitor) or a device's name (Disks) from one reading to
  the next; when it is gone, the selection stays on its row. Disks says so in the status bar
  (a stick pulled out). The view follows the selection only when the user moves it, so a
  reading never scrolls the list away from where the user put it.
* **Disks uses lsblk's own code.** `bdev_describe()` and `bdev_walk()` (driver/bdev.c,
  include/sys/bdev.h) are lsblk's row and order, made public; lsblk itself calls them and
  prints byte for byte what it printed before. guisys_disks() (shell/guisys_disk.c) adds the
  mounts (vfs_mount_views()), how full a mounted one is (vfs_statfs(), which FAT answers from
  the count it made at the mount) and its volume label, and then the devices unplugged while
  mounted, which are off lsblk's list but still mounted (Files lists them too).
* **Nothing waits for a device.** guisys_disks() reads no block: an unmounted volume's FAT
  label is not shown, and its file system is the partition's type, as lsblk has it. The FAT
  label of a mounted one comes from statfs.
* **Mount and Eject only for USB**, through struct gui_fs (the shell's commands with their
  output kept, shell/guifs.c), with their last line in the status bar, failures too (Files
  shows failures in a dialog; the brief asked for the status bar here). On a disk's row,
  Mount mounts its first partition that is not mounted, so "mount the stick" works from
  either row. Eject is offered only when something of the stick is mounted (also unplugged).
* **The Edit menu.** System Monitor: Copy, Find, Find next and Go to pid ("Go to pid…" is its
  goto_label); Disks: Copy. The rest are greyed, and Ctrl+C never leaves the desktop from
  these windows.
* **Files** is refreshed by Disks' Mount and Eject when it is open (files_refresh(), as
  Notepad's Save does); Show in Files uses gui_act(ACT_FILES) and files_reveal().
* **Shared code of the track** is shell/guimeter.c (sizes in KB for people, the share of a total
  in 32-bit arithmetic, the bar, a list's key moves), declared in shell/gui_sys.h and built for
  the host tests on the Makefile's GUI_SYS_OBJ line.
* **State.** gui_state grows by 988 bytes (838,600 to 839,588 at -m32): three fields at the end
  of struct gui_disk (lsblk's size string, a mounted volume's file system and label) in Disks'
  32 rows, and the two windows' own fields (include/sys/gui_sysapps.h).

## Checks

* tests/host/test_guisys.c (unittest-gui): the order of every column and its reverse, the
  selection following its pid across readings, a pid that goes away, the keys, the wheel and
  the scroll bar; End task's question, Enter, Esc and Cancel, End task, kill's refusal, a
  kernel thread, a task that ended while the question was open; the pages and their copied
  text (the uptime read again each second, nothing read while tucked away); Find, Find next,
  Go to pid; Disks' tree and columns (narrow and maximized), a new stick appearing, the
  details, Mount and Eject through a fake file system that watches them (a disk's row, the
  toolbar, a double-click, a failing mount, Notepad's unsaved changes), Show in Files, a stick
  pulled out while mounted and then gone; a desktop with no system and no files; every
  window inside guard pixels at 640x400, 960x540 and 1280x800, maximized, off the edge and at
  its smallest.
* tools/gui_apps_sys.py (qemu-usb.py's guiapps scenario) reads the windows back from the
  framebuffer, cell by cell against the console font: the shells on F1-F4 and the kernel's
  threads, sorting by name and back, End task refused for the desktop's own shell and done for
  the shell on F2 (init starts a new one, with a new pid), plausible memory for the 512 MB
  guest, the System page (QEMU's processor, UEFI, laptop, 1920x1080x32, an uptime that goes
  on), lsblk's devices in Disks, Mount of the data stick (its label and a bar: the stick
  carries sys/fill.bin, 4 MB, for it) and Eject, after which the stick is read back on the
  host and checked by fsck. The stick is left unmounted, also after a failure.

## Open, or for later

* A task's CPU time is not shown: struct gui_task has no field for it (ps prints the ticks).
* The Processes page shows at most GUI_TASK_MAX (64) tasks; the status bar then says "64 of N
  tasks".
* Task names are cut at 15 characters (struct gui_task, struct procinfo): "Kernel Idle Tas".
* vfs_statfs() takes the file system's lock: while another task holds it for a long write,
  Disks' reading (and the desktop's frame) waits for that write.
* The laptop has not run these windows; QEMU has.
