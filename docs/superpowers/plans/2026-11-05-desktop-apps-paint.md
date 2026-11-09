# The desktop applications, track paint: the account

Branch `apps/paint`. Built to `2026-11-05-desktop-apps.md`. On the laptop none of this has run: emulation and the host only.

## Where the code comes from

No other program's source was used or recalled. The methods, and where each came from:

- **Lines and strokes:** the idea of J. E. Bresenham, "Algorithm for computer control of a digital plotter", IBM
  Systems Journal 4(1), 1965: one point for each step along the longer axis, the other coordinate the whole number
  nearest the true line's, kept as a remainder in whole numbers. Since the review (below), `line_rows()` is worked out
  here from that rounding (j = floor((2mi + n) / 2n) at step i of n, its remainder starting at n and moving by 2m, less
  2n when the point moves across), in its own structure and names; the first version followed the published
  all-octant listing with one error term (A. Zingl's) statement for statement, which the review found and which is
  gone. A stroke is a dab of the brush at each point of the line.
- **Ellipses:** worked out here from the ellipse's equation, x²/a² + y²/b² ≤ 1, tested at the pixels' centres in doubled
  whole numbers. The integer square root is, since the review, `root64()`: the root's bits decided from the highest,
  each kept when the square with it does not pass the number (the definition of the root, bit by bit, with a
  32 x 32-bit product); the first version was the widely published digit-by-digit listing, now gone. The outline is
  the ellipse less the one `size` pixels inside it, each side reaching the next rows' ends (my own rule, so that flat
  parts have no gaps).
- **Flood fill:** the run-based seed fill of P. Heckbert, "A Seed Fill Algorithm", Graphics Gems, 1990 (fill a run whole,
  queue the runs touching it above and below), written from that description, as the review found. The bound on its
  stack, the rows marked where a run was left out, and the scan of those rows that takes over are my own.
- **The history:** tiles saved before they are first changed, run-length coded when that is smaller, swapped back and
  forth by Undo and Redo: a general technique, written here from that description.
- **Text:** the console's 8 x 16 font (`driver/font8x16.c`, Spleen, already in the tree with its licence) and code page
  437 (`driver/cp437.c`); typed UTF-8 is decoded as RFC 3629 describes.
- **The palette:** black to white, the desktop's own colours (`user/include/gui.h`'s GW_*), and twelve hues chosen for
  Paint. No other program's palette.
- **Files:** the decoders and encoders are libsurf's (track img's account says where they come from); replacing a file is
  this tree's `user/bin/saveutil.h`. Putting a transparent pixel on white is the plain weighted average the PNG standard
  describes for compositing against a background (its decoders' "alpha channel processing"), in whole numbers.

Behaviour: the conventions shared by raster paint programs generally, not any one program's (the left button draws in the
first colour and the right in the second; the eraser paints the second colour; the colour picker goes back to the tool
before it; a moved or pasted selection floats until it is put down). From SurfOS itself: Notepad's history (a change too
large to keep drops it) and the Image Viewer (the navy around a picture).

## What was built

### Phase 1: the model (`user/wpaint/`)

Everything that is not the window or a system call, host-tested natively under AddressSanitizer and
UndefinedBehaviorSanitizer (`tests/host/test_wpaint.c`).

| File | What |
|---|---|
| `paint.h` | the model's interface: the canvas, the tools, the selection, the history, the colours, the view |
| `ppriv.h` | what the model's files share: counted memory, the two writes to the canvas, steps, shapes |
| `canvas.c` | counted memory; the canvas; New, a picture taken as the canvas, a new size, Clear |
| `history.c` | the tiles, the steps, Undo and Redo, the bounds |
| `draw.c` | dabs, strokes, lines, rectangles, ellipses, the flood fill, text |
| `tools.c` | the tools on the pointer and keys, the selection and Paint's clipboard, typing, the colours, the status line |
| `view.c` | zoom, scrolling, the mapping between the view and the canvas, rendering, what changed |

**The canvas.** 32-bit pixels, 0x00RRGGBB, as a window's are, at any size memory allows up to what libsurf's codecs read and
write: 4096 pixels a side and 16,000,000 in all (`WIMG_SIDE_MAX`, `WIMG_PIXELS_MAX`; `PAINT_SIDE_MAX` and `PAINT_PIXELS_MAX`
are those). That second limit is the integrator's change of 11/05 (the brief said 4096 by 4096, which is 16,777,216 pixels):
the codecs could neither write nor read such a canvas again, so Paint makes none. The largest canvases are 4000 x 4000 and
4096 x 3906; New, a new size and an opened picture beyond them are refused, saying so. Paint has no transparency: a picture
with some is put on white when it is opened. A new canvas is 640 x 400, white.

**Memory: 132,194,304 bytes (126 MB) at most, which the brief's 64 MB does not hold.** The largest canvas is 64,000,000
bytes by itself, so Paint keeps at most twice that and 4 MB (`PAINT_MEM_MAX`): the canvas, and in what is left the history,
the change under way, the selection's and the clipboard's pixels, a fill's work. Every block is taken through `pm_alloc()`,
which counts it; when one would pass the bound, the history's oldest steps go first, and only then is the action refused,
with the status line saying why (`Not enough memory for a canvas of 3000 x 3000`, `Not enough memory to copy ...`). At sizes
2000 x 2000 the bound's parts come to about 82 MB at most: the canvas 16 MB, the history 32 MiB, the clipboard and a
lifted selection 16 MB each (the first account said "well within 64 MB" here, which the review showed to be wrong).
What even the whole history could not make room for is refused before any step goes (`pm_room()`).
- *Why twice the canvas.* On the largest canvas a new size, a selection of all of it lifted or copied, and a picture opened
  in its place each need a second canvas's room beside the first; the 4 MB are the row tables of two canvases, a fill's
  stack and marks (2.3 MB at most) and the history's tables. With less, those would be refused on the largest canvases.
- *It came down with the canvas.* It was 128 MiB (134,217,728 bytes), twice a 4096 x 4096 canvas; capping the canvas at
  16,000,000 pixels brings it to 126 MiB. It cannot come further down without refusing the actions above on the largest
  canvases; the test `canvas()` makes a 4000 x 4000 canvas and does each of them within the bound.
- *Opening a file holds more for a moment:* the file's bytes (at most `PF_FILE_MAX`, 72 MiB) and what the decoder takes
  (the picture's pixels, at most 64,000,000 bytes, and its own work, a progressive JPEG's coefficients, at most
  `WIMG_COEFS_MAX`, 64 MiB), beside Paint's own, until the picture becomes the canvas and is counted. **The true peak,
  then, is about 340 MB** (338,800,640 bytes: 132,194,304 + 75,497,472 + 64,000,000 + 67,108,864): Paint at its bound, the largest
  file, a progressive JPEG decoded. The decoders bound all of that themselves before they ask for it (image.h). The
  file's bytes already went as soon as the decoder was done; nothing else can go sooner, since the canvas, its history
  and the clipboard must stay as they were if the file turns out to be no picture, and a decoder gives a whole picture or
  none. The man page and docs/GUI.md say so too.

**Every write to the canvas goes through two calls,** `fill_span()` (a run of one colour) and `copy_span()` (a row of
pixels), both clipped. Before they write, they save the tiles they touch into the change under way, and they mark what the
window must draw again. So no tool can change a pixel the history did not see, and the window presents only what changed.

**The history saves changed regions.** The canvas is cut into tiles of 64 x 64 (`PAINT_TILE`). The first time a change
touches a tile, its pixels are saved: as (count, colour) pairs when there are few runs, else as they are. A blank tile costs
8 bytes, a photograph's 16 KB. Undo takes the step out of the history and swaps: the tile's pixels now are saved, the step's
put back; Redo swaps again, so a step serves both ways. A new size is a step that holds the whole canvas before it (most of
a drawing's tiles compress to almost nothing), and its undo makes a canvas of the old size again.

Bounds: 100 steps (`PAINT_UNDO_STEPS`) and 32 MB (`PAINT_UNDO_MAX`), the change under way counted; the oldest step goes
first. A change that alone is larger, or whose tiles cannot be had, is made all the same but cannot be undone: the history
is dropped with it (nothing before it can be reached across it), and the status line says so, as Notepad's history goes
with a paste too large to keep. When Undo cannot keep the pixels it replaces (no memory), it still undoes, and says that it
cannot be redone. A change that changes nothing (a fill of the colour already there) is no step, and leaves Redo as it was.

The canvas's state moves with the history (each kept step goes from one state to another), so `paint_modified()` is false
again when Undo goes back to what was saved or opened.

**The tools.** The left button draws in the first colour and the right in the second.
- Pencil (one pixel), brush (discs of 1, 2, 3, 5, 8, 12, 16 or 24 pixels), eraser (squares of those sizes in the second
  colour): a step from press to release, a dab at each point of the line between the pointer's positions.
- Line, rectangle and ellipse, outlined (in the brush's size) or filled: shown while they are dragged (the view draws them
  over the canvas, the canvas untouched) and drawn when the button is let go, from the same rows, so what is drawn is what
  was shown. Shift (when a pointer event carries it) keeps a line at a multiple of 45 degrees and makes squares and circles.
  An ellipse touches its box on all four sides, and its outline has no gaps.
- Fill: 4-connected, of the exact colour clicked. Iterative, with a stack of at most 16,384 runs (256 KB); when it is full,
  a run is left, and once the stack is empty the canvas is scanned for runs of the colour beside a pixel this fill made (a
  bit each, 2 MB at the largest canvas), until there are none. So any picture is filled whole in bounded memory.
- Text: a click starts it, keys type it in the first colour, in the console's font at 1 to 8 times, up to 255 characters
  and 16 lines. Enter starts a line, Backspace takes one back, Esc or Undo drops it; a click elsewhere, another tool, or
  anything that puts down what floats draws it, one step. Characters beyond ASCII come as UTF-8 and show as the console's,
  or `?`.
- Colour picker: a click takes the colour into the first colour (the right button: the second), and the tool before it
  comes back.
- Select: a drag marks a rectangle of the canvas. A drag inside it, or an arrow (Shift: ten pixels), lifts its pixels: they
  float, and where they were becomes the second colour. What floats is put down by a click outside, Enter, Esc, another tool,
  or anything else that draws: one step from the lift. Undo before that puts it back. At least a pixel of it stays on the
  canvas while it is moved; what is past the edges when it is put down is lost.

**Paint's clipboard.** Copy and Cut keep the selection's pixels; Paste floats them at the top left of what the view shows,
with the selection tool chosen. The clipboard and what floats share their pixels (`struct pbuf`, counted references, never
changed once made), so a paste costs nothing more and Copy of what floats costs nothing. The clipboard stays when a new
canvas is made or a picture opened. It is Paint's own: the desktop's clipboard holds text.

**The view.** Zoom from 1/8 to 32 times, in 14 levels (`PAINT_ZOOMS`). Zoomed out, a view pixel shows the canvas pixel
in the middle of those it stands for, and a click goes to that same pixel. Zooming keeps the canvas's point under the
pointer (or the view's middle) where it is. A canvas smaller than the view is shown in its middle, on the desktop's
navy. Fit is the largest level that shows it whole, at most 100%. `paint_render()` fills any rectangle of the view: the
canvas, and over it what floats, the shape being dragged, the text being typed with its cursor (two pixels, colours
turned over), and the selection's outline in black and white dashes. `paint_damage()` turns what the model marked, in
canvas pixels, into the view's rectangle to draw and present.

### Phase 2, first part: the files (`user/wpaint/files.c`, `files.h`)

Merged first: img's codec commit, `git merge --no-ff aea719bd` (apps/img: the decoders moved into libsurf, and the BMP and PNG
encoders), as the integrator named it. Nothing of Paint's was touched by it.

| What | How |
|---|---|
| Open | the file read whole (at most `PF_FILE_MAX`, 72 MB: 16,000,000 pixels are about 64 MB as a BMP or PNG); `wimg_decode()`, which knows BMP, PNG, GIF and JPEG from the first bytes, never the name; transparency put on white (`pf_flatten()`: each channel `(c x alpha + 255 x TT) / 255`, rounded); the picture taken as the canvas (`paint_adopt()`). The history starts again, the clipboard stays. A file that cannot be read or decoded leaves the canvas as it was, and the message says why (the decoder's own words after "Not read:"). Since the review, `pf_open()` also says what the file had that the canvas has not (`PF_LOST_ALPHA`, `PF_LOST_DEPTH`) |
| Save | the kind by the name's ending, either case: `.bmp` a BMP of 24 bits (`wimg_save_bmp()`), `.png` a PNG of 8 bits a sample, RGB (`wimg_save_png()`, alpha 0: Paint's pixels are solid). Any other ending is refused, saying so; `pf_save_name()` gives the name Save as offers for a picture opened from a GIF or JPEG (`photo.jpg` becomes `photo.png`). What floats and what is typed is put down first. The codec gets the canvas's own pixels and hands the bytes to `put()` as it makes them, so nothing as large as the picture is held twice |
| A file the user has | replaced only through `user/bin/saveutil.h`: the new file beside it, fsynced and renamed over it; the old one as it was until then. `su_begin()`'s refusals are the desktop's pair care: a name beside the file that looks like the other half of a save that failed ("... may share its space: have the disk checked first (fsck)"), and no free `.tmp` name. A rename that may have half happened returns `PF_KEPT` and names the new file, which stays |
| A file Paint makes | `O_CREAT | O_EXCL`, fsynced; unlinked again when anything fails on the way. Made by another program between the look and the open: saved over as a file the user has, once |
| Messages | sentences for the window's message box: "That volume is read-only: choose another folder", "The volume is full", "It has more than the 72 MB Paint opens", the system's words otherwise |

**libgcc.** img noted that a program calling `wimg_scale()` must link libgcc (a 64-bit division). Paint calls neither
`wimg_scale()` nor `wimg_shrink()`: its view samples the canvas itself. Of the codec objects only `image_scale.o` refers to
`__divdi3` (checked by compiling each for i486 at O0 and O2), and no decoder or encoder calls into it, so it is not taken
from `libsurf.a` and Paint links without libgcc. Paint's own files need no libgcc helper at either level either. Phase
3's link confirms it: `wpaint` links at -O0 and -O2 with no libgcc on its line.

### Phase 2, second part: the window (`user/wpaint/window.c`, `panel.c`, `window.h`, `main.c`)

Merged first: kit's toolkit and desk's amended window ABI (`git merge --no-ff 2ba18949`, apps/kit with apps/desk's
1acdce47 and b513bfe2). The Makefile met kit's and img's host-test blocks at the same place; both kept, kit's first, in
the integrator's order. The window follows kit's pattern (wkit): `window.c` the window's logic, `pw_open()`,
`pw_event()` and `pw_close()`, driven by the fake desktop in the tests; `main.c` only the loop.

| Part | What |
|---|---|
| The layout | under the toolkit's root: an hbox of the tool palette (an area 60 wide) and the canvas (an area that grows, `GK_FOCUSABLE | GK_OWN_WHEEL`), then the colours (an area 44 high) and a status bar. 800 x 440 asked for, at least 480 x 312 (the palette's 246 rows, the colours and the status bar): a 640 x 400 desktop's room is 638 x 317 |
| The canvas | the toolkit's scroll bars in pixels of the zoomed canvas (`gk_area_extent(area, cw, 1, ch, 1)`), the model's view kept as the toolkit has it (its size without the bars, its scroll: `sync_view()` before each event and in the draw function, `push_view()` after a zoom or a new size). The draw function renders the part asked for, 16 rows at a time, into a buffer that `gw_blit()` puts in the window: a program writes no window pixels but through libgui. After each event the model's damage goes to `gk_damage()` and the toolkit presents only it |
| The panels | areas Paint draws (`panel.c`): the toolkit has no tool palette or colour swatch. The tools' icons are 16 x 16 bitmaps made for Paint by a scratch script from simple shapes and finished by hand (the text tool's is the console's A); the chosen tool, size, style or scale lit in the accent as the desktop's toolbars light theirs. `pw_tools_hit()` and `pw_colours_hit()` say what is where, for the clicks and the tests alike |
| The pointer | the left and right buttons draw (the model's press, move and release, in canvas coordinates), the middle one drags the view; a drag past the view's edge scrolls it, up to 32 pixels an event. The wheel scrolls 16 pixels a row, across with Shift, and zooms with Ctrl about the pointer |
| The keys | the canvas takes a typed character before the menus' shortcuts (gk.h), so the tools' letters, [ ], + -, X and the text typed are the canvas's; a Ctrl key is a menu's shortcut first. With nothing selected the arrows, Page Up and Down, Home and End move the view |
| Menus | File, Edit, Image, Tools, Colours (40 of the bar's 44 cells) and two items in the desktop's Help, every one within the desktop's limits: Image > Zoom holds the 14 zoom levels, which is why the model has 14 (1/6 left out), Tools exactly 14 entries. The checked and greyed items follow the model after each event; gk sends the menus again only when one changed |
| Files | Open and Save as through `gk_choose()`, only in answer to a key, a click, a menu item or a box's answer, as the desktop requires; the types `png bmp gif jpg jpeg` and `png bmp`. A chooser's folder must be absolute, so a picture's path is made absolute (`getcwd()`) when it comes from the command line or a typed line. Save of a picture with no name, or one of a kind Paint does not write, is Save as with `.png` offered. New, Open and Close with changes ask Save, Don't save or Cancel; Save there may need Save as first, and an Open after it waits for a message box's OK, since a chooser cannot follow a chooser's answer by itself |
| Text | the desktop's clipboard pastes into text being typed (Edit > Paste, Ctrl+V), its line ends as Enter, its control characters left out (since the review) |
| The title | "Untitled - Paint", "sea.png - Paint", with `*` before it while there are changes, as Notepad's |
| The theme | the view's backdrop is the theme's darkest (`WIN_COLOR_DARK`), the panels the theme's band, buttons and accent, followed at `GK_EV_THEME` |
| Starting | `wpaint [picture]`: the picture opened, or a message over a new canvas when it cannot be. The window is laid out first (`gk_flush()`), so that a picture larger than the view is fitted to the view's real size: the first screen checks found it fitted to a view of nothing, at 12%, and the host test now checks a start with a small and a large picture. Status 0, or 1 without a desktop, 2 for a wrong use |

### Phase 3: wiring and checks

Merged first: desk's implementation (`git merge --no-ff 2c5afd30`). The Makefile and `tools/gui_apps_scenario.py` met
desk's insertions and kit's at the same places; all kept, desk's first, in the integrator's order. Then, by pure
insertion: Paint's program and host-test blocks in the Makefile, `TRACKS += ('gui_apps_paint',)`, Paint's section in
docs/GUI.md; and new: `rootfs/apps/wpaint.app`, `rootfs/man/wpaint.txt`, `tools/gui_apps_paint.py`.

**The registry entry names every kind under `open-with`, none under `types`.** rootfs/apps/README's example is Paint's
(`types = bmp png`, `open-with = gif jpg jpeg`), but its rule is the other way: "a viewer names its pictures in types,
an editor that also opens them names them here (open-with), so that a double-click shows a picture". The Image Viewer
becomes a program of track img's with an entry of its own; were both to claim BMP and PNG, the first by file name would
win the double-click, and `wpaint.app` sorts before `wview.app`, so Paint would take every picture's double-click from
the viewer and the kernel log would complain of the duplicate. So: `open-with = bmp png gif jpg jpeg`. Files' Open
with offers Paint for each; a double-click keeps showing a picture. The icon is a canvas with three blobs and a brush,
drawn for Paint in the registry's palette.

## Tests

- **`tests/host/test_wpaint.c`, the model:** 957 checks. Natively on the Mac with clang, `-fsanitize=address,undefined`,
  in under two seconds, and with the Docker image's gcc in `make unittest`. What they check: sizes and the memory bound
  (a canvas refused leaves the old one; the codecs' limits, 4096 x 3907 and 4001 x 4000 refused and 4096 x 3906 made;
  on a 4000 x 4000 canvas a clear undone, a new size undone, all of it lifted and copied, within the bound); each brush
  size's disc, both buttons, the eraser's squares, a stroke with no gaps, points thousands of pixels off the canvas;
  lines with their preview, Shift, rectangles' outline pixel counts at two widths, filled and backwards; ellipses of
  140 sizes outlined at two widths and filled: inside their box, touching all four sides, symmetric both ways, one
  8-connected piece; the fill against a fill done the slow way on 60 random walls with stacks of 16,384, 7, 2 and 1
  runs and a comb no stack can hold, a diagonal wall, a 2048 x 2048 fill; text glyphs at scales 1 and 3, UTF-8, `?`,
  Backspace, Esc, Undo, the limits, text past the edges; the picker; the selection moved, nudged, put down, undone,
  copied, pasted, dragged, cut, deleted, past the edges, Select all, the outline's dashes; 16 steps undone pixel for
  pixel on a canvas whose edge tiles are 2 and 6 pixels, what was saved, Redo dropped and kept, 100 steps, the byte
  bound trimming, changes too large and memory too short; a new size, its undo and redo, with what floats and a
  selection; every zoom both ways, zooming about a point, scrolling, Fit, the view's pixels at 100%, 200% and 50%, in
  part and clipped at the view's edges, what changed; colours as typed; the status line; a drag under way when
  something else comes; and 12 rounds of 70 random actions, each undone to the blank canvas and redone to the end, with
  the memory counted checked after every action.
- **`tests/host/test_wpaint_files.c`, the files:** 691 checks, natively the same way and in `make unittest`. The codecs,
  the model and `files.c` are built with the host tests' counting allocator (`tests/host/undertow/alloc.c`, as img's
  block builds the codecs), and `files.c` with `tests/host/wpaint_host.h`, which gives `saveutil.h` the host's names of
  `struct stat`'s fields and `open()` a mode, and makes each file call fail on request. They check: the kinds by name
  and the name Save as offers; transparency on white, exactly; canvases of six sizes from 1 x 1 to 4096 x 3 saved as
  BMP (its size to the byte) and PNG and opened again pixel for pixel, the largest canvas (4000 x 4000) in both too;
  what floats and what is typed saved with the picture; a file replaced (one fsync, one rename) and one made (no
  rename); `write` failing at each of its calls, with ENOSPC on a file the user has (the file as it was, no `.tmp`)
  and EIO on one Paint makes (gone again); writes of 7 bytes at a time, and a write of nothing; `fsync`, `close` and
  `open` failing; a rename that changed nothing (the `.tmp` gone) and one that may have half happened (kept, and
  named), and the save after it refused by the pair care; all ten `.tmp` names taken; a `.tmp` that will not go; a
  folder of the name, a folder that is not there, a kind Paint does not write; opening a file not there, a folder, an
  empty file, a file read short or failing, one over 72 MB (sparse), one that is no picture; each of the 125 pictures
  in `tests/fixtures/wundertow/images/`, whole and cut in half, opened exactly as the decoders read it on white (125) or
  refused with their message and the canvas as it was (125); every allocation of an open (a progressive JPEG's) and of
  a save failing in turn, with nothing left held and the old file as it was; the model refusing a picture its bound
  cannot hold, the decoder's pixels freed; and the history and the clipboard across an open.
- **`tests/host/test_wpaint_window.c`, the window on the fake desktop:** 162 checks, natively (x86_64 under Rosetta, as
  blibc's printf has x86 assembly, kit's way) with both sanitizers, and in `make unittest`. No desktop refused; the
  window's parts in place, the canvas on the backdrop, the brush lit; a drag with the brush, a click presenting less
  than an eighth of the window; undo and redo by their shortcuts, the title's `*` coming and going; tools by the palette,
  the keys and the Tools menu, lit and checked; sizes by the palette and [ ], filled by the menu and the palette; text
  typed, shown before it is drawn, drawn by a click elsewhere; the picker; the selection copied, pasted, dragged, put
  down, nudged, deleted, Select all and Esc; zoom by +, Ctrl+0, the Zoom submenu (its dot following), Ctrl and the
  wheel about the pointer; scrolling by the wheel, across with Shift, Home, the arrows and the middle button; Fit and
  the status line's size and zoom; the window resized; the colours by both buttons, the swap, Edit... with a colour
  and with no colour; Save as through the chooser (the name and kinds offered), Save without one, Save as BMP in the
  file's folder, Open (the kinds read), a GIF saved as PNG (`.png` offered, its folder absolute), a large picture
  fitted, a file that is no picture refused, a desktop without choosers (the path typed), a kind Paint does not write
  refused; New, Open, the close button and File > Close asking first, with Cancel, Don't save and Save (through Save
  as, then the chooser to open); a picture given at the start at 100%, a large one fitted to the view, one that cannot be; the theme
  followed.
- **`tools/gui_apps_paint.py`, on the screen** (`GUIAPPS_ONLY=gui_apps_paint`, the guiapps scenario of a USB-booted
  SurfOS): Apps > Programs lists Paint and starts it with the focus; Files' Open with offers it for a BMP on the stick
  and starts it with the path; the picture shown; a drag with the brush, Ctrl+Z, Ctrl+Y, Ctrl+S in place; Paint's
  menus in the bar and Image > Zoom > 200% by the keyboard; Ctrl+0; the fill with a palette colour; Save as through the
  desktop's chooser (in the picture's folder; its own title looked for anywhere in its bar, since desk's fix round
  names the program there instead of the window) to a PNG; Open of a file that is no picture;
  the close button asking, Esc, then File > Close asking again and Don't save; the stick ejected. The host reads both
  files back (the PNG with a reader of the checks' own, from the PNG standard, over Python's zlib), and the scenario's
  FAT checkers look the stick over. Everything is left as it was found, also after a failed check (`tidy()`: boxes and
  choosers dismissed, Paint closed without saving, the stick ejected, Files closed), so that the tracks after Paint's in
  the scenario start from the desktop they expect.

**For desk (found on the screen, not changed here):** the desktop's close button can only offer to kill a program once
the program has answered it with a question of its own. `shell/guiclient.c`'s `client_close()` sets `closing` at the
first press and never clears it, so after Paint's "Save the changes?" and Cancel, the next press of the close button is
the desktop's "End the program?" (End loses the changes), not a second `WIN_EV_CLOSE`. Every application with an
unsaved-changes question meets it. Paint's checks close the second time by File > Close (Ctrl+W), which asks Paint's
question again. The integrator confirmed it on 11/05 and put it in desk's fix round (1447a4c2, which came to Paint
with kit's stage 3 on 11/09): the close button now sends a program that reads its events the close again, and
Paint's screen checks press it a second time after Cancel and see Paint's own question, not "End the program?".

## The review

An independent review read f3bee9a8 (11/05). What held up: replacing files only through saveutil.h, new files O_EXCL
and gone again on failure, Save as saving exactly the path chosen, the unsaved-changes questions, the arithmetic, the
fuzzing (1,500 model rounds, 3,000 with failures injected), no recursion, the fill against a plain BFS. One fix round,
on top of the four commits, in topic commits; the reviewer's probes (`scratchpad/review-paint/`, read there, never
changed) were built against the fixed tree and run again: each finding below is gone in them.

- **Critical: Cut when the copy fails deleted the selection, then read a clipboard of NULL** (`paint_cut()` took "the
  clipboard changed" for "copied"; `paint_copy()` had dropped the old clipboard to make room). Now `to_clipboard()`
  says whether the copy is there, Cut and Copy go by that alone, and the old clipboard goes only once the new copy
  exists: a copy that fails changes nothing and says "the clipboard keeps what it had". Copy and Cut end a drag first,
  so Ctrl+X during a stroke cuts what the stroke drew. Test: `cut_without_memory()` (the probe's shape at 200 x 200: a
  stroke under way, Select all, no room even were the history gone: nothing deleted, the clipboard as it was).
- **High: transparency and 16-bit depth were lost on Save without a word.** `pf_open()` now returns what the file had
  that the canvas has not (`PF_LOST_ALPHA`: a pixel the decoder gave was not solid; `PF_LOST_DEPTH`: a PNG of 16 bits a
  sample, or a BMP of bit fields wider than 8 bits, read from the header the decoder already checked). The open
  message says so ("its transparency on white", "kept at 8 bits a channel"), and a save that would replace that file
  asks first: "Saving replaces logo.png without its transparency", with Save as..., Save and Cancel. Save as... is the
  first button, so Enter's: the toolkit's question box (kit's `gk_confirm()`) makes its first button the default and
  has no way to choose another, and Save as... never touches the file, as Cancel would not; Esc is Cancel. Once a save
  has replaced the file, or the picture lives in a file Paint wrote, nothing is asked. Tests: the files' fixtures say
  what they lost (15 transparency, 9 depth, eleven by name), and the window opens and saves over a transparent PNG and
  a 16-bit one (Esc, Save as... to another file, Save, and Close's Save going through the question).
- **Medium: changes made behind a box could be thrown away without a question** (the bar's menus reached Paint while
  its "Now choose the picture to open" box was up, and Esc or a box put in its place opened the chooser). Now the bar's
  menus do nothing while one of Paint's boxes is open (kit is making boxes modal for them too); that box opens the
  chooser only on OK; and New and Open look again just before the canvas goes: if it changed since the person's answer
  (the canvas's state is kept at that answer), the question is asked again, and a picture already chosen waits on it
  (`PW_THEN_OPEN_PATH`). Tests: the bar's Select all and Clear behind the box, Esc on it, Clear while the Open chooser
  is up (asked again: Don't save opens the picture, Cancel keeps the canvas), Clear behind New's size box.
- **Medium: Undo of a large paste lost its Redo silently** (after the swap the step held the pasted pixels, more than
  the history's 32 MB, and the trim took it). The step an Undo has just swapped is now never trimmed for the bytes: it
  stays over the bound, so Redo works, until a new change or another step's room needs it (memory stays within
  `PAINT_MEM_MAX`, which every block is counted against). After a Redo the oldest undo goes first as before, that
  step's own way back included, so the redos still ahead stay whole. And when an Undo or a Redo costs redos beyond it
  all the same, the status line says so ("not enough room in the history for the redos beyond it"). The review's
  fuzzer, run here with failures injected, showed why that last part matters: on f3bee9a8 it found 15 rounds of 3,000
  where a redo went without a word; with the exemption alone 26; with the message, none. Tests: `undo_large_after()`.
- **Low: Cut of a floating paste did nothing and said "Copied".** Cut of what floats now cuts it: a paste goes (the
  canvas as it was), a lifted part leaves the second colour in one step named Cut, which Undo takes back.
- **Low: a lost button release drew a long line.** The canvas loses the keys (GK_EV_FOCUS 0) when the window loses the
  focus and when a box opens over it; Paint then ends a drag where it was (`let_go()`), so a move once the focus is back
  draws nothing. Tests: the focus lost mid-stroke, and Ctrl+W's question opening mid-stroke.
- **Low: the fill could take seconds on a crafted picture** (the reviewer's teeth: 148 scans of the whole canvas, 1.56
  s at 4000 x 4000). A run left out because the stack was full now marks its row, and the scan reads only marked rows
  (a run left unfilled beside the fill can only be in one). The reviewer's probe, at 4000 x 4000: the teeth both ways
  0.05 s, noise from 60% to 80% open 0.17 to 0.20 s, each the same as a BFS fill. Test: `hard_fill()`, the teeth upside
  down at 2048 x 2048 against the slow fill, its scans reading fewer rows than the canvas has (40) and the time printed.
- **Low: pasted text fed control bytes to the text tool.** C0 but the line ends, DEL, and C1 (UTF-8's C2 80 to C2 9F)
  are left out. Test: a paste with each, and an e acute kept.
- **Low: refused actions still cost history, and a failed copy the clipboard.** `pm_room()` gives up the history's
  oldest steps only when that makes room: what even the whole history could not make room for is refused with every
  step kept. A change of several blocks (a pixel buffer, a fill's work, a new size) asks for all of them at once.
  Test: `refused_keeps_history()` (a copy, a fill, New and a new size refused, the steps all there).
- **Low: the memory claims.** Corrected as above (82 MB at 2000 x 2000, about 340 MB at the peak of an open), here, in
  paint.h, the man page and docs/GUI.md.
- **Code origin:** `line_rows()` and the square root rewritten from the mathematics (Where the code comes from). The
  lines are checked against the closed form (`lines()`: every line within 13 pixels each way), the ellipses' rows
  against their definition in boxes up to 4096 x 4096 (`ellipse_rows()`).

### The second review

The same reviewer read the fix round (f3bee9a8..421462d7) on 11/09 and found it sound to ship: every finding fixed,
6,000 fuzz rounds with failures injected and 600 of the window clean, the line against its closed form on 636,804
lines. Three lows were left, fixed in a last round with the merge the integrator named:

- **Merged** apps/kit d38ea834 (`git merge --no-ff`): kit's stage 3 (a box greys the program's menus, a shortcut acts
  once a press, a drag starts only from a press seen, choosers kept on EAGAIN, kit's chooser checks finding the
  chooser by its own title) and desk's fix round 1447a4c2. The Makefile and the TRACKS line met theirs at the same
  places; all kept. Paint's window test had sent menu items from the bar while a box was up; the toolkit greys them
  now, so the desktop refuses them, and the test hands Paint the item's event itself to reach Paint's own guard (in
  the merge commit, so that it passes alone). Paint takes none of stage 2's or 3's new widgets.
- **The check that waited for desk:** the close button, "Save the changes?", Esc, the close button again: Paint's
  question again, not "End the program?" (`tools/gui_apps_paint.py`), then File > Close asking too.
- **A large Cut hid its own warning:** when the Cut's step could not be kept, "Cut W x H pixels" said over lose()'s
  "it cannot be undone"; now it comes first and lose()'s words stay. Test: `cut_too_large()`.
- **The save question compared paths as written:** the same file through ".." or in another case was replaced without
  it. Both paths are now made canonical and compared without case (`same_file()`): SurfOS's `struct stat` names the
  device by name, not number, so the file's identity by stat is not the same call on SurfOS and the host; off FAT the
  comparison can only ask once more. Test: Save as through "sub/../" and in capitals, asked; another file, saved.
- **A question replaced did nothing:** the close button pressed while "Save the changes?" was up asked again, but the
  first question's -1 (which the toolkit gives a box another replaces) cleared what the new one waited on. A -1 that
  comes while a box is open is now no one's Cancel, and a new question forgets an old Save as. Test: the close button
  twice, then Don't save: closed. The reviewer's probe_cut2 and probe_window5, built against the tree, say the same.

## The gates

The last ones on the tree of 490094db (the four commits, the fix round's seven, the merge of kit's stage 3, and the
last round's three), clean (`git status` empty), before this account's update was committed; the account changes
nothing they build or run.

- `make unittest` (the Docker image's gcc, 32-bit): passed. Paint's among it: test_wpaint 957 checks, test_wpaint_files
  691 (125 pictures opened, 125 refused; 15 said they lost transparency, 9 depth), test_wpaint_window 162; test_guidesk
  reads `wpaint.app`; test_man.py finds Paint's page.
- The guiapps scenario, whole, through the lock: passed at -O0 and at -O2, 423 of 423 checks each, Paint's 34 among
  them (the second close press's two new). kit's two chooser checks that failed on 11/09 pass with kit's stage 3.
- `make test-all` (not asked of the last round, which touched no kernel or shell code): passed on 51f37c5f at -O0
  and -O2, 558 smoke checks and 51 of Files each. On 51f37c5f the guiapps scenario was 362 of 364 at both levels, the
  two failures kit's, since fixed.
- Natively on the Mac, with AddressSanitizer and UndefinedBehaviorSanitizer: the three tests clean.
- Each commit of the series alone, from a clean tree of it: natively, its host tests under both sanitizers and every
  file of `user/wpaint/` compiled for i486 against SurfOS's headers with warnings as errors; in the build image, the
  Makefile read whole (`make -n all images`), Paint's host tests made and run, and from the window's commit on
  `wpaint` linked.

**Found by the first `make unittest`:** in the Docker image a 32-bit `readdir()` fails at its first call with
EOVERFLOW (a ten-line program showed it: none of four entries read, and all four with the large-file call). The image
runs emulated on this Mac, and the offsets of its folders are 64-bit, which the 32-bit `struct dirent` cannot hold. So
test_wpaint_files read none of the fixtures and could not empty its folder, and test_wpaint_window would not have
emptied its folder either. Both now define `_FILE_OFFSET_BITS 64` before their first include (folded into their commits,
as nothing had been reported). macOS ignores it. No other host test lists a folder with the C library.

## Left out, and why

- **Transparency.** Not in the brief, and every tool, the view and both files would need it. A picture's is put on white.
- **Saving GIF or JPEG.** The brief asks for BMP and PNG, which are what libsurf writes.
- **Anti-aliasing, fill tolerance, rotating and flipping, scaling the picture, cropping to the selection.** Not in the brief.
- **Undo of New and Open.** They start the history again, as opening a file in Notepad does.
- **kit's second stage for itself** (`gk_slider`, `gk_palette`, `gk_progress`, `gk_text_area`, apps/kit 229cb2e9). It
  came after Paint's window was done and passing on the screen, and the integrator offered it only to tracks that would
  use it. Paint's tool palette, brush sizes and colours stay areas of its own (`panel.c`), already tested; moving them
  onto the new widgets would reopen the window's tests for nothing a user would see. Nor kit's fix of `gk_next()`
  (eab009d3): kit found that only a finite wait that had run out could hold events back after a burst, and Paint waits
  with -1, which never returns 0, so the integrator said Paint does not need it.
