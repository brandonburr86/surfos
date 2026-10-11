# SurfOS text file browser

The user requested an additional application developed independently of the web
browser, then merged into the core branch. `files [directory]` is a libsurf user
program: no kernel or web-browser changes. Start in the current directory, or
an explicit path. Show an ancestor tree, directories before files, byte sizes,
modification times, selected-file details and filesystem type. All entries,
including hidden files, are visible by default; `.` toggles hidden entries.

Up/Down, Home/End and Page Up/Down select and scroll. Enter opens a folder or
launches `/initrd/bin/edit` for a text file. Right opens folders; Left/Backspace
returns to the parent; `/` goes to root; `r` refreshes; `q` or Ctrl+Q exits.
Returning from edit reloads metadata and preserves selection. Parent navigation
selects the directory just left. Failed navigation preserves the previous view.

Compatible serial/Telnet terminals support SGR mouse reporting: clicking a row
selects it, clicking an already selected row opens it, and clicking Open/Edit
opens it. Ancestor-tree clicks navigate up; the wheel scrolls selection. Native
SurfOS has no mouse driver, so its screen uses the keyboard. Mouse reporting is
disabled during editing and on exit. Escape parsing is bounded and times out.

Do not launch edit on devices, non-regular files, files larger than its 1 MiB
limit, or files whose first 4096 bytes contain binary control characters. Read-only
files may be viewed in edit; its existing save error handling remains authoritative.
Render filenames as safe display text, never terminal control sequences. Clip
long names, adapt to terminal sizes, and keep file operations on the full path.

Verification: boot the real program in QEMU; assert screen navigation, metadata,
empty/long/hidden names, binary/device refusal, edit-save-return behavior, mouse
input, malformed input, shell recovery and small Telnet terminal sizes. Build at
O0/O2; run host and existing QEMU regressions. Merge into `ai-dev` preserving
unrelated work and verify the merged tree. No remote push or hardware flash.
