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
tucked away in the taskbar it waits, and reads again as soon as it is back. A reading that
fails keeps what was read before, and the status bar says so.

* **Processes** lists every task as `ps` does: its pid, name, state (running, ready, blocked,
  sleeping, zombie), console (F1-F4, `pty n` for a terminal's session, `-` for none) and
  whether it is a program or a kernel thread. A kernel thread's console is the one of the
  task that started it: the boot's threads are on F1, a `tcpecho` started from F2 is on F2,
  and one started from a terminal's session is on F1.
  * Click a heading to sort by it, and again to reverse the order (a ▲ or ▼ beside it says
    which). S sorts by the next column and R reverses it. Tasks that sort alike stay in
    ascending pid order, also when the order is reversed.
  * Click a task, or move with the arrows, Page Up, Page Down, Home and End. The selection
    follows its task while the list changes. When the task is not in a reading (it ended),
    nothing is selected until you move: the status bar says that its pid is not in the list
    now, Down goes on from its row, and if it is listed again it is selected again. The
    wheel and the scroll bar move the list.
  * **End task** (or Del) asks first ("End edit (pid 9) on F2?", then
    "Ends now or after a system call; unsaved work is lost."), with Cancel as the default, so
    Enter alone ends nothing. It ends programs only: for a kernel thread the button is greyed
    and Del says why in the status bar (init and the idle task are kernel threads). A program
    running its own code ends at once; one inside a system call or a kernel command ends when
    that call does, however long it takes ("edit (pid 9) ends when its system call does"), so
    that nothing it holds, such as a volume's lock, is left held; once it is gone from the
    list, the status bar says so ("edit (pid 9) has ended"). A task whose system call never
    ends is never ended by End task; the shell's `kill`, which kills at once after 2 seconds,
    is the way out. The kernel refuses this desktop's own task, the shell that ran `gui`
    ("surfsh (pid 12) runs this desktop: Ctrl+Q leaves it"), and the shells above it
    ("surfsh (pid 4) runs the shell this desktop runs in"): a new shell on the desktop's
    console would take its keys. Any other console's shell may be ended: init starts a new
    one on that console, with a new pid, as when the shell's `kill` ends it (three such ends
    within 30 seconds leave that console with the recovery shell).
  * The status bar describes the selected task, and counts the tasks at its right. Up to 128
    tasks are listed; with more, the newest 128, and the count says "the newest 128 of N".
* **Memory** shows physical memory (the 4 KB pages SurfOS manages, the RAM the firmware
  reported, and any above 4 GB, which a 32-bit kernel leaves unused), the kernel heap and the
  DMA heap. Each has a bar of what is used (teal; coral from 90%) and its numbers. A heap
  grows when it needs to, up to a limit (the kernel heap's 256 MB of address space, the DMA
  heap's fixed arena), and its bar is of that limit: "at most 256 MB".
* **System** shows the processor, the firmware (UEFI or BIOS), the profile (laptop or
  standard), the display (the framebuffer's mode, and the desktop's size in its own pixels),
  the build (/initrd/etc/build-id), the uptime (h:mm:ss) and how many tasks and kernel timers
  there are.
* In a short window the Memory and System pages scroll: the arrows, Page Up and Down, Home
  and End, the wheel, or their scroll bar.
* **The Edit menu** is System Monitor's while it is in front: Copy (Ctrl+C) copies the
  selected task's line, or the Memory or System page as text (for Notepad, or a bug report;
  a page that could never be read copies nothing, and the status bar says so); Find (Ctrl+F)
  selects the next task whose name holds what you type, in either case, after the selected
  one (or from the row of the one that ended), going round past the end, and Find next does
  it again; Go to pid (Ctrl+L) selects a task by its pid.
* Left and Right change the page. Esc closes the window.

**Disks** shows the block devices as `lsblk` lists them: each disk with its partitions under
it, and for each its name, size, bus (usb, ata, ram), type (`disk gpt`, `part mbr FAT32
LBA`), model or partition name, and status (`mounted`, `boot drive: protected`, ...), in
lsblk's own words. The columns are as wide as what they hold, and the model gives way first
when the window is narrow, as in lsblk. It reads the devices again about every two seconds
while it is open, so a stick that is plugged in appears by itself. A reading that fails keeps
the list read before, and the status bar says so.

* Below the list are the selected device's details: what it is (a USB disk and its model, or
  a partition of which table), and for a mounted volume where it is mounted, its file system,
  read-only or read-write, its volume label, and a bar of the space used, with the numbers
  (or why its file system could not say).
* **Mount** (M, or Enter) mounts the selected USB volume at `/<device>`; on a stick's own row
  it mounts the stick's first partition that is not mounted, and selects it, scrolling to
  it. **Eject** (E) unmounts every volume of the selected stick and flushes it; the status
  bar then says `usb1: safe to remove`. Both are the shell's own `mount` and `eject`, so
  their rules hold: the SurfOS boot drive mounts read-only (the details say so beforehand),
  a busy volume is not ejected, and nothing is mounted unless you ask. What they say goes to
  the status bar. Eject also refuses, with a message in the status bar, while Notepad has
  unsaved changes to a file on that stick, as Files does.
* **Show in Files** (or Enter on a mounted volume) opens Files at the mount point. Files, when
  it is open, shows a mount or an eject made in Disks at once.
* **Refresh** (Ctrl+R) reads the devices again now. A stick pulled out while mounted stays in
  the list, `unplugged: eject it`, until Eject unmounts it. A device that is not in a
  reading (pulled out) stays followed: nothing is selected (M, E and Enter say so), the
  status bar says it is not there any more, Down goes on from its row, and if it comes back
  it is selected again.
* Ctrl+C copies the selected device's line. A double-click on a row is Enter.

Its limits: Mount and Eject are for USB volumes (the initrd and internal disks only show).
Disks reads nothing from a device itself, so an unmounted volume's own label is not shown
(lsblk does not show it either). How full a mounted volume is comes from its file system,
which answers once another task's write to that volume is done, so Disks can pause while a
long copy to that volume runs; FAT counts a read-only volume's free space once, when it is
mounted or first asked. A file system that keeps no count of its space (the initrd's tarfs)
has no bar. Up to 32 rows are listed; with more devices, the sticks pulled out while mounted
are kept, so that they can still be ejected.

Keys, for the table in docs/GUI.md:

| Keys | Action |
| --- | --- |
| Left, Right | System Monitor's previous and next page |
| S, R | System Monitor: sort by the next column, reverse the order |
| Del | System Monitor: End task (asks first) |
| Up, Down, Page Up, Page Down, Home, End | System Monitor's Memory and System pages: scroll, in a short window |
| Ctrl+C, Ctrl+F, Ctrl+L | System Monitor: copy the task's line or the page, find a task by name, go to a pid |
| Enter, M, E | Disks: mount (or show a mounted volume in Files), mount, eject |
| Ctrl+R, Ctrl+C | Disks: read the devices again, copy the device's line |

## Decisions

* **End task asks, then the kernel decides.** The question is a dialog with End task and
  Cancel, Cancel the default (Files' Delete does the same). For a kernel thread (init and the
  idle task are kernel threads), or a task that has ended, the button is greyed and Del
  explains in the status bar without asking; for every program the question is asked and
  struct gui_sys's `kill` decides. Its refusals that End task can meet are the desktop's own
  task and the shells above it, and the status bar shows them as the kernel words them. The
  question says whether init starts another shell, and (10/18/26) that the task ends now or
  after a system call: which of the two the kill meets the list cannot tell (a reading is up
  to a second old, and a program waiting for a key is inside a call yet ends at once, its
  wait cut short), so the question names both rather than struct gui_task getting an in-call
  flag that would often say the wrong one.
* **kill** (shell/guisys.c) finds and kills the task with interrupts off and copies its name
  first: the foundation's version read `t->name` after `kill_task()`, when the zombie may
  already have been collected and freed by its parent. 10/18/26 (the review): it also
  refuses every shell above the desktop's own task (curTask's parents), because init would
  start a new shell on the desktop's console, which would take the desktop's keys; and its
  kill is `kill_task_patient()` (kernel/task.c), the hangup's rule without a hangup: inside a
  call the task ends when the call does, however long, never killed at once after 2 s with a
  lock held (a zombie holding a volume's lock would stop the desktop at Disks' next statfs).
  The kernel log says such a kill waits in words of its own ("... after it was asked to end:
  waiting for its end"); the hangup's own line is unchanged (ptytest looks for it). Each
  refusal says why ("runs this desktop: Ctrl+Q leaves it", "runs the shell this desktop runs
  in", "starts the consoles' shells: it is never ended", "is a kernel thread: the shell's
  kill command ends one"); a zombie is "had ended already"; a task in a call "ends when its
  system call does" (or kernel command); a task already in task_exit() (TC_EXIT: its files
  are closing) "is ending already".
* **What a column sorts by.** State: running, ready, blocked, sleeping, zombie. Console: F1-F4,
  then the ptys, then none. Kind: programs first. Tasks that sort alike stay in ascending pid
  order, also when the order is reversed. The console shown is the one the kernel reports
  (task_info()): a kernel thread's is its creator's (kthread_create()), F1 for one started
  from a pseudo-terminal.
* **When they read.** System Monitor every second, Disks every two seconds, from the tick, and
  at once when the window opens or after End task, Mount, Eject and Refresh. Nothing is read
  while the window is tucked away; the next tick after it comes back reads at once. Reading
  sets `dirty` (the uptime changes every second anyway).
* **Readings are whole, and a failed one changes nothing.** The tasks are copied in one go
  with interrupts off (task_info_all(), kernel/task.c), so a task collected meanwhile cannot
  shift another out of a reading; with more than GUI_TASK_MAX (now 128), the newest are kept,
  so a runaway program's latest children can be seen and ended. Disks' device list is walked
  with interrupts off, so a stick unplugged meanwhile cannot cut the walk short. A reading
  that fails (no memory) keeps the last one's rows, memory and machine (tasks and disks leave
  their output as it was when they fail: sys/gui_sys.h), and the status bar says so.
* **Selections** follow a pid (System Monitor) or a device's name (Disks) from one reading to
  the next. 10/18/26: until the user moves the selection, also through a reading without it:
  nothing is selected meanwhile (so End task can never ask about the task that took its row),
  the status bar says it is not in the list, Down goes on from its row (hint), and it is
  selected again if it comes back. The view follows the selection only when the user moves
  it, and when Mount selects what it mounted. 10/18/26: when End task's kill waited for a call
  (the task was still listed right after it), the first whole reading without the task puts
  "has ended" in the status bar in place of kill's words, if the selection still follows it (a
  reading cut to the newest 128 says nothing: the task may only be off it); Find next with
  nothing selected starts at the row the followed task was on, as Down does; Disks' M, E and
  Enter with nothing selected say so.
* **Disks uses lsblk's own code.** `bdev_describe()` and `bdev_walk()` (driver/bdev.c,
  include/sys/bdev.h) are lsblk's row and order, made public; lsblk itself calls them and
  prints byte for byte what it printed before. guisys_disks() (shell/guisys_disk.c) adds the
  mounts (vfs_mount_views()), how full a mounted one is and its volume label (vfs_statfs(),
  or the error it gave: statfs_err), and then the devices unplugged while mounted, which are
  off lsblk's list but still mounted (Files lists them too). When there are more devices than
  its 32 rows, the unplugged ones are kept and lsblk's are cut, since only Eject can end those.
* **What Disks can wait for.** It reads nothing from a device itself: an unmounted volume's FAT
  label is not shown, and its file system is the partition's type, as lsblk has it. But
  vfs_statfs() takes the file system's lock, so a reading can wait behind another task's
  long write to that volume, and FAT counts a read-only volume's free space once (at its
  mount, or at the first statfs). The FAT label of a mounted volume comes from statfs.
* **Mount and Eject only for USB**, through struct gui_fs (the shell's commands with their
  output kept, shell/guifs.c), with their last line in the status bar, failures too (Files
  shows failures in a dialog; the brief asked for the status bar here), also when the
  reading right after them finds a device gone. On a disk's row, Mount mounts its first
  partition that is not mounted, so "mount the stick" works from either row, and scrolls to
  it. Eject is offered only when something of the stick is mounted (also unplugged); it
  refuses while Notepad has unsaved changes to a file on any of that stick's mounts (the
  path's next character after the mount point must be a /: /usb1p10 is not /usb1p1).
* **Short windows.** The window manager allows a height of 200. The Processes list and
  Disks' list scroll as they did; the Memory and System pages now scroll too, by lines of 24
  pixels, inside a clip between the toolbar and the status bar, with a scroll bar when their
  lines do not fit.
* **The Edit menu.** System Monitor: Copy, Find, Find next and Go to pid ("Go to pid…" is its
  goto_label); Disks: Copy. The rest are greyed, and Ctrl+C never leaves the desktop from
  these windows. Copy on a Memory or System page that was never read copies nothing.
* **Files** is refreshed by Disks' Mount and Eject when it is open (files_refresh(), as
  Notepad's Save does); Show in Files uses gui_act(ACT_FILES) and files_reveal().
* **Shared code of the track** is shell/guimeter.c (sizes in KB for people, the share of a total
  in 32-bit arithmetic, the bar with coral from exactly 90%, a list's key moves), declared in
  shell/gui_sys.h and built for the host tests on the Makefile's GUI_SYS_OBJ line.
* **State.** gui_state grows by 3,468 bytes (838,600 to 842,068 at -m32): GUI_TASK_MAX from 64
  to 128 (2,304), four fields at the end of struct gui_disk (lsblk's size string, a mounted
  volume's file system, label and statfs error) in Disks' 32 rows, and the two windows' own
  fields (include/sys/gui_sysapps.h; 10/18/26: the pid and name End task's kill waits for).

## Checks

* tests/host/test_guisys.c (unittest-gui): the order of every column and its reverse (ties in
  ascending pid order both ways), the selection following its pid across readings, one that
  misses it once and readings that fail, a pid that goes away, the keys, the wheel and the
  scroll bar; End task's question, Enter, Esc and Cancel, End task, kill's refusal, a kernel
  thread, a task that ended while the question was open; the pages and their copied text
  (the uptime read again each second, nothing read while tucked away, nothing copied when
  never read), the pages at the smallest height, scrolled to their ends; the task count's
  "of"; Find (also round past the end), Find next, Go to pid; Disks' tree and columns
  (narrow and maximized), a new stick appearing, the details, Mount and Eject through a fake
  file system that watches them (a disk's row, the toolbar, a double-click, a failing mount,
  Notepad's unsaved changes, also on a mount whose name begins as another's and on an
  unplugged stick), Mount scrolling to what it mounted, Show in Files, a stick pulled out
  while mounted and then gone, readings of Disks that fail or miss a device once; the
  meters' arithmetic at its edges and coral from exactly 90%; a desktop with no system and
  no files; every window inside guard pixels at 640x400, 960x540 and 1280x800, maximized, off
  the edge and at its smallest. And the kernel's side itself, built with stand-ins for the
  rest of the kernel (tests/host/guisys_host.c): kill's refusals and words, the patient kill
  only for what may be ended, the tasks' copy and its failure, Disks' rows with the unplugged
  ones in their order, statfs failures, the 32-row cap keeping the unplugged rows, and a
  failure that leaves the rows alone. 10/18/26: End task's kill that waits for a call, then
  "has ended" (not after the user moved on, nor while the list is cut), Find next with the
  followed task gone, Disks' M, E and Enter with nothing selected, kill of a task that is
  ending already or was killed already (the stand-in kill_task_patient() has the kernel's
  rule).
* shell/selftest.c (`selftest`, which the smoke test runs) tries the kernel's side on the
  target, with kernel threads, in about 2.7 s: kill_task_patient() of one in its own code (it
  ends at once), of one in a kernel command whose wait a kill cuts short (sleep_intr(): it ends
  at once, at its call's end), and of one in a call that does not look (a loop of sleep_ms()
  past KILL_WAIT_MS: alive 2.25 s later, the kernel log's "... after it was asked to end:
  waiting for its end", then it ends with its call); kill_task() 0.5 s after End task's kill
  (killed at once 2 s after itself, not after End task's, and the log's "killed at once"); and
  a hangup's kill (closing the master of a pseudo-terminal of its own, the thread alone on it)
  followed by End task's: the hangup's rule and log line stand, and it ends with its call.
  task_info_all() is checked against task_info() over the live list with interrupts off: the
  whole list, the newest 3 in order, none.
* tools/gui_apps_sys.py (qemu-usb.py's guiapps scenario) reads the windows back from the
  framebuffer, cell by cell against the console font, with the pointer parked away from them:
  the shells on F1-F4 and the kernel's threads, sorting by name and back, End task refused
  for the desktop's own shell and done for the shell on F2 (init starts a new one, with a new
  pid, and the status bar says the old one has ended), plausible memory for the 512 MB guest,
  the System page (QEMU's processor, UEFI, laptop, 1920x1080x32, an uptime that goes on),
  lsblk's devices in Disks (its columns found from its headings), Mount of the data stick
  (its label and a bar: the stick carries zz-sys-fill.bin, 4 MB, for it, at its root where it
  sorts after every other track's file and folder, so their lists keep their order) and
  Eject, after which the stick is read back on the host and checked by fsck. The stick is
  left unmounted, also after a failure (found by its name), and Esc is sent only to a menu or
  a dialog that is open.

## Open, or for later

* A task's CPU time is not shown: struct gui_task has no field for it (ps prints the ticks).
* The Processes page shows at most GUI_TASK_MAX (128) tasks, the newest; the status bar then
  says "the newest 128 of N".
* Task names are cut at 15 characters (struct gui_task, struct procinfo): "Kernel Idle Tas".
* vfs_statfs() takes the file system's lock: while another task holds it for a long write,
  Disks' reading (and the desktop's frame) waits for that write.
* A task whose system call never ends is never ended by End task; the shell's `kill` command
  (which kills at once after 2 s) still can. The guide says so.
* The laptop has not run these windows; QEMU has.
