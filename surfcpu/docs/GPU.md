# SurfGPU: VRAM, graphics scan-out and a drawing engine

A basic GPU for the SurfCPU SoC on the Arty A7-100T, asked for by the owner ("build a basic GPU
in another part of the chip"). It has three parts, all in block RAM and fabric, with no DDR
involved:

```
                     sys_clk (25 MHz)                                  pix_clk (25.17 MHz)
 SurfCPU dbus --> D decoder --> vram window (0x3000_0000) --+
                            --> gpu registers (0x1000_9000) |
                                   |                        |
                            surf_gpu (command FIFO,         |
                            engine) -- VRAM master --> surf_vram port A        port B <-- surf_video
                                   ^   (lower priority)     (CPU first)   (32-bit read)   scan-out:
                                   |                                                   GPAL, overlay,
                            video vblank pulse                                         FRAME_CRC --> VGA
```

* **surf_vram**: 300 KiB (307,200 bytes) of true dual-port block RAM. Port A (sys_clk, 32 bits,
  byte enables) is shared by the CPU's window and the engine, the CPU always first; port B
  (pix_clk, 32-bit reads) feeds the scan-out.
* **surf_video** (the existing block, with `HAS_GFX = 1`): CTRL.MODE 2 shows VRAM pixels through
  a 256-entry RGB444 palette (GPAL), MODE 3 the same with the text layer over it; GFX_BASE,
  GFX_STRIDE and GFX_SCALE place the picture; FRAME_CRC is a CRC-32 of every frame as it left
  the pins, in every mode, so the picture can be checked exactly without a camera.
* **surf_gpu**: a command FIFO and an engine that draws on VRAM four 8-bit pixels per access:
  SET_TARGET, SET_CLIP, FILL_RECT, COPY_RECT, LINE, EXPAND, WRITE_RECT, FENCE, WAIT_VBLANK, and
  since GPU2 (G4, section 16) SET_ZBUF, TRIANGLE (flat or Gouraud, with a 16-bit depth buffer in
  VRAM) and FLIP (video GFX_BASE written at the next vertical blanking).

The GPU exists only in designs built with it (`HAS_GPU = 1`: sysinfo.FEATURES.GPU). Every other
design is unchanged: without it the vram and gpu windows are unmapped, MODE 2 and 3 are black
and the new video registers answer with errors, as before. In this version the GPU is a SurfBus
slave only (SURFBUS.md is unchanged).

**Status (2026-10-18).** G0 (this document and the contract changes) is done. G1 (VRAM, the
graphics scan-out, GPAL, FRAME_CRC, the CPU window, FEATURES.GPU, gpudiag's CPU-drawn scenes) and
G2 (the engine) are done in simulation and in synthesis: the gpudiag design passes every gate of
the open flow and its netlist runs gpudiag to PASS (section 12). Four independent reviews'
findings are fixed (sections 12.4 to 12.7), and the bitstreams to test (gpudiag of section 12.7,
DDR+GPU of section 12.6) can be rebuilt to their sha256 (section 14). Their physical tests
(section 14) are open: nothing has run on a board.
Section 11 has the milestones.

**G4 (2026-10-25, section 16).** GPU2 (gpu.ID "GPU2", soc_version 0.4.0) adds SET_ZBUF, TRIANGLE
(flat and Gouraud, exact coverage with the top-left rule, a 16-bit depth buffer in VRAM) and FLIP
(GFX_BASE at the next vertical blanking) to the engine, with the C model, its tests, the RTL tests
against it, mutants, and gpudiag's GPU2 checks, scenes and spinning cube: done in simulation, and
in synthesis as section 16.11 records: the gpudiag bitstream passed every gate of the open flow,
while the DDR+GPU bitstream's LVS proof is outstanding. Its physical test (section 14: gpudiag
with the cube) is open; SurfOS's boot stays on G2's DDR+GPU bitstream until G4's has its proof.

## 1. Decisions and where this deviates from the brief

The architecture was decided by the integrator (the SurfCPU GPU brief of 2026-10-17); this
section records the refinements and the deviations.

| Point | Decided | Done | Why |
|---|---|---|---|
| VRAM size, ports | 300 KiB TDP BRAM, A 32-bit sys (CPU first), B 32-bit read pix; about 75 RAMB36 | as decided; 5 banks of 64 KiB (the last 44 KiB): 76 RAMB36; CPU and engine answer one cycle after the access, the pixel port two pix_clk edges after the address | one-cycle answers keep SurfBus rule 7 ("block RAM answers in 1 cycle"); the pixel port has a register after the bank multiplexer, where timing is free; 64 KiB banks (16Kx2 block RAMs) cost one RAMB36 more than 16 KiB ones but route: section 2 |
| CPU window | 0x3000_0000, decodes 512 KiB, error above 300 KiB, byte enables, no fetch | as decided | |
| Engine on port A | CPU priority, bounded short CPU latency | the CPU's request takes port A in the cycle it is offered; the engine waits that cycle | the CPU never waits, so its latency is exactly the scratch RAM's |
| Graphics modes | MODE 2 graphics, MODE 3 graphics + text | as decided, with the proposed overlay rule | |
| GFX_BASE, GFX_STRIDE | byte offset; stride | both must be multiples of 4 (bits 1:0 read 0) | the scan-out reads aligned words; an unaligned base would need a byte rotator for no use |
| GFX_SCALE | 1 or 2 | 2 doubles; 0 and 3 act as 1 | every value has a defined meaning |
| GPAL | 256 x RGB444 | block RAM (a RAMB18): RGB332 when the FPGA is configured, not changed by a reset | a 3,072-bit register file would cost about 3,000 flip-flops; the text palette keeps its reset |
| FRAME_CRC | CRC-32 of the composited visible pixels | zlib's crc32 over each pixel as a little-endian 16-bit word, plus FRAME_COUNT (frames counted with the CRC, crossing with it) | FRAME_COUNT tells software which frame a CRC belongs to, and FRAME_COUNT then FRAME_CRC is a consistent pair (section 3) |
| Commands | the nine of the brief | as decided, plus NOP (opcode 0) | a FIFO of zero words drains harmlessly |
| Errors | overflow, unknown opcode, bad parameters | as decided, plus CTRL.ABORT | a command waiting for data words that never come could otherwise only be ended by a reset |
| IRQ | gpu = 5 | as decided | |
| Designs | gpudiag (no DDR) and ddr_gpu, 100T only | as decided; a `board` line in the design list restricts a design to the boards it names, and tools/fpga-build.sh refuses others | the 35T's 50 RAMB36 cannot hold 75 for VRAM |
| soc.toml | additions keep map_version, soc_version 0.3.0 | as decided; MODE 2..3 change from "reserved (black)" to the graphics modes: a reserved value given a meaning, an addition | |

## 2. VRAM (`rtl/soc/surf_vram.v`)

**Size and layout.** 307,200 bytes = 76,800 32-bit words, byte addresses 0 .. 0x4AFFF. It holds
one 640x480 picture at 8 bits per pixel, or any other layout software chooses through GFX_BASE
and GFX_STRIDE (two 320x240 pictures take 150 KiB, leaving 150 KiB for a later z-buffer or
sprites). Little-endian: the pixel at byte address a is byte a mod 4 of word a / 4, so pixel x
of a line is in bits 8(x mod 4)+7 .. 8(x mod 4) of its word.

**Banks.** Five banks of 16,384 words (64 KiB), the last one 11,264 words (44 KiB); word
address bits 16:14 choose the bank and 13:0 the word in it (`BANK_LOG2` = 14). Each bank is
written like `surf_textram` (its proven dual-clock form): port A in sys_clk with byte write
enables, reading only when it does not write (the block RAM's NO_CHANGE mode: READ_FIRST is
unsafe with the other port on another clock, UG473), port B in pix_clk, read only. A pixel-port
read of a word at the instant port A writes it is a collision, whose read data UG473 leaves
undefined: those four pixels may show any colour in that one frame (not just the old or the new
value); the word written is kept, memory is never corrupted. Yosys maps each full bank to 16 RAMB36 of 16Kx2, as it maps the 64 KiB scratch RAM,
and the last to 12: 76 RAMB36, with a 5-input multiplexer per bit and port. (The first version,
19 banks of 4,096 words, took 75 RAMB36 but had 19-input multiplexers that gathered 672 block RAM
outputs per port in one place; nextpnr could not route that design in reasonable time, section
12.2.)

**Port A** (sys_clk) has two users:

| User | Interface | Rules |
|---|---|---|
| CPU window | SurfBus slave (`s_*`), `req_ready` always 1 | offset = `s_req_addr[18:0]`; an offset at or above 307,200 or a fetch is answered with `rsp_err` and does nothing; reads return the whole word, writes honour `req_be`; the answer comes in the next cycle |
| GPU engine | `g_req`, `g_we`, `g_be[3:0]`, `g_addr[16:0]` (word), `g_wdata`; `g_gnt`; `g_rvalid`, `g_rdata` | `g_gnt = g_req && !s_req_valid`: granted when the CPU does not offer a request in that cycle. A granted read's word comes with `g_rvalid` in the next cycle. A word address at or above 76,800 reads 0 and writes nothing |

**Port B** (pix_clk): `pix_addr[16:0]` (word), `pix_data` = that word two pix_clk edges later
(edge 1: the block RAM's read; edge 2: the bank multiplexer's register); words at or above 76,800
read 0.

## 3. Graphics scan-out (in `rtl/peripherals/surf_video.v`)

`surf_video` gains a parameter `HAS_GFX` (default 0). With 0 it is the block of IO.md section
3, unchanged: MODE 2 and 3 black, the new registers absent. With 1:

**Modes** (CTRL.MODE): 0 the test pattern and 1 text, exactly as before; 2 graphics; 3 graphics
with the text layer over it. In mode 3, per pixel: a set glyph pixel (or the cursor) shows the
text foreground colour; otherwise a cell whose background index is not 0 shows the text
background colour; otherwise the graphics pixel shows. So text with background 0 floats over the
picture, and a background colour makes an opaque box. ENABLE = 0 is black in every mode.

**Where a pixel comes from.** For screen pixel (x, y), with S = 2 when GFX_SCALE = 2 and S = 1
otherwise, the VRAM byte address is

```
    a = (GFX_BASE + (y / S) * GFX_STRIDE + x / S) mod 2^19
```

and its value v (0..255) shows as GPAL[v]; addresses at or above 307,200 give v = 0. The scan-out
keeps a line address that starts at GFX_BASE and adds GFX_STRIDE after every line (after every
second line with S = 2), so no multiplier is needed.

**Frame latch.** GFX_BASE, GFX_STRIDE and GFX_SCALE cross from sys_clk in their own
`surf_cdc_word` and are taken at the same point as CTRL, CURSOR and SCROLL (which cross together
in another): the start of the last blanking line (line 524). Each frame is drawn with one value of
each register, so a page flip (GFX_BASE alone) never tears: the new base shows from the next frame
whose latch it reaches. Values written by separate stores are separate values, though: each
reaches the pixel domain on its own, within a microsecond of its store, and a frame takes what has
arrived by its latch. The exact rule: **registers written one after the other change the same
frame when their stores come between STATUS.VBLANK_EV (the start of blanking, line 480) and
1.3 ms after it**; the latch comes 1.4 ms after VBLANK_EV (44 lines). Stores on either side of a
latch split: that frame shows, say, the new mode with the old placement, and the next frame both.
`test-gpu-video` checks both halves of the rule. This is documented rather than changed (the first
GPU review's finding 4): one crossing for CTRL and the GFX_ registers would not make separate
stores one either, since a crossing can start between two of them; only a commit register would
(writes kept until software commits them, then taken at one latch), which G3 can add if the driver
needs to change mode and placement mid-picture. VRAM itself is not latched: what is written while
a frame is shown shows in that frame from that point down.

**GPAL.** 256 entries of RGB444 at video offset 0x400 + 4n, in a dual-clock block RAM like the
text RAM: port A for the bus (read and write), port B for the scan-out. Writes apply as they
arrive (as the text palette does). Its initial contents (configuration, and simulation time 0)
are RGB332: R = n[7:5], G = n[4:2], B = n[1:0], widened to 4 bits by repeating their top bits
(R4 = {R, R[2]}, G4 = {G, G[2]}, B4 = {B, B}); entry 0 is black. A reset does not change it.

**Pipeline.** The text and pattern path keeps its stages S0..S4 (IO.md section 3). The graphics
path starts from a second pair of counters running one pixel clock ahead of hcnt and vcnt:

| Stage | Graphics work |
|---|---|
| G0 (S-1) | line address + x / S: VRAM word address and byte lane |
| G1 (S0) | `vram_addr` registered (port B address) |
| G2 (S1) | the block RAM reads |
| G3 (S2) | `vram_data` (the bank multiplexer's register): byte lane -> GPAL address |
| G4 (S3) | GPAL's registered output: the graphics colour, next to the text colour of S3 |

so the composited colour is chosen in S3 and registered to the pins in S4 as before, and the
syncs keep their four stages.

**FRAME_CRC** (in every mode). In S3, every visible pixel's colour p = R << 8 | G << 4 | B, the
value registered to the pins, goes into a CRC-32 as two bytes, low byte first: (G << 4 | B),
then R. The CRC is CRC-32/ISO-HDLC (zlib's `crc32`: reflected polynomial 0xEDB88320, initial
value 0xFFFFFFFF, final XOR 0xFFFFFFFF), restarted at pixel (0, 0) and finished after pixel
(639, 479). So, in Python, with the frame as a 480x640 array of RGB444 values:

```
    FRAME_CRC == zlib.crc32(numpy.asarray(frame, dtype='<u2').tobytes())
```

At the finish FRAME_COUNT goes up by one, and both cross to sys_clk together in one
`surf_cdc_word`. They change about 160 pixel clocks before the frame's vertical blanking starts
(when STATUS.VBLANK_EV sets), so a FRAME_CRC read after VBLANK_EV belongs to the frame that just
ended. Software reads the two with two loads, and a frame can end between them, so a read of
FRAME_COUNT keeps the CRC of the frame it counts, and the next read of FRAME_CRC returns that
copy, however many frames have passed since; a FRAME_CRC read with no FRAME_COUNT read since the
last FRAME_CRC read returns the newest CRC. **FRAME_COUNT, then FRAME_CRC, is a consistent pair**
(the first GPU review's finding 3: without the copy, count N could come with CRC N+1, which G4's
frame-by-frame tearing check could not live with).

**Checking a still picture** (the diagnostic's method, section 10). After the last VRAM or
register change, read c0 = FRAME_COUNT, then wait until FRAME_COUNT - c0 >= 2 and read FRAME_CRC:
the frame the last FRAME_COUNT read counted started after the change, whatever moment within a
frame it came at, so its CRC is that of the final picture. Two frames at 59.94 Hz: at most 33.4 ms.

## 4. Registers

The tables are in [MEMORY-MAP.md](MEMORY-MAP.md) (from spec/soc.toml); this section explains
them.

### 4.1 video (new registers, only with FEATURES.GPU)

| Offset | Register | Meaning |
|---|---|---|
| 0x080 | GFX_BASE | VRAM byte offset of the top-left pixel; bits 18:2 (multiple of 4); reset 0 |
| 0x084 | GFX_STRIDE | bytes per VRAM line; bits 15:2 (multiple of 4); reset 640 |
| 0x088 | GFX_SCALE | 1 = 640x480, 2 = 320x240 doubled, 0 and 3 act as 1; reset 1 |
| 0x08C | FRAME_CRC (rc) | CRC-32 of the last complete frame; after a FRAME_COUNT read, of the frame it counted, and the read drops that copy (section 3) |
| 0x090 | FRAME_COUNT (rc) | frames whose CRC was taken since the pixel domain's reset (16 bits, wraps); a read keeps that frame's CRC for the next FRAME_CRC read |
| 0x400..0x7FC | GPAL | graphics palette, RGB444 |

The two FRAME registers are `rc` in spec/soc.toml (reading has a side effect, which the register
gives: keeping, or dropping, the copy of a CRC); the others in this table are `rw`.

CTRL.MODE 2 and 3 are the graphics modes. Reads of GPAL take the same single cycle as every other
register (the block RAM's registered output is the answer). A mode change and its placement are
written as separate registers: to have them change the same frame, write them after
STATUS.VBLANK_EV and within 1.3 ms of it (section 3, the frame latch).

### 4.2 gpu at 0x1000_9000

| Offset | Register | Meaning |
|---|---|---|
| 0x000 | ID | 0x32555047 ("GPU2"): this command set, GPU1's (0x31555047) and section 16's |
| 0x004 | STATUS | BUSY (0); sticky OVERFLOW (1), BAD_OPCODE (2), BAD_PARAM (3), FENCE_EV (4); write 1 to clear a sticky bit |
| 0x008 | IE | FENCE (0): interrupt while FENCE_EV; ERR (1): interrupt while any sticky error |
| 0x00C | CTRL | ABORT (0): write 1 to empty the FIFO and stop the running command |
| 0x010 | CMD | write: the next command word into the FIFO (dropped with OVERFLOW when full) |
| 0x014 | FIFO_FREE | free FIFO words |
| 0x018 | FENCE | value of the last completed FENCE (reset 0) |
| 0x01C | INFO | FIFO_DEPTH (15:0) = 64, VRAM_KIB (31:16) = 300 |

Register rules as for every peripheral (SOC.md section 4): word accesses only, one-cycle
answers, errors for other offsets, partial words and fetches, without effect. A CMD write never
stalls the bus: software checks FIFO_FREE (the FIFO frees a word per cycle while the engine reads
parameters or data). BUSY is 0 only when the FIFO is empty and the engine idle, and then every
write of every command is in VRAM.

The interrupt (intc source 5, gpu) is `FENCE_EV & IE.FENCE | (OVERFLOW | BAD_OPCODE | BAD_PARAM) & IE.ERR`.

## 5. Commands

A command is a header word followed by its parameter words, then, for EXPAND and WRITE_RECT, its
data words, all written to CMD in order. Header bits 31:24 are the opcode; header bits not listed
are ignored. Coordinates x, y are signed 16-bit (bits 15:0 and 31:16 of a word: `x | y << 16`),
sizes w, h unsigned 16-bit (`w | h << 16`). `firmware/include/surfcpu/gpu.h` has the encodings for
C; `rtl/peripherals/surf_gpu.v` and `sim/models/gpu_model.c` use the same values, and a test
compares them.

| Op | Command | Header bits | Parameter words | Data words |
|---|---|---|---|---|
| 0x00 | NOP | | 0 | |
| 0x01 | SET_TARGET | | base; stride; w \| h << 16 | |
| 0x02 | SET_CLIP | | x0 \| y0 << 16; x1 \| y1 << 16 (unsigned) | |
| 0x03 | FILL_RECT | 7:0 colour | x \| y << 16; w \| h << 16 | |
| 0x04 | COPY_RECT | 7:0 key, 8 KEY | sx \| sy << 16; dx \| dy << 16; w \| h << 16 | |
| 0x05 | LINE | 7:0 colour | x0 \| y0 << 16; x1 \| y1 << 16 | |
| 0x06 | EXPAND | 7:0 fg, 15:8 bg, 16 TRANSPARENT | x \| y << 16; w \| h << 16 | ceil(w h / 32) |
| 0x07 | WRITE_RECT | | x \| y << 16; w \| h << 16 | ceil(w h / 4) |
| 0x08 | FENCE | 0 IRQ | value | |
| 0x09 | WAIT_VBLANK | | 0 | |
| 0x0A | SET_ZBUF (GPU2, section 16) | | base; stride | |
| 0x0B | TRIANGLE (GPU2, section 16) | 7:0 colour, 8 GOURAUD, 9 ZTEST, 10 ZFUNC, 11 ZWRITE, 12 CULL_CW, 13 CULL_CCW | 3 x (`X \| Y << 16` in 12.4; `Z \| C << 16`) | |
| 0x0C | FLIP (GPU2, section 16) | | base | |

Any other opcode sets STATUS.BAD_OPCODE; that word is dropped and the next word is read as a
command header (so after an error software should ABORT and start again).

**The target** is a rectangle of pixels in VRAM: pixel (x, y), 0 <= x < W, 0 <= y < H, is the
byte at base + y * stride + x. SET_TARGET sets base, stride, W and H and the clip to (0, 0, W, H).
It is refused (BAD_PARAM, nothing changes) unless: base and stride are multiples of 4, base <
307,200, stride < 65,536, W and H < 32,768, and either W = 0, H = 0, or (W <= stride or H = 1) and
base + (H - 1) * stride + W <= 307,200. So every pixel of a target is in VRAM and two lines never
share a word. Reset: base 0, stride 640, 640 x 480, and the clip (0, 0, 640, 480), as SET_TARGET
would set them (gpudiag's first run after a reset and test_gpu check them: section 10).

**The clip** is [x0, x1) x [y0, y1), unsigned; the effective clip is it intersected with the
target, [x0, min(x1, W)) x [y0, min(y1, H)). A command writes only pixels inside the effective
clip; everything else of it is dropped, never wrapped. An empty clip (x0 >= x1 ...) draws nothing.

**FILL_RECT** writes the colour to every pixel of [x, x + w) x [y, y + h) inside the clip.

**COPY_RECT** copies the w x h rectangle at (sx, sy) to (dx, dy) in the target. Destination pixel
(px, py) of [dx, dx + w) x [dy, dy + h) is written when it is inside the clip and its source
(px + sx - dx, py + sy - dy) is inside the target, with the source's value; with KEY, source
pixels equal to the key colour are not copied. The result is as if the whole source were read
before anything is written, for every overlap: rows run bottom-up when dy > sy, and within a row
right to left when dy = sy and dx > sx.

**LINE** draws from (x0, y0) to (x1, y1), both endpoints included, with this algorithm (the
model's and the engine's), each pixel written only if inside the clip:

```
    dx = |x1 - x0|;  dy = -|y1 - y0|;  sx = x0 < x1 ? 1 : -1;  sy = y0 < y1 ? 1 : -1;  err = dx + dy
    loop:  plot(x0, y0)
           if x0 == x1 and y0 == y1: stop
           e2 = 2 * err
           if e2 >= dy: err += dy; x0 += sx
           if e2 <= dx: err += dx; y0 += sy
```

It plots max(|x1 - x0|, |y1 - y0|) + 1 pixels; a line from B to A may differ from A to B in its
middle pixels.

**EXPAND** draws a 1-bit-per-pixel bitmap of w x h: bit set = fg, clear = bg (or nothing, with
TRANSPARENT). The bitmap is w h bits in raster order, row 0 first, packed 32 to a word, the first
pixel in bit 31 (MSB first, so a font row byte goes in as it is: an 8 x 16 glyph's 16 row bytes
are four words, `row0 << 24 | row1 << 16 | row2 << 8 | row3`, ...). Rows are not padded; the last
word's unused low bits are ignored. Every data word is consumed even when the pixels it holds
are clipped.

**WRITE_RECT** writes w x h pixels from the data words, in raster order, four to a word, the
first in bits 7:0 (VRAM's own byte order); rows are not padded. Clipped pixels are consumed and
dropped.

**FENCE** sets the FENCE register to its value once every earlier command is done (their writes
are in VRAM); with IRQ it also sets STATUS.FENCE_EV.

**WAIT_VBLANK** holds the commands after it until the start of the next vertical blanking
interval (the event that sets video STATUS.VBLANK_EV).

Commands run in order, one at a time; a command's writes are in VRAM before the next command
starts. ABORT stops the running command between two accesses. EXPAND and WRITE_RECT write a
destination word (four pixels) only once they have every one of its pixels, so after an ABORT up
to three pixels taken from the last data words may not have been drawn.

## 6. The engine (`rtl/peripherals/surf_gpu.v`)

A 64-word command FIFO (`surf_fifo`, distributed RAM), a sequencer that takes the header and the
parameter words, a 16-cycle shift-add multiplier (no DSP) for the row addresses (y * stride) and
SET_TARGET's bound, and these walkers:

* **Rectangles** (FILL_RECT, EXPAND, WRITE_RECT, COPY_RECT): row by row, one destination word
  (four pixel lanes) per step, with a byte-enable mask for the lanes inside the rectangle and the
  clip. FILL_RECT and COPY_RECT walk only the clipped rectangle; EXPAND and WRITE_RECT walk the
  whole one, so that their data stream is consumed exactly, and mask the clipped lanes (a word
  with no lane to write is not written).
* **EXPAND / WRITE_RECT data**: a 64-bit buffer of stream bits or bytes; a FIFO word is taken
  only when the next destination word needs a bit or byte the buffer does not have, so the
  engine never reads past the command's data.
* **COPY_RECT**: a two-word window on the source row (lo, hi) and a byte funnel: destination
  lane L takes byte L + s of {hi, lo}, s = (sx - dx) mod 4. Each step reads one new source word
  and writes one destination word, in the direction section 5 gives; with a key, a lane whose
  source byte equals the key is masked.
* **LINE**: one pixel per step, the algorithm of section 5, with the row address kept by adding
  or subtracting the stride.

Throughput at sys_clk with port A free: FILL_RECT, EXPAND and WRITE_RECT one destination word
(four pixels) per cycle, plus a cycle per 32 bits (EXPAND) or 4 words (WRITE_RECT) of data
taken; COPY_RECT two cycles per destination word; LINE one pixel per cycle; a few cycles per row
and about 20 to start a command. A 640 x 480 FILL_RECT takes about 78,000 cycles (3.1 ms). The
CPU's VRAM accesses take port A first, so a CPU copying through the window slows the engine; the
CPU itself is never slowed.

## 7. The SoC

`surfcpu_soc` gains a parameter `HAS_GPU` (default 0). With 1 the D decoder maps the gpu and vram
regions to `surf_gpu` and `surf_vram` (D targets `SURF_DT_GPU` and `SURF_DT_VRAM`), `surf_video`
gets `HAS_GFX = 1` and VRAM's pixel port, the GPU's interrupt is intc source 5, and
sysinfo.FEATURES has GPU (bit 8), and the engine's FLIP writes `surf_video`'s GFX_BASE through
`gfx_base_we` / `gfx_base_in` (GPU2, section 16.7). With 0 both regions go to the decoder's error
slave and nothing of the GPU is built. The I decoder never maps them (no fetch). The board top takes
`HAS_GPU` from the define `ARTY_HAS_GPU`, as it takes `HAS_DDR` from `ARTY_HAS_DDR`.

## 8. Clocks, crossings and timing

| Signal | Crossing | Block |
|---|---|---|
| VRAM | port A sys_clk, port B pix_clk | dual-clock block RAM (no logic crosses) |
| GPAL | port A sys_clk, port B pix_clk | dual-clock block RAM |
| GFX_BASE, GFX_STRIDE, GFX_SCALE (32 bits) | sys -> pix | `surf_cdc_word` (its constraints: BOARD.md) |
| FRAME_CRC, FRAME_COUNT (48 bits) | pix -> sys | `surf_cdc_word` |
| vertical blanking start (to the engine's WAIT_VBLANK and FLIP) | pix -> sys | the existing `surf_pulse_sync` |
| FLIP's GFX_BASE (GPU2) | none: a sys_clk port into the register a CPU store writes, which then crosses with GFX_STRIDE and GFX_SCALE | `surf_cdc_word`, as above |

Every new crossing uses rtl/lib blocks whose scoped constraints the designs already apply, or a
dual-clock block RAM, which has no timing path between its ports (BOARD.md section 4). The
longest new paths are the CPU's VRAM read (block RAM, the 5-way bank multiplexer, the response
multiplexer, the core's load path, in one 40 ns sys_clk cycle) and the engine's address and mask
arithmetic (section 12.2 has the critical paths).

## 9. Resources (estimates before synthesis; section 12 has the results)

| Part | LUT | FF | RAMB36 | DSP |
|---|---|---|---|---|
| surf_vram | ~320 (bank multiplexers; Yosys) | ~80 | 76 | 0 |
| graphics scan-out, GPAL, FRAME_CRC | ~400 | ~250 | 0.5 | 0 |
| surf_gpu | ~2,500 | ~900 | 0 (FIFO in LUTRAM) | 0 |
| total added | ~3,200 | ~1,250 | ~76 | 0 |
| the DDR design with it | ~10,900 / 63,400 | ~6,500 / 126,800 | ~112 / 135 | 4 / 240 |

Block RAM is the scarce resource: about 83 % of the 100T's after this. The 35T (50 RAMB36) cannot
hold VRAM, so the GPU designs are for the 100T only.

## 10. The diagnostic and the designs

**Designs** (boards/arty_a7/designs/, 100T only, not in release/release.txt yet):

* `surfcpu_arty_a7_gpudiag`: the SoC with `ARTY_HAS_GPU`, no DDR, and `gpudiag` in the boot
  ROM. Quick to build and to test on the board.
* `surfcpu_arty_a7_ddr_gpu`: the DDR boot design (`ARTY_HAS_DDR`, SurfBoot) plus `ARTY_HAS_GPU`.

**gpudiag** (`firmware/diagnostics/gpudiag.c`, a boot ROM image like bramdiag): checks the
identity and FEATURES.GPU, the VRAM window (March C- over 300 KiB through the window, byte lanes,
the error above 300 KiB, no fetch), the video registers and GPAL, the engine's registers and
errors, GPU2's commands (section 16: a refused SET_ZBUF, depth without a depth buffer refused with
nothing drawn where it would draw, SET_TARGET dropping the depth buffer with no depth written where
it was, FLIP waiting for the blanking), then draws a list of scenes
(`firmware/diagnostics/gpuscenes.c`) and, for each, prints the FRAME_CRC of a frame that started
after the scene was complete and compares it with the value the C model computed for that scene at
build time (`build/fw/gen/gpuscenes_expect.h`), and then the cube's 16 frames at full rate with
every frame shown checked (section 16.9). It ends with `GPUDIAG: PASS (147 checks)` or `GPUDIAG:
FAIL (...)`; after a PASS the cube spins on under a result bar with a line a second, after a FAIL
the last scene shows with a red result bar; either way it echoes the UART, and Ctrl-R runs it
again. A rerun cannot see the GFX_ values a reset leaves (the run before changed them): it says so
in a line under its banner ("a rerun (Ctrl-R): ...") and makes 146 checks, passing none it does not
make. PASS means every check of the run's kind ran and passed: a missing FEATURES.GPU, a gpu window
that traps or reads another ID than "GPU2", each engine scene that could not run, and a run with
another number of checks than a complete one of its kind (GPUDIAG_CHECKS 147, GPUDIAG_RERUN_CHECKS
146) all fail it. (GPU1's gpudiag made 124 and 123; GPU2's at bfaac48, the gpudiag bitstream of
section 14, makes 146 and 145, before the review of 3fea963 added a check: section 16.12; section
14.1 has the scenes.)
`sim/gpu/gpudiag_check.py` judges a capture (sections 12.4 to 12.8): a run that ended with PASS and a passing CRC line for
every scene is whole and judged (with the total of its kind); a run seen only in part (the console
opened late, a line lost or garbled, a RESET before its end) is cut short and not judged, but
whatever it shows of a failure counts, a FAIL line or a scene's CRC other than the model's, and so
does a line showing a failure outside any run; the capture must end with a whole run (it may end
inside the echo line printed after it), and a capture of reruns only is not a full board test (the
reset values were never checked: section 14). With `--build-id`, or `--manifest` (the manifest of
the gpudiag bitstream tested, which must be a gpudiag build's that passed its checks), no run may
show another build and one whole run at least must show this one (a run whose identity lines were
garbled shows none); only the manifest says whether the build was clean, so only with it does a
dirty banner or bitstream line fail. The first run after a reset checks the engine's reset target
and clip themselves, wherever the engine shows them: after the refused target, a fill at (0, 0) (the
base, the clip's left and top edges), an 8x2 fill at (636, 478) (the stride, the right edge at 640
exactly, the bottom edge at 480 or past: rows past 479 are past VRAM's end) and an 8x2 copy from
(636, 479) to (0, 4), whose source runs past the target's right and bottom edges (its width and
height 640 and 480, not more). A clip edge past the target changes nothing a program can see, since
the engine clips to both. test_gpu draws the same before any SET_TARGET. A rerun first sets the
target and clip, since ABORT keeps whatever an abandoned run left.

**What gpudiag checks of the engine, and what it does not.** On the board it checks: gpu.ID and
INFO; idle state; a full FIFO behind a WAIT_VBLANK, OVERFLOW, and the drain at the next blanking; an
unknown opcode and a refused SET_TARGET (sticky, and the refused target not taken); the target and
clip a reset leaves (the first run); the error interrupt (through intc with IE.ERR only); a fence
and its interrupt (IE.FENCE), cleared; ABORT (the FIFO empty and the engine idle at once, nothing
queued running later); BUSY held while a WRITE_RECT waits for its last data word; the FRAME pair
(sections 3, 4.1); and through the scenes' CRCs FILL_RECT at every alignment and edge with a clip,
LINE in every octant, overlapping COPY_RECT in eight directions with the key, EXPAND opaque and
transparent, WRITE_RECT, WAIT_VBLANK with a page flip, 320x240 doubling, the overlay. It does not
check, and only the RTL tests do: OVERFLOW through the error interrupt; BAD_PARAM from SET_CLIP or a
drawing command; ABORT in the middle of a drawing command (gpudiag aborts behind a WAIT_VBLANK);
COPY_RECT from sources left of or above the target (its first run copies from past the right and
bottom edges); random command streams against the model; the engine waiting behind the CPU on VRAM's
port A; BUSY at cycle precision (it can only see BUSY while the engine waits for data); GFX_
placements that wrap at 512 KiB, stride 0 and the scale values 0 and 3; GPAL written in mid-frame; a
reset of the pixel domain alone. The scenes are written once and run in three places: on the CPU
(writing the gpu, video and vram registers), in the host model (the expected CRCs), and in the RTL
tests.

## 11. Milestones

Each milestone lists its simulation, synthesis and physical tests. The physical tests are the
integrator's, run with the owner; a milestone is not done while its physical test is open.

| Milestone | Work | Simulation | Synthesis | Physical (integrator, owner) |
|---|---|---|---|---|
| G0 | this document, spec/soc.toml (soc_version 0.3.0), make gen, MODULES.md, SOC.md | `make check-generated` | | |
| G1 | surf_vram, the graphics scan-out, GPAL, FRAME_CRC, the CPU window, FEATURES.GPU, the C display model, gpudiag's window and CPU-drawn scenes, the gpudiag design | VRAM unit test with random CPU and engine traffic; surf_video's graphics modes captured and compared with the model, FRAME_CRC equal to the model's and to the captured pins' CRC; the old video tests unchanged; gpudiag on the RTL SoC (surfsim) prints the model's CRCs | gpudiag through the open flow, every gate; its netlist runs gpudiag as the RTL does | load the gpudiag bitstream: its UART transcript ends in PASS with the expected CRCs; the monitor shows the scenes (the owner looks) |
| G2 | surf_gpu, its C model, the engine's scenes in gpudiag, both designs | directed tests of every command and edge case, randomized command streams compared with the model at every FENCE, mutants caught; gpudiag's engine scenes in surfsim | gpudiag and ddr_gpu through the open flow, every gate; dram_clk's margin recorded | gpudiag PASS with the engine scenes; ddr_gpu boots SurfOS from the card as the DDR design does |
| G3 | a SurfOS driver (boards/arty_a7) with shell commands (`gfx mode`, `gfx fill`, `gfx line`, `gfx text`, `gfx crc`) and a demo | the kernel's driver against the RTL SoC with HAS_GPU (surfsim), its commands' FRAME_CRCs against the model | ddr_gpu with the driver's kernel | the demo on the monitor; `gfx crc` equal to the model's |
| G4 | GPU2 (section 16, 2026-10-25): SET_ZBUF (a 16-bit z-buffer in VRAM), TRIANGLE (flat and Gouraud in the palette, exact coverage with the top-left rule, depth tests), FLIP at vertical blanking, the 320x240 double-buffered layout; gpudiag's GPU2 checks and scenes and a spinning cube (no 16-bit colour format: the G4 brief did not ask for one) | done (16.10): the model against hand-worked cases and properties, the RTL against the model at every FENCE (seeds 1 to 3, random streams, ABORT inside TRIANGLE, the CPU on port A), FLIP against the frame latch, mutants; gpudiag on the RTL SoC, every frame of the cube checked by FRAME_CRC | gpudiag at bfaac48 through every gate, LVS proved in full (seed 3, sys_clk 44.71 MHz), and its netlist runs gpudiag to PASS (146 checks, 1.97 s simulated); ddr_gpu at bfaac48 through every gate but LVS, which is outstanding (dram_clk +1.04 ns; section 16.11). The review of 3fea963's fixes (16.12) changed gpudiag but no RTL: the gpudiag bitstream is to be built again with them | pending: gpudiag over JTAG with the cube on the monitor (a new frame every second video frame or sooner, the brief's 30+ a second on the 59.94 Hz monitor; no torn frame; from its UART lines), from the bitstream built after the review (bfaac48's lacks its checks: 16.12); SurfOS's boot stays on G2's ddr_gpu until G4's has its proof (section 14) |
| G5 | optional: the GPU constrained to a region of the die, a faster GPU clock (50 MHz, its own clock domain and crossing on port A) | the crossing's tests at several clock ratios | placement in the region; timing of the faster clock | the G2 and G4 tests again at the faster clock |

## 12. Record

### 12.1 Simulation and host tests

G1 (2026-10-18, `make test-gpu`; [evidence](evidence/gpu/g1-sim.txt)):

| Test | Result |
|---|---|
| `test-gpu-model`: the C model against GPU.md | 38 checks: GPAL's RGB332, CRC-32's check value, frame CRCs computed independently with Python's zlib (black, the test pattern 0x2077DB89, graphics at scale 1 and 2, past 300 KiB), the overlay rule, and the engine's commands traced by hand |
| `test-gpu-vram`: CPU window, engine port and pixel port at once | seeds 1-3: about 2.67 million checks each: 290,000 CPU accesses (17,000 of them errors at or above 300 KiB or fetches), every one answered in the next cycle and none waiting; 132,000 engine grants, never in a cycle the CPU asked; 520,000 pixel-port reads; all 76,800 words read back |
| `test-gpu-video`: the io track's video suite on the `HAS_GFX = 1` build | 122 checks per seed, seeds 1-3 (the test pattern, text, cursor, scroll, frame latch, resets: unchanged) |
| `test-gpu-video`: the graphics tests | 53 checks per seed, seeds 1-3, 48 frames each: registers and GPAL, FRAME_CRC of the test pattern, of black, of text, of graphics and of the overlay equal to the model's and to the captured frame's; 8 placements (doubling, wrapping, stride 0, scale 0) pixel by pixel; mid-frame GFX_ writes change only the next frame, GPAL at once; FRAME_COUNT one per frame; a pixel-domain reset |
| `test-gpu-interconnect`: tb_interconnect with `HAS_GPU = 1` | seeds 1-3, 24,932 accesses each, the gpu and vram targets reached (about 600 and 450 times), fetches to them refused; `HAS_GPU = 0` in test-soc-units as before |
| `test-gpu-soc`: gpudiag on the RTL SoC (`build/sim/surfsim_gpu`) | `GPUDIAG: PASS (90 checks)`: identity, the VRAM window (March C- over 300 KiB, the errors), the registers, 7 scenes whose FRAME_CRCs equal the model's and the CRC of a frame captured at the VGA pins; 23.9 million cycles (0.96 s simulated) in 11 s |

G2 (2026-10-18; [evidence](evidence/gpu/g2-sim.txt)):

| Test | Result |
|---|---|
| `test-gpu-encodings` | the 10 opcodes, 3 header bits, gpu.ID and VRAM's size agree in gpu.h, surf_gpu.v and surfcpu_soc.v |
| `test-gpu-engine`: surf_gpu with surf_vram against the C model | seeds 1-3, 699 checks each: the registers and their error rules; FILL_RECT at every x mod 4 and width 1-9, at each corner, from x = -32768 with w = 65535, zero sizes, the clip at each edge, an empty clip, a full screen (76,850 cycles: 4 pixels a cycle); SET_TARGET's seven refusals and its limits (the last 192 bytes, one line wider than its stride, an empty target); COPY_RECT overlapping in eight directions by 1 and 5 pixels at every source alignment, the key, sources outside the target, the clip, 65535 x 65535 from far out, a 639x400 unaligned overlapping copy (129,260 cycles: 2 a word); LINE in every octant with its endpoints, a point, clipped and far lines; EXPAND at every alignment, opaque and transparent, sizes crossing words, clipped; WRITE_RECT at every alignment, clipped; unknown opcodes; a full FIFO and OVERFLOW; the fence and error interrupts; ABORT inside a WRITE_RECT; WAIT_VBLANK. Then 120 random chunks per seed (about 1,900 commands, 30,000 words, 780,000 pixel writes) with VRAM compared byte for byte at 134 fences, the CPU reading VRAM and writing its spare top meanwhile (the engine waited 6,000 to 11,000 cycles for it) |
| `test-gpu-soc`: gpudiag with the engine on the RTL SoC | `GPUDIAG: PASS (117 checks)`: the G1 sections, the engine's registers and errors (a full FIFO behind a WAIT_VBLANK, OVERFLOW, an unknown opcode, a refused target, a fence's interrupt at intc source 5), and 13 scenes (6 drawn by the engine) whose FRAME_CRCs equal the model's and those of frames captured at the pins; 33.0 million cycles. SurfBoot on the same SoC answers its monitor |

| `test-gpu-mutants`: 18 one-line bugs in copies of surf_gpu, surf_vram and surf_video | 18 of 18 caught ([evidence](evidence/gpu/mutants.txt)): copies in the wrong direction, the clip's right edge inclusive, EXPAND's bits in the wrong order or its transparency ignored, WRITE_RECT's lanes misaligned, LINE's tie rule, the copy key ignored, the engine writing without its grant, OVERFLOW never set, SET_TARGET's bound off by one, COPY's window not shifted, VRAM giving the engine priority over the CPU or its bound inclusive, the overlay rule, FRAME_CRC's byte order, GFX_ settings not waiting for the frame latch, GFX_SCALE ignored; 83 s |
| `gpu-netlist-sim`: the G2 gpudiag build's synthesized netlist (the one LVS proved the bitstream equal to) run from its pins | `GPUDIAG: PASS (117 checks)` as on the RTL SoC (identity "Arty A7-100T (0x100) ... features 17f"), every scene's FRAME_CRC the model's ([evidence](evidence/gpu/g2-netlist-sim.txt)); 1.32 s simulated in 44 minutes |

`make check` passes with the GPU in it: 32 minutes at 95d0bd1 (the documents being edited) and
26 minutes at 6644f17, on the Mac on 2026-10-18 while other sessions were using it. `test-gpu`
adds 1 minute 35 seconds to it from a clean build directory and `lint-gpu` about 20 seconds; the
mutants and the netlist simulation are not part of it.

The existing suites pass unchanged with the GPU code in the tree (`HAS_GPU = 0`): lint-soc,
lint-io, test-io-video (123 checks per seed), test-io-icarus, test-soc-units, test-soc-system,
test-soc-surfsim, test-soc-boot (`BRAMDIAG: PASS (569 checks)`), board-tools (262 checks, the
board line among them), board-wiring (the GPU designs' pins too), test-release (53 tests).

### 12.2 Synthesis

The open flow (`make build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=...`: Yosys 0.69,
nextpnr-himbaechel 1.0.0, Project X-Ray, in surfcpu-openxc7:2026-10-15), seed 1 unless said.
nextpnr's timing is an estimate, not Vivado's sign-off (OPEN-FLOW.md 6.4). Nothing below has
been loaded on a board.

How the gpudiag design got there:

| Build | Result | What changed |
|---|---|---|
| G1 (VRAM in 19 banks of 4,096 words) | synthesis and placement fine (111 RAMB36 and 1 RAMB18; the placer's estimate 63 MHz sys_clk, 138 MHz pix_clk); the router did not converge: stopped after 45 minutes and 10 iterations with 1,117 wires still overused, no bitstream | the 19-input bank multiplexers gathered 672 block RAM outputs per port in one place: VRAM became 5 banks of 16,384 words (b434c49) |
| G2 first (7451a24) | routed in 24 iterations (297 s); timing met (sys_clk 46.7 MHz, pix_clk 96.6 MHz); two gates failed: the netlist check (a 48-bit ROM Yosys made of the opcode decode's case statement has no RAM cell) and LVS (13,987 pins proved, 8 differ: port B address pins 4..11 of GPAL's RAMB18) | the decode became comparisons (c23f04a); GPAL was moved to distributed RAM (a07f185) to step around the RAMB18 |
| G2 second (a07f185) | netlist check PASS; LVS 15,002 proved, 300 differ: the read addresses of GPAL's distributed RAM and the 12 registers after it. The same signals as before, and in both builds they are fed by the scan-out's byte-lane delay (glane1..3), which Yosys had made into two SRL16E cells, the only SRL cells in any SurfCPU build on record | GPAL went back to its RAMB18 (407bd26); the lane delay got a reset, so it stays in flip-flops (95d0bd1) |
| G2 final (95d0bd1) | **every gate PASS**: synthesis, bitstream, round trip, block RAM, LVS (14,001 proved, 0 differ, in 23 minutes), clocks, the netlist check (every RTL memory in RAM cells, the multiplier in its 4 DSP48E1) and the boot ROM check; timing met on seed 1 | |

The G2 gpudiag design ([evidence](evidence/gpu/synth-gpudiag.txt)), 31 minutes on the Mac:

| | Result |
|---|---|
| timing (nextpnr) | pix_clk 82.67 MHz (+27.627 ns at 25.17 MHz), sys_clk 45.61 MHz (+18.075 ns at 25 MHz); no hold violations; cross-domain paths 1.13 and 1.88 ns (reported, checked against nothing) |
| sys_clk critical path | 21.92 ns (4.48 logic, 17.44 routing): the engine's target height `t_h` through the clip comparisons and the engine's address into VRAM's bounds check (surf_vram.v:69) and the write enable of a block RAM of bank 1 on the far side of the die (the last two hops 5.6 ns) |
| pix_clk critical path | 12.10 ns: S3's pixel to the FRAME_CRC update (surf_video.v:495) |
| LUTs | 7,898 of 63,400 (12.5 %); 12,029 LUT positions |
| flip-flops | 3,950 of 126,800 (3.1 %) |
| CARRY4 | 620 of 15,850 (3.9 %) |
| block RAM | 112.5 of 135 RAMB36 (83.3 %): VRAM 76, GPAL one RAMB18, and the SoC's 36 |
| DSP48E1 | 4 of 240 (the multiplier; the engine uses none) |
| memories (netlist check) | the 5 VRAM banks in 16, 16, 16, 16 and 12 RAMB36; GPAL in 1 RAMB18; the command FIFO in 11 RAM64M |

Compared with the bramdiag build on record (docs/evidence/board/openxc7-soc.txt, SoC 0.2.0 at
e3961772: 4,261 LUTs, 2,666 flip-flops, 36 RAMB36, sys_clk 62.16 MHz), that is roughly 3,600
LUTs, 1,300 flip-flops and 76.5 RAMB36 more, and the engine now sets the sys_clk critical path
(45.6 MHz, 1.8 times the 25 MHz clock).

The DDR+GPU design at 32bffc8 ([evidence](evidence/gpu/synth-ddr-gpu.txt)), 43 minutes on the
Mac, beside the DDR design without the GPU on record (docs/evidence/board/openxc7-ddr.txt,
surfcpu_arty_a7_ddr on the 100T at 92a2b510, seed 1). The LiteDRAM core's generated files came
from the surfcpu worktree's `make litedram-gen` output, identical to ip/litedram/arty.sha256 (the
build checks them):

| | surfcpu_arty_a7_ddr_gpu | surfcpu_arty_a7_ddr (on record) |
|---|---|---|
| gates | **every gate PASS** (LVS 18,760 proved, 0 differ, in 32 minutes; netlist check, boot ROM check) | every gate PASS (LVS 10,878 proved) |
| placement | seed 1 of 1 to 8 routed and met every clock | seed 1 |
| dram_clk (80 MHz) | 88.79 MHz, **+1.237 ns** | 88.04 MHz, +1.141 ns |
| sys_clk (25 MHz) | 53.98 MHz, +21.476 ns | 56.55 MHz, +22.318 ns |
| pix_clk (25.17 MHz) | 104.45 MHz, +30.150 ns | 146.63 MHz, +32.904 ns |
| u_dram.iodelay_clk (200 MHz) | 299.76 MHz, +1.664 ns | 346.14 MHz, +2.111 ns |
| clk100_g (100 MHz) | 982.32 MHz, +8.982 ns | 990.10 MHz, +8.990 ns |
| LUTs | 11,289 (17.8 %); 16,718 positions | 7,672 (12.1 %); 11,348 positions |
| flip-flops | 6,498 (5.1 %) | 5,214 (4.1 %) |
| block RAM | 112.5 of 135 RAMB36 (83.3 %) | 36 (26.7 %) |
| DSP48E1 | 4 | 4 |

dram_clk's margin did not shrink in this placement (+1.24 ns against +1.14 ns): its critical
path stays inside LiteDRAM (its ZQ-calibration timer to a bank machine, 11.26 ns). One placement is one
sample, and the DDR designs have met dram_clk in about three placements of four (DDR.md 10.3),
which is why the design keeps its eight seeds. The sys_clk critical path is again the engine's
(18.52 ns: its column group `cg` through the lane clip test and VRAM's bounds check into a block
RAM's write enable).

### 12.3 Placement: "another part of the chip"

Where nextpnr put the GPU in the G2 gpudiag build (`sim/gpu/placement.py` over its netlist and
placement.json, cells given to a part by their RTL file or, for cells abc made, by their
neighbours; [evidence](evidence/gpu/synth-gpudiag.txt)). The 100T has eight clock regions,
X0..X1 by Y0..Y3, Y3 at the top of the die (grid row 0); most of the CPU's cells are at the top
left, in X0Y3.

| Part | Cells | Clock regions | Tiles |
|---|---|---|---|
| engine (surf_gpu) | 3,606 LUT, 832 FF, 273 CARRY4 | X0Y3 (4,273 cells), X0Y2 (438) | x 10..55, y 1..68 |
| VRAM (surf_vram) | 76 RAMB36, 424 LUT, 47 FF | all eight: X0Y2 362 cells, X0Y3 100, the others 7 to 37 | x 14..128, y 5..207 |
| video (with the scan-out) | 1,002 LUT, 638 FF, GPAL's RAMB18, 2 RAMB36 | X0Y2, X0Y3 | x 10..108, y 21..103 |
| CPU | 3,833 LUT, 937 FF | X0Y3 (4,370 cells), the rest scattered | x 10..128, y 1..166 |

In the DDR+GPU build (same report, [evidence](evidence/gpu/synth-ddr-gpu.txt)) the picture
shifts: LiteDRAM takes the right half (X1Y1..X1Y2, next to the DDR3 bank), the CPU moves to the top
(X1Y3 2,941 cells, X0Y3 1,568), and the engine lands below it in X0Y2 (3,962 of its cells; tiles
x 51..80, y 20..115), with VRAM's block RAM again all over the die.

So the engine did not go to "another part of the chip" by design: in the gpudiag build the placer
put it in the CPU's own clock region, next to the CPU and the bus it talks to, and in the DDR
build wherever the CPU and LiteDRAM left room. VRAM's block RAM is wherever the block RAM
columns are: 76 of the 135 RAMB36 cannot help being spread over the die, which is what the long
last hops of the sys_clk critical path show.

**Can the open flow keep a block in a region?** Not today. Probed on 2026-10-18 in
surfcpu-openxc7:2026-10-15 (nextpnr-himbaechel 1.0.0, e860c9c): its XDC reader takes
`set_property` on ports and cells (a cell's LOC or BEL fixes it to one site), `create_clock`,
`set_false_path` and a setup form of `set_multicycle_path`; any other command is skipped with
"ignoring unsupported XDC command", and the binary has no `create_pblock`, `add_cells_to_pblock`
or `resize_pblock`. Region constraints exist inside nextpnr (it confines a regional clock
buffer's loads to the buffer's clock region), but this build has no Python (no `--pre-place`
script), so there is no way in from outside. A LOC on every GPU cell would work in principle,
but the flow does not pass cell properties to nextpnr: boards/arty_a7/nextpnr-xdc.awk gives it
only port properties and `create_clock` and lists the rest as not checked. Putting the GPU in a
region (G5) therefore needs a flow change: either a step that turns a region into per-cell LOCs
after synthesis and lets those properties through, or a nextpnr with pblock support. Neither
was done here (the brief: do not change the flow).

### 12.4 The first review (2026-10-18)

An independent review of 1bcfc96 found no critical defect (it ran the engine against the model
with wider random tests, 880 random ABORTs among them) and eight findings. Each fix has a test
that fails on 1bcfc96 and passes after, where a test is possible:

| Finding | Fix | Test (on 1bcfc96 / after) |
|---|---|---|
| 1 major: gpudiag said PASS with the engine missing (a wrong gpu.ID: 90 checks, LD0 green) | a missing engine, each skipped engine scene and a run short of its 117 checks fail (f356769) | `test-gpu-noengine`: the HAS_GPU = 0 SoC and one with gpu.ID 0x31555048 must FAIL saying why (PASS 90 / FAIL 2 of 6 and 8 of 98) |
| 2: ABORT's FIFO flush and the IE gating untested | test_gpu queues words behind a blocked WAIT_VBLANK and ABORTs; each interrupt with its enable off, the other on, its own on (5dc67ba) | the three mutants of the review (passed / caught; 21 of 21) |
| 3: FRAME_CRC and FRAME_COUNT could pair two frames | a FRAME_COUNT read keeps its frame's CRC for the next FRAME_CRC read (31b8467) | test_video_gfx: FRAME_CRC two frames after FRAME_COUNT must be the counted frame's (newest / counted) |
| 4: CTRL and GFX_ cross separately | documented, not changed: the exact rule in section 3 (31b8467) | test_video_gfx checks both halves of the rule (passes on both: nothing changed) |
| 5: tests took 0x3000_0000 as unmapped; no SurfOS boot on the GPU SoC | soc.toml's `unmapped_probe` hole; bramdiag, SurfBoot's tests and the hello kernel probe it (01ee85b, 1e12818); `test-gpu-system-boot` on check | bramdiag on the GPU SoC (FAIL / PASS 569 checks); SurfOS boots from a card on it (21 of 21 checks, 89 s, as test-soc-system-boot) |
| 6: two `board` lines refused every board in fpga-build.sh | they add up, as in release.py (74a9cc8) | test_tools.sh dry-runs a two-line design for both boards (refused / taken); release-test.py |
| 7: SoC 0.2.0 left in docs, SURFBUS topology, "either value", a 512 KiB vram linker region | fixed (d9cc652); soc.toml `bytes` makes the region 300 KiB (01ee85b) | make check-generated |
| 8: gpudiag_check.py could mix two runs | it judges the last complete run only, and wants the full check total (f356769) | test_gpudiag_check.py (4 of 11 fail / 11 of 11) |
| SRL trap (the review found the cause) | the flow refuses SRL cells after synthesis (3422a76) | make test-flow: Yosys's SRLs for a chain without a reset refused, with one passed; openxc7-build.sh on that chain stops at check-srl (exit 1), passes with the reset |

`-nosrl` was measured first, by Yosys synthesis of every design alone (with and without it, the
flow's script): no design has an SRL either way, but every netlist differs, for example the
baseline 798 cells against 820 and the 100T DDR design 13,642 against 14,070 (more LUTs), so the
flow's synthesis stays as it was and the reset of 95d0bd1 stays too.

**The designs rebuilt at these fixes**, at c217f27 (the RTL and ROM of d9cc652 plus this
record; [gpudiag](evidence/gpu/synth-gpudiag-r1.txt), [DDR+GPU](evidence/gpu/synth-ddr-gpu-r1.txt)):

| | surfcpu_arty_a7_gpudiag (seed 2) | surfcpu_arty_a7_ddr_gpu (seed 1) |
|---|---|---|
| gates | every gate PASS: check-srl, synthesis, bitstream, round trip, block RAM, LVS (14,034 proved, 0 differ), clocks, netlist, boot ROM | every gate PASS: the same, LVS 18,793 proved, 0 differ |
| timing (nextpnr) | pix_clk 101.22 MHz (+29.845 ns), sys_clk 41.82 MHz (+16.086 ns) | clk100_g +8.991 ns, dram_clk 85.78 MHz (**+0.842 ns**), iodelay_clk +2.327 ns, pix_clk +30.385 ns, sys_clk 47.45 MHz (+18.927 ns) |
| LUTs, flip-flops, RAMB36 | 7,933 (12.5 %), 3,983, 112.5 | 11,304 (17.8 %), 6,531, 112.5 |
| build | 29 minutes | 40 minutes |

The gpudiag build at d9cc652 with seed 1 never routed: router2's overused wires fell to 2,358 by
iteration 11 and then grew, to 5,981 at iteration 61, when the build was stopped after four hours.
Since 95d0bd1, whose seed 1 routed in 256 s, only the FRAME pair (about 40 cells), the ROM and
the build id had changed; the placement came out different and router2 did not converge on it.
router2 has no iteration limit and the flow no time limit on it, so the flow's seed list (which
this design does not have anyway) would never have moved on. Seed 2 routed in 29 iterations
(176 s). G1's VRAM failed the same way, so these designs sit near what router2 resolves: every
VRAM address, data and enable net reaches block RAM all over the die. Two remedies are left for
the integrator and G5: a time limit per seed in the flow, so that the next seed is tried, and
copies of VRAM's port signals registered next to each bank. dram_clk met its constraint again,
with less margin than the first DDR+GPU build (+0.84 against +1.24 ns; the DDR design on record
+1.14): one more sample. The CRCs of section 14.1 did not change (the scenes and the model did
not).
The rebuilt gpudiag netlist runs gpudiag to `GPUDIAG: PASS (117 checks)` with every FRAME_CRC
the model's ([evidence](evidence/gpu/g2-netlist-sim-r1.txt); 45 minutes), and `make check`
passes at these fixes (30 minutes on the loaded Mac), now with `test-gpu-noengine` on test-gpu
and `test-gpu-system-boot` beside the soc track's system boot.

### 12.5 The second review (2026-10-18)

A second independent review, of gpu-fix-1 at 589223c, found one major, seven minor and three
cosmetic findings. The fixes are 96198e6..72ba06c (code) and this record; each has a test that
fails on 589223c and passes after, where a test is possible:

| Finding | Fix | Test: on 589223c / after |
|---|---|---|
| 1 major: every Ctrl-R rerun of gpudiag failed: 116 checks ran, a complete run makes 117 | a rerun says so under its banner and is held to 123 checks; the GFX_ reset values need a reset (f3a8469) | test-gpu-soc types Ctrl-R, as board-test.py does for bramdiag when the port opened late, and judges the rerun with gpudiag_check: `FAIL (1 of 117 checks failed)` on 589223c (the review's run of the same commands) / `PASS (123 checks, a rerun)`; bramdiag's rerun on the GPU SoC passes too (569 checks, twice) |
| 2: gpudiag_check judged the last complete run | the capture's final run must have ended and passed (12110cf) | test_gpudiag_check.py: 5 of its 16 checks fail with the 589223c checker / 16 of 16; the review's captures a_hung_last_run and b_garbled_banner: PASS (117) / FAIL, saying why (a later run that never ended; a FAIL behind a garbled banner) |
| 3: half of the FRAME pair rule untested | test_video_gfx: other reads between the halves, a lone FRAME_CRC after a pair (880fc4c; 75 checks a seed) | the mutants pair-never-cleared and pair-cleared-by-any-read: not caught, test_video_gfx PASS (69 checks) on 589223c / caught, FAIL (3 of 75) each |
| 4: gpudiag blind to five engine faults | checks of ABORT, the error interrupt, a refused target, BUSY in a last group and the FRAME pair (f3a8469); section 10 lists what gpudiag does not check | seven SoC mutants judged by gpudiag on the RTL SoC: each `GPUDIAG: PASS (117 checks)` and gpudiag_check PASS in both forms with 589223c's gpudiag / FAIL each; the whole mutant set: 23 of 33 caught by 589223c's tests and gpudiag / 33 of 33 |
| 5: no time limit per placer seed | pnr-timeout, 1800 s by default, recorded as a timeout, then the next seed (fa30d9f); gpudiag's list gets seeds 1 to 8 | test-flow: a stand-in nextpnr that hangs is stopped at the limit, recorded, and the next seed routes (589223c has no limit: its gpudiag build at d9cc652 ran four hours until stopped by hand); dry runs; test_tools.sh (its two pnr-timeout checks fail with 589223c's fpga-build.sh). Given the same inputs and seed, the flows of 589223c and of now make byte-identical outputs: bitstream, FASM, placement, netlists and reports of the baseline and the bramdiag SoC design |
| 6: c217f27's bitstreams could not be rebuilt to their hash | this round's are built with SOURCE_DATE_EPOCH, the commit's time; section 14 gives the exact command | c217f27's gpudiag: .bit header 14:01:22, manifest build_time 13:57:13 (no epoch) / both 2026/10/18 18:05:03 UTC, the epoch 1792346703. No second build of these two (optional); finding 5's check built two other designs twice to the same bytes |
| 7: soc_regions.sbs probed 0x1000_9000, gpu.ID on a GPU SoC | it probes the never-mapped hole, `unmapped_probe` at 0x4000_0000 (72ba06c) | none fails on 589223c: the scripts run on HAS_GPU = 0 only, where 0x1000_9000 is unmapped (latent); test-soc-system, test-soc-surfsim and the netlist's run of the script pass with the new probe |
| 8: "the ROMs differ there, nowhere else" | corrected in section 13 (the expected SoC version and the build id differ too) | none possible (text) |
| 9: FRAME_CRC "ro", and "rc = pops a FIFO" | both FRAME registers rc; the legend says such a read has the side effect its register describes (0e96078); gpu.h holds no registers (they are in the generated soc.h) | make check-generated (passes on both: a documentation fix) |
| 10: srlcheck without SRLC16 and CFGLUT5 (the review said SRLC16E, which 589223c had), no manifest line, exit 2 called "SRL cells" | fixed (96198e6) | a netlist with each kind: 589223c's srlcheck finds 3 of 5, not SRLC16 or CFGLUT5 / 5 of 5 (its self-test); the rebuilt designs' manifests carry `check SRL cells PASS` and `check_srl`; srlcheck exits 2 on an unreadable netlist, which the flow now reports as that (by reading the script: no test runs the flow on one) |
| 11: a bare board line and CRLF read differently | fpga-build.sh and release.py read them alike (facca21) | release-test.py's board-line test on 589223c's release.py: FAIL (the bare line not refused) / OK; test_tools.sh's three board-line checks on 589223c's fpga-build.sh: FAIL (it kept the CR, so "arty_a7_100t" was no board, and took the bare line as no limit) / PASS (270 of 270) |

On finding 1 the choice was between a real reset and honest counting. The SoC has no reset that
software can trigger (SurfBoot's monitor `reset` jumps to the reset vector, which resets no
peripheral), and passing the reset-value check on a rerun would claim a check not made, so a rerun
is counted for what it does. On finding 4, gpudiag gained the checks that are cheap and safe on the
board (about a tenth of a second in all, VRAM bytes the scenes overwrite later, no CRC changed);
what it still does not check is listed in section 10, and the RTL tests and mutants cover it. On
finding 5, the default of 1800 s rests on the 14 place and route runs on record: 4.6 to 337 s,
median 69 s, the slowest the DDR+GPU design, with steps up to 1.8 times slower on the loaded Mac
(OPEN-FLOW.md); this round's two took 276 and 274 s.

**The designs rebuilt at these fixes**, at 72ba06c (the round's final code commit) from a clean
tree, with SOURCE_DATE_EPOCH=1792346703 (72ba06c's commit time), LC_ALL=C and TZ=UTC, each trying
its seeds 1 to 8 under the limit of 1800 s ([gpudiag](evidence/gpu/synth-gpudiag-r2.txt),
[DDR+GPU](evidence/gpu/synth-ddr-gpu-r2.txt); section 14 has the command that rebuilds them):

| | surfcpu_arty_a7_gpudiag | surfcpu_arty_a7_ddr_gpu |
|---|---|---|
| gates | every gate PASS: check-srl, synthesis, bitstream, round trip, block RAM, LVS (14,034 proved, 0 differ), clocks, netlist, boot ROM | every gate PASS: the same, LVS 18,793 proved, 0 differ |
| seed | 1, the first tried (placement and routing 276 s) | 1, the first tried (274 s) |
| timing (nextpnr) | pix_clk 87.47 MHz (+28.292 ns), sys_clk 43.19 MHz (+16.846 ns) | clk100_g +9.013 ns, dram_clk 80.56 MHz (**+0.087 ns**), iodelay_clk +2.142 ns, pix_clk 108.04 MHz (+30.468 ns), sys_clk 52.28 MHz (+20.874 ns) |
| LUTs, flip-flops, RAMB36 | 7,959 (12.6 %), 3,983, 112.5 | 11,355 (17.9 %), 6,531, 112.5 |
| boot ROM sha256 | gpudiag.mem 117f4bea68ab8d73919927f5c49dbecfc4767b52ebc3bec3f001042e3dbddaaa | surfboot.mem be8aa3df6f4145905b61e27c577a294468cc3d4b506ee8695df0c22788754386 |
| bitstream sha256 | 359b44e0f628838c92cf0a582f341923e370389b195509c389ff42bb71876a85 | 0eaca07605799f90bd9528c14656554f4d972ddf3cbc067a6988388eae3592c3 |
| build | 27 minutes | 41 minutes |

dram_clk met its constraint with the least margin yet: +0.087 ns, against +1.24 ns at 32bffc8 and
+0.84 ns at c217f27 (the DDR design on record +1.14). nextpnr's estimate passes, but each
placement is one sample and this one leaves almost nothing (section 13). The CRCs of section 14.1
did not change: the scenes and the model did not. The rebuilt gpudiag netlist runs gpudiag to
`GPUDIAG: PASS (124 checks)` with every FRAME_CRC the model's ([evidence](evidence/gpu/g2-netlist-sim-r2.txt);
1.42 s simulated in 48 minutes). `make check` passes at 72ba06c (31 minutes on the loaded Mac,
the documents being edited: test-gpu with the Ctrl-R reruns, test-flow, board-tools 270 of 270,
test-release 53 tests) and again at 4adbbe9, this record's first commit, from a clean tree
(29 minutes), and test-gpu-mutants catches 33 of 33 at both (5 minutes); the same mutants
against 589223c's tests and gpudiag, and the flows of 589223c and of now side by side, are in the
[evidence](evidence/gpu/review2-sim.txt).

### 12.6 The third review (2026-10-18)

A third independent review, of gpu-fix-2 at f0a33e7, found nothing critical or major: seven minor
and four cosmetic findings. It could not run simulations, so its cases were run here. The fixes are
216a40c..e119a78 (code) and this record; each has a test that fails on f0a33e7 and passes after,
where a test is possible:

| Finding | Fix | Test: on f0a33e7 / after |
|---|---|---|
| M1: a capture passed when an earlier complete run had failed (an intermittent fault, then Ctrl-R or RESET, then a passing run: the review's cases B, C) | every run that reached its GPUDIAG line must have passed, and so must a run a RESET cut short (216a40c) | test_gpudiag_check.py: 12 of its 32 cases fail with the f0a33e7 checker / 32 of 32; the review's cases B and C: PASS / FAIL |
| M2: the checker failed working boards: tools/uart.py capture's time-stamped lines (D, D2), a rerun known only by its line (E, L), any byte after the end (F), a RESET in a rerun with a garbled banner (I) | the stamps are taken off; a run of the rerun's total is a rerun; after the end only the echo, blank lines and bytes without a word may follow; a run whose banner is lost starts at its board, bitstream or identity line (216a40c); section 14 names the capture command and what appending with LOG= means | the review's cases D, D2, E, F, I: FAIL / PASS; L: see M3 |
| M3: the documented Ctrl-R path skipped the reset-value check: a GPU whose GFX_ registers reset wrongly passed with 123 checks (K) | section 14: press RESET, not Ctrl-R; a capture of reruns only fails, saying it is no full board test, unless --accept-rerun (216a40c) | the review's K: PASS (123) / FAIL (PASS with --accept-rerun); L, a rerun whose start the console missed: FAIL as "a complete run makes 124" / FAIL as a rerun alone (PASS with --accept-rerun) |
| M4: dram_clk at +0.087 ns passed (72ba06c's DDR+GPU build) | the slack rule (3d5673a): every clock at least max(0.5 ns, 4 % of its period), or a design list's min-slack; a thinner placement is thin and the next seed is tried; with every seed thin the best is kept, THIN, and make release refuses it without PRERELEASE (e119a78) | test-flow, a thin stand-in seed then a good one: f0a33e7's seed loop takes the thin one ("seed 1: pass", +0.087 ns) / "seed 1: thin", seed 2 taken; every seed thin: THIN, the best kept; slackcheck's self-test; release-test; the rebuilt DDR+GPU design below |
| M5: the release recorded seed 1 for every design | MANIFEST's placer_seed comes from the build record's [openxc7] table (manifest_format 2); release-verify gives each rebuild its recorded seed; compare_records names the seed (e119a78) | release-test's three new tests: 3 of 3 fail with f0a33e7's release.py / pass; 1.0.0-f791b8a1's MANIFEST says seed 1 for the 100T surfcpu_arty_a7_ddr_bramdiag, placed with seed 3 (its files are left as they are; RELEASE.md says so) |
| M6: an out-of-memory kill (exit 137) was recorded as a timeout | timeout --verbose says when it stopped nextpnr; 137 without that is "killed", with its time (3d5673a) | test-flow, a stand-in killed by SIGKILL after 0 s: f0a33e7 records "timeout (nextpnr stopped after 2 s, exit 137)" / "killed (exit 137: SIGKILL, not from the time limit ...)" |
| M7: section 14's rebuild steps lacked make litedram-gen and the settings that must be unset | added, with PLACER_SEEDS as recorded (this record) | by reading, against what a fresh clone needs: the images, a clean checkout, the epoch, build-rom, litedram-gen, the unset variables and each pinned seed |
| C1: Ctrl-C no longer stopped placement and routing | timeout --foreground (3d5673a) | test-flow's Ctrl-C through a pty: on f0a33e7 the build ended 598.5 s after Ctrl-C (the stand-in's sleep) / 0.0 s |
| C2: pnr-timeout 00 (timeout's no limit) was taken | refused by openxc7-build.sh and in a design list (3d5673a) | test-flow (0, 00, 000) and test_tools.sh: f0a33e7 runs "timeout -k 60 00" / refused |
| C3: an attempt stopped after routing said "not routed" | its log decides (3d5673a) | test-flow: f0a33e7 "not routed" / "routed, but stopped before nextpnr finished (its outputs are not used)" |
| C4: a rerun after a run abandoned inside an engine scene failed on a working engine | gpu_registers sets the reset target and clip after its ABORT (78bc15d) | test-gpu-soc's gpudiag-leftover rerun: f0a33e7 GPUDIAG: FAIL (2 of 123 checks failed: the refused target's fill, the BUSY check) / PASS (123 checks, a rerun) |

On M2 the checker now reads what working boards and the documented tools produce, without a path
to a false PASS: a run is judged by what it printed itself (its summary and total, which gpudiag's
own completeness check fixes), lost or garbled lines can only drop a run into "cut short", and a
cut-short run is ignored only when it failed nothing and a finished run follows. On M3 a rerun
stays what gpudiag can do on Ctrl-R, but the board test is a first run after RESET (it resets both
clock domains, and start.S copies .data again): the checker refuses a capture of reruns only unless
asked (--accept-rerun), and section 14 says to press RESET. On M4 the default is max(0.5 ns, 4 % of
the period), as the review proposed: every design of release 1.0.0 meets it (the least margin is
the 35T DDR design's dram_clk, +0.930 ns), and of the builds on record only three would have been
thin: two DDR placements that had failed on the board through BUFGCTRL route-throughs, rebuilt with
that fix (+0.263 and +0.132 ns; not on record as run again), and 72ba06c's DDR+GPU build (+0.087 ns,
never on a board) (OPEN-FLOW.md section 3). A design list may ask for more, or less, on a clock.

**The designs rebuilt at these fixes**, at e119a78 (the round's final code commit) from a clean
tree, with SOURCE_DATE_EPOCH=1792363017 (e119a78's commit time), LC_ALL=C and TZ=UTC, under the
slack rule ([gpudiag](evidence/gpu/synth-gpudiag-r3.txt), [DDR+GPU](evidence/gpu/synth-ddr-gpu-r3.txt);
section 14 has the command that rebuilds them):

| | surfcpu_arty_a7_gpudiag | surfcpu_arty_a7_ddr_gpu |
|---|---|---|
| gates | every gate PASS: check-srl, synthesis, bitstream, round trip, block RAM, LVS (14,034 proved, 0 differ), clocks, slack margin, netlist, boot ROM | every gate PASS: the same, LVS 18,793 proved, 0 differ |
| seed | 1, the first of its list, which passed the rule (placement and routing 326 s) | 7, pinned (PLACER_SEEDS=7): of the eight seeds the one with the most margin (below; 229 s) |
| timing (nextpnr) | pix_clk 92.05 MHz (+28.860 ns), sys_clk 45.34 MHz (+17.946 ns) | clk100_g +9.022 ns, dram_clk 86.78 MHz (**+0.977 ns**), iodelay_clk +2.327 ns, pix_clk 96.72 MHz (+29.385 ns), sys_clk 44.80 MHz (+17.677 ns) |
| LUTs, flip-flops, RAMB36 | 7,915 (12.5 %), 3,983, 112.5 | 11,332 (17.9 %), 6,531, 112.5 |
| boot ROM sha256 | gpudiag.mem 78dab052fe2926721ef5692cf935b7aba86342aa6de9ebdb820d1848b20f4439 | surfboot.mem cb83ad33c0f25c9dbd95880e758bd9e288139413bdb1964b7bb8977f3b176005 |
| bitstream sha256 | 88a5e71c0564335b2aea18b431a55081259630eb454a241753f7c0cfcec832cc | b3c750cbe0caeaf867f541a73bcd038d92e41ac26130f3e6908f54491c2e68f2 |
| build | 33 minutes | 41 minutes |

Every seed of the DDR+GPU design's list at e119a78, on the build's own netlist (the flow with an
unreachable min-slack, so that it tries all eight; every other clock had several times what the
rule wants):

| seed | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| dram_clk | 83.77 MHz | 79.18 MHz | 80.38 MHz | 81.87 MHz | 77.97 MHz | 78.38 MHz | 86.78 MHz | 85.16 MHz |
| slack | +0.563 ns | -0.129 ns | +0.059 ns | +0.285 ns | -0.325 ns | -0.258 ns | +0.977 ns | +0.758 ns |
| verdict | passes | misses 80 MHz | thin | thin | misses | misses | passes | passes |

Left to its list, the flow took seed 1, which passes the rule by 0.063 ns (that build passed every
gate and is not kept: bitstream d3186c2c...). Seed 7 has the most margin and the only margin above
the least any design on record has run on a board with (about 0.9 ns), so the bitstream pins it.
The design is close to what router2 resolves at 80 MHz: three placements in eight miss it, and its
limiting path is again LiteDRAM's zqcs_timer_count0 (11.52 ns, 8.22 ns of it routing). The CRCs of
section 14.1 did not change: the scenes and the model did not. The rebuilt gpudiag netlist runs
gpudiag to `GPUDIAG: PASS (124 checks)` with every FRAME_CRC the model's
([evidence](evidence/gpu/g2-netlist-sim-r3.txt); 1.42 s simulated in 51 minutes). `make check`
passes at e119a78 from a clean tree (30 minutes; test-gpu with the leftover rerun, test-flow,
board-tools 275 of 275, test-release 56 tests) and test-gpu-mutants catches 33 of 33 (5 minutes);
each finding's tests on f0a33e7 and after are in the [evidence](evidence/gpu/review3-sim.txt).

### 12.7 The fourth review (2026-10-18)

A fourth independent review, of gpu-fix-3 at d8fdcee, found nothing critical or major: two minor
and five cosmetic findings. The fixes are 64cb9bb..04caffe (code) and this record; each has a test
that fails on d8fdcee and passes after, where a test is possible:

| Finding | Fix | Test: on d8fdcee / after |
|---|---|---|
| m1: C4's SET_TARGET ran on every run, so the first run after a reset no longer checked the engine's reset target and clip (the review's mutant, a reset base of 4096: PASS, 124 checks) | only a rerun sets them, as the GFX_ reset values are handled (64cb9bb) | test-gpu-mutants' new soc-target-reset-base and soc-clip-reset: each `GPUDIAG: PASS (124 checks)` and gpudiag_check PASS in both forms with d8fdcee's gpudiag / each FAIL; 35 of 35 |
| m2: an earlier run seen only in part failed a capture whose whole first run passed (the review's P2, P3, P5, G1; a regression of gpu-fix-3) | a run is whole only with PASS and every scene's passing CRC line; any other is cut short and not judged, but what it shows of a failure counts, a CRC other than the model's among it (5352299) | test_gpudiag_check.py: 5 of its 41 cases fail with d8fdcee's checker (P2, P3, P5, G1, F1) / 41 of 41; the review's adversarial4.py: P2, P3, P5, G1 FAIL / PASS; F1 PASS / FAIL; F3 to F8, F10 and the two-build case FAIL / FAIL |
| c1: a crash of slackcheck exited 1, "thin": a report with a clock at 0 MHz was kept as THIN, even over a real thin seed (probes C, D) | slackcheck judges only positive, finite figures (exit 2, the reason on stderr), a missed constraint is never thin, a crash of the script exits 3, and the loop keeps only slackcheck's own thin (80c7e0a) | the review's probes at d8fdcee: C and D THIN, the 0 MHz seed kept over the real thin one / test-flow: those placements fail, saying why, and the real thin seed is kept; a broken slackcheck fails the attempt; slackcheck's self-test, 11 cases |
| c2: no reason for a report without a clock (`fail (the slack rule: )`), and a min-slack clock named twice took the last value (probes A, H) | the reason is written; a clock named twice is refused by slackcheck, openxc7-build.sh and in a design list (80c7e0a) | d8fdcee: an empty reason; `--min dram_clk=0.5 --min dram_clk=0.05` judged with 0.05; a dry run naming the clock twice taken / test-flow: the reasons, the duplicate refused; test_tools.sh: a design list naming it twice refused (276 checks) |
| c3: make release left PLACER_SEEDS unused without a word; the format-1 note named 1.0.0's design in any old release's report | PLACER_SEEDS refused as ROM is; the note names that design in 1.0.0-f791b8a1's report only (04caffe) | release-test.py's two changed tests: FAIL with d8fdcee's release.py (PLACER_SEEDS taken; "seed 3" in another release's report) / pass (56 tests) |
| c4: section 14's DDR+GPU step built the list's seed 1 (+0.563 ns), not the seed-7 bitstream section 12.6 argues for | section 14 names the bitstream to test, where it is and how to rebuild it at e119a78 with PLACER_SEEDS=7, and says that another commit means another placement, which the slack rule decides (this record) | by reading |
| c5: a THIN build's note listed the seeds tried, so release-verify's rebuild with the kept seed alone gave another note | the note names the kept placement's least margin only (80c7e0a) | test-flow: the note of a list and of its kept seed pinned are the same (at d8fdcee they differ by the list) |

Two of the review's cases stay as they were, by choice. F2 (a failed run whose output from the
failing scene on was lost, then RESET and a good run) passes: it cannot be told from P4 (a run a
RESET cut short, then a good run), which must pass. U1 and U2 (tools/uart.py capture stopping in
the middle of the GPUDIAG line, as `--until 'GPUDIAG: (PASS|FAIL)'` can) fail, now saying why: the
line's total tells a first run from a rerun, so the checker does not guess it, and section 14's
capture command waits for the whole line.

**The bitstreams.** gpudiag changed (m1), so it was rebuilt at 04caffe, the round's final code
commit, from a clean tree with SOURCE_DATE_EPOCH=1792382496 (04caffe's commit time), LC_ALL=C and
TZ=UTC, its seeds 1 to 8 under the slack rule ([evidence](evidence/gpu/synth-gpudiag-r4.txt)):
seed 1, the first of its list, passed the rule in 178 s (pix_clk 97.67 MHz, +29.485 ns; sys_clk
44.12 MHz, +17.332 ns, the least margin, against 1.600); every gate PASS (LVS 14,034 proved, 0
differ); 7,929 LUTs (12.5 %), 3,983 flip-flops, 112.5 RAMB36; ROM gpudiag.mem
abe5a4dff4ef1f3bd112567f50baeaf278d002ea7b9934ff0094806e8df988d2; bitstream sha256
5a43a578bdb70c366d867e825d2c98168d7d40e56a91765523a61a7514b18846; 28 minutes. The DDR+GPU
design's RTL and SurfBoot did not change in this round, so its bitstream to test stays section
12.6's: e119a78, seed 7, dram_clk +0.977 ns, sha256 b3c750cb...68f2, rebuilt only from e119a78
(section 14). The rebuilt gpudiag netlist runs gpudiag to `GPUDIAG: PASS (124 checks)` with every
FRAME_CRC the model's ([evidence](evidence/gpu/g2-netlist-sim-r4.txt); 1.42 s simulated in 50
minutes). `make check` passes at 04caffe from a clean tree (30 minutes: test-gpu, test-flow,
board-tools 276 of 276, test-release 56 tests), and test-gpu-mutants catches 35 of 35 (6 minutes);
each finding's tests on d8fdcee and after are in the [evidence](evidence/gpu/review4-sim.txt).

### 12.8 The fifth review (2026-10-19)

A fifth independent review, of gpu-fix-4 at ecff4cd, found nothing critical or major: four minor
and two cosmetic findings. The fixes are 8da49c5..317c8f4 (code) and this record; each has a test
that fails on ecff4cd and passes after, where a test is possible:

| Finding | Fix | Test: on ecff4cd / after |
|---|---|---|
| m-A: a line showing a failure outside any run was dropped: a section's end ("gpu registers and errors: 1 of 14 checks failed") between two runs, or before the first when the console opened inside a FAIL line (the review's X4, X5 passed) | every line outside the runs goes through the failure test too, which also takes a FAIL line whose spaces were lost (8af5ab9) | the review's adversarial5.py: X4, X5 PASS / FAIL; test_gpudiag_check.py's 4 new cases of the kind pass with ecff4cd's checker / fail; the review's sweeps (RESET or the console opened at each character of a good run, one byte garbled at each position of a failing run, a failing run cut at each character): no false fail or pass, as before |
| m-B: `uart.py capture --until 'GPUDIAG: (PASS\|FAIL) \([^)]*\)'` stops where its last read ended, which can be inside the echo line after the summary, and the checker failed such a file (E2 to E4, E6; 8 of 24 captures through a pseudo-terminal) | the capture's last line may be a strict prefix of the echo line after a whole final run (8af5ab9); section 14's capture waits until the echo line is whole: `--until 'GPUDIAG: (PASS\|FAIL) \([^)]*\)\n+UART echo: [^\n]*\n'` (this record) | adversarial5.py: E2, E3, E4, E6 FAIL / PASS; test_gpudiag_check.py: 5 new cases fail / pass, and 3 (a last line that is not the echo line's start, more after the prefix, the prefix after a failed run) fail on both; the review's pty_capture.py, 12-byte reads every 1 ms, 24 captures each: the old pattern 8 FAIL with ecff4cd's checker / 24 PASS; the new pattern 24 PASS with either checker |
| m-C: the first run's fill at (0, 0) saw only the reset base and the clip's left and top edges, not what its comment and section 10 claimed (the review's mutants: a reset stride of 320 and a clip bottom edge of 1 caught by neither gpudiag nor test_gpu, a right edge of 4 by gpudiag only through the BUSY check) | after the refused target an 8x2 fill at (636, 478) and an 8x2 copy from (636, 479) to (0, 4) too, in the one check (124 and 123 checks, the 14.1 CRCs unchanged); test_gpu draws the same before any SET_TARGET (8da49c5) | test-gpu-mutants' ten new mutants (stride 320, clip right edge 4, bottom edge 1, width 644, height 481; each as gpu and soc), with ecff4cd's gpudiag and test_gpu: 1 of 10 caught (soc-clip-reset-right-4, by the BUSY check's WRITE_RECT at (16, 0), which that edge clips away) / 10 of 10, each failing at the word it should; 45 of 45 |
| m-D: section 14 built and loaded a bitstream before naming the one to test, checked no sha256 and no build id, and the checker could not tell builds apart (B1: a whole run of another gpudiag build passed) | section 14 rewritten: the recorded bitstream, its sha256 checked first, the build its banner must show, the rebuild apart in 14.2 (this record); gpudiag_check `--build-id`, and `--manifest` for the build_id and source_dirty of fpga-build's manifest: a run of another build or a dirty one where the build was clean fails, and so does a capture whose whole runs show no build (7b51a4a); make test-gpu-soc checks the tree's build id, gpu-netlist-sim the manifest's, and the GPU SoC models rebuild when the build id changes (they kept an old one, which that check found: 317c8f4) | B1 PASS / FAIL with `--build-id d8fdceeb` or a manifest naming it, PASS for the build itself (ecff4cd's checker has no such option: exit 2); test_gpudiag_check.py: 13 new cases (67 in all) |
| c1: section 14 ran sim/gpu/gpudiag_check.py and tools/uart.py as programs (mode 100644: exit 126), from surfcpu/ | `python3 ...`, every path from the repository root, as `make -C surfcpu` (this record) | by reading |
| c2: slackcheck wrote `--out` after its try, so a failed write exited 1 with a traceback | inside it: exit 3, "internal error" (1c5e26c) | the review's probe: exit 1 / 3; slackcheck's self-test runs the program both ways (13 cases) |

**The bitstreams.** gpudiag changed (m-C), so it was rebuilt at 317c8f4, the round's final code
commit, from a clean tree with SOURCE_DATE_EPOCH=1792395168 (317c8f4's commit time), LC_ALL=C and
TZ=UTC, its seeds 1 to 8 under the slack rule ([evidence](evidence/gpu/synth-gpudiag-r5.txt)): seed
1, the first of its list, passed the rule in 229 s (pix_clk 94.35 MHz, +29.125 ns; sys_clk 46.38
MHz, +18.440 ns, the least margin, against 1.600); every gate PASS (LVS 14,034 proved, 0 differ);
7,907 LUTs (12.5 %), 3,983 flip-flops, 112.5 RAMB36; ROM gpudiag.mem
e55bbbfd4c8658f746078ce886ade206c0a4f05485a1a4b297e53f2321adfd5f; bitstream sha256
a44314088944e7e2eb1330b8c22bd0cc4f10c1806c3edc2fa145f9fee7ef6c82; 31 minutes. Its banner and
identity line show build 317c8f4b. The DDR+GPU design's RTL and SurfBoot did not change in this
round, so its bitstream to test stays section 12.6's: e119a78, seed 7, dram_clk +0.977 ns, sha256
b3c750cb...68f2, build e119a787. The rebuilt gpudiag netlist runs gpudiag to `GPUDIAG: PASS (124
checks)` with every FRAME_CRC the model's and the manifest's build id
([evidence](evidence/gpu/g2-netlist-sim-r5.txt); 1.42 s simulated in 47 minutes). `make check`
passes at 317c8f4 from a clean tree (30 minutes: test-gpu, test-flow, board-tools 276 of 276,
test-release 56 tests), and test-gpu-mutants catches 45 of 45 (8 minutes); each finding's tests on
ecff4cd and after are in the [evidence](evidence/gpu/review5-sim.txt).

### 12.9 The sixth review (2026-10-19)

A sixth independent review, of gpu-fix-5 at 9536fe3, found nothing critical or major and found the
code sound: the reset checks pin the target's width and height at 640 and 480 exactly,
test-gpu-mutants catches 45 of 45 and 13 more edge mutants of the review's, and the checker's sweeps
and pseudo-terminal captures are clean. What remained was section 14's procedure and two checker
points: two minor findings and two cosmetic ones. Neither the RTL nor gpudiag changed, so the
bitstreams to test stay section 12.8's (gpudiag, 317c8f4) and 12.6's (DDR+GPU, e119a78). The fixes
are 4637857..2902a6a (code) and this record:

| Finding | Fix | Test: on 9536fe3 / after |
|---|---|---|
| 1 (minor): step 7 said to run m7 "with `DESIGN=surfcpu_arty_a7_ddr_gpu`", but make board-test passed no DESIGN, so m7 asked for surfcpu_arty_a7_ddr, whose manifest is not in the sc/gpu build directory: the identity check became a skip and m7 could pass on the wrong design; and it asked for the monitor's `info` without saying how to reach the monitor | make board-test passes DESIGN as `--design`; board-test refuses to start (exit 2, before it asks anything) when the design's build manifest is missing or names another design, and a comparison that still finds none is a FAIL, no longer a skip (2902a6a); step 7 runs `make -C surfcpu board-test M=m7 DESIGN=surfcpu_arty_a7_ddr_gpu` and names the lines that show e119a787; step 8 reaches the monitor with BTN1 held at RESET (this record) | kit_test's two new refusals, against fake boards a run would pass on (no manifest; a manifest of another design with the same build id): PASS (10 and 27 checks, "not compared") / refused, exit 2, no evidence; `make -n board-test M=m7 DESIGN=surfcpu_arty_a7_ddr_gpu`: no `--design` / `--design "surfcpu_arty_a7_ddr_gpu"`; m7 without a design: run on with surfcpu_arty_a7_ddr and no manifest / refused; the fake set 20 of 20, and test-os-kit and test-os-kit-rtl pass |
| 2 (minor): section 14 said nothing about a FAIL, so a passing retry in a new file could stand for G1 on an intermittent board | the FAIL rule: any FAIL is that test's result, kept, recorded and investigated; a retry is a new test that does not replace it; G1 and G2 pass only when every capture made for them passed or each failure is explained (this record) | by reading |
| 3 (cosmetic): `--manifest` read only build_id and source_dirty, so a gpudiag run judged with the DDR+GPU manifest of its build id passed (B6) | the manifest must name a gpudiag design whose boot ROM is gpudiag.mem and say result "pass", read from its top only (4637857) | B6 PASS / FAIL; test_gpudiag_check.py's 4 new cases of the kind (another design's manifest, another boot ROM, a failed build, no design named) pass with 9536fe3's checker / fail; 73 of 73 |
| 4 (cosmetic): section 14 and section 10 said "every run shows the manifest's build", which is not what the checker asks | both say it now: no run shows another build, one whole run at least shows this one, and only the manifest knows whether the build was clean; so does the checker's docstring (4637857) | B12 (a banner garbled, the bitstream line intact) and B14 (`--build-id` alone, a dirty build) pass on both, as now documented |

The review's adversarial6.py with the new checker: B6 FAIL, every other case as with 9536fe3's, and
its sweeps (the echo line cut at each character, RESET or the console opened at each character of a
good run, a byte garbled at each position of another build's or a dirty run) as before. `make check`
passes at 2902a6a from a clean tree (30 minutes: test-gpu with test_gpudiag_check's 73 cases,
test-os-kit with the fake boards' 20 checks and m3 and m7 on the RTL system, board-tools 276 of 276,
test-flow, test-release 56 tests), and so does test-os-kit-rtl (m5, m8 and m9 on the RTL system, 3
minutes); each finding's tests on 9536fe3 and after are in the
[evidence](evidence/gpu/review6-sim.txt).

## 13. Risks

* **Block RAM 83 %.** VRAM's 76 RAMB36 spread over most of the 100T's block RAM columns, so its
  multiplexers and the CPU's read path span the die. At 25 MHz there is room; the DDR design's
  80 MHz dram_clk had +1.13 ns and placed in three tries of four, and the added block RAM and
  routing may cost it placements. The DDR+GPU design met it with +1.24 ns (section 12.2), +0.84 ns
  (12.4) and +0.087 ns (12.5): nextpnr's estimate passed, with the margin almost gone. Since the
  third review the open flow takes a placement only if every clock keeps max(0.5 ns, 4 % of its
  period) (OPEN-FLOW.md section 3). The build of section 12.6 has +0.977 ns (seed 7), but three of
  its eight seeds miss 80 MHz and two are thin, so a change to the design may leave no seed with
  margin (the flow then says THIN). Registering VRAM's port signals next to each bank (G5) would
  give the router room.
* **0x3000_0000 was a "known unmapped" address** in older tests (bramdiag's probes, SurfBoot's
  monitor test, the hello kernel). Since the first review they probe spec/soc.toml's
  `unmapped_probe` hole at 0x4000_0000, which no SurfCPU SoC will decode, and bramdiag passes on
  the GPU SoC (section 12.4). The ROM images built from this branch differ from the released ones
  (1.0.0) there and in two more places, as any ROM built from it does: the SoC version they expect
  (bramdiag's CPU test compares mimpid with SURF_SOC_VERSION, now 0x300 for 0.3.0; it was 0x200)
  and their build id.
* **Pixel reads during writes.** A word the CPU or the engine writes at the instant the scan-out
  reads it is a block RAM collision: UG473 leaves the read data undefined, so those four pixels
  may show any colour in that frame (the NO_CHANGE mode keeps the memory itself right). For
  FRAME_CRC this means a frame during which software draws into the visible page has a CRC no
  model can predict, not even one that knows when each write landed: only frames shown while
  nothing was written to what they show (still pictures, or drawing into a hidden page and
  flipping GFX_BASE) have a reproducible CRC. The diagnostic takes CRCs of still pictures only
  (section 3), and G4's frame-by-frame tearing check must draw into the hidden page.
* **The engine's complexity** (COPY_RECT's alignment and overlap, EXPAND's stream) is covered by
  the randomized comparison with the model and by mutants; the model itself is checked by
  directed tests written from section 5, not from the RTL.
* **Open-flow sign-off.** As for every SurfCPU bitstream, nextpnr's timing is an estimate and the
  CDC constraints are Vivado's only (OPEN-FLOW.md).
* **SRL cells and LVS.** In both failed G2 builds the open flow's LVS check reported every signal
  fed by an SRL16E as different (section 12.2). The first review found why: the LVS extractor's
  tile model has no shift mode, and what it does not implement reads as 0. Whether those two
  bitstreams were right was never established. The flow now refuses a netlist with SRL cells right
  after synthesis (OPEN-FLOW.md 6.5; `-nosrl` would have changed every design's netlist), so a
  future chain of three registers without a reset stops the build at check-srl with its name, not
  an hour later in LVS. The GPU designs have none (the lane delay keeps its reset).
* **GPU2's size (G4).** The triangle walker nearly doubles the engine (surf_gpu alone in Yosys:
  about 7,000 LUTs, 3,000 flip-flops and 2 DSP48E1, against about 3,600 LUTs for GPU1's engine in
  the whole design), and its lanes sit between the CPU's bus and VRAM's port A, where the sys_clk
  critical path already was. Its first version (about 9,500 LUTs and 26 DSP48E1) did not route
  within the open flow's time limit: section 16.11 records that, what the flow made of the smaller
  one, and the DDR+GPU design's dram_clk margin, which every placement of a bigger design puts at
  risk again (section 12.6's eight seeds).
* **The clamp of 16.4 is never exercised.** It is kept as the brief decided, but the arithmetic
  keeps every covered value within 1/8 of the plane, so no test can show it working; mutants that
  remove it survive by design (16.10). A later change of the precision would make it matter, and
  would need a test that does.
* **The cube's frame check needs the CPU's attention every frame.** gpudiag reads FRAME_COUNT
  continuously and drains its UART lines through a ring, so a frame is never missed in simulation;
  on the board, a missed frame shows in its line (missed > 0) and fails the check rather than
  passing a frame unseen.

## 14. The physical tests (the integrator's, with the owner)

Nothing here has run on a board. These are the steps for G1, G2 and G4 on the owner's A7-100T,
with the Pmod VGA on JB and JC and a monitor (WIRING.md); no card or keyboard is needed for
gpudiag. Every command runs from the root of the worktree whose build directory holds the two
bitstreams to test (section 14.2 rebuilds them elsewhere), as `make -C surfcpu` does. The scripts
run with `python3`: they are not executable files. Since G4 the gpudiag bitstream to test is
GPU2's (bfaac48, section 16.11): it makes every check GPU1's made (G1, G2) and GPU2's (G4, with the
cube), so GPU1's gpudiag bitstream of section 12.8 (317c8f4) is superseded and needs no test.
**That bitstream carries gpudiag as it was at bfaac48, before the review of 3fea963** (section
16.12): it lacks that review's checks (16.12 lists them: four of the top-left rule's six cases on
the SoC, what the refused depth triangles would have drawn, the exact frame-rate rule, frames out
of order in the free run, the cube's distinct frames), and gpudiag is to be built again with them
after the re-review, when the integrator says (no RTL changed: only the boot ROM and the build id
differ). Until then the steps below are those of the bfaac48 bitstream, with that commit's checker
and expected values (step 4); where the gpudiag of the review's fixes differs, they say so. The
SurfOS test (G2) stays on G2's DDR+GPU bitstream (e119a78, section 12.6), which every gate proved:
G4's DDR+GPU bitstream (bfaac48) passed every gate but LVS, whose proof is outstanding (section
16.11). Once it has one, it takes e119a78's place in steps 6 to 8, with its own build id, sha256
and `SoC 0.4.0`, and SurfOS then boots with the GPU2 engine in the design. The table's two
bitstreams come from two commits; section 14.2 builds both into one clone.

| Test | The bitstream, in surfcpu/build/fpga/arty_a7_100t/ | Built at, seed | sha256 | The build its banner must show |
|---|---|---|---|---|
| G1, G2, G4 | `surfcpu_arty_a7_gpudiag/surfcpu_arty_a7_gpudiag.bit` | bfaac48 (section 16.11), seed 3 | `f9a5ac9cb2430ac938b861e08f1fc31bbb1df602414099998cd02aa9e0131c9f` | `SurfGPU diagnostic 1.0.0 (build bfaac489)`, then `bitstream build bfaac489 flags 0, ...` |
| G2 (SurfOS) | `surfcpu_arty_a7_ddr_gpu/surfcpu_arty_a7_ddr_gpu.bit` | e119a78 (section 12.6), seed 7 | `b3c750cbe0caeaf867f541a73bcd038d92e41ac26130f3e6908f54491c2e68f2` | `SurfBoot 1.1.0 (build e119a787), the SurfCPU boot ROM` |

**A FAIL is a result.** Any FAIL is the result of the test it came from: a FAIL line or `GPUDIAG:
FAIL` from gpudiag (also in a run whose start the console missed), a cube line with a torn or a
missed frame or fewer than 30 frames drawn, gpudiag_check's FAIL, a FAIL or INCOMPLETE from
board-test, a build id other than the table's, or the owner's no about the picture. Keep its
capture or evidence file as it is, record the FAIL, and find out why. A retry (RESET, a new capture
file) is a new test: it does not replace the failed one. G1, G2 and G4 pass only when every capture
and run made for them passed, or when each failure is explained (its cause found and shown to lie
outside the design, for example a loose Pmod cable, and written down with the capture).

G1, G2 and G4, the diagnostic and the cube:

1. Check the bitstream before anything else:

       shasum -a 256 surfcpu/build/fpga/arty_a7_100t/surfcpu_arty_a7_gpudiag/surfcpu_arty_a7_gpudiag.bit

   It must print the table's sha256. Anything else is not the bitstream this record describes:
   stop, and rebuild it (section 14.2) or ask.
2. Load it: `make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_gpudiag`
   (volatile: the flash is not touched; tools/program.sh also checks that the manifest next to the
   file describes it).
3. Start the capture into a new file, then press RESET:

       python3 surfcpu/tools/uart.py capture auto --out surfcpu/build/gpudiag-1.txt \
         --until 'cube +10 s:[^\n]*\n'

   It stops once the cube's tenth line is whole, about 13 seconds after RESET. The first run after
   RESET is the board test. Its banner must read `SurfGPU diagnostic 1.0.0 (build bfaac489)` and
   its identity line `bitstream build bfaac489 flags 0, features 17f`, after `board Arty A7-100T
   (0x100), SoC 0.4.0`: another build id is another gpudiag or another bitstream, so stop. Then come
   its sections, one line per scene with its FRAME_CRC (each the one in section 14.1), the cube's
   section (`cube: 16 frames at full rate`, 6 checks), `GPUDIAG: PASS (146 checks)` as the
   synthesized netlist printed it in simulation (section 16.11), the echo line, the cube's header,
   and a line a second like `cube    1 s:  60 drawn,  60 shown, 0 missed, 0 torn (in all ...)`
   (the gpudiag of the review's fixes makes 147 checks, and its line says `1 apart` after the
   frames shown and `0 out of order` after `0 torn`, in the second and in all: section 16.9):
   about 60 frames drawn and 60 shown each second (59.94 frames a second, a new frame every video
   frame; the brief's goal is 30 or more, which bfaac48's checker takes as 30 drawn in each second),
   none missed (every frame shown was checked) and none torn (every frame shown was a whole frame of
   the cube, by its FRAME_CRC). If the console opened after that run began, press RESET
   again, not Ctrl-R: a rerun (145 checks) does not check the values a reset leaves, so it is no
   full board test. `tools/uart.py console auto --log surfcpu/build/gpudiag-1.log` works too
   (Ctrl-] to close it after ten cube lines); `--log` appends raw bytes, so give each test its own
   file: runs of another build in it fail the check.
4. Check the capture against the build:

       python3 surfcpu/sim/gpu/gpudiag_check.py --uart surfcpu/build/gpudiag-1.txt --frames - \
         --expect surfcpu/build/fw/gen/gpuscenes_expect.h --cube-seconds 10 \
         --manifest surfcpu/build/fpga/arty_a7_100t/surfcpu_arty_a7_gpudiag/manifest.txt

   It passes only when no line shows a failure, every run that ended passed, the capture ends with
   one and its cube lines (the capture may end inside one), one at least is a first run, no run
   shows a build other than the manifest's (bfaac489) and one whole run at least shows that one, no
   run shows a dirty banner or bitstream line (the manifest says the tree was clean), and ten cube
   lines at least follow the last run, each with 30 or more frames drawn and none missed or torn.
   **Use bfaac48's gpudiag_check.py for the bfaac48 bitstream**: the clone of section 14.2 has it,
   or `git show bfaac48:surfcpu/sim/gpu/gpudiag_check.py > /tmp/gpudiag_check_bfaac48.py`. From the
   review of 3fea963 on (section 16.12) the checker wants the cube's lines with their frames out of
   order, which bfaac48's gpudiag does not print, and 58 to 61 frames shown a second.
   The manifest must be a gpudiag build's that passed its checks. `--build-id bfaac489` names the
   build without the manifest, but does not know whether it was clean. `--accept-rerun` would take
   reruns alone, which is not this test. `--expect` must hold the expected values of the
   bitstream's own commit, bfaac48: the clone of section 14.2 has them in build/fw/gen/ after its
   last build-rom. From 32e1a86 on scene 13's value differs (a04d1e8a, section 16.11; 9368fe97 from
   the review's fixes on, with the cube's), so a tree that has built a later gpudiag gives a false
   FAIL on scene 13.
5. The monitor: each scene shows for about a twentieth of a second while it is checked (GPU2's four
   last: a fan and a mesh of flat triangles; eight Gouraud ramps; two triangles crossing in depth;
   a fan of Gouraud triangles with SURFGPU, flipped to), then the cube spins, one turn in about two
   seconds, under a green bar `GPUDIAG: PASS   the SurfGPU cube: Gouraud triangles, a depth buffer,
   page flips`. The owner confirms: up to three faces in their six colours, each shaded from dark
   to bright, the hidden faces hidden, smooth motion with no picture split across a line or
   flickering, the bar steady. (The cube of the review's fixes stands on a corner and turns about
   the vertical: two or three faces in every frame, all six colours over a turn; section 16.9.) (After a FAIL there is no cube: the last scene stays with a red bar,
   as GPU1's gpudiag did.) The cube lines are the frame-by-frame record: the CRC of every frame
   shown was one of the cube's.

G2, the DDR+GPU design with GPU1's engine (e119a78), which boots SurfOS from the card exactly as
`surfcpu_arty_a7_ddr` does:

6. Check its bitstream first: `shasum -a 256
   surfcpu/build/fpga/arty_a7_100t/surfcpu_arty_a7_ddr_gpu/surfcpu_arty_a7_ddr_gpu.bit` must print
   the table's sha256; then load it with `make -C surfcpu program-volatile BOARD=arty_a7_100t
   DESIGN=surfcpu_arty_a7_ddr_gpu`. Not a bitstream from `make build-fpga ...
   DESIGN=surfcpu_arty_a7_ddr_gpu` at another commit: that is another placement (seed 1 at e119a78
   gave +0.563 ns, other commits other placements).
7. Run m7 of PHYSICAL-TESTS.md with this design:

       make -C surfcpu board-test M=m7 DESIGN=surfcpu_arty_a7_ddr_gpu

   (`DESIGN=` reaches board-test as `--design`; `ARGS="--design surfcpu_arty_a7_ddr_gpu"` does the
   same.) It asks you to load that design and compares the bitstream with its build manifest,
   surfcpu/build/fpga/arty_a7_100t/surfcpu_arty_a7_ddr_gpu/manifest.txt (build_id e119a787,
   source_dirty false); it refuses to start when that manifest is missing or is another design's.
   Without `DESIGN=` it asks for `surfcpu_arty_a7_ddr`, whose manifest this build directory does not
   have, and refuses. The lines that show the identity: in the transcript, SurfBoot's `SurfBoot 1.1.0
   (build e119a787), the SurfCPU boot ROM` and `soc: Arty A7-100T (0x100), SoC 0.3.0, memory map 2,
   25000000 Hz; bitstream e119a787` (no `dirty`); in the evidence's identity section, `SurfBoot
   1.1.0, ROM build e119a787`, `bitstream build e119a787, flags 0, board id 0x100` and `the
   bitstream's build id e119a787 is the manifest's (e119a787, ...)`; and among its checks `ok the
   bitstream's build id e119a787 is the manifest's` and `ok its dirty flag (0) matches the
   manifest's source_dirty = false`.
8. The GPU among the features, from SurfBoot's monitor. Open the console (`python3
   surfcpu/tools/uart.py console auto --log surfcpu/build/ddrgpu-info-1.log`), hold BTN1 down,
   press and release RESET, and keep holding BTN1 until `surfboot>` appears (SurfBoot first prints
   `BTN1 held: staying in the monitor`); release BTN1, type `info` and Enter. It must print `SurfBoot
   1.1.0 build e119a787`, `sysinfo: magic 46525553, SoC 00000300, map 2, board 00000100, bitstream
   build e119a787 flags 0, time 1792363017` and `clocks: sys 25000000 Hz, timebase 1000000 Hz, pixel
   25173611 Hz; features 1ff (uart timer irq gpio spi ps2 video ddr)`: 1ff has the GPU bit (0x100),
   which the label list predates. Another build id is another bitstream or boot ROM. Press RESET
   (holding no button) to boot again.

### 14.1 Expected FRAME_CRCs

The C model's values for gpudiag's scenes and the cube (`build/fw/gen/gpuscenes_expect.h`, written
by `make build-rom`; `make gpu-scenes` writes each scene's frame as
`build/fw/gpuscenes/scene-NN.ppm` and every 16th frame of the cube's free run as `cube-NN.ppm`).
They do not depend on the build: the same numbers on every bitstream with this gpudiag. Where the
gpudiag of the review of 3fea963's fixes (section 16.12) differs from bfaac48's, the gpudiag of the
bitstream of section 14, both are given: bfaac48's in parentheses.

| Scene | What the monitor shows | FRAME_CRC |
|---|---|---|
| 0 | colour bars (MODE 0) | 2077db89 |
| 1 | text, the 256 glyphs (MODE 1) | 50e89802 |
| 2 | VRAM 640x480 through GPAL's RGB332 | 14ee83ce |
| 3 | the same VRAM through 16 greys | a5b99e4d |
| 4 | the 320x240 page at 150 KiB, doubled | 857895ee |
| 5 | text over graphics (MODE 3), a blue box, the cursor | 6fb93532 |
| 6 | ENABLE 0: black | c656b350 |
| 7 | engine: fills at every alignment, the four edges, a clip | 4218a69c |
| 8 | engine: lines in every octant, a frame, clipped lines | e590bd48 |
| 9 | engine: overlapping copies in eight directions, the key | eeef382e |
| 10 | engine: EXPAND letters (opaque, transparent, scaled), sprites | c957aa55 |
| 11 | engine: the 320x240 page drawn, flipped to after WAIT_VBLANK | 2eb6a588 |
| 12 | engine: the demo | f501eb46 |
| 13 | engine: flat triangles, a fan and a mesh, culling, a clip, six diamonds of four triangles (the top-left rule at each edge, 16.12) (GPU2) | 9368fe97 (bfaac48's, without the diamonds and with the culling pairs half a pixel higher: bbb6a4cb; a04d1e8a from 32e1a86 to the review's fixes, on no bitstream) |
| 14 | engine: Gouraud triangles in eight ramps (GPU2) | f4bcafe0 |
| 15 | engine: depth at 320x240, triangles crossing (GPU2) | bbe8cf82 |
| 16 | engine: page B drawn behind, shown by FLIP (GPU2) | e82b76e5 |
| cube 0, 15 | the cube's first and last checked frames (MODE 2) | 62bae657, 7e872f01 (bfaac48's cube: f896539f, bac874e0) |
| cube 0, 127 | the free run's first and last frames of a turn, under the bar | 8d8aea5e, d8ce830c (bfaac48's: 17a65f96, 9b266b0c) |

Scenes 0 to 12 are GPU1's, unchanged. In simulation (2026-10-25, `make test-gpu-soc`) every scene's
CRC was also that of a frame the VGA capture took at the RTL SoC's pins, and gpudiag printed each
of them on the netlist of the gpudiag bitstream to test (section 16.11).

### 14.2 Rebuilding the bitstreams (not part of the test)

Only when a bitstream of the table in section 14 is missing or its sha256 differs. Each rebuilds
to the same sha256 only from a clean checkout of its own commit (the build id is the commit's,
the build time the epoch's, the .bit header holds the output path, and the seed is pinned), with
the same images, on the same kind of host. Any other commit has another build id, so another
netlist and another placement for every seed: the slack rule then decides which seed is taken
(OPEN-FLOW.md section 3), and the bitstream is not the one this record describes. On a fresh
clone, from its root:

    git clone <the repository> surfos && cd surfos   # git status must show nothing at each checkout
    unset SURFCPU_FPGA_OUT ROM OPENXC7_PNR_TIMEOUT PLACER_SEEDS   # build/fpga, the lists' ROMs, 1800 s
    export LC_ALL=C TZ=UTC
    git checkout e119a78                             # the DDR+GPU design, section 12.6
    export SOURCE_DATE_EPOCH=1792363017              # git log -1 --format=%ct e119a78
    make -C surfcpu build-rom
    make -C surfcpu litedram-gen                     # its LiteDRAM core (fetches pinned sources once)
    PLACER_SEEDS=7 make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr_gpu
    git checkout bfaac48                             # gpudiag, section 16.11, last: its build/fw is the checker's
    export SOURCE_DATE_EPOCH=1792971531              # git log -1 --format=%ct bfaac48
    make -C surfcpu build-rom
    PLACER_SEEDS=3 make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_gpudiag

on an Apple Silicon Mac (Darwin-arm64, the containers aarch64) with surfcpu-toolchain:2026-10-15
(image sha256:e4389c3c6d93...) and surfcpu-openxc7:2026-10-15 (sha256:b802ed1bac37...; its chip
database's sha256 is checked against devenv/openxc7/chipdb.sha256). PLACER_SEEDS gives each
build only the seed it was placed with (without it the lists' seeds are tried in turn, and a
loaded machine could stop at the time limit a seed that routed here). Nothing may be untracked
either: devenv/run.sh would build the boot ROM as dirty while the manifest says clean (section
16.11). Expected: surfboot.mem
cb83ad33c0f25c9dbd95880e758bd9e288139413bdb1964b7bb8977f3b176005 and `surfcpu_arty_a7_ddr_gpu.bit`
b3c750cbe0caeaf867f541a73bcd038d92e41ac26130f3e6908f54491c2e68f2; gpudiag.mem
aaee817dc8a0ad3c37d3fb95050cbe8504fc4dcda1cc3339820cae96cdc8947a and `surfcpu_arty_a7_gpudiag.bit`
f9a5ac9cb2430ac938b861e08f1fc31bbb1df602414099998cd02aa9e0131c9f. Then test them from that clone's
root as section 14 says, its step 1 first. gpudiag at bfaac48 was built twice, with a dirty and with
a clean boot ROM (section 16.11), and the two placed and routed alike, seed for seed; a second build
of the same inputs, to compare the sha256, was not made.

## 16. GPU2: triangles, depth and page flips (G4)

G4 (the brief [plans/2026-10-25-gpu-g3-g4.md](plans/2026-10-25-gpu-g3-g4.md), track gpu-g4) adds
three commands to the engine: SET_ZBUF (a 16-bit depth buffer in VRAM), TRIANGLE (flat or
Gouraud, with a depth test) and FLIP (video GFX_BASE written at the next vertical blanking). The
command set becomes "GPU2": gpu.ID reads 0x32555047, soc_version is 0.4.0 (spec/soc.toml, map 2
kept: additions only), and every GPU1 command keeps its encoding and behaviour, so a GPU1
program runs unchanged. gpudiag checks the new commands and ends with a spinning cube drawn by
the engine (section 16.9). This section is the specification the C model (sim/models/gpu_model.c)
is written from and the RTL (rtl/peripherals/surf_gpu.v, surf_gpu_tri.v) is compared with; the
arithmetic below is exact, so the model, the RTL and a reader get the same pixels.

### 16.1 Decisions and where this deviates from the brief

| Point | Decided (the brief) | Done | Why |
|---|---|---|---|
| ID, version | "GPU2" 0x32555047, soc_version 0.4.0, new opcodes from 0x0A | as decided: SET_ZBUF 0x0A, TRIANGLE 0x0B, FLIP 0x0C; gpu.h keeps GPU1's value as `GPU_ID_GPU1` | the SurfOS driver (G3) accepts either ID |
| SET_ZBUF | base, stride; 16 bits a pixel at base + y stride + 2x; refused unless it fits the target as SET_TARGET's rule; none after a reset | as decided, and an accepted SET_TARGET leaves no depth buffer (a refused one changes nothing; ABORT keeps it) | the depth buffer was checked against the target it was set for: a larger target set later could reach past its end, past VRAM's end or into other data |
| TRIANGLE flags | 7:0 colour; GOURAUD, ZTEST, ZFUNC, ZWRITE, CULL_CW, CULL_CCW | bits 8 to 13 in that order (section 16.2) | |
| Coverage | pixel centres, the top-left rule, exact edge functions on 12.4 | as decided (16.3) | |
| Interpolation | affine plane gradients, one division per gradient (a sequential divider), at least 16 fractional bits, no rounding along a row, clamped to the vertices' range | gradients per 1/16 pixel with 20 fractional bits (16 per pixel), floor; every value taken relative to vertex 0, rounded to the nearest; clamped (16.4). One sequential (one bit a cycle) divider makes the four gradients in turn, and one multiplier every product of the setup (16.8) | values relative to vertex 0 do not depend on the clip or on where the walk starts. A first version ran four dividers at once and had a multiplier for every product (a setup of 59 cycles instead of 247); its gpudiag build did not route (16.11). The clamp is kept as decided, but with this precision it never changes a value (16.4 proves it), so a mutant without it cannot fail any test: the model counts every clamp that would change a value, and the tests require none |
| The walk | the box cut by the clip, rows, groups of four pixels; about 6 cycles a covered group with depth, 1 without; a setup of a few hundred cycles | as decided, with the order of a group's accesses fixed (16.5) | the order decides the result when a program lets the depth buffer overlap the target, so the model and the RTL must agree on it |
| FLIP | waits for the next blanking, writes GFX_BASE as a CPU store would (a port into surf_video, sys_clk) | as decided; a CPU store to GFX_BASE in the same cycle wins over the FLIP's | one rule for a case software should not make; the CPU's store is the one on the bus |
| Depth clearing | FILL_RECT 0xFF over the depth bytes | as decided, with the command words (16.6) | |
| 320x240 layout | A at 0, B at 76,800, depth at 153,600 (stride 640) | as decided (16.6) | |
| The cube | gpudiag after its checks: 8 vertices turned on the CPU, 12 Gouraud triangles with depth, FLIP; expected CRCs from the model; a line a second with frames drawn and shown and torn frames | as decided: 16 frames at full rate are part of the checks (in simulation too), then after a PASS the cube runs on under a fixed result bar, a line a second (16.9) | every picture on the screen counts in FRAME_CRC, so the bar over the running cube says the same for every passing run (first or rerun); a failed run shows its red bar over the last picture as GPU1's gpudiag did |

### 16.2 The commands

| Op | Command | Header bits | Parameter words |
|---|---|---|---|
| 0x0A | SET_ZBUF | | base; stride |
| 0x0B | TRIANGLE | 7:0 colour, 8 GOURAUD, 9 ZTEST, 10 ZFUNC (0 less, 1 less or equal), 11 ZWRITE, 12 CULL_CW, 13 CULL_CCW | for each vertex 0, 1, 2: `X \| Y << 16` (12.4), then `Z \| C << 16` |
| 0x0C | FLIP | | base |

Header bits not listed are ignored, as are bits 31:24 of a vertex's `Z | C` word. gpu.h has the
encodings (`GPU_OP_SET_ZBUF`, `GPU_TRI_*`, `GPU_ZC(z, c)`, `GPU_FX(pixels)`).

**SET_ZBUF** sets the depth buffer: pixel (x, y) of the target has its depth, an unsigned 16-bit
value (0 near, 0xFFFF far), at VRAM byte base + y * stride + 2x, little-endian. It is refused
(BAD_PARAM, nothing changes) unless base and stride are multiples of 4, base < 307,200, stride <
65,536, and, with W and H the target's, either W = 0, H = 0, or (2W <= stride or H = 1) and base
+ (H - 1) * stride + 2W <= 307,200: every pixel's depth is in VRAM and two lines never share a
word. A reset leaves no depth buffer, and so does an accepted SET_TARGET; a refused SET_TARGET,
SET_CLIP and ABORT keep it.

**TRIANGLE** draws the pixels of the effective clip (section 5) whose centres the triangle covers
(16.3), in the flat colour (header bits 7:0) or, with GOURAUD, the vertices' colours interpolated
(16.4); with ZTEST a pixel is drawn only where its depth is less than (ZFUNC 0) or not greater
than (ZFUNC 1) the depth buffer's, and with ZWRITE every pixel drawn has its depth written. A
TRIANGLE with ZTEST or ZWRITE and no depth buffer is refused (BAD_PARAM, nothing drawn; its six
words are taken). A triangle of zero area draws nothing, and so does one culled by its winding
(16.3); neither is an error. Vertices may lie anywhere in the 12.4 range: nothing outside the
effective clip is written, and nothing wraps.

**FLIP** waits, as WAIT_VBLANK does, for the start of the next vertical blanking (the event that
sets video STATUS.VBLANK_EV), then writes base into video GFX_BASE as a CPU store of base would
(bits 18:2 kept), and is done; the next command starts in the next cycle. Section 16.7 gives the
timing: the frame after that blanking shows the new base, and commands after the FLIP may draw
into the picture shown until then.

### 16.3 Coverage

Coordinates are signed 12.4 fixed point: a vertex word's bits 15:0 are X and bits 31:16 are Y,
each a two's complement count of sixteenths of a pixel (-2048.0 to 2047.9375 pixels). All of
this section is in those units. The centre of pixel (px, py) is (16 px + 8, 16 py + 8).

The vertices as given are v0, v1, v2. Their doubled signed area is

```
    A = (X1 - X0) (Y2 - Y0) - (Y1 - Y0) (X2 - X0)
```

With y down the screen, A > 0 means v0, v1, v2 go clockwise on the screen and A < 0
counter-clockwise. A = 0 draws nothing; CULL_CW draws nothing when A > 0, CULL_CCW when A < 0.
Otherwise, when A < 0, v1 and v2 are swapped, so that the triangle walked is clockwise (its area
-A > 0); only the edges below use the swapped order.

For the (possibly swapped) vertices, edge e (0, 1, 2) runs from vertex e to vertex (e + 1) mod 3,
with dX_e = X_b - X_a and dY_e = Y_b - Y_a (a = e, b = (e + 1) mod 3). Its edge function at a point
(x, y) is

```
    E_e(x, y) = dX_e (y - Y_a) - dY_e (x - X_a)
```

which is positive inside the triangle. Edge e is a **top or left edge** when dY_e < 0, or dY_e = 0
and dX_e > 0 (y down: a left edge has the inside to its right, a top edge is horizontal with the
inside below it). Pixel (px, py) is **covered** when for each of the three edges, at its centre
c = (16 px + 8, 16 py + 8), either E_e(c) > 0, or E_e(c) = 0 and edge e is a top or left edge.

So a pixel whose centre lies exactly on an edge two triangles share is covered by exactly one of
them (the edge runs one way in each, and exactly one of the two directions is top or left), and
triangles that tile a region cover each of its pixels exactly once: the rule is the same as
taking the centre moved right by an infinitesimal t and down by an infinitesimal t^2, a point on
no edge. Coverage depends only on the three points, not on their order (a rotation or the swap
gives the same edges). The values are exact integers: |dX_e|, |dY_e| <= 65,535, and for a centre
the walk visits |E_e| < 2^34.

**The box.** Only centres inside the vertices' bounding box can be covered: the columns from
floor((minX + 7) / 16) to floor((maxX - 8) / 16) and the rows from floor((minY + 7) / 16) to
floor((maxY - 8) / 16), with minX, maxX, minY, maxY taken over the three vertices. The walk
(16.5) covers that box cut by the effective clip, [x0, min(x1, W)) x [y0, min(y1, H)); when it is
empty the triangle draws nothing.

### 16.4 Depth and colour

The depth Z (a vertex word's bits 15:0 of its second word) and, with GOURAUD, the colour index C
(bits 23:16) are interpolated with the same rule; v_i is vertex i's value, in the order given (the
swap of 16.3 changes nothing here). With A from 16.3 (not 0):

```
    Nx = (v1 - v0) (Y2 - Y0) - (v2 - v0) (Y1 - Y0)          GX = floor(Nx * 2^20 / A)
    Ny = (v2 - v0) (X1 - X0) - (v1 - v0) (X2 - X0)          GY = floor(Ny * 2^20 / A)
```

(floor of the exact quotient, which is the same for A and -A with Nx, Ny negated with it). GX and
GY are the plane's slopes per sixteenth of a pixel with 20 fractional bits (per pixel, 16 times
that: 16 fractional bits). The value at the centre c = (cx, cy) of a covered pixel is

```
    V = v0 * 2^20 + GX (cx - X0) + GY (cy - Y0)
    value = min(max(floor((V + 2^19) / 2^20), vmin), vmax)
```

rounded to the nearest (halves up) and clamped to the smallest and largest of v0, v1, v2. V is
taken relative to vertex 0, so a pixel's value does not depend on the clip, on the box or on
where the walk starts, and moving one pixel right adds exactly 16 GX (one row down 16 GY): the
engine steps it by exact additions, so nothing accumulates along a row or down the triangle.

**The error is less than 1/8, so the clamp never acts.** Let P(c) be the exact plane at c: with
GX = Nx 2^20 / A - ex, 0 <= ex < 1, and likewise ey, V = 2^20 P(c) - ex (cx - X0) - ey (cy - Y0).
A covered centre lies in the closed triangle, so P(c) is between vmin and vmax, and |cx - X0|,
|cy - Y0| <= 65,535 (both are in the vertices' range), so |V - 2^20 P(c)| < 2 * 65,535 < 2^17 =
2^20 / 8. Rounding to the nearest then lands in [vmin, vmax]: the clamp, kept as the brief
decided, is a safety net that never changes a covered pixel's value. (The model counts the values
it would change, `tri_clamped`, and the tests require 0.)

**Widths.** The same bound gives the engine its widths: for a covered pixel -2^17 < V < 65,535 *
2^20 + 2^17 < 2^36 for depth and < 255 * 2^20 + 2^17 < 2^28 for colour, so the RTL keeps V, the
gradients and every step in 37 bits (depth) and 29 bits (colour) and lets them wrap: modulo
2^37 (2^29) the sums are the same, and at a covered pixel the true V fits, so the wrapped value
is the true one. (Values at pixels that are not covered may wrap; they are never written. The
model computes V exactly with 128-bit integers and counts covered values outside those widths,
`tri_wide`: the tests require 0.) The quotients need the full division: |Nx| < 2^33, so Nx 2^20
has 53 bits, and A < 2^33.

### 16.5 The walk and the order of a group's accesses

The engine walks the box of 16.3 cut by the clip, row by row from the top, and in each row the
groups of four pixels that are the target's words (pixels 4g to 4g + 3, as FILL_RECT walks),
from the group of the box's left column to that of its right column. A pixel of a group is a
candidate when its column is inside the cut box, and a candidate is drawn as follows. For each
group with at least one covered candidate, in this order:

1. with ZTEST, the depth word of pixels 4g and 4g + 1 is read if either is covered, then that of
   4g + 2 and 4g + 3 if either is covered; a covered pixel passes when its depth (16.4) is less
   than (ZFUNC 0) or not greater than (ZFUNC 1) the buffer's. Without ZTEST every covered pixel
   passes;
2. with ZWRITE, the passing pixels' depths are written: the word of 4g and 4g + 1 (if either
   passes), then that of 4g + 2 and 4g + 3 (if either passes), each with byte enables;
3. the passing pixels' colours are written, one word with byte enables (if any passes).

So a group's reads come before its writes, and its depth before its colour; the order matters
only to a program that lets the depth buffer overlap the target, where it decides the result.
The depth words of a group are whole words (16.2's rule keeps every target pixel's depth in VRAM
and two lines apart), and a depth word read for a pixel at the target's right edge may hold
bytes past it: they are read, never written.

Cycles, with VRAM's port A free (the CPU's accesses take it first, as for every command): a group
without a covered candidate takes 1 cycle and no access; a covered group without depth 1 (its
colour write); with ZWRITE alone 2 or 3 (the depth words with a passing pixel, then the colour);
with ZTEST 1 to 2 reads, a cycle for the last word to arrive, then the writes, 6 in all for both
words and every write. Rows add no cycles. The setup (16.8) takes 22 cycles for a flat triangle
without depth and 247 with depth or GOURAUD (the divisions), before the first group.

### 16.6 Clearing the depth buffer, and the 320x240 double-buffered layout

No command clears depth: a FILL_RECT of 0xFF over the depth buffer's bytes, with a target set to
them, makes every depth 0xFFFF (the farthest). VRAM holds two 320x240 pictures and their depth
buffer exactly:

| Bytes | What |
|---|---|
| 0 .. 76,799 | picture A, stride 320 |
| 76,800 .. 153,599 | picture B, stride 320 |
| 153,600 .. 307,199 | depth, stride 640 (320 pixels of 2 bytes) |

GFX_SCALE 2 shows either picture doubled to 640x480. One frame into the back buffer B, with
every word written to CMD (hexadecimal):

```
    01000000 00025800 00000280 00F00280    SET_TARGET 153600, 640, 640 x 240: the depth bytes
    030000FF 00000000 00F00280             FILL_RECT 0xFF, (0, 0), 640 x 240: every depth 0xFFFF
    01000000 00012C00 00000140 00F00140    SET_TARGET 76800, 320, 320 x 240: picture B
    03000000 00000000 00F00140             FILL_RECT colour 0, (0, 0), 320 x 240
    0A000000 00025800 00000280             SET_ZBUF 153600, 640 (after the SET_TARGET)
    0B000Bxx ...                           TRIANGLEs with GOURAUD | ZTEST | ZWRITE (0x0B00)
    0C000000 00012C00                      FLIP 76800: B shows from the next frame
```

and the next frame into A (base 0) the same way. The FILL_RECTs take about 38,400 and 19,200
cycles (2.3 ms in all at 25 MHz).

### 16.7 FLIP and the frame latch

FLIP takes video's vertical blanking pulse in sys_clk, as WAIT_VBLANK does (surf_video's
`vblank`, the start of line 480 crossed by `surf_pulse_sync`), and in the next cycle writes
surf_video's GFX_BASE register through a port of its own (`gfx_base_we`, `gfx_base_in`; a CPU
store in the same cycle wins). From there the value goes the way of a CPU store (section 3): it
crosses to the pixel domain in its `surf_cdc_word` within about a microsecond and is taken at the
frame latch, the start of line 524, 1.4 ms after the blanking began. So the frame after that
blanking shows the new base, whole. The picture shown until then was sent completely before the
blanking (its last visible line is 479), so the commands after the FLIP may draw into it at once.
With every frame drawn into the back buffer and then FLIPped, a frame never tears, and the CPU
never times anything: a FLIP that reaches the engine after a blanking began waits for the next one.

### 16.8 The engine (rtl/peripherals/surf_gpu.v, surf_gpu_tri.v, surf_gpu_div.v)

surf_gpu's sequencer takes up to six parameter words now (its counter has 3 bits). SET_ZBUF uses the
shift-add multiplier SET_TARGET uses for (H - 1) * stride, then checks and keeps base, stride and a
valid flag (S_ZBUF; an accepted SET_TARGET clears the flag). A TRIANGLE whose ZTEST or ZWRITE finds
no depth buffer sets BAD_PARAM in S_SETUP; any other starts surf_gpu_tri with the header, the six
words, the target, the depth buffer and the effective clip, and surf_gpu waits in S_TRI, giving
VRAM's port (the engine's side of port A) to the walker until it is idle. FLIP waits in S_FLIP for
the blanking pulse, then writes `gfx_base_we` / `gfx_base_out` for one cycle. ABORT stops the walker
too (its `stop` input) between two accesses.

surf_gpu_tri sets the triangle up in registers. One 37 x 18 signed multiplier (two DSP48E1) makes
every product of the setup, one a cycle: its operands are chosen by the state and the step, its
product is registered, and a pair's first product is held while the second is made, so that each
step's result is a difference of two registered products. One surf_gpu_div makes the four
quotients one after the other:

| State | Cycles | Work |
|---|---|---|
| T_AREA | 3 | A from two products (dx1 dy2 - dy1 dx2), the vertices' box cut by the clip (16.3) |
| T_DECIDE | 1 | A = 0, culled or an empty box: done; v1 and v2 swapped when A < 0; \|A\| kept |
| T_SETUP | 17 | 16 products: each edge's function at the first group's lane 0 (two each), with the top-left rule folded in (E - 1 for an edge that is not top or left, so a lane is covered when its three are >= 0); the numerators Nx, Ny of depth and colour (two each); the first row's addresses (y0 * stride, for the target and the depth buffer); and the edges' steps per pixel (-16 dY) and per row (16 dX), which need no product |
| T_DIV | 220 | only when ZTEST, ZWRITE or GOURAUD needs them: the four gradients in turn, 55 cycles each (restoring division, one quotient bit a cycle over the 53 bits of \|N\| * 2^20, the floor of a negative quotient from the remainder; only the quotient's low 37 bits are kept) |
| T_START | 5 | (four products, only after T_DIV) V at the first lane 0 relative to vertex 0, modulo 2^37 (depth) and 2^29 (colour): GX (cx - X0) + GY (cy - Y0) + v0 2^20 |
| T_LANES | 1 | lane 0's edge functions and values, twice (at the group and at its row's first group), and three pixel steps of each (lane 3's offset) |

so 22 cycles before the first group for a flat triangle without depth, 247 with depth or GOURAUD,
and 4 for one that is culled, of zero area or outside the clip. The products are 17 x 18 bits
signed (edges, numerators, A), 15 x 16 unsigned (the rows) and 37 x 18 kept modulo 2^37 and 2^29
(the start values). The first version had a multiplier for every product and four dividers
running at once (a setup of 59 cycles, 26 DSP48E1); its gpudiag build did not route within the
open flow's time limit (16.11), and the setup was made sequential as the brief had it.

The walk keeps lane 0's quantities in registers stepped by exact additions: three 36-bit edge
functions, a 37-bit depth and a 29-bit colour value, each twice (at the group and at its row's
first group); a group's step adds 4 pixel steps, a row's step adds the row step to the row copy.
Lane L adds L pixel steps to lane 0's (3 steps registered when the walk starts), so a lane's
coverage is its box test and the sign bits of three sums, and its value a rounding ((V >>> 20) +
V[19]) and a clamp of one sum; the depth compared is that value registered. (The first version
kept every lane in registers of its own, 1,392 flip-flops where these take 522; with the change,
surf_gpu alone in Yosys has about 2,250 LUTs and 770 flip-flops fewer, 16.11.) The walk's states:

| State | Access | Next |
|---|---|---|
| T_GRP | none when no lane is covered (the next group); the colour word without depth; with ZTEST the first depth word needed; with ZWRITE alone the first depth word with a passing lane | T_RD1 or T_RDW (ZTEST), T_ZW1 or T_CW (ZWRITE alone), the next group |
| T_RD1 | the second depth word | T_RDW |
| T_RDW | none: the last word read arrives (both words kept in registers) | T_CMP |
| T_CMP | the lanes compared with registered values: the first depth word with a passing lane (ZWRITE), else the colour word, else none | T_ZW1, T_CW, the next group |
| T_ZW1 | the second depth word | T_CW |
| T_CW | the colour word | the next group |

Each access waits while the CPU has the port. The rule of 16.5 gives the cycles: 1 a group without
depth (covered or not), 2 or 3 a covered group with ZWRITE alone, 6 a covered group with ZTEST and
ZWRITE that needs both depth words (4 with ZTEST alone); the measured figures are in 16.10.

### 16.9 The cube (firmware/diagnostics/gpucube.c, gpudiag.c)

The cube stands on a corner: the CPU turns the corners (+-1, +-1, +-1) of a cube (Q12) so that
(1, 1, 1) lies on the vertical axis (about z by 45 degrees, then about x by arctan(1 / sqrt 2) =
35.26 degrees: Q14 cosines and sines 11585, 13377 and 9459), then for frame k about that axis by k
steps of a 128-step turn (Q14 sines from a quarter-wave table), projects them from 4.5 units with a
focal length of 160 pixels into 12.4 coordinates of a 320x240 picture, maps each corner's depth to
0..65535 and gives it a shade (11..31) of its face's ramp of 32 that grows as it comes nearer. Only
integer +, -, *, / and arithmetic >> of int32 values that never overflow: the same numbers on
SurfCPU and on the host. A face shows when its normal turns more than arcsin(1 / 4.5) = 12.8
degrees towards the eye, so the eye, looking across the axis, sees two of the faces or three in
every frame, never one alone, and all six over a turn: of the 128 frames, 66 show three faces and
62 two, the second most shown face always 1,390 pixels or more. A turning cube cannot show three faces in
every frame and all six over a turn (three faces always shown would be the same three), so the
first cube's comment (three faces) could not hold; and a frame of one face alone is the same picture
as that frame turned about the face's axis by a quarter or a half turn, which a turn about two axes
reaches: the first cube, turned about y by k + 7 and about x by 2k + 11 steps, showed one face
alone in 40 frames, and frames 9 and 73 were the same picture (the review of 3fea963's L1,
section 16.12). A frame is 105 command words (16.6's layout): the depth buffer cleared
(FILL_RECT 0xFF over its 640 x 240 bytes), the picture cleared to the background (entry 255), SET_ZBUF,
twelve TRIANGLEs with GOURAUD, ZTEST (less) and ZWRITE, no culling (the depth buffer hides the far
faces), FLIP to the picture drawn, and a FENCE that counts the frame. Frame k depends only on k mod
128.

**The expected CRCs.** gpuscenes_expect (the build's host tool) draws the frames through the C
model and writes two tables into gpuscenes_expect.h: frames 0 to 15 shown in MODE 2
(gpu_cube_expect_check) and frames 0 to 127 under the result bar in MODE 3 (gpu_cube_expect_run),
each the FRAME_CRC of the picture its FLIP shows. It fails the build, writing no header, when two
frames of either table have the same CRC (gpudiag knows a frame by its CRC, so the order of the
frames shown would be unknowable) or when a frame of the turn shows fewer than two faces (pixels of
fewer than two of the six ramps in the picture drawn).

**In gpudiag's checks** (section "cube: 16 frames at full rate", 6 checks): frame 0 is drawn into A
and shown by the CPU (MODE 2, GFX_STRIDE 320, GFX_SCALE 2) and its still picture's CRC compared;
then frames 1 to 15 at full rate. The CPU records each frame's words and pushes them as FIFO_FREE
allows (it never waits for the engine), and meanwhile reads FRAME_COUNT and, whenever it moved,
FRAME_CRC (the pair of section 3: the CRC of the frame counted). Every frame shown must be one of the
16 frames of the table (none torn); it must follow the frame shown before it (none out of order: a
FLIP waits for a blanking, so after d video frames counted the frame shown is the same one again or
at most d frames on); every one of the 16 must have been shown; no frame may pass unseen
(FRAME_COUNT moving by more than one); and each of frames 2 to 15 must come at most two video frames
after the one before it (the rate: a new frame every second video frame or sooner, 29.97 a second
on this 59.94 Hz display, the brief's 30 or more; frame 1 follows the still picture of frame 0, so
its own wait is not a rate); frame 15's still picture is compared last. The rate rule
is per frame: until the review of 3fea963 (L3, section 16.12) the check wanted frames 1 to 15
within 32 video frames, which a cube at 28 frames a second passed (two frames three video frames
after the one before, the rest two: gpudiag-slow.mem, below). On the RTL SoC frames 1 to 15 were on
the screen within 16 video frames, one video frame apart (`(16 video frames, 16 shown, 1 apart at
most)`, frame 0 counted among those shown): a new cube frame every video frame, 59.94 a second.

**After a PASS** the cube spins on under a fixed result bar (" GPUDIAG: PASS   the SurfGPU cube:
...", green, text row 0, MODE 3: the same for every passing run, first or rerun, so that its frames
have fixed CRCs) until Ctrl-R, with the UART echo going on. Every frame shown is checked against
the 128 frames of gpu_cube_expect_run as above, and a line a second goes out through a ring that the
loop drains as the UART takes it (a waiting printf of a line would stop the checks for longer than a
frame):

```
cube: a line a second: frames drawn (FLIPs done), shown (frames on the screen), apart (the most video frames from a new frame of the cube to the next: 2 is one every second video frame, 29.97 a second), missed (frames not checked), torn (frames that were none of the cube's 128), out of order (frames that did not follow the one before)
cube    1 s:  60 drawn,  59 shown, 1 apart, 0 missed, 0 torn, 0 out of order (in all 60 drawn, 59 shown, 1 apart, 0 missed, 0 torn, 0 out of order)
```

The frames out of order follow the rule of the checks above, the free run's frame 127 followed by
frame 0; the count was kept by the first version and printed nowhere (the review's L1): it is
printed since and judged. Apart is the rate rule of the checks above, kept for the free run: the
most video frames from one new frame shown to the next among the second's (one that ends in the
second counts in it), or more when the second's last new frame was first shown longer ago than that,
so a cube that stopped is not 0 apart; in all, the most of the run. gpudiag_check.py judges every
such line: a torn frame, a missed one, one out of order, a count of frames shown other than 58 to
61, a new frame more than two video frames after the one before (more than 2 apart), or fewer than
29 frames drawn fails the capture. The last is what the rule makes of a second (59.94 video frames
hold 29 or 30 frames that come every second video frame), a backstop. Until the re-review of b2c765d
(low 2, 16.12) the lines had no apart and gpudiag_check wanted 30 frames drawn in each second: a
cube at exactly a frame every second video frame passes the checks, but draws 29 in about one second
of 32 (29.97 a second), which failed it. Shown counts FRAME_COUNT's steps, the video frames of the
second (59.94 a second: 59 or 60 in a line, one fewer or more where a look falls by the second's
edge), every one of them checked when none is missed: a line with 0 shown checked nothing, and
passed until the review (L2). `--cube-seconds N` wants N such lines after the last whole run (the
board test, section 14). After a FAIL there is no cube: the red result bar shows over the last
picture, as GPU1's gpudiag did.

**gpudiag-slow.mem** (test-gpu-soc only, built with GPUDIAG_TEST_SLOW): in the checks each frame's
words end with a WAIT_VBLANK, frames 5 and 9's with two, so that frames 2 to 15 come two video
frames apart and frames 6 and 10 three (15 frames in 32 video frames, 28 a second); gpudiag must fail
the rate check and no other (`GPUDIAG: FAIL (1 of 147 checks failed)`, `up to 3`).
**gpudiag-half.mem** (GPUDIAG_TEST_HALF): a WAIT_VBLANK after every frame of the checks and of the
free run, every frame two video frames after the one before (29.97 a second, 29 or 30 drawn in a
second): the checks pass it (`2 apart at most`) and gpudiag_check passes its free run, every line
`2 apart`. **gpudiag-slowfree.mem** (GPUDIAG_TEST_SLOW_FREE): the checks at full rate, then in the
free run a WAIT_VBLANK after every frame and two after every 32nd (frames 5, 37, ...), a frame three
video frames after the one before about once a second, 29 or 30 drawn in each: gpudiag passes, and
gpudiag_check must fail the free run by its `3 apart` and nothing else.

### 16.10 Tests (simulation)

Each test was written from this section, not from the RTL; the RTL is compared with the C model,
which is checked against hand-worked expectations ([evidence](evidence/gpu-g4/sim.txt); the review of
3fea963's additions, 16.12, [evidence](evidence/gpu-g4/review-fix.txt)).

| Test | What and how many |
|---|---|
| `test-gpu-model` | 86 checks (G2's 38 and GPU2's): a 4x4 triangle pixel by pixel in both windings, the top-left rule with a top, a bottom, a left and a right edge through pixel centres and in six diamonds of four triangles around a pixel centre (each triangle's vertices from 0, 1 or 2 on, in both windings: every diamond covered exactly once, and each of the rule's six cases, edge 0, 1 or 2 a top or a left edge, taking 14 to 38 pixels in each winding), culling both ways, zero areas, a triangle over the whole 12.4 range clipped by the target and by a clip; Gouraud values x + 2y exactly, halves rounded up and down, an inexact gradient whose floor decides a pixel (truncation would give 1, the floor gives 0); SET_ZBUF's six refusals and its limits, SET_TARGET dropping it, the depth layout, the depth functions step by step, two triangles meeting in depth at a known column, FLIP's base. Properties in the counting mode: 400 squares split along either diagonal, 300 fans of 3 to 16 points round a point, 120 jittered triangulated grids of 2 to 9 cells a side (every triangle in a random vertex order) cover every pixel of their hull exactly once (the hull's pixels worked out separately: a half-open box, or a convex polygon's own edges); 300 random triangles cover the same pixels in all six vertex orders; 3,000 random Gouraud triangles with depth over the whole 12.4 range (27.6 million pixels) need the clamp nowhere and no value wider than the RTL keeps |
| `test-gpu-encodings` | 13 opcodes, 9 header bits and the ID agree in gpu.h, surf_gpu.v and surf_gpu_tri.v |
| `test-gpu-engine` | seeds 1 to 3, 1,135 checks each: G2's tests, then SET_ZBUF's refusals and limits; triangles at every lane alignment and four sub-pixel offsets in both windings; edges through centres and a shared-edge mesh of different colours; the top-left rule's six cases in both windings in the model test's diamonds (each triangle its own colour), then each of their 24 triangles alone in a target of its own, the engine's VRAM read back: every pixel of each diamond drawn exactly once; culling and zero areas; the clip's edges, huge, far, slivers, one pixel; every combination of GOURAUD, ZTEST, ZFUNC, ZWRITE and the culls (64 x 2 triangles); triangles crossing in depth over full-range ramps; a depth buffer overlapping the target; FLIP against the blanking pulse (and ABORT while it waits); 24 TRIANGLEs a seed aborted at random moments (6, 7 and 10 of them inside their walk, two of them, in seed 1, between a group's depth and its colour write), each leaving a prefix of the walk; then 120 random chunks with GPU2 in them (804, 830 and 785 TRIANGLEs, a third of them on the half-pixel grid with legs along a row and a column through pixel centres; 523, 518 and 485 walked, 0.80, 0.64 and 0.64 million pixels, 26, 17 and 18 FLIPs, depth buffers, depth clears, the CPU on port A meanwhile), where each of the top-left rule's six cases took 46 pixels or more (seeds 1 to 6; a run of 100 chunks or more fails when one takes none). VRAM compared with the model byte for byte 177 times a seed, and every FLIP write checked to come in the cycle after a blanking pulse |
| the walk's cycles | a triangle with a box of 14,938 groups, 7,499 of them covered, over seeds 1 to 3: flat 14,987 to 14,995 cycles (one a group), Gouraud 15,213 to 15,219 (the divisions), ZWRITE alone 30,019 to 30,032 (3 a covered group), ZTEST and ZWRITE 52,329 to 52,342 (6 a covered group), each with the bus's few dozen cycles of writing the command and STATUS polled about 10 cycles apart; the setup: a culled triangle 29 to 41 cycles from its first word to idle, one of a single pixel 55 to 68 flat and 281 to 285 with Gouraud and depth (since the review of 3fea963's diamonds run before them, the bus's random gaps fall elsewhere: at bfaac48 the same engine took 14,991 to 15,002, 15,211 to 15,221, 30,019 to 30,032, 52,326 to 52,334, 30 to 47, 52 to 71 and 283 to 298) |
| `test-gpu-video` | the io track's 122 checks a seed on the HAS_GFX build, and 90 graphics checks a seed (seeds 1 to 3), 15 of them the FLIP port: in mid-frame the next frame wholly, at a blanking's start the frame after it (three times), a CPU store in the same cycle winning |
| `test-gpu-soc` | gpudiag on the RTL SoC: `GPUDIAG: PASS (147 checks)`, every scene's FRAME_CRC the model's and that of a frame captured at the VGA pins (17 scenes, G2's 13 unchanged), the cube's frames 1 to 15 within 16 video frames, one video frame apart (16 frames shown), none torn, missed or out of order; its first second of free run `60 drawn, 59 shown, 1 apart, 0 missed, 0 torn, 0 out of order`; then the Ctrl-R rerun (146 checks) and the earlier reruns; gpudiag-slow.mem, the cube at 28 frames a second, fails the rate check alone (16.9); gpudiag-half.mem, every frame two video frames after the one before, passes the checks (`30 video frames, 30 shown, 2 apart at most`) and gpudiag_check (two seconds of free run, 31 and 30 drawn, `2 apart`); gpudiag-slowfree.mem passes gpudiag and its free run fails gpudiag_check by `3 apart` alone (its first second: 30 drawn, 59 shown); SurfBoot and bramdiag as before |
| `test-gpu-noengine` | gpudiag_check's 100 self-test cases (27 of them the cube's lines); gpudiag fails without the GPU and with gpu.ID 0x32555048 |
| `test-gpu-mutants` | all of them at db3e65c (G1's and G2's too), in two halves side by side: 69 of 69 caught by test_gpu, test_vram, test_video_gfx and the model's tests (GPU2's: 26 of 26 engine mutants, 12 of 12 model mutants), and 27 of 27 by gpudiag on the RTL SoC (GPU2's: 13 of 13); the three clamp mutants survive, as 16.4 proves they must (below). The review of 3fea963 (16.12) added the top-left rule's six per-edge cases for the engine, the SoC and the model (tri-top-edge-excluded and soc-tri-top-edge-excluded are tri-e0-top-excluded and soc-tri-e0-top-excluded now) and two of depth drawn without a depth buffer, for the engine and the SoC. At bfaac48 the run was 56 of 56 and 19 of 20: soc-tri-top-edge-excluded got past gpudiag, none of whose triangles then had a horizontal edge through pixel centres; scene 13's culling pairs half a pixel down (32e1a86) caught it, by scene 13's CRC (0x313fd79e, the model's a04d1e8a) |

**The three survivors, and why each is equivalent** (also in sim/gpu/mutants.py, beside each). The
common step is 16.4's: at a covered pixel's centre |V - 2^20 P(c)| < 2^17 and the exact plane P(c)
lies between the smallest and the largest vertex value, so the rounded t = floor((V + 2^19) /
2^20) is in [vmin, vmax], and a clamp to [vmin, vmax] returns t unchanged.

* `tri-depth-clamp-removed` (zround without its clamp): a lane's depth reaches VRAM only as the
  depth word of a passing lane (zbe0 and zbe1 are made from `pass`) and the depth test only for a
  covered lane (`pass = cov && zok`); at a covered lane the true V fits 37 bits (16.4's widths) and
  t is in [zmin, zmax], so zval is t with the clamp or without it. No input changes a depth word
  or a test's outcome.
* `tri-colour-clamp-removed` (cround without its clamp): a lane's colour reaches VRAM only in a
  passing lane's byte (`m_be = pass`), and only with GOURAUD (without it the colour is the
  header's); at a covered lane the true V fits 29 bits and t is in [cmin, cmax]. No input changes
  a pixel written.
* `model-clamp-removed` (the model's clamp, `c = t`): the model evaluates a value only at a covered
  pixel and computes V exactly (128 bits), so t is in [vmin, vmax] and c = t either way; its count
  `tri_clamped` of the values the clamp would change, which every test requires to be 0, is the
  same claim checked over 27.6 million random pixels.

### 16.11 Synthesis and the netlist (2026-10-25 and 26)

Every number from the open flow is nextpnr's estimate, not Vivado sign-off, and nothing was loaded
on a board: the physical tests are section 14's. **Where the gates stand:** the gpudiag bitstream
at bfaac48 passed every gate of the open flow, LVS proved in full; the DDR+GPU bitstream at
bfaac48 passed every gate but LVS, whose proof is outstanding (below). Until it has one, SurfOS's
gfx on the board can use G2's DDR+GPU bitstream (GPU1, section 12.6), which is proved.

**The bitstreams of this record** (each from a tree without changes to tracked files; "dirty" marks
a boot ROM built while untracked files were in the tree):

| Design | Built at, seed | Boot ROM (sha256) | Bitstream (sha256) | Gates | Use |
|---|---|---|---|---|---|
| gpudiag | 804127d, seeds 1-2 tried | gpudiag.mem fb548286... | none | not routed (1,800 s a seed) | superseded |
| gpudiag | bfaac48, seed 3 | gpudiag.mem 7b51212d..., dirty | 8ab254ac... | every gate | superseded: its banner says "dirty tree" |
| ddr_gpu | bfaac48, seed 1 | surfboot.mem 6c8c8ea1..., dirty | 1914436b... | every gate but LVS | superseded: dirty boot ROM |
| **gpudiag** | **bfaac48, seed 3** | gpudiag.mem aaee817d... | **f9a5ac9c...** | **every gate, LVS proved in full** | **the one to test (section 14)** |
| ddr_gpu | bfaac48, seed 1 | surfboot.mem 6da7c06a... | 582570ab... | every gate but LVS (outstanding) | not to test until its proof |

The gpudiag bitstream to test carries gpudiag as it was at bfaac48. That gpudiag checks the
top-left rule's top case for edge 0 only at engine level (test_gpu), not on the SoC: its scenes had
no horizontal edge through pixel centres, and the SoC mutant of that rule got past it (16.10).
Scene 13's fix came after the build (32e1a86) and changes scene 13's FRAME_CRC from bbb6a4cb to
a04d1e8a; the review of 3fea963 then changed gpudiag further (16.12: scene 13 9368fe97, a new cube,
147 checks), and lists what the bitstream lacks of it. The next gpudiag bitstream carries them.

**The first build did not route.** At 804127d, with a multiplier for every product of the setup and
four dividers at once ([evidence](evidence/gpu-g4/synth-gpudiag-r1.txt)), the gpudiag design took
23,772 LUT positions, 6,886 flip-flops, 1,628 CARRY4 and 30 DSP48E1. Its seed 1 placed (sys_clk
estimated at 42.95 MHz), but its router had 36,147 wires overused at iteration 1 and still 7,334 at
iteration 5, about three minutes an iteration, when the flow's limit of 1,800 s a seed stopped it;
the build was then stopped by hand (its container only) in seed 2. Two changes made the engine
smaller without changing a value or an access (16.8; test_gpu passed after each): the setup on one
multiplier and one divider (1987e0a), and lanes 1 to 3 as sums of lane 0's registers (bfaac48).
surf_gpu alone in Yosys (synth_xilinx, an estimate of its cells): 9,542 LUTs, 3,730 flip-flops,
1,284 CARRY4 and 26 DSP48E1 at 804127d; 6,994, 3,004, 965 and 2 at bfaac48.

**The boot ROM's dirty flag (a finding).** The first builds of both designs at bfaac48 passed their
gates with boot ROMs built as dirty: `make build-rom` ran while this record's evidence files were
untracked, devenv/run.sh counts any untracked file that git does not ignore as dirty (so
FW_BUILD_DIRTY 1 and a "dirty tree" banner), and tools/fpga-build.sh writes the manifest's
source_dirty from the tracked files and the design's own sources only (false). A capture of such a
bitstream fails `gpudiag_check --manifest` (a dirty banner where the manifest says clean), so both
were built again at the same commit with nothing untracked, and the build script used for this
record now refuses an untracked file ([evidence](evidence/gpu-g4/synth-gpudiag-r2.txt)). The two
rules are left as they are, for the flow's owner to settle.

**The gpudiag design at bfaac48**, the bitstream to test
([evidence](evidence/gpu-g4/synth-gpudiag.txt)), 2 hours 31 minutes on the Mac; it placed and
routed exactly as the build with the dirty ROM:

| | Result |
|---|---|
| gates | **every gate PASS**: SRL cells, synthesis, bitstream, round trip, block RAM, LVS (16,212 proved, 0 differ, in 46 minutes), clocks, the slack margin, the netlist check (every RTL memory in RAM cells as in G2; 6 DSP48E1, the CPU's 4 and the setup's 2) and the boot ROM check |
| placement | seed 3 of 1 to 8: seeds 1 and 2 did not route within 1,800 s (their routers came down to 647 and 71 wires overused, then diverged); seed 3 routed in 91 iterations (165 s) |
| timing (nextpnr) | sys_clk 44.71 MHz (+17.632 ns at 25 MHz, the rule wanting 1.600 ns), pix_clk 90.50 MHz (+28.674 ns at 25.17 MHz); no hold violations; cross-domain paths 1.27 and 1.68 ns (reported, checked against nothing) |
| sys_clk critical path | 22.37 ns (4.72 logic, 17.59 routing): G2's, GPU1's engine target height `t_h` (surf_gpu.v:158) through the clip and VRAM's bounds check into the enable of a block RAM of bank 0; the walker is not on it |
| pix_clk critical path | 11.05 ns: S3's pixel to the FRAME_CRC update, as in G2 |
| LUTs | 12,034 of 63,400 (19.0 %); 19,881 LUT positions |
| flip-flops | 6,161 of 126,800 (4.9 %): surf_gpu 916, the walker 1,898, its divider 144 |
| CARRY4 | 1,287 of 15,850 (8.1 %) |
| block RAM | 112.5 of 135 RAMB36 (83.3 %), as in G2 |
| DSP48E1 | 6 of 240 |

Against G2's gpudiag (12.2: 7,898 LUTs, 3,950 flip-flops, 620 CARRY4, sys_clk 45.61 MHz), GPU2
adds about 4,100 LUTs, 2,200 flip-flops and 670 CARRY4, no block RAM, and leaves sys_clk where it
was.

**Is gpudiag near the open flow's routing limit?** What is known: seeds 1 and 2 placed, and their
routers came down to 647 and 71 wires overused (iterations 17 and 53), then diverged, and were at
7,397 and 9,206 when the flow's limit of 1,800 s a seed stopped them; seed 3 routed in 91
iterations (165 s of routing, 494 s of nextpnr in all). G2's gpudiag had one seed of two diverge
too (12.2's record of its first build), and its last build routed with its first seed in 4
minutes. The die is far from full (15.7 % of the LUT positions), and the DDR+GPU design, bigger
(24,629 positions), routed with its first seed in 18 iterations: so the congestion is local and
depends on the placement, not on the design's size alone. Where it is was not measured: router2
reports how many wires are overused, not where, and section 12.3's placement study was not
repeated for G4. The engine's VRAM port reaches 76 block RAMs spread over the die (12.3), next to
the CPU, which is the likely place. So the design routes, but with one seed in three, at a
quarter of the time limit: the next change to the engine may need more seeds, a region for the GPU
(G5) or a smaller engine. A rebuild that changes only the boot ROM should place and route as this
one did with PLACER_SEEDS=3: the two builds of gpudiag at bfaac48, whose boot ROMs differ, routed
alike, seed for seed (not run again for this record).

**The DDR+GPU design at bfaac48** ([evidence](evidence/gpu-g4/synth-ddr-gpu.txt)), SurfOS's design
with GPU2, built again with the clean boot ROM as gpudiag was (the two builds placed, routed and
timed alike, and their LVS was killed at the same point), beside G2's (12.2):

| | surfcpu_arty_a7_ddr_gpu at bfaac48 | at 32bffc8 (G2) |
|---|---|---|
| gates | every gate PASS but LVS, **outstanding**: the flow's proof was killed at this session's Docker cap of 3.5 GB (exit 137 after 3,058 s, 8,606 proved, 0 differ) | every gate PASS (LVS 18,760 proved) |
| placement | seed 1 of 1 to 8 routed (18 iterations, 54 s) | seed 1 |
| dram_clk (80 MHz) | 87.24 MHz, **+1.037 ns** | 88.79 MHz, +1.237 ns |
| sys_clk (25 MHz) | 43.44 MHz, +16.982 ns | 53.98 MHz, +21.476 ns |
| pix_clk (25.17 MHz) | 106.94 MHz, +30.373 ns | 104.45 MHz, +30.150 ns |
| u_dram.iodelay_clk (200 MHz) | 345.78 MHz, +2.108 ns | 299.76 MHz, +1.664 ns |
| clk100_g (100 MHz) | 993.05 MHz, +8.993 ns | 982.32 MHz, +8.982 ns |
| LUTs | 15,467 (24.4 %); 24,629 positions | 11,289 (17.8 %); 16,718 positions |
| flip-flops | 8,709 (6.9 %) | 6,498 (5.1 %) |
| block RAM | 112.5 of 135 RAMB36 (83.3 %) | the same |
| DSP48E1 | 6 | 4 |

dram_clk keeps its margin above the rule's 0.5 ns (+1.04 ns); its critical path stays LiteDRAM's
own (its ZQCS timer to a bank machine's read, 11.46 ns), and sys_clk's is GPU1's engine's column
group `cg` (surf_gpu.v:201), both as in G2. One placement is one sample (12.2).

**The DDR+GPU proof, outstanding, and its planned route.** The flow's LVS keeps one incremental Z3
session (lvs_equiv's default, `--solver libz3`), whose memory grows with the questions: it was past
3.5 GB at 41 % of them (8,606 proved of about 21,000, none differing), so a whole run would want
about 8.5 GB, more than the Docker VM's 7.75 GiB. lvs_equiv's own `--solver <cmd>` route asks the
same questions (the same miter for each register, output and memory-boundary bit, in the same order:
fasm2netlist 0948889f, solver.cpp and lvs_equiv.cpp) and runs one solver process a question on the
whole network written out as SMT-LIB 2. The image has the Z3 library but no solver program, so
`build/g4-lvs/z3cmd.py` hands each question to the image's own libz3 4.13.3 (Z3_eval_smtlib2_string;
an error or any other answer counts as unknown, never as proved; it is self-tested on a known
unsat, a known sat and a malformed question first), and `build/g4-lvs/lvs-solver.sh` runs it with
the flow's arguments on a build's own files. A 15-minute trial on the first DDR+GPU build: 2,346
proved, 0 differ, 0 unknown, with peaks of 1.13 GB (the largest process) and 2.09 GB (the container,
page cache included); its question files grew from 3.2 MB to 11.4 MB. The whole proof by this route
is expected to take 15 to 20 hours within the cap. It is planned once, on the DDR+GPU bitstream
that goes to the board, after the review: a fix to the RTL would need another build and another
proof.

**The gpudiag netlist in simulation** ([evidence](evidence/gpu-g4/netlist-sim.txt)): the
synthesized netlist of the gpudiag bitstream to test, run from its pins with its boot ROM inside it
(`make gpu-netlist-sim`, Verilator, NETLIST_MAX_MS=3000), printed `SurfGPU diagnostic 1.0.0 (build
bfaac489)` and `bitstream build bfaac489 flags 0, features 17f`, every section with the model's
CRCs (the 17 scenes, GPU2's four among them, and the cube's 16 frames at full rate in 16 video
frames), and `GPUDIAG: PASS (146 checks)` at 1.974 s of simulated time, after 3 hours 19 minutes
(165 microseconds simulated a second); gpudiag_check passed the transcript against the build's
manifest. It is the
first evidence that synthesis made the triangle engine, the depth buffer and FLIP as the RTL says
(LVS proves the bitstream equal to the synthesized netlist, not the netlist to the RTL), and it is
redone once, on the bitstream that goes to the board.

### 16.12 The review of 3fea963 (2026-10-26)

An independent review of G4 at 3fea963 found nothing high. It agreed that the RTL and the C model
are correct (a reference it wrote from this section alone, and an emulation of the RTL's bit widths,
gave the model's pixels) and found two medium and three low gaps in the tests and in gpudiag. The
fixes are cd523de to db3e65c and this record; no RTL and no model changed. Each fix was checked by
putting its bug back into today's code alone and running the test again
([evidence](evidence/gpu-g4/review-fix.txt)). The gpudiag runs used three boot ROMs on the same RTL
SoC (surfsim with HAS_GPU = 1): gpudiag as at bfaac48 (the gpudiag bitstream's, 16.11), as at
3fea963 (with scene 13's half-pixel shift, 32e1a86) and as fixed.

| Finding | Fix | Test: the bug put back / after |
|---|---|---|
| M1 medium: only some of the top-left rule's six per-edge cases were tested (16.3 numbers the edges as the triangle is walked, so whether a top or a left edge is edge 0, 1 or 2 depends on the vertex order and the winding): three RTL mutants, edge 1 without its top case, edge 0 without its left one and edge 2 without its top one, passed test-gpu-engine on seeds 1 to 3; random vertices anywhere on the 1/16 grid practically never put an edge through pixel centres; gpudiag's scenes left four cases unseen | (1) diamonds of four right triangles around a pixel centre, their legs along its row and column through pixel centres, each triangle's vertices from 0, 1 or 2 on and in both windings (each diamond gives every triangle a top and a left leg shared with a neighbour, for which it is a bottom or a right edge): test_gpu_model counts every diamond covered exactly once and every one of the twelve cases (six, in both windings) taking pixels (cd523de); test_gpu draws them against the model, then each of the 24 triangles alone into a target of its own and reads the engine's VRAM back, every pixel of each diamond (worked out from the diamond's own edges) drawn exactly once (06bc0a1); (2) a third of the random stream's triangles on the half-pixel grid with legs along a row and a column through pixel centres, a corner inside the clip, the vertices in any of their six orders; test_gpu counts the pixels each case takes, and a run of 100 chunks or more fails when one of the six takes none (06bc0a1); (3) mutants.py has each case dropped alone for the engine, the SoC and the model (4154bd5; tri-top-edge-excluded and soc-tri-top-edge-excluded are tri-e0-top-excluded and soc-tri-e0-top-excluded now); (4) scene 13 draws six such diamonds, each quarter its own colour (a7174e5) | (1) the six engine mutants against 3fea963's test_gpu, seeds 1 to 3, 120 chunks: edge 0 left, edge 1 top and edge 2 top pass on every seed; edge 0 top (the shared-edge mesh), edge 1 left (a slanted left edge through centres, in the triangles crossing in depth) and edge 2 left (eleven directed FENCEs) fail / against today's all six fail on every seed, the first four at the diamonds' FENCE (10 to 53 words); with the model's rule broken for one edge alone, 3fea963's model tests already failed (their squares and grids in random vertex orders), and today's diamond checks fail too; (2) test_gpu --random-only with 3fea963's random vertices put back: no top case's mutant changes a chunk on seeds 1 to 3, the left cases' only on seed 2 (edge 2's also on seed 3), through slanted edges, and 6, 3 and 5 of the six cases take no pixel / today's random part alone fails each of the six mutants on every seed (first at chunk 0 to 44), each case taking 46 pixels or more (seeds 1 to 6); (3) all caught (16.10); (4) the six SoC mutants: gpudiag as at bfaac48 catches edge 0's and edge 2's left cases (scene 13, edge 2's also scene 15), as at 3fea963 also edge 0's top case; edge 1's top and left cases and edge 2's top case pass both / gpudiag as fixed fails all six, each by scene 13's CRC |
| M2 medium: gpudiag's "depth without a depth buffer is refused" could not fail: the watched word (VRAM byte 8000) lay outside the triangle, and the misaligned SET_ZBUF's own BAD_PARAM stood for the TRIANGLE's; the second check's VRAM half was as vacuous | the refused SET_ZBUF checked on its own and the sticky bits cleared before the TRIANGLE; each check sets the words it watches to 0x5A5A5A5A before its TRIANGLE: two of the target's that the triangle covers, and for the second check (now with ZTEST and ZWRITE) also two depth words of the dropped buffer, a depth the triangle would pass, so that a triangle drawn with that buffer writes them; one check more, 147 in all (146 in a rerun) (6440fd1); two SoC (and engine) mutants, ZWRITE alone not refused, and the refusal's BAD_PARAM set while the triangle is drawn all the same (two lines: a mutant may carry several edits now) (6440fd1) | soc-tri-zwrite-alone-not-refused and soc-tri-refused-but-drawn pass gpudiag as at bfaac48 and as at 3fea963 / gpudiag as fixed fails them: the first `a TRIANGLE with ZWRITE and no depth buffer: STATUS 0x0; 2 of the 2 target words it covers drawn` (1 of 147 checks), the other the same with STATUS 0x8 and `a TRIANGLE with ZTEST and ZWRITE after SET_TARGET dropped the depth buffer: STATUS 0x8; 4 of the 4 words it would write changed` (2 of 147); soc-zbuf-kept-by-set-target fails all three (by its STATUS; as fixed, `STATUS 0x0; 4 of the 4 words`); test_gpu catches both engine mutants at test_zbuf's FENCE, which is as it was at 3fea963 |
| L1 low: the cube's frames 9 and 73 were the same picture (one face showing; a half turn about y maps corner i onto corner i ^ 5), so the free run's order check was weakened; its out-of-order count was printed nowhere | the cube stands on a corner and turns about the vertical, two or three faces in every frame (16.9) (58a7f9c); gpuscenes_expect fails the build when two frames of either table have the same CRC, or a frame of the turn shows fewer than two faces (58a7f9c); a frame shown is out of order unless it is the last one shown or at most d frames on, d the video frames counted since; the free run's line prints the count, and gpudiag_check fails a line with one (21be632) | 3fea963's gpucube.c with today's gpuscenes_expect: `cube frames 9 and 73 (under the bar) are the same picture, CRC 7ceb422a`, and 40 frames of one face: exit 1, no header / today's cube: 128 distinct frames, 66 with three faces and 62 with two; gpudiag_check without its order rule: 1 of test_gpudiag_check's 94 cases fails (`the cube: a frame out of order`) / all pass |
| L2 low: gpudiag_check ignored a cube line's frames shown: `60 drawn, 0 shown, 0 missed, 0 torn` passed with nothing checked | a line must show 58 to 61 frames, the video frames of its second (16.9) (6099f06) | gpudiag_check without the rule: 3 of the 94 cases fail (0, 57 and 62 shown pass) / all pass |
| L3 low: the 16-frame rate check (frames 1 to 15 within 32 video frames) passed a cube at 28 frames a second | every one of frames 2 to 15 at most two video frames after the one before (16.9); gpudiag-slow.mem in test-gpu-soc must fail it, and nothing else (db3e65c) | gpudiag-slow with the old rule: `(32 video frames, 32 shown, 3 apart at most) ok (6 checks)`, `GPUDIAG: PASS (147 checks)` / with today's: `FAIL frames 2 to 15 each at most 2 video frames after the one before (30 a second or more): up to 3`, `GPUDIAG: FAIL (1 of 147 checks failed)` |

Found on the way: test_gpu's check that the model's clamp changed no value and that no value was
wider than the RTL keeps came after the run's verdict, so it could not fail a run; it comes before
it now (e087dd4). And the review counted edge 0's left case among those gpudiag showed only in the
free run's cube frames: on the SoC, gpudiag as at bfaac48 and as at 3fea963 already failed its
mutant, by scene 13's CRC (at bfaac48 158 pixels of its mesh and its fan, whose slanted edges pass
through pixel centres); edge 1's left case is the one only the free run's cube frames would have shown (as
torn frames).

**What the bfaac48 bitstream lacks.** The gpudiag bitstream of section 14 carries gpudiag as it was
at bfaac48 (146 checks, scene 13 bbb6a4cb, the first cube). Of this round's checks it lacks: on the
SoC, the top-left rule's top cases of edges 0, 1 and 2 and the left case of edge 1 (the last would
show in its free run's cube lines as torn frames); the VRAM halves of the depth refusals and the
refused SET_ZBUF's own BAD_PARAM, so a TRIANGLE drawn without a depth buffer passes it; the frame
by frame rate rule, so a cube at 28 frames a second passes it; frames out of order in the free run;
and distinct cube frames (frame 73 stands for frame 9). No RTL changed, so its 146 checks pass on
today's RTL SoC as they did. Today's gpudiag_check fails its free run's lines (they have no frames
out of order): a capture of it goes to bfaac48's checker (section 14 step 4).
