# Files: the SurfOS text file browser

Build with `./devenv/build.sh`, boot with `./devenv/run.sh`, then type:

```
files
files /initrd
files /hda1
```

`files` starts in your current folder. The left side shows the path from root
to that folder; the right side lists its directories first, then files, sorted
by name. Entries show byte sizes and modification dates. The footer shows the
selected name, exact byte size, filesystem and whether the mount is read-only.
A `~` marks a clipped name; file operations always use its full original name.
On narrow terminals the tree and date columns give way to the file list.

| Key | Action |
| --- | --- |
| Up / Down (or k / j) | Select an entry |
| Enter or e | Open the folder or launch `edit` on the file |
| Right | Enter the selected folder |
| Left / Backspace | Go to the parent, selecting the folder just left |
| Home / End | First / last entry |
| Page Up / Page Down | Move by a screenful |
| / | Go to the filesystem root |
| r / Ctrl+L | Reload the folder |
| . | Show / hide dotfiles (shown initially) |
| q / Ctrl+Q / Ctrl+C | Return to the shell |

In `edit`, Ctrl+S saves and Ctrl+Q returns to Files. The folder reloads so that
sizes reflect saved edits, and your selected filename remains selected. A file
on a read-only mount can be viewed in the editor; saving reports its normal
read-only error. Devices, special files, files over the editor's 1 MiB limit,
and files with binary control bytes in the first 4096 bytes are not opened.
The initial binary check is a convenience, not a complete file-type detector.
Use `hexdump` for binary data.

Compatible serial/Telnet terminals can send SGR mouse reports. Click a row to
select it; click the selected row again or Open/Edit in the footer to open it.
Click an ancestor folder in the tree to navigate there. The wheel moves the
selection. Mouse reporting is disabled while `edit` owns the terminal and when
Files exits. SurfOS currently has no native mouse driver: its own VGA/UEFI
screen uses the keyboard controls above. F1-F4 retain their console switching.

Directories are read as complete snapshots, with a limit of 8192 displayed
entries. Failed reads preserve the last successful view and report an error.
Filenames containing control or non-ASCII bytes are displayed using `?` for
those bytes; their original names are retained for navigation and editing.
The minimum terminal size is 20 columns by 8 rows.

## Verification

`make test-files` boots the real application in QEMU using temporary disk and
initrd copies. It exercises hierarchy, sorting, metadata, keyboard and mouse
navigation, editor save/return, binary/device refusal, shell recovery and a
40x12 Telnet terminal. It also runs as part of `make test` / `make test-all`.

From macOS, after building:

```
python3 tools/qemu-files.py --kernel build/docker/build/O0/surfos.bin \
  --initrd build/docker/build/images/initrd.tar \
  --disk build/docker/build/images/test.img
```
