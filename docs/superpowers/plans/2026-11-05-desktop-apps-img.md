# The desktop applications: track img

The brief is `2026-11-05-desktop-apps.md`. This track moved the picture decoders out of Wundertow into libsurf, gave libsurf
encoders for BMP and PNG (stage 1), and made the Image Viewer a program, `wview` (stage 2). None of it has run on the laptop:
only host tests and QEMU, until the owner tries it.

## Stage 1: pictures in libsurf

### What was built

| Where | What |
|---|---|
| `user/include/image.h` (was `user/wundertow/image.h`) | the pictures' calls for every program: the decoders as they were, the encoders, `wimg_error()` |
| `user/libsurf/image.c`, `image_bmp.c`, `image_gif.c`, `image_jpeg.c`, `image_scale.c` (were `user/wundertow/`'s) | moved; `ut_error()` became `wimg_error()`, and one message changed (below) |
| `user/libsurf/image_png.c` (was `user/wundertow/`'s) | moved; its own inflate and CRC table gave way to libsurf's `inflate.c` and `crc32.c` |
| `user/libsurf/image_save.c` | new: `wimg_save_bmp()` (24 bits) and `wimg_save_png()` (8 bits a sample, RGB or RGBA) |
| `tests/host/test_wundertow_image.c` | not changed: built against libsurf's objects now |
| `tests/host/test_image_save.c` | new: the encoders against the decoders, by hand, under every failure |
| `tests/test_image_files.py` | new: what the encoders write, read by Python's zlib and struct, and by sips on the Mac |
| `Makefile` | the img block before `unittest: $(BUILD)/host/test_blibc`; Wundertow's image lines point at libsurf's objects |
| `docs/UNDERTOW.md` | the pictures paragraph says where the decoders are now |

Wundertow needs no change of its own: its `browser.c`, `helper.c` and `loader.c` include `"image.h"`, which is found in
`user/include/` now (`-Iuser/include` in the programs' flags, `-idirafter user/include` in the host tests'), and the program
takes the decoders from `libsurf.a`.

### Decisions a reviewer would ask about

- **The names stay `struct wimg`, `wimg_*` and `WIMG_*`.** It is a move, not a rewrite: Wundertow's callers and every test
  stay as they were, and git follows the files. A neutral name was not worth the churn, and `test_wundertow_layout.c` has a
  `struct image` of its own that a generic header would have met.
- **`wimg_error()` is Undertow's `ut_error()` made again, the same way:** formatted apart (an argument may be the buffer
  itself), cut to the caller's buffer, anything but printable ASCII as `?`. Its working buffer is `WIMG_ERR_MAX`, 160
  bytes, where `ut_error()`'s was 2400: the longest message the decoders can make is 94 characters.
- **One message changed:** "not a picture this browser reads (BMP, GIF, PNG, JPEG)" is "not a picture of a kind read here
  (BMP, GIF, PNG, JPEG)" now that the Image Viewer says it too. `test_wundertow_image` looks for "not a picture", and
  `test-web` prints only "failed" for such a picture.
- **The PNG decoder over libsurf's inflate keeps its three rules.** Its data part (`data()` in `image_png.c`) is new:
  - *Inflate stops at the first byte past the rows.* The bytes every row takes with its filter's are known from IHDR
    (`data_bytes()`, in 64 bits, pass by pass for Adam7), and inflate is given room for those and one more, never more:
    what follows that byte is not read, so a small file that says it inflates to gigabytes still costs nothing.
  - *The IDAT chunks are one stream however they are cut,* each checked by its CRC when inflate first needs one of its
    bytes, an empty one passed over, and any other chunk ending the data, as before.
  - *A stream that ends with the rows is checked whole:* its last block's end by inflate, its Adler-32 (RFC 1950 section
    8.2) here, as before.
  The zlib header is checked here as before. Inflate's output goes through a buffer of 4 KB into the rows; the struct the
  decoder allocates grows from about 37 KB to about 50 KB (inflate's state is 46 KB), and the number of allocations is
  the same.
- **What it reads is what it read, but for Huffman codes zlib refuses.** The old inflate went on with a dynamic block whose
  codes left room over (an incomplete code, RFC 1951 section 3.2.7) or had no code for the block's end, until a code fell
  into the hole; libsurf's refuses such a block at once, as zlib does, so libpng's readers never show these files either.
  The evidence that nothing else changed:
  - `test_wundertow_image`, not changed by a byte, passes with **359,813 checks, the same count as before the move**. The
    count is a fingerprint: a decode that fails makes three checks and one that succeeds two, and the out-of-memory loops
    make one check per allocation, so a different outcome or allocation count anywhere would change it.
  - Runs of the old decoders (the brief's commit) against the new ones, by hand on the Mac: each of the 125 fixtures
    whole, cut at every length to 4 KB and at 400 lengths past it, with random damages (a PNG's CRCs made right again),
    and each PNG's deflate data alone damaged; two runs, of 8,000 and 20,000 damages a fixture, with other seeds.
    **8,122,288 inputs, no difference of outcome or pixels** but 21, every one a stream zlib refuses too (18 "invalid
    distances set", 3 "invalid code -- missing end-of-block"; Python's zlib asked of each). The messages differ in some
    31,700 refusals of damaged PNGs, between "a PNG whose data is corrupted" and "a PNG that ends too early": the old
    one's look-ahead met the end of the data at other places than inflate does, and libsurf's inflate says the data ran
    out wherever it did. A third run, of both decoders built with AddressSanitizer and UBSan, read 970,144 more inputs
    without a complaint.
- **The encoders take a `put()` that receives the bytes as they are made,** so a picture is never held a second time as a
  file: a program saves through `user/bin/saveutil.h` by writing them to `s.fd`, and its own `put()` keeps the write's
  errno. `put()` failing stops the save at once (it is never called again), -1 comes back, and nothing is held.
- **Only what the decoders read again is written:** a picture over `wimg_alloc()`'s limits (4096 a side, 16,000,000
  pixels) is refused before anything is allocated. **For Paint:** a canvas of 4096 by 4096 is 16,777,216 pixels, over
  that limit, as it is over the decoders'; 4000 by 4000 and 4096 by 3906 are the largest of those shapes that are written
  and read.
- **BMP: 24 bits, the 40-byte header, rows from the bottom, no resolution,** as Microsoft documents BITMAPFILEHEADER and
  BITMAPINFOHEADER; it is what every reader takes. BMP of 24 bits has no transparency: each pixel's colour is written as
  it is (a GIF's pixel that is not there is black), and the header says so.
- **PNG: 8 bits a sample, colour type 2 or 6, not interlaced, no ancillary chunks.** RGBA's alpha is 255 - TT, so a
  picture goes out and comes back bit for bit. Each row's filter is chosen as the PNG standard's section 12.8 suggests for
  true colour: all five tried, the least sum of the bytes as signed numbers kept, the first of a tie. Deflate is libsurf's
  at level 6 (RFC 1950's FLEVEL 2, so the header is `78 9C`). IDAT chunks are 32 KB. No `sRGB`, `gAMA` or `pHYs`: SurfOS's
  pixels are the screen's, and an untagged PNG is taken as sRGB by the readers tried (sips keeps every value).
- **Memory:** a BMP save takes 32 KB; a PNG save deflate's 310 KB, four rows of the picture's width and 32 KB, all asked for
  before anything is written.

### What was left out, and why

- Writing other kinds (GIF, JPEG), palettes, 16 bits, interlacing: not asked for.
- A choice of deflate level for the caller: level 6 only, until a program needs another.
- `tests/host/test_wundertow_image.c`'s header still says it tests `user/wundertow/image*.c`: the file is left byte for
  byte as it was, so that its passing says the most it can.

### For the integrator's documents

- `CLAUDE.md`: the layout's `wundertow/ ... 11/1/26 image* the picture decoders` and the Undertow section's "Picture
  decoders (`image*.c`)" are libsurf's now (`user/libsurf/image*.c`, `user/include/image.h`), with the encoders
  (`image_save.c`); the layout's libsurf list gains `image*.c`.
- `docs/ARCHITECTURE.md` and `docs/README.md`, wherever they place the decoders in Wundertow.
- A new picture format still gets a family in `tools/wundertow-images.py`, and `test_wundertow_image` still cuts and
  damages every file: only the place of the code changed.

### Tests

| Test | Checks | What |
|---|---|---|
| `test_wundertow_image` | 359,813 | as before the move: the 125 fixtures, each cut short and damaged; every allocation failing; the hostile files of each format; making pictures smaller |
| `test_image_save` | 58,289 | 92 pictures (15 sizes from 1 x 1 to 4096 x 3 and 1 x 4096, six kinds of content, two of 640 x 480) as BMP, RGB PNG and RGBA PNG, read again by the decoders pixel for pixel; BMP's headers and padding; PNG's chunks, CRCs (a bitwise CRC of the test's own), IHDR, zlib header, rows inflated and unfiltered by the test, Adler-32, every row's filter the least-sum one, every filter chosen somewhere; every allocation failing, and `put()` failing at each of its calls, for each kind; the limits, before anything is allocated; 4000 x 4000 and 4096 x 3906 begun |
| `tests/test_image_files.py` | 921 without sips, 2301 with | every file `test_image_save --write` leaves, read by Python alone (zlib's CRC and decompress, the filters undone by hand; BMP by struct) and compared with the pixels written beside it; on the Mac, sips says each one's size, kind and alpha, and converts the PNGs to BMP and the BMPs to PNG, and the pixels are the same (where alpha is 0, a colour sips need not keep) |

By hand on the Mac, natively with clang under AddressSanitizer and UndefinedBehaviorSanitizer: `test_wundertow_image`
(359,813) and `test_image_save` (58,289) pass. The 276 files `test_image_save --write` leaves were also read by the old
decoders, with their own inflate, and by the new ones: the same pixels, every one. On a PNG of 2000 x 1500, the new
decoder takes 26 ms where the old took 32 (natively, at -O2), libsurf's inflate having a fast loop. A scratch fuzzer
under both sanitizers wrote 6,000 random pictures (1 x 1 to 700 x 300, noise, gradients, transparency) in the three
forms: every one read back exactly, and 54,000 damaged copies of them were read (17,514 still pictures) without a
complaint. A picture at the limit, 4096 x 3906, was written in each form and read back whole. Deliberate bugs in a copy
of `image_save.c` (Average's rounding, the alpha, BMP's padding and size field, the filter choice and its sum, a leak on
an allocation failure, the last IDAT, the colour type) each fail `test_image_save`; the three that pass are equivalent
(Paeth's left and above exchanged, which gives the same prediction whenever they tie; `v <= 128` for `v < 128`, where
both give 128; a loop guard that saves only time).

## Stage 2: the Image Viewer as a program

### What was built

| Where | What |
|---|---|
| `user/wview/view.h`, `view.c` | the model: the views and their arithmetic as `shell/guiview.c` had them, the toolbar, the keys, the pointer, the title, the status bar, the details, `view_paint()` |
| `user/wview/folder.c` | the folder's pictures: which names, their order, the steps |
| `user/wview/main.c` | the window (libgui), the files, the decoders, the chooser, Copy path |
| `shell/guiview.c` | the desktop's Image Viewer: only its entry now (the program, its Help, Sketch's answers), `viewer_takes()`, and `viewer_show()` starting the program on a file |
| `shell/gui_media.h` | `viewer_place()` and `viewer_zoom()` gone with the kernel's viewer; the comments of what stays |
| `rootfs/apps/wview.app` | the registry's entry (desk's format): the pictures it opens, its icon |
| `rootfs/man/wview.txt` | the manual |
| `tests/host/test_wview.c` | the model's host test, carrying on `test_guimedia.c`'s viewer test |
| `tests/host/test_guimedia.c`, `test_guisuite.c` | the kernel viewer's checks out (they are `test_wview`'s now); the entry, Files' rule and `viewer_show()` checked instead |
| `tools/gui_apps_img.py` | the guiapps scenario's checks |
| `tools/gui_apps_media.py` | two lines: the chooser's title (below) |
| `Makefile`, `tools/gui_apps_scenario.py`, `docs/GUI.md` | the insertions the brief allows |
| the review round | `user/libsurf/image_jpeg.c` (its inverse DCT, Huffman tables and bit reader written again), `image_png.c`, `image_bmp.c` and `user/include/image.h` (the bombs), `tests/host/test_wundertow_image.c` (`bombs()`), `tests/test_image_jpeg.py` (new: libsurf's JPEGs against sips), `shell/guicore.c` (one line: a program's application has its tick), the Makefile's Wundertow link and `unittest-img` |

The files outside the brief's list (`shell/gui_media.h`, `test_guimedia.c`, `test_guisuite.c`, `gui_apps_media.py`) had
to change with `shell/guiview.c`: they declared, tested or read the kernel's viewer, which is gone. Each change is the
least that keeps them true; nothing else in them was touched. In the review round `shell/guicore.c` gained one line
(`gui_tick()` runs the tick of an application that is a program, whose slot never opens), for H1's report of a viewer
whose window never came; `test_wundertow_image.c` is Wundertow's test of the decoders, which are libsurf's now.

### How the desktop reaches it

- **Apps > Image Viewer, its tile and Next window:** `gui_app_viewer.program` is `/initrd/bin/wview`, so the launcher
  (`guicore.c`'s `app_program()`) starts it, or brings its window back, as it does the Clock's `wclock`. Its window opens
  at the Image Viewer's place (`guiclient.c`, `gui_app_slot_of()`).
- **Files' Enter or double-click on a picture:** `wview.app` names the pictures in `open-with`, not in `types`, so no
  registry entry takes the double-click and Files' own rule does: `viewer_takes()`, now any of .bmp, .png, .gif, .jpg and
  .jpeg in any case, then `viewer_show()`, which starts `wview` with the path as its one argument and keeps its pid as the
  Image Viewer's. Its window stands where the Image Viewer's stood and its tile brings it back. After the review, one at
  a time: a file opened while the Image Viewer's `wview` runs replaces it (`viewer_replace()`: the one there is told to
  close, as it has nothing to save, or ended when it has no window yet or did not go when told), so a few
  double-clicks no longer use up the 8 windows the desktop's programs may have. What is no file (a folder, a device,
  a name no longer there) is said in a message, as the desktop's own viewer said it, and nothing starts; a start
  refused is said; with every program's window in use but the viewer's, nothing starts and that is said; and a `wview`
  whose window has not come 5 seconds after its start is said on the screen (the entry's `tick()`). With `types`, the
  registry would start it instead (`reg_open()`), at the next cascade place and as no application's window: the tile
  would start a second one, and the media and suite tracks' checks, which look for the Image Viewer where it stands,
  would not find it. So a double-click reaches `wview` this way only while no registry entry lists pictures under
  `types`: an entry that does takes them, by desk's rule, and starts its own program.
- **File > Open with** lists the entry, "Image Viewer", which starts a new `wview` the registry's way (`reg_open()`, the
  cascade, each in a place of its own); the desktop's own item for the Image Viewer is left out then, the entry's
  program being the Image Viewer's (desk's `open_with_items()`).
- **Screenshots** are 24-bit BMPs named "Screenshot YYYY-MM-DD HHMMSS.bmp": `viewer_takes()` takes them, and the name's
  spaces stay in the one argument (`guisys_win.c`). The screen checks open one from the folder.
- **Sketch's** Save as BMP and Open BMP stay the desktop's (`guibmp.c`), and the Image Viewer's entry still answers their
  dialogs, as it did.

### Decisions a reviewer would ask about

- **The same window, at the same place.** wview opens 558 x 371 of content, the desktop's Image Viewer's 560 x 400 with its
  frame. Its room, toolbar row, status bar and colours are the old window's, its views the old arithmetic (`fit_zoom`,
  the steps, `zoom_to` keeping the middle, the pan), so the media track's screen checks of the Image Viewer read it as they
  read the kernel's. The status bar begins as the old one did ("sea24.bmp  42 x 30, 24-bit") and goes on
  (" BMP  (1 of 2)").
- **What is new:** PNG, GIF (its first frame) and JPEG through libsurf's decoders; shrinking by an average (the kernel's
  took the nearest pixel); transparency over squares of grey; the folder's next and previous picture; the details; Copy
  path.
- **The toolbar** has Open, ◄, ►, Fit, Actual, Zoom in, Zoom out, Details, Copy path: "Actual size" became "Actual" so that
  all nine fit the old window's least width, 540 pixels.
- **32-bit arithmetic.** `view.c` divides only in 32 bits: the middle of a zoom in two steps, exact, its ranges (16 times,
  4096 pixels, 1/64) checked to fit; so wview links libsurf alone, without libgcc (below).
- **Memory.** A file of at most 32 MiB is read whole (the decoders need it so), or 62 MiB for a BMP (the review's M2), and
  let go once decoded. The picture is kept as decoded, up to the decoders' 16,000,000 pixels: 61 MiB, which with the
  rest goes over the brief's 64 MB. Shrinking big pictures as Wundertow does (2048 a side) would cap it at 16 MB, but the
  actual size would then no longer show the picture's own pixels, which is what a viewer is for. While a new picture is
  decoded the old one stays, so for a moment two are held: the very worst, a picture of 16,000,000 pixels shown while a
  32 MiB progressive JPEG decodes (64 MiB of coefficients, its planes and its pixels), comes to about 246 to 250 MiB
  (258 to 262 MB), which the laptop has and a program's 1 GB of address space holds; a 62 MiB BMP comes to less, about
  184 MiB. libsurf's heap grows by sbrk and keeps its peak until the program ends, so a `wview` that once held that
  much holds it until it is closed. Every allocation is checked; when one fails the status bar says so and the old
  picture stays.
- **The folder** is listed when a file is opened (not at each step): its first 4096 pictures, 256 KB of names, by their
  endings, in the order of numbers in names compared as numbers. A file among them that is no picture is said when it is
  reached, the picture before staying, and the next press goes on past it.
- **No menus of its own** (`WIN_MENU`): the desktop's File and Edit stay while its window is in front, as for the old
  Image Viewer, and Edit > Copy sends Ctrl+C, which is Copy path.
- **Open** asks the desktop's chooser (`WIN_DIALOG`) in the picture's folder, for bmp, png, gif, jpg and jpeg. Its answers:
  a path chosen is taken (`WIN_TAKE`) on the answer's event, whatever request it answers, and shown when it answers
  Open, since a path left untaken keeps the next `WIN_DIALOG` busy (`EBUSY`, desk's fix round); a chooser cancelled,
  also one put away for another dialog, changes nothing; `EAGAIN` (the window's own events wait to be read, win.h) says
  so in the status bar, to try again; `EBUSY`, `EACCES` and `EINVAL` say theirs. An answer that is no picture leaves
  the folder and the place of the picture still shown (the review's L4). The desktop's bar names the program
  before the chooser's own title (by the window's title at 2c5afd30, by its registry title after desk's fix round), so
  the screen checks find the chooser by its own title, Open a file, wherever the bar puts it (`gui_apps_media.py`'s
  `open_chooser()`, which its two Image Viewer checks and `gui_apps_img.py` use).
- **Help and the registry's title.** wview adds no Help items (it has no menus); the desktop's Help keeps the Image
  Viewer's lines, which say it is the program `wview` now. `wview.app`'s title is "Image Viewer": UTF-8 with no control
  characters, and not the desktop's sign or its names, as desk's fix round checks titles.
- **Keys** are the old ones and N, Space, P, Backspace, I, Ctrl+O, Ctrl+C, Ctrl+W and Esc. A key with Alt is left alone
  (`WIN_MOD_ALT`: another program's business). A held key repeats (`WIN_MOD_REPEAT`) only where repeating means
  something: the arrows, Page Up and Down, + and -, N, P, Space and Backspace (`view_repeats()`); a held O would ask the
  chooser again and again, a held I would show and hide the details. Ctrl with a digit comes as the digit, so Ctrl+0 is
  Fit and Ctrl+1 the actual size, as 0 and 1 are.

### What was left out, and why

- `struct gui_viewer` (`include/sys/gui_media.h`) keeps its fields, though only `sketch` is used now and nothing sets
  `loading` (`gui_working()` and `test_guiclient.c` still read it): trimming it touches Sketch's `guibmp.c` and
  `guicore.c`, outside this track; for the integrator.
- Rotating, a slideshow, editing and saving pictures, printing, a GIF's later frames: not asked for.

### For the integrator's documents

- `CLAUDE.md`: the layout gains `user/wview/` ("11/05/26, the desktop's Image Viewer as a program: view.c the model,
  host-tested by test_wview; folder.c the folder's pictures; main.c its window"); the shell list's "guiview.c + guibmp.c"
  becomes "guiview.c (11/05/26: the entry that starts wview) + guibmp.c"; "Programs' windows" gains "Apps > Image Viewer
  `wview`".
- `docs/README.md`'s row for GUI.md is still right, and so is `CLAUDE.md`'s "(the Image Viewer opens it)" for
  `wundertow --render`.

### Tests

| Test | Checks | What |
|---|---|---|
| `test_wview` | 270 | that the window fits a desktop of 640 x 400 (from the review); the old viewer test's views with its numbers (Fit, the steps, the middle kept, the pan, the double-click, the wheel), the middle at the extremes (4096 x 16, 1/64 to 16 times); the toolbar's labels, places and states; painting (an average, transparency over the squares, clipping); the status bar, the title and the details, dates among them; the folder's order, its limits and its steps; which keys repeat |
| `test_guimedia` | 448 (1,086 before: the kernel viewer's checks went to `test_wview`) | the entry: the program, Files' rule (screenshots' names among them), Apps' start at the Image Viewer's place, `viewer_show()` starting `wview` on a path (spaces and all), a start refused, a folder, a device and a file not there refused before anything starts; from the review, one viewer from Files (a start whose window had not come ended, a window told to close, one that did not go ended), no start when the programs have every window, a start whose window never comes said once, and only Sketch's numbers going to Sketch; Hex and Sketch as before |
| `test_guisuite` | 173 (181 before) | Files' Enter on a picture starts `wview` with its path, at the Image Viewer's place |
| `test_wundertow_image` | 359,823 | stage 1's 359,813, the same outcome check by check after the JPEG decoder's rewrite, and the review's bombs (10): a PNG of 4 MB of empty fixed or dynamic blocks refused for its length in little time, 16 megapixels of RLE8 runs past a row's end within 4 times the same picture in rows, 128 JPEG scans refused at the 33rd within 16 times the same picture in 2 scans |
| `tests/test_image_jpeg.py` | 126 (on the Mac) | every JPEG fixture by libsurf against macOS's sips (above); without sips it says so and passes |
| guiapps, `gui_apps_img.py` | 39 at -O0 on 5453c2bc; the review round's (three pictures from Files, one viewer left) not yet run | in the Terminal: wview on a PNG of the stick, fitted and actual, its toolbar, N through a GIF, a BMP, a JPEG, a PNG with transparency, a PNG cut short and a screenshot's name, around and back, the details, Copy path pasted in the Terminal; then Files' Enter on a PNG (at the Image Viewer's place), O and the desktop's chooser to a JPEG, Ctrl+W, Apps > Image Viewer with no picture |
| guiapps, whole | 358 of 358 at -O0 on 5453c2bc; not yet run on the review round's tree | every track's, the media track's Image Viewer checks and the suite's Files-to-Image-Viewer check now reading wview |

Gates. On 5453c2bc, the end of stage 2: the whole `make unittest` (99 suites passed, none failed) and the whole guiapps
scenario at -O0 (358 of 358). On 3165be0a, the review round's code (the commit after it changes only documents): the
whole `make unittest` again (99 suites, none failed; `test_wundertow_image` 359,823 and its UBSan build the same,
`test_wview` 270, `test_guimedia` 448, `test_guidesk` 139 with `wview.app`), the builds at -O0 and -O2 and both USB
images. Not run when this was written: guiapps at -O0 and -O2, test-all, test-web at -O0 and -O2 and `make -C surfcpu
test-os-qemu`. They wait at the lock (`06-gates/suite-lock.sh`, ticket t.32 since 20:11), whose two places have been
held since 19:55 by other tracks' runs; the results go to the scratchpad's `apps-img/gates/summary` when they come.
Natively on the Mac: the JPEG decoder against sips (126 checks) and against itself before the rewrite, the review's
fuzzer over the new decoders under AddressSanitizer and UBSan (1.8 million inputs, nothing found), `test_wview` and
`test_wundertow_image` under both sanitizers.

### Found on the way (for other tracks)

- **libgui's `gw_button()` takes its label in code page 437, `gw_text()` in UTF-8** (`guikit_button()` is the
  desktop's `gd_button()`, which draws bytes as the console's characters). wview's arrows are therefore "\x11" and
  "\x10"; a trial on the desktop showed "◄" in UTF-8 as three characters. For kit: say so in `gui.h`, or decode.
- **A program that calls `wimg_scale()` links libgcc**, as Wundertow does: `image_scale.c` divides in 64 bits
  (`__divdi3`), and libsurf has no 64-bit division. For Paint, if it scales with `wimg_scale()`.
- Before desk's merge, a trial: wview built into a USB image by a make fragment of this track's own (not committed), and
  the guiapps scenario run with only these checks (a scratch wrapper adding them to `TRACKS`): 26 of 26 at -O0 and at
  -O2. It found the two points above.

## The review

An independent review read stage 2's tip (5453c2bc) against the brief's commit (e5f9a2e0) and desk's merge (ab7b1c33).
It found the decoders memory-safe (about 21 million fuzzed inputs under AddressSanitizer and UBSan: no crash, read past
an end, leak or uninitialised read), the writers' files right, Wundertow's pixels as before, and view.c, folder.c,
main.c and the desktop's entry sound. Its findings, and what was done about each:

- **H1, each open from Files started a new wview over the last.** A few double-clicks used up the 8 windows the
  desktop's programs may have (GUI_CLIENTS, WIN_PER_DESK), after which nothing could open one, wview's "no window"
  going to the console behind the desktop; each hidden viewer held its picture too. Now `viewer_show()` replaces the
  Image Viewer's wview (`viewer_replace()`): one with a window is told to close (it has nothing to save), one that was
  told and did not go, or one started so lately that its window has not come, is ended, and the new one stands in its
  place. With every program's window in use but the viewer's, nothing starts and a message says so. A wview whose
  window has not come 5 seconds after its start is said on the screen: the Image Viewer's entry has a `tick()`, and
  `gui_tick()` (`shell/guicore.c`, one line) now runs the tick of an application that is a program even though its slot
  never opens. Open with's entry still starts others, in the cascade, each in a place of its own. Tested in
  `test_guimedia` (a start before the window came ended, a window told to close, one that did not go ended, the 8
  windows, the late start said once) and on the screen: three pictures opened from Files one after the other, then
  the chooser's, and when that viewer closes no other is left.
- **M1, crafted files froze wview for seconds.** Each is bounded now, and each bomb is in `test_wundertow_image`'s
  `bombs()` with a bound of time measured against a picture of its size (10 checks):
  - PNG: inflate is given no more of the IDAT chunks than twice the rows' bytes, 8 a row and 4 KB (`data_budget()`;
    deflate's worst, stored blocks, adds 5 bytes to 64 KB, an encoder that flushes at each row 6 a row). A stream that
    wants more is "a PNG whose data is far longer than its rows". The review's 32 MB of empty dynamic blocks for a
    1 x 1 picture: 3.0 s natively before the move, 6.0 s after it, 0.001 s now; its empty fixed blocks 0.24 s, now 0.
  - BMP RLE8 and RLE4: a run or a literal draws what fits in the row and drops the rest at once
    (`image_bmp.c`'s `rle()`): the review's 32 MB of runs of 255 past the row's end, 2.3 s, take 0.010 s, less than
    the same picture in rows of runs.
  - JPEG: WIMG_SCANS_MAX is 32, not 128. Each scan goes over every block, so 128 scans of EOB runs over a 4000 x 4000
    picture (35 KB) took 0.64 s; they are refused at the 33rd now, after 0.15 s. Real files have far fewer:
    libjpeg's simple progression, which most progressive files come from, writes 10 scans for colour, 6 for grey, 14
    for three components of RGB and 18 for CMYK, and Photoshop's progressive files have 3 to 5.
  (Times natively on the Mac at -O2, the review's files.)
- **M2, BMPs the desktop used to show were refused.** wview reads a BMP of up to 62 MiB now (BMP_MAX: the decoders'
  16,000,000 pixels at 32 bits, and 1 MiB for its headers), so 4000 x 3000 in 24 bits (36 MB) shows again; other kinds
  keep 32 MiB. The manual says both.
- **L1.** The memory above is in MiB, about 246 to 250 MiB at worst (258 to 262 MB), and says that the heap keeps its
  peak until wview ends.
- **L2.** `test_guimedia` has its check that only Sketch's three numbers go to Sketch again (acts 23 and 19 change
  nothing). The check that the Image Viewer's narrowest window fits a desktop of 640 x 400 is in `test_wview` now,
  since the window and its sizes (VIEW_MIN_W) are wview's: `test_guimedia` is built without the C library's headers
  that `view.h` needs.
- **L3.** EAGAIN from WIN_DIALOG is said as win.h means it: "this window's events wait to be read: try again".
- **L4.** A chooser's answer that is no picture (a file of another kind, a folder, "/") leaves the folder and the place
  of the picture still shown, so N and P step where they did and the status bar keeps "(k of n)". With no picture
  shown yet the folder is listed all the same, so that N and P step from the file.
- **L5.** Wundertow links libgcc after libsurf too, so that libsurf's 64-bit divisions (image_scale.c) are met
  whatever Wundertow's own objects call.
- **L6.** The account says that a double-click reaches wview only while no registry entry lists pictures under
  `types` ("How the desktop reaches it").
- **The JPEG decoder's origin.** Its inverse DCT (lines 87-169 at 5453c2bc) was libjpeg's jidctint.c jpeg_idct_islow:
  the same twelve constants, the z1-z5 and tmp data flow, the shortcut for empty columns, descale shifts of 11 and 18,
  and a comment naming libjpeg's method. Its Huffman tables, bit reader and decoding (lines 30-40, 178-204, 225-244,
  264-294) matched another decoder's (stb_image's stbi__build_huffman, stbi__grow_buffer_unsafe and
  stbi__jpeg_huff_decode): a 9-bit fast table, limits against the buffer's top 16 bits with a sentinel at 17, a delta
  per length, a buffer kept at its top and filled while it held 24 bits or fewer. libsurf ships only code of its own,
  so these were written again from ITU-T T.81 alone, as their own commit:
  - the inverse DCT from A.3.3's definition, separated into the one-dimensional sums down the columns and along the
    rows, with the symmetry of the cosines (out(n) and out(7-n) from the sums over the even and the odd k): a table of
    C(k)/2 cos((2n+1)k pi/16) to 13 bits, 3 bits kept between the passes, and the sums running only as far as the
    block's last row and column with a coefficient that is not 0. No fast factorisation, so no paper's either;
  - the Huffman tables from Annex C (HUFFSIZE and HUFFCODE, C.1 and C.2) and F.2.2.3 (MINCODE, MAXCODE and VALPTR,
    F.15; DECODE, F.16), with a lookahead of my own: two tables of 256 bytes that say the length and value of a code of
    8 bits or fewer that the next 8 bits begin with; a longer code goes on bit by bit as F.16 does from length 9;
  - the reading of the bits from F.1.2.3 (0xFF 0x00 is a data byte) and B.1.1.5 (0xFF and anything else a marker,
    fill bytes 0xFF before it), a byte at a time into an accumulator whose last bits are the ones to come.
  The progressive refinement was not flagged and is as it was; the comments that cite libjpeg's behaviour (the JFIF
  and Adobe rules, inverted CMYK) stay as compatibility notes, and no comment calls code libjpeg's.
  What changed in Wundertow's pictures (and wview's): JPEGs' pixels are within 3 a channel of what they were
  (all 57 fixtures: 0 to 3 apart; a photograph of 4000 x 3000: 3 at most, 0.07 on average), and as close to macOS's
  own decoder as before (`tests/test_image_jpeg.py` against sips: within 3, 0.5 on average, at full chroma, and 8,
  3 on average, where chroma is sampled less: sips smooths chroma as it widens it, libsurf repeats it; 126 checks; the
  files where the two follow different colour rules, CMYK and two of the rule's own fixtures, are left out, and the
  test says why). `test_wundertow_image`'s expectations needed no change: its 359,813 checks give the same outcome one
  by one before and after. Of 19,070 whole, damaged and cut-short fixtures compared before and after, 6 differ only in
  their message: a file cut just before a restart marker is "a JPEG that ends too early", where it was "a JPEG whose data
  ends before a restart marker" (the bits are read a byte at a time now, as they are needed, so the end is met by the
  search for the marker). A 12-megapixel JPEG takes 0.15 s where it took 0.10 (natively, -O2). The review's fuzzer,
  run again over the new decoders under AddressSanitizer and UBSan: 1.8 million inputs, nothing found.
- **The decoders' origin** in "Where the code comes from" was a pointer to Wundertow's milestone brief, which has no
  such section; it says what each decoder follows now.

## Where the code comes from

### Stage 1

- **The decoders** came over from `user/wundertow/` as they were, written in the browser's milestone 2 (11/1/26);
  `2026-11-01-browser-m2-m3.md` is that milestone's brief and says nothing of their origin, so it is said here. The
  GIF, BMP and PNG decoders follow their published specifications (the GIF89a specification, Microsoft's documentation
  of the BMP headers, the PNG standard) and are original as far as the review could tell. The JPEG decoder's inverse
  DCT, its Huffman tables and their decoding, and its reading of the scans' bits followed other programs' code too
  closely (libjpeg's integer inverse DCT, another decoder's tables and bit reader: "The review" says how), and were
  written again in the review round from T.81 itself; the rest of the JPEG decoder (the segments, the scans, the
  progressive refinement, the colours) was not flagged and is as it was. The only new decoding code of stage 1 is the
  PNG decoder's data part, written here from RFC 1950 (the zlib header, Adler-32) and the PNG standard; the inflate
  under it is libsurf's, whose origin the archive track's account (`2026-11-01-cli-suite-archive.md`) states.
- **The BMP encoder:** Microsoft's documentation of BITMAPFILEHEADER and BITMAPINFOHEADER.
- **The PNG encoder:** the PNG standard (ISO/IEC 15948, the W3C's PNG second edition): the signature, the chunk layout and
  CRC, IHDR, the five filters (section 9.2) and the filter heuristic (section 12.8); RFC 1950 for the zlib stream. Deflate
  and the CRC are libsurf's.
- **The tests:** this tree's test allocator and the image test's way of building files; Python's `zlib`, `struct`, and
  macOS's `sips` as readers.

### Stage 2

- **`view.c`'s views**, the zoom steps, Fit, the pan and the double-click: the desktop's Image Viewer in this tree
  (`shell/guiview.c` as it was), moved to the window's own coordinates and to 32-bit division, with its checks
  (`test_guimedia.c`'s viewer test, which `test_wview.c` carries on with the same numbers).
- **The average** where a picture is shown smaller: `image_scale.c`'s rule in this tree (transparency counted in full,
  colour only where there is some). The squares behind transparency: a common way of showing it, drawn here in the
  desktop's colours.
- **The folder's order:** numbers in names compared as numbers, the way macOS's Finder orders names (its behaviour,
  not its code), written here; the merge sort is the textbook's bottom-up one.
- **The date in the details:** the Gregorian calendar's rules, written here.
- **`main.c`:** the shape of `user/wclock/main.c` in this tree (libgui's loop, presenting what changed), and desk's
  calls (`WIN_DIALOG`, `WIN_TAKE`, the modifiers) as `include/surfos/win.h` documents them.
- **The desktop's side** (`shell/guiview.c`'s entry, `viewer_show()`): `guiclock.c`'s entry and desk's `reg_open()` in
  this tree.
- **`wview.app`'s icon:** drawn here, a framed landscape in the registry's palette.
- **The screen checks:** the guiapps Driver and the media, suite and terminal tracks' helpers in this tree; the
  pictures are two made in Python and the browser's test pictures.

### The review round

- **The JPEG decoder's inverse DCT, Huffman tables and bit reader:** ITU-T T.81 alone (A.3.3, Annex C, F.2.2.3,
  F.1.2.3, B.1.1.5), as "The review" says; the factors are the cosines of A.3.3's formula, worked out here.
- **The bounds on bombs:** the PNG budget from RFC 1951's stored blocks (5 bytes of overhead each) and the PNG
  standard's rows; the RLE rule from Microsoft's documentation of BI_RLE8 and BI_RLE4; the number of scans from what
  encoders are known to write (libjpeg's simple progression and Photoshop's choices: their behaviour, not their code).
- **The bombs in the tests:** the review's own designs (its gen_bombs.py: empty deflate blocks, runs past a row, EOB
  runs over many scans), written again in C in `test_wundertow_image.c`.
- **The viewer's replacement and its report:** the desktop's own calls in this tree (`client_close()`, struct
  gui_sys's kill, the launcher's `app_starting`).

No other program's source was used. The one behaviour matched is named above: the folder's order, Finder's (and the
encoders' numbers of scans, which are only a limit's reason).
