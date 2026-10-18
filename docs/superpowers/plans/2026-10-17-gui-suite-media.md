# Track MEDIA: the Image Viewer, Hex and Sketch's BMP files (2026-10-17)

Track MEDIA of [the suite's brief](2026-10-17-gui-suite.md), on branch `gui/media` from the foundation (7b44434).
This file says what was built, what the user guide (docs/GUI.md) needs, what shell/gui_media.h offers Files,
and the decisions taken on the way.

## For the user guide (docs/GUI.md)

### Image Viewer

**Image Viewer** shows BMP pictures. Open one with the toolbar's Open, or press O; the chooser lists a folder
as Notepad's does. (Once Files is wired to it: double-click a `.bmp` in Files.)

* **What it reads.** Uncompressed BMP files of 1, 4 and 8 bits (with a palette) and of 24 and 32 bits, stored
  bottom-up or top-down, with any of the usual headers (BITMAPINFOHEADER and its V2 to V5 versions; 32-bit
  BI_BITFIELDS with the usual masks too). Other files are refused, and the message says why: RLE compression,
  16-bit pixels, OS/2 bitmaps, a PNG, JPEG or GIF (or a BMP with a PNG or JPEG inside), a file cut short. A
  picture is drawn opaque (its alpha, if any, is not used).
* **Size.** Up to 16384 x 16384 pixels. A picture of more than 1,048,576 pixels (1024 x 1024) is reduced
  while it is read, keeping its proportions (1920 x 1080 is kept at 1365 x 768), and the status bar says
  `kept at 1365 x 768`. A picture is read a piece at a time, one piece in each frame the desktop draws (at most
  128 reads and 256 KB plus one 64 KB chunk a frame, and a single read when it opens), so the desktop goes on
  answering while a big file is read:
  the window and the status bar count the percent (`Reading sea.bmp... 45%`), and Esc stops the reading. When
  a file cannot be read to its end (it was cut short, or the stick was pulled out), the window and its status
  bar say so; no message box interrupts what you are doing meanwhile.
* **The views.** Fit is the default: a picture larger than the window is shrunk to fit it; a smaller one is
  enlarged by a whole number (2 times, 3 times, ...), as far as it fits, so that its pixels stay square. Press 1
  (or the toolbar's Actual size) for the actual size, + and - (or Zoom in, Zoom out) to zoom between 1/64 and
  16 times, F or 0 to fit again; a double-click anywhere in the picture area (the picture or the dark space
  around it) switches between Fit and the actual size.
  Zooming keeps the point in the middle of the window where it is. Fit follows the window as it is resized
  or maximized.
* **Moving a zoomed picture.** Drag it with the mouse, or use the arrows (32 pixels a press), Page Up and Page
  Down (the picture area's height less 32 pixels), Home (the top left) and End (the bottom right), or the wheel
  (up and down, or sideways when the picture is no taller than the window).
* **The status bar** shows the name, the picture's size and bits per pixel, and the zoom (`Fit 1000%`,
  `150%`). The title bar shows the file's name.
* Closing the window forgets the picture.

### Hex

**Hex** shows any file's bytes, read-only. Open one with the toolbar's Open, or press O.

* **A row** is the offset (8 hexadecimal digits), 16 bytes in hexadecimal, and the same bytes as characters:
  printable ASCII as it is, and a grey dot for every other byte (a real `.` is dark). A header row numbers the
  columns.
* **Any size.** Only what the window shows is read, so a file of any size up to 4 GB opens at once.
* **Moving.** The arrows move the cursor a byte or a row, Page Up and Page Down a window's rows, Home and End
  to the file's first and last byte; the wheel and the scroll bar (drag the thumb, or click above or below it)
  move the view. Click a byte, in either column, to put the cursor there, or drag over several to select them
  (the view scrolls when the pointer passes its edge).
* **Copy.** Ctrl+C (Edit > Copy, or the toolbar's Copy) puts the selected bytes on the clipboard in
  hexadecimal, `DE AD BE EF`, up to 43,690 bytes; Edit > Select all selects the whole file. The toolbar's Copy
  offset puts the cursor's offset there, `0x0000001F`.
* **Go to offset.** Ctrl+L (Edit > Go to offset…, or the toolbar's Go to) takes `0x1F00` in hexadecimal or
  `7936` in decimal; the row goes to the top of the window. An offset past the end is refused.
* **Find.** Ctrl+F (Edit > Find…, or the toolbar's Find) finds bytes from the start of the selection on (the
  cursor's byte when only one is selected): text as typed, or bytes in hexadecimal written as two or more pairs
  with spaces between them (`DE AD BE EF`) or after `0x` (`0xDEADBEEF`). A single pair such as `FF` is text
  (the two characters F and F): one byte is written `0xFF`. Text in double quotes is always text (`"DE AD"`
  finds the five characters). Up to 128 bytes, matched exactly (letters in their case). The match is selected.
  N, Edit > Find next or the toolbar's Find next finds the next one; at the end of the file the status bar says
  `Not found from 0x… to the end`, and says it again at the next N (Home, then Find next, searches from the
  start). A Find whose text is not one (`0xZZ`) stops a search that was running. A long search runs a piece at
  a time (the status bar counts the percent), and Esc stops it.
* **The status bar** shows the cursor's offset in hexadecimal and decimal, how many bytes are selected, the
  last message, and the file's size. The title bar shows the file's name.
* Hex opens files, not devices. Closing the window forgets the file.

### Sketch

Replace "Sketches are temporary: they are lost when the desktop exits, and Sketch does not write files." with:

* **Save as BMP** (Ctrl+S, File > Save as BMP… while Sketch is in front, or the Save button) saves the drawing
  as a 400 x 180 24-bit BMP (216,054 bytes), which the Image Viewer, Sketch itself and other computers can
  open. It asks for the name and the folder (the chooser starts in the folder of the picture Sketch saved or
  opened last, else in the last chooser's), and before it replaces a file that is there already, as Notepad's
  Save as does, except the file Sketch itself saved last. Unsaved sketches are still lost when the desktop
  exits.
* **Open BMP** (File > Open BMP…, or the Open button) reads a BMP of up to 8 MB into the canvas (a 1920 x 1080
  screenshot is 6 MB; a larger file is refused, and the Image Viewer shows it): a picture larger than the
  canvas is shrunk to fit it (its proportions kept), and a smaller one is put in the middle; each colour
  becomes the nearest of Sketch's six. It replaces the drawing (as Clear does); a picture that cannot be read
  leaves the drawing as it was.
* The line under the canvas says what Save and Open did, until the drawing changes (a stroke, Clear, Ctrl+N).

### The menus and keys tables

* File: "With Sketch in front: Open BMP…, Save as BMP… (Ctrl+S), then New note, Open…, Close window."
* Edit: "With Hex in front: its Copy, Select all, Find…, Find next and Go to offset… (the others are greyed)."
* Apps: Image Viewer and Hex are in the suite's part (they have no shortcut).
* Keys: "Ctrl+S | ... (in Sketch: Save as BMP)". Image Viewer: O, F or 0, 1, + and -, the arrows, Page Up and
  Down, Home, End, Esc (stops a reading). Hex: O, N, the arrows, Page Up and Down, Home, End, Ctrl+C, Ctrl+A,
  Ctrl+F, Ctrl+L, Esc (stops a search). Both take none of the desktop's shortcuts; Esc closes them when it has
  nothing to stop, Tab goes to the next window.

## What shell/gui_media.h offers Files

```c
void viewer_show(struct gui_state *g, const char *path);  /* the Image Viewer opens and starts reading path */
bool viewer_takes(const char *name);                      /* *.bmp, in any case */
void hex_show(struct gui_state *g, const char *path);     /* Hex opens on path's first bytes */
```

Both open their window (gui_open) and say in a dialog when the file cannot be shown; both read only with
gui_fs's read_at, and must be called from a handler (not while drawing). Files' Open (`files_act`, case
`ACT_FILES_OPEN` in shell/guifiles.c) could read:

```c
if(viewer_takes(gui_basename(path))) { viewer_show(g, path); break; }
if(gui_guard(g, PEND_OPEN_PATH, path)) {
    int r = note_load(g, path);
    if(r == 0) gui_open(g, GUI_NOTEPAD);
    else if(r == -EINVAL && g->dialog.kind == GUI_DLG_MESSAGE) { dialog_close(g); hex_show(g, path); }  /* binary */
}
```

(note_load returns -EINVAL for a file with NUL bytes, and for a device, which hex_show refuses in its own
words.) Once that is in, the Help lines (`help[]` in guiview.c and guihex.c, 57 characters at most) can say
that Files opens a `.bmp` in the Image Viewer, and a file that Notepad does not open in Hex.

Also there for tests and for anyone who needs to know where the picture is: `viewer_place()` (its place on
the screen) and `viewer_zoom()` (16.16). Sketch's are `sketch_save_as()`, `sketch_open()`, `sketch_save()`,
`sketch_load()`, and the BMP reader and writer that the viewer and Sketch share are `bmp_parse()`,
`bmp_open()`, `bmp_pixel()`, `bmp_reduce()` and `bmp_header24()` (shell/guibmp.c).

## Decisions

1. **A picture's memory: reduced while it is read**, rather than refused. gui_state keeps at most
   GUI_VIEW_PIXELS (1,048,576) pixels of 3 bytes (R, G, B): 3 MB. A bigger picture is read at the largest size
   that fits, its proportions kept, each kept pixel the source pixel in the middle of what it stands for
   (nearest neighbour), and only the rows needed are read. The sanity limit is 16384 pixels a side (a row of
   16384 32-bit pixels is the 64 KB a read takes). Its status bar says `kept at W x H`.
2. **Reading in the tick, bounded in reads as well as bytes.** A picture is read in the tick, and one read at
   once in the handler that opens it (a small picture is there with no frame's wait); Esc stops it. A read
   counts for its bytes, 2 KB at least, against the frame's 256 KB: so a frame makes 128 reads at most and reads
   256 KB and one 64 KB chunk at most. (Each read_at is an open and a walk of the file's clusters on the
   kernel, so the reads count, not only the bytes.) A read takes the next row the picture needs, and the needed
   rows after it as far as a chunk holds them while each costs no more than a read of its own would: a row next
   to the last always, one past skipped rows only when those and it come to 2 KB at most (they are read and
   passed over). So rows of 2 KB or more are read exactly as 0c245c0 read them, adjacent ones together and the
   rows a reduced picture skips never, and short rows (narrow and 1-bit pictures) in spans of many instead of a
   read each: a 256 x 16384 1-bit picture took 8,193 reads in one handler at 0c245c0, and takes 9 in all now
   (the headers, then 8 spans of 64 KB; 2 of them in the handler). Big pictures take as many frames as at 0c245c0, and one more (the opening handler reads one piece, where
   it read a frame's worth), reading the same bytes:

   | Picture | Frames 0c245c0, 2a75187, now | Bytes read 0c245c0, 2a75187, now |
   |---|---|---|
   | 4000 x 3000 x 24 | 40, 81, 41 | 10.6, 23.3, 10.6 MB |
   | 3000 x 2000 x 32 | 37, 54, 38 | 10.0, 16.1, 10.0 MB |
   | 1920 x 1080 x 24 | 16, 19, 17 | 4.4, 6.1, 4.4 MB |
   | 16384 x 16384 x 1 | 7, 63, 8 | 2.1, 17.8, 2.1 MB |
   | 1024 x 16384 x 32 | 63, 204, 64 | 16.8, 54.5, 16.8 MB |

   Hex's Find reads 256 KB a frame.
3. **Drawing.** The decoded pixels are scaled at render time, nearest neighbour, clipped to the window and the
   screen, with no allocation and no division in the loop (each row steps through the decoded pixels as a
   quotient and a remainder, exactly). On the desktop's own 32-bit 0x00RRGGBB frame the pixels are written
   straight into the surface; any other surface goes a pixel at a time through gd_rect.
4. **Fit enlarges small pictures by whole numbers only** (up to 16 times), so that Sketch's drawings and other
   small pictures keep square pixels; larger pictures are shrunk by whatever fraction fits.
5. **The wheel moves the picture** and does not zoom: pointer events carry no modifier keys, and Ctrl+wheel
   would not be told apart. Zooming is +, -, 1, F and the toolbar.
6. **Hex's characters are printable ASCII and grey dots**, not code page 437 glyphs: a hex view is read for
   its bytes, and UTF-8 text would show as pairs of 437 characters. Its byte pitch adapts to the window (24
   pixels a byte when there is room, 22 at the default 620, 20 at 640 x 400), and it is never narrower than 580.
7. **Hex's Find uses Knuth-Morris-Pratt**: the matcher keeps how much of the pattern it has matched from one
   piece to the next, so a match across two pieces of 64 KB (or two frames) is found without rereading an
   overlap, and the worst case stays linear (a naive search with an overlap is quadratic on files like "AAAA").
   It searches from the selection's start (Find) or after the match selected (Find next), exact bytes, and
   does not wrap round to the top. The last match is kept until a new Find, so after "Not found" Find next
   says "Not found" again instead of finding the last match once more. A Find whose text is no pattern stops a
   search that runs, and the search itself stops at once with no pattern (never a match of nothing).
8. **Hex's Find syntax**: two or more pairs of hexadecimal digits with single spaces, or 0x and digits, are
   bytes; anything else (and anything in double quotes) is text. A single byte is `0xDE`.
9. **Copy** copies the selected bytes in hexadecimal (as `DE AD BE EF`, so a selection pastes into Hex's own
   Find); Copy offset is a toolbar button (the Edit menu has no slot for it). The bytes are all read before the
   clipboard is touched (43,690 at most, which fit a 64 KB piece), so a copy that fails changes nothing.
10. **Sketch's Save as BMP always shows the chooser** (with the last name), Ctrl+S included: Sketch has no file
    of its own until it is saved, and the Save chooser's Enter is quick. The overwrite question is Notepad's,
    but not for the file Sketch saved last (as Notepad does not ask about its own file). A file Sketch opened
    is not its own: saving over it asks.
11. **Open BMP fits** the picture into the canvas (shrunk if larger, proportions kept, centred) rather than
    cropping it, so that any picture shows whole; Sketch's own 400 x 180 files come back pixel for pixel.
    Colours go to the nearest of the six by squared RGB distance. The new canvas is built aside and copied in
    only when every row was read. It does not ask before it replaces the drawing, as Clear does not. It reads
    in the dialog's answer, a row at a time (180 reads at most), so it takes files of up to 8 MB (a 1920 x
    1080 screenshot is 6 MB): each read walks the file's clusters from its start, and a bigger file would keep
    the desktop waiting (a 16384 x 16384 32-bit one took 181 reads and 11.8 MB in one handler). A larger file
    is refused, with the Image Viewer named, which reads in the tick. (Reading it in a tick of Sketch's own
    would need one: Sketch is not one of the suite's applications, and gui_tick() only calls theirs.)
12. **Where Sketch's pieces live.** Its file code is in shell/guibmp.c; its dialogs' answers come through the
    Image Viewer's answer hook (gui_app_act(GUI_VIEWER, n), n exactly 20, 21 or 22), so gui_answer() is
    unchanged; its two menu
    actions are numbers beyond the suite's dialog range (ACT_SKETCH_SAVE and ACT_SKETCH_OPEN in
    shell/gui_media.h) rather than new entries of guipriv.h's enum, which stays untouched; its palette is
    exported from guicore.c as sketch_paints[] (it was the static paints[]).
13. **With Sketch in front the File menu hides Notepad's Save, Save as and Show in Files** (they were greyed),
    as it does with Files in front, so that Ctrl+S appears once.
14. **Closing the Image Viewer or Hex forgets the picture or the file**, as closing Notepad forgets its text.
15. **Sketch's line under the canvas** says what Save or Open did until the drawing changes (a stroke, Clear,
    Ctrl+N), and then the hint again.
16. **The tick never opens a dialog.** What goes wrong while a picture is read in the tick (a file cut short, a
    stick pulled out) is said in the window and its status bar only: a dialog would replace the one in front,
    such as Notepad's question about unsaved changes (whose answer would then never come, leaving the desktop
    to quit at a later save) or a chooser with a name typed in it. The handlers' dialogs (a refused file, a
    failed first piece) open only when no other dialog is in front. A file checked while another picture is read
    is checked with a palette of its own, so a refused file does not stop that reading.

## Open points

* Files is not wired to the Image Viewer and Hex yet (the integrator's, above), and docs/GUI.md is not
  updated (the integrator's, from this file).
* gui_fs's read_at (shell/guifs.c, the foundation's) opens the file for each call, so on FAT the cluster chain
  is walked from the file's start every time: a read far into a big file costs time in proportion to its
  offset, in clusters. That is nothing for pictures and ordinary files, but searching hundreds of megabytes in
  Hex (a read every 64 KB) or reading a huge picture's rows gets slow, still a frame's budget at a time and
  stoppable with Esc. A position kept between calls in guifs.c (or the FAT vnode kept cached) would make it
  linear; not changed here, as guifs.c is not the track's.
* The arrow keys move the pointer while the Image Viewer shows no picture and Hex shows no file: that was done
  in the integration (gui/suite df2da45), where struct gui_app has an optional `wants_arrows` hook that
  gui_wants_arrows() asks, and guiview.c and guihex.c set it (g->viewer.ready, g->hex.path[0] != 0). It is not on
  this branch, so that the merge does not conflict.
* Everything here was checked on the host and in QEMU (OVMF, USB sticks), not yet on the laptop.

## Files

Commits on gui/media: 81b9376 (the applications, Sketch's files and the host test), 65fce6b (the guiapps
checks), 3f1b836 and 0c245c0 (this file); then the first review's fixes.

Added: shell/guibmp.c (BMP reading and writing, Sketch's Save as BMP and Open BMP), shell/gui_media.h (the
private header above). Rewritten from the foundation's stubs: shell/guiview.c, shell/guihex.c,
include/sys/gui_media.h (the state), tests/host/test_guimedia.c, tools/gui_apps_media.py.

Shared files touched, only where the track had to:

* shell/guicore.c, Sketch's parts only: the header comment, `#include "gui_media.h"`, the palette's name
  (sketch_paints[], exported), Sketch's two File menu items and the one condition that hides Notepad's Save
  items while Sketch is in front, gui_act's two cases, the press on the Open and Save buttons, Ctrl+S while
  Sketch has the focus, the drawing of the two buttons and the line under the canvas, and (the review) that
  line cleared where the canvas changes: stroke(), the Clear button and ACT_CLEAR_SKETCH.
* Makefile: `GUI_MEDIA_OBJ = $(BUILD)/host/kernel/shell/guibmp.o`, the track's own line.

gui_state grows by 3,501,068 bytes (838,600 to 4,339,668 at -m32): 3 MB of picture, 64 KB of reading,
216 KB for Sketch's file, 64 KB for Hex's searches and copies, 4 KB of Hex's page, 1 KB for the palette of a
file being checked (the review), and the rest small.

## Tests

* tests/host/test_guimedia.c: each kind of BMP that is read (24 and 32 bits, bottom-up and top-down, BI_BITFIELDS
  after a 40-byte header and in V4 and V5 ones, 52- and 56-byte headers, 8-bit with 4 and 256 colours, an index
  past the palette, 4 and 1 bits at odd widths) and each refusal (PNG, JPEG, GIF, OS/2 arrays and headers, an
  unknown header, RLE8 and RLE4, JPEG and PNG inside, other compressions, 16 and 2 bits, masks on 24 bits or
  unusual ones, planes, sizes of zero, negative, too large, INT_MIN); hostile files (16384 x 16384 x 32 bits in
  a small file, offsets beyond the end and inside the headers, pixels or palettes cut short by a byte, a file
  that shrinks while it is read); the kept size (bmp_reduce); the viewer's Fit, actual size, zoom steps,
  limits, panning by keys, wheel and drag, the double-click, the toolbar, Fit after maximizing, all checked
  against the screen's pixels; a 1.1-million-pixel picture read over frames and sampled, Esc, and a 16384 x
  16384 x 32-bit (1 GB) picture made as it is read, kept at 1024 x 1024 in 256 frames of 256 KB; Hex's first
  row on the screen, its keys, Go to (hexadecimal, decimal, past the end, junk), Find (bytes, text, quoted,
  0x, invalid, too long), Find next and the end, Copy, Copy offset, the mouse and the scroll bar, matches across
  pieces (one Knuth-Morris-Pratt needs: AAAB in A's) and at the end, and a 4,000,000,000-byte file (the end,
  Go to past 2 GB, the thumb's ends, a search over a dozen frames, Esc); Sketch's menu, Save as BMP read back
  pixel for pixel, the question before replacing, folders, a read-only volume, a failing disk, Open BMP of its
  own file, a larger, a smaller and a coloured picture, a refused one, the 1 GB one, its buttons; no file
  system at all; and every window (with a picture, a 4 GB file, Sketch's note) inside guard pixels at 640x400,
  960x540 and 1280x800, also maximized, at its narrowest and hanging off the edge. After the first review:
  a reading that fails in the tick under Notepad's question about unsaved changes (the question stays, Cancel
  leaves nothing pending, a later Ctrl+S saves and stays) and under Sketch's Save chooser with a name typed (it
  stays and saves), the failure in the window and the status bar; read_at calls and bytes in the opening
  handler and in each frame for a 256 x 16384 1-bit picture (kept at 128 x 8192, spans of the file) and a
  1024 x 1100 one; 4- and 1-bit pictures through the viewer on the screen; "kept at 1073 x 976" in the status
  bar; a file refused while another is read (that reading goes on, its palette its own); viewer dialog
  numbers that are not Sketch's; a Find that is no pattern ("0xZZ", "", "0xABC", 129 bytes) during a search;
  a copy failing part-way (shorter file, read error) leaving the clipboard; Find next after "Not found";
  the copy's limit of 43,690 bytes and the refusal of one more (nothing after the clipboard touched); a page
  that cannot be read (said once, not read again every frame) and a search whose read fails; devices refused
  by Hex, the Image Viewer and Sketch; a file of 0xFFFFFFFF bytes (End, Up, Go to 0xFFFFFF00, Down at the
  end); Sketch's line cleared by a stroke, Clear and Ctrl+N; "shrunk to fit" for a 1 x 1000 picture; Sketch
  refusing the 1 GB picture and reading a 1920 x 1080 one in 181 reads. After the second review: the frames
  a 4000 x 3000 x 24 photo and a 1024 x 16384 x 32 picture take (41 and 64 at most, 0c245c0's and one more),
  the bytes they read (the rows they need, no others), 128 reads and 256 KB and a chunk a frame at most, one
  read in the opening handler, their pixels; Sketch opening a file of 8 MB exactly and refusing one a byte
  longer ("more than the 8 MB Sketch opens").
* tools/gui_apps_media.py (qemu-usb.py's guiapps scenario): Files mounts the APPSTICK; Hex's first row of
  media/bytes.bin and a Find on the screen; the Image Viewer's 24-bit picture fitted and at its actual size and
  an 8-bit top-down one, colours read off the screen; Sketch draws, saves media/drawn.bmp, clears, opens it
  again; Eject; and on the host, drawn.bmp read back and checked (a 24-bit 400 x 180 BMP with the line). After
  a failure it lets go of the mouse button, closes a dialog or a menu left open, closes what it opened, puts
  Sketch's brush back to ink, and ejects the stick (Files and the stick are counted as opened from the moment
  Ctrl+B and Enter are sent).
