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

## Where the code comes from

- **The decoders** came over from `user/wundertow/` as they were; Wundertow's account
  (`2026-11-01-browser-m2-m3.md`) says what they were written from. The only new decoding code is the PNG decoder's data
  part, written here from RFC 1950 (the zlib header, Adler-32) and the PNG standard; the inflate under it is libsurf's,
  whose origin the archive track's account (`2026-11-01-cli-suite-archive.md`) states.
- **The BMP encoder:** Microsoft's documentation of BITMAPFILEHEADER and BITMAPINFOHEADER.
- **The PNG encoder:** the PNG standard (ISO/IEC 15948, the W3C's PNG second edition): the signature, the chunk layout and
  CRC, IHDR, the five filters (section 9.2) and the filter heuristic (section 12.8); RFC 1950 for the zlib stream. Deflate
  and the CRC are libsurf's.
- **The tests:** this tree's test allocator and the image test's way of building files; Python's `zlib`, `struct`, and
  macOS's `sips` as readers.
- No other program's source was used, and no program's behaviour was matched.
