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
| G3 | a SurfOS driver (boards/arty_a7) with shell commands (`gfx mode`, `gfx fill`, `gfx line`, `gfx text`, `gfx crc`) and a demo; **done in simulation and on the host (2026-10-25, section 15)**; then (2026-10-26, 15.8) GPU2's commands (`gfx tri`, `gfx zbuf`, `gfx zclear`, `gfx flip`, `gfx target`), `gfx cube` with every frame shown checked, and the re-review's three lows, **done in simulation and on the host**, with the review of that round's fixes (15.9) | the driver and `gfx` against the C model on the host (`test-gpuos-host`, with GPU2's commands against the GPU2 model and the cube's frames against their table); SurfOS on the RTL SoC with HAS_GPU (surfsim_gpu): every `gfx crc` the CRC of a frame at the VGA pins and, for graphics alone, the model's (GPU2's triangles worked out from section 16 alone); the overlay; the demo; `gfx cube 1`, every frame at the pins one of the table's; no GPU on QEMU virt and with HAS_GPU = 0 (`test-gpuos`, `test-os-qemu`) | none of its own: the kernel runs on section 14's DDR+GPU bitstream (GPU2's commands and the cube need G4's, 16.11) | **pending** (sections 15.6 and 15.8): the demo and the cube on the monitor; `gfx crc` equal to the model's |
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
of order in the free run, the cube's distinct frames; and, as its capture is judged with its own
checker, a cube line's frames shown and the free run's rate frame by frame), and gpudiag is to be
built again with them after the re-review, when the integrator says (no RTL changed: only the boot
ROM and the build id differ). Until then the steps below are those of the bfaac48 bitstream, with
that commit's checker and expected values (step 4); where the gpudiag of the review's fixes
differs, they say so. The SurfOS test (G2) stays on G2's DDR+GPU bitstream (e119a78, section 12.6),
which every gate proved: G4's DDR+GPU bitstream (bfaac48) passed every gate but LVS, whose proof is
outstanding (section 16.11). Once it has one, it takes e119a78's place in steps 6 to 8, with its
own build id, sha256 and `SoC 0.4.0`, and SurfOS then boots with the GPU2 engine in the design.
The table's two bitstreams come from two commits; section 14.2 builds both into one clone.

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
4. Check the capture against the build, with the checker and the expected values of the
   bitstream's own commit, bfaac48 (the three commands as they are, from the same root):

       git show bfaac48:surfcpu/sim/gpu/gpudiag_check.py > /tmp/gpudiag_check_bfaac48.py
       git show f6e14c4:surfcpu/docs/evidence/gpu-g4/gpuscenes_expect-bfaac48.txt > /tmp/gpuscenes_expect_bfaac48.h
       python3 /tmp/gpudiag_check_bfaac48.py --uart surfcpu/build/gpudiag-1.txt --frames - \
         --expect /tmp/gpuscenes_expect_bfaac48.h --cube-seconds 10 \
         --manifest surfcpu/build/fpga/arty_a7_100t/surfcpu_arty_a7_gpudiag/manifest.txt

   It passes only when no line shows a failure, every run that ended passed, the capture ends with
   one and its cube lines (the capture may end inside one), one at least is a first run, no run
   shows a build other than the manifest's (bfaac489) and one whole run at least shows that one, no
   run shows a dirty banner or bitstream line (the manifest says the tree was clean), and ten cube
   lines at least follow the last run, each with 30 or more frames drawn and none missed or torn.
   The first file is bfaac48's checker. The second is the gpuscenes_expect.h that bfaac48's build
   writes (a generated file, so not in git at bfaac48): f6e14c4 keeps it byte for byte (sha256
   6ad57705bb6ed81c0c66ad0c7e89f5f79ef62d893674726f8e0ade22f01c1779), and the clone of section 14.2
   writes the same file into its build/fw/gen/ at its last build-rom. `git show` takes both from
   the repository, whatever is checked out. **Not the tree's checker or the tree's expected
   values**: they are those of the gpudiag after the review of 3fea963 (section 16.12) and fail a
   bfaac48 capture that passes. That gpudiag makes 147 checks, so a run of 146 counts as a rerun;
   its scene 13 is 9368fe97 (bfaac48's bbb6a4cb) and its cube's CRCs differ; and the tree's checker
   wants cube lines with frames apart and out of order, which bfaac48's gpudiag does not print (the
   re-review of b2c765d, low 1: with bfaac48's header and the tree's checker, `FAIL: 0 of the
   cube's lines after the last run, 10 wanted`). The manifest must be a gpudiag build's that passed
   its checks. `--build-id bfaac489` names the build without the manifest, but does not know whether
   it was clean. `--accept-rerun` would take reruns alone, which is not this test.
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

## 15. SurfOS's driver and the `gfx` command (G3)

**Status (2026-10-25).** G3 is done in simulation and on the host: the driver and the `gfx`
command run against the C model on the host, and SurfOS on the RTL SoC with SurfGPU prints, for
every `gfx crc`, the CRC of a frame captured at the VGA pins and, where the display shows graphics
alone, the CRC worked out for the same commands from this document alone (gfxref); the GPU's
interrupt, the overlay, the demo, the bench and the two "no GPU" systems (QEMU virt, the SoC with
HAS_GPU = 0) pass too (section 15.4). An independent review's six medium and seven low findings
are fixed, each with its test (section 15.7). Nothing here has run on a board: the physical test
(section 15.6) is the integrator's, with the owner.
The brief is [plans/2026-10-25-gpu-g3-g4.md](plans/2026-10-25-gpu-g3-g4.md); the G3 commands use
GPU1's command set only, on a GPU1 or a GPU2 (G4's superset). The cube and GPU2's commands in
SurfOS (`gfx cube`, `gfx tri`, `gfx flip`, `gfx zbuf`, `gfx zclear`, and `gfx target`) came in the
round after G3 and G4 merged, with the three lows of G3's re-review: section 15.8 (2026-10-26),
done in simulation and on the host; their physical test is open too. An independent review of
that round found two mediums and four lows; they are fixed, each with its test (section 15.9).

### 15.1 The driver

| File | What |
|---|---|
| `boards/arty_a7/gpu.h`, `gpu.c` | the driver: pure code over `struct gpu_port`, as `sdcard.c` is over its SPI host |
| `boards/arty_a7/gpuhw.c` | the port on the board; the driver's entry in board.c's table; `gfx`'s kernel side |
| `boards/arty_a7/gfx.c` | the `gfx` command, pure code over the driver |
| `boards/arty_a7/gfxcube.c`, `gen/gfxcube_expect.c` | `gfx cube` and its expected frames' generator (15.8) |
| `boards/arty_a7/textcon.c` | CTRL.MODE in the text console's copy of CTRL; a panic switches back to text |
| `boards/arty_a7/board.c`, `arty.h` | `gpu` in the driver table, `gfx` in the board's commands, its line in `drivers` |
| `arch/riscv32/kernel.mk` | `driver/font8x16.c` compiled into the Arty kernel, from where it is, unchanged; since 15.8 gpudiag's `gpucube.c` too, and `gfx cube`'s table generated |
| `arch/riscv32/kshell.c` | `gfx` on a board without one (QEMU virt): "no GPU on this board" |
| `arch/riscv32/fbcon.c`, `include/arch/riscv32/riscv/board.h` | the display's panic hook |
| `boards/arty_a7/tests/gpu/` | the host test (`test_gpu.c`), the stand-in for the hardware (`gpustub.c`), `gfxscene.c` (gfx lines through the driver in the model), `gfxref.c` (test-gpuos's pictures from this document alone) |
| `boards/arty_a7/tests/gpuos_test.py` | SurfOS with `gfx` on the RTL SoC (`test-gpuos`) |
| `surfcpu/mk/area-gpuos.mk` | `test-gpuos-host` (on test-os and check), `test-gpuos` (on check), `gfxscene`, `gfxref`, `evidence-gpuos` |

No shared kernel file changed (kernel/, fs/, driver/, mm/, lib/, shell/, include/ outside
include/arch/riscv32/): the x86 kernel is as it was.

**Present** when sysinfo.FEATURES has GPU and video, gpu.ID reads "GPU1" (0x31555047) or "GPU2"
(0x32555047) and INFO gives a FIFO and 300 KiB. The driver names the two IDs itself
(`GPUDRV_ID_GPU1`, `GPUDRV_ID_GPU2`): G4 gives gpu.h's `GPU_ID_VALUE` GPU2's value. Without
FEATURES.GPU the gpu window (unmapped then) is never touched. Said once at boot:

```
+GPU: SurfGPU GPU1 (gpu.ID 0x31555047), VRAM 300 KiB at 0x30000000
+GPU: a 64-word command FIFO; fences wait on intc source 5 (IE.FENCE)
```

or `+GPU: none: sysinfo.FEATURES has no GPU (a bitstream without one)` (an unknown gpu.ID or an
INFO without a FIFO or 300 KiB is said with the value read); `drivers` lists `gpu` (ok or not
present) and its fences and errors. The probe ABORTs before it reads FENCE, the value fences are
counted from (a FENCE that a warm restart left in the FIFO cannot complete after that read), and
ends with the engine reset: ABORT, the sticky bits cleared, the reset target (640x480 at 0,
stride 640) and its clip.

**Command words** go to CMD only while FIFO_FREE says there is room: FIFO_FREE is read, that many
words are written, then it is read again. Two seconds in which FIFO_FREE stays 0 is a stopped
engine: said (with STATUS, FIFO_FREE and FENCE), ABORT, the command fails; the sticky errors found
then (an OVERFLOW is often why) are said and cleared with it, not left for the next command.

**Fences.** A submission that must be finished ends with `FENCE value` (with IRQ when the port has
the interrupt), the values counted from the FENCE register read at the probe. The wait polls FENCE
for 2 ms (`spin_us`), yielding between reads, then sleeps on the GPU's interrupt in 10 ms steps
(intc source 5, IE.FENCE: the handler clears FENCE_EV and sets an event) or, without an
interrupt path (no FEATURES.IRQ), sleeps 1 ms at a time; two seconds in which neither FENCE nor
FIFO_FREE moves is a stopped engine (50 ms once Ctrl+C has come to a run putting things back:
15.8's N1). The waits on the interrupt, and those it woke, are counted (15.8's N3). The poll
comes first because a task the interrupt wakes runs only at the next tick (the scheduler runs in
trap context; up to 10 ms later), so every short command would otherwise take a tick. `gfx bench`
polls its fences to the end and `gfx demo` for 20 ms, so their times are the engine's, not the
scheduler's.

**Errors.** After every finished submission STATUS's OVERFLOW, BAD_OPCODE and BAD_PARAM are read;
each one set is said on a line of its own in words, they are cleared (written 1), the engine is
ABORTed and the driver's target and clip are queued again (SET_TARGET, SET_CLIP), ahead of
anything sent after them: the known state. ABORT never waits for them (the FIFO is empty after
it), so a stopped engine costs no second timeout and Ctrl+C is not held up. Ctrl+C (the task's
interrupt flag) ends every wait the same way: ABORT, the target and clip (and on a GPU2 the depth
buffer) queued again; where it ends a wait that has nothing to ABORT (gfx crc's wait for frames),
it says only `interrupted (Ctrl+C)`. The run's result says which (GPU_EINTR, GPU_ECANCEL: 15.8's
N2).

**The claim.** A mutex, taken for one `gfx` command's run (trylock, then 20 ms sleeps that Ctrl+C
cuts short): a second console's command waits, saying who has the GPU, and the commands of two
consoles never interleave. The driver's state is its own struct, used only under the claim.

**Display modes** (CTRL.MODE 1 text, 2 graphics, 3 graphics with the text over it). textcon.c
keeps the copy of CTRL that it rewrites for the cursor, so the mode lives in that copy:
`arty_text_lock()` takes the console lock, `arty_text_set_mode()` sets the mode bits with
interrupts off, and the cursor's own read-modify-write is made with interrupts off too.
GFX_BASE, GFX_STRIDE, GFX_SCALE and the mode follow section 3's frame latch rule: the driver
samples the clock and STATUS.FRAME (it counts as a blanking begins) as one atomic pair, in a loop;
when FRAME moved between two samples taken less than 1 ms apart, the blanking began less than
1 ms ago, and the four stores are made at once in that same atomic stretch: within 1.3 ms of
VBLANK_EV, before the latch 1.4 ms after it, so the picture changes in one frame. A pair further
apart (an interrupt handler, another task) is not used. Without a blanking in 250 ms (no pixel
clock) the registers are written at once and that is said. The console lock is held meanwhile
(about a frame). A panic switches the display back to text (`board_display.panic`), so a panic's
report is seen over any picture.

**The palette** is left as the configuration made it (RGB332) unless a command loads another (a
warm reset keeps GPAL). Named palettes: `rgb332` (R4 = {R, R[2]}, G4 = {G, G[2]}, B4 = {B, B}, as
section 3), `grey` (entry n = (n >> 4) in each of red, green and blue: 16 greys of 16 entries) and
`ramps` (8 hues of 32 shades for G4's Gouraud: entry n is hue n >> 5 of white, red, green, blue,
yellow, cyan, magenta and orange (0xF80), shade i = n & 31, each component C of the hue becoming
(C i + 15) / 31: black at i = 0, the hue at i = 31). `gfx` names the palette GPAL holds.

**Text** is one EXPAND of 8n x 16 for a string of n cells, the rows of the console's own font
(`driver/font8x16.c`) packed as section 5 says, the words UTF-8 decoded as the console decodes them
(`driver/cp437.c`). **Pixels** through the CPU's VRAM window are the target's, after a fence.
**FRAME_CRC** of a still picture as section 3 says: after a fence, c0 = FRAME_COUNT, then
FRAME_COUNT until it is two on (sleeping 5 ms between reads), then FRAME_CRC.

### 15.2 The command

| | |
|---|---|
| `gfx` | the GPU: ID, VRAM, the engine and FIFO, the display mode, GFX_BASE/STRIDE/SCALE, the palette, the target, the clip, the depth buffer (GPU2, 15.8), the words and fences sent and FENCE, the interrupts taken, the fences that asked for one (15.9's L3) and the fence waits that slept on the interrupt and were woken by it (15.8's N3), errors (and by kind), timeouts, ABORTs, display changes, FRAME_COUNT and FRAME_CRC |
| `gfx mode text\|gfx\|overlay [1\|2]` | the display mode; for gfx and overlay the scale given (the current GFX_SCALE when not), GFX_BASE 0, GFX_STRIDE 640 or 320, and the target becomes that picture (640x480, or 320x240 at VRAM 0); text keeps the target and the picture |
| `gfx clear [c]` | FILL_RECT of the target, colour c (0) |
| `gfx fill x y w h c` | FILL_RECT |
| `gfx rect x y w h c` | the outline as four LINEs; its far corner (x + w - 1, y + h - 1) must be a coordinate; w or h 0 draws nothing |
| `gfx line x0 y0 x1 y1 c` | LINE |
| `gfx copy sx sy dx dy w h [key]` | COPY_RECT, with KEY when a key is given |
| `gfx text x y fg bg\|- words...` | EXPAND in the console's font; `-` transparent; the words joined by one space |
| `gfx pal rgb332\|grey\|ramps`, `gfx pal n rgb444` | a palette, or entry n |
| `gfx put x y c`, `gfx get x y` | one pixel of the target through the window at 0x3000_0000 |
| `gfx crc` | `gfx crc: frame N, crc XXXXXXXX (a frame begun after the last change)` |
| `gfx bench` | on the target: 8 fills of it, 8 copies of its left half onto its right half, 256 lines between points of it (chosen before the clock starts), 64 strings of 32 cells (as many as fit on a target narrower than 256), each wholly inside it; each from the first word to its fence, in pixels a second, the pixels counted those drawn |
| `gfx demo [seconds]` | the animation below, 10 s when not given (1 to 3600); Ctrl+C ends it |
| `gfx reset` | ABORT, the sticky bits cleared, the reset target and clip |
| `gfx target base stride w h` | SET_TARGET: the target another picture of VRAM (15.8) |
| `gfx zbuf base stride\|off`, `gfx zclear [b]` | GPU2: the depth buffer for the target, or none; every depth b * 257 (15.8) |
| `gfx tri [how] ...` | GPU2: a triangle, flat or Gouraud, with depth or not, culled or not, vertices with sixteenths (15.8) |
| `gfx flip [base]` | GPU2: FLIP, GFX_BASE written at the next vertical blanking (15.8) |
| `gfx cube [seconds]` | GPU2: gpudiag's spinning cube, every frame shown checked against the model's (15.8) |
| `gfx help` | the list |

Coordinates and sizes are section 5's (x, y -32768 to 32767, w, h 0 to 65535, colours 0 to 255),
in decimal or 0x hex (a leading zero is still decimal: 012 is twelve), in the target's pixels; the
clip cuts, nothing wraps. Every argument is checked before a word is sent (a usage line,
CMD_USAGE); a drawing command prints nothing when it worked and returns once its fence came and no
error was found (CMD_OK), else CMD_FAIL with what went wrong. A line of 16 words or more is
refused: the kernel shell hands a command 16 words and drops the rest without a word, so a `gfx`
line holds 15 (`gfx text` 9 of text). Every line `gfx` and the driver print fits the console's 80
columns, whatever the counts in it (the host test measures every line it sees, and prints each
message at its widest).

**The demo.** One target 320 wide and 496 high holds two pages of 320x240 (A at VRAM 0, B at 76,800)
and a row of sprites below them (a ball made with EXPAND and the font's ☻, on the key colour 0xE3).
The display shows a page at scale 2 (GFX_STRIDE 320). Each frame is drawn into the page not shown:
WAIT_VBLANK first (by then the page shown last has left the screen: the frame that showed it has
ended its last line), the clip on that page's rows, eight bands of colour, a turning fan of 16
lines, five bouncing boxes (FILL_RECT and an outline), six balls and two faces copied from the
sprite row with the key, a line of text; then a FENCE, and once it came GFX_BASE is written: a
single register, which the next frame latch takes whole. No frame ever shows a page being drawn
(the host test checks every engine word against the page on the screen). Before it starts the demo
keeps the display (mode, GFX_ registers), GPAL (it draws in RGB332), the target and clip, and the
158,720 bytes of VRAM it draws on (a copy in the kernel heap, four words a loop; without memory it
says so and runs anyway); when it ends, also on Ctrl+C or a stopped engine, it puts them all back
and the engine is idle. Whenever VRAM or GPAL changes on the way in or out, the display shows the
console (text mode, which shows neither): it switches to text, then keeps VRAM, loads RGB332 and
draws its first page; on the way out VRAM and GPAL go back under the console, then the target and
clip (queued: after a stopped engine, which was said and ABORTed once, nothing waits for the
engine again), then the display from before (since 15.8 this is `gfx_keep_begin()` and
`gfx_keep_end()`, which `gfx cube` uses too and which keep GPU2's depth buffer as well). So no
frame shows the old picture in the demo's palette or over its pages, or the demo's display while
its pages are overwritten (the host test checks every CPU write of VRAM
and GPAL, and every engine word, against what the screen shows then). It prints a line a second and
the frames a second at the end.

### 15.3 Decisions, and where this deviates from the brief

| Point | Done | Why |
|---|---|---|
| The driver's files | `gpu.c`/`gpu.h` the driver, plus `gpuhw.c` the board's port | the brief names gpu.c and gpu.h; the port split (sdcard.c and sd.c's) lets the host test run the driver's own code against the model |
| `gfx` on QEMU virt | answered by `kshell.c` after the board's table (`absent_cmds`), not listed by `help` | boards/qemu_virt/board.c is not this track's; QEMU's shell is otherwise as it was |
| After an error or a stopped engine | ABORT, then the target and clip queued, not waited for | waiting would double a stopped engine's timeout and hold up Ctrl+C; queued, they still reach the engine before anything later |
| The fence wait | polled for 2 ms, then the interrupt | the brief's interrupt wait; the poll avoids a tick's latency per command (15.1) |
| The frame latch rule | the blanking caught by atomic (clock, STATUS.FRAME) samples less than 1 ms apart | interrupts stay on (an interrupts-off wait for VBLANK_EV could last a frame and lose UART input); an interrupt handler could not take the console lock that textcon's copy of CTRL needs |
| The demo | also puts VRAM back | "puts the display back as it was": the picture shown before comes back whole (test-gpuos checks its CRC) |
| textcon.c | the cursor's change of the CTRL copy made with interrupts off; a panic hook | the mode's change and the cursor's must not interleave; a panic in a graphics mode would not be seen |
| `include/arch/riscv32/riscv/board.h` | `struct board_display` gains `panic` | the hook above (QEMU virt has no display, so nothing else changes) |
| `gfx put`, `gfx get` | in target coordinates | what `gfx fill` and the others use |
| The IDs | `GPUDRV_ID_GPU1`, `GPUDRV_ID_GPU2` | G4 changes `GPU_ID_VALUE` to GPU2's; the driver keeps accepting both whichever gpu.h it builds against |
| The host test's summary line | `test_arty: N of N checks passed` | it links boards/arty_a7/tests/harness.c, whose main() prints that name |
| A `gfx` line of 16 words or more | refused (usage) | the kernel shell drops the words past 16 without a word (arch/riscv32/kshell.c is not this track's); a picture drawn short would pass for a good one |
| The demo's way in and out | the console on the screen while VRAM and GPAL change | no frame shows a display over pixels or a palette not its own; the price is the console for a frame or two at each end |
| test-gpuos's expected CRCs | gfxref, from this document alone; gfxscene must agree with it | a mistake in gpu.c or gfx.c would be in gfxscene too, which runs them; gfxscene stays the owner's tool for other pictures |
| The review's finding 12, in kernel/sync.c's event_wait_timeout() | not changed | shared kernel code, not this track's. Its window (an event set between its check and its sleep wakes nobody) costs a fence wait at most one 10 ms step, after which FENCE is read again; the driver keeps its order (the event cleared, FENCE read, then the wait), so a FENCE done before the check is never waited for |

The host test's stand-in uses the C model's `struct gpu_engine` fields `base`, `stride`, `tw`, `th`,
`cx0`..`cy1`, `fence`, `sticky`, `writes`, `in_cmd`, `np`, `need`, `hdr` and `data_left`, and
`gpu_engine_idle()`: a change to those in G4's model needs the stand-in changed with it.

### 15.4 What was tested ([evidence](evidence/gpu-g3/))

| Test | Result |
|---|---|
| `make test-gpuos-host` (boards/arty_a7/tests/gpu/test_gpu.c, seeds 1, 7, 123; then gfxref) | 678 of 678 checks, 18 tests: every `gfx` drawing command's VRAM equal to a second model fed GPU.md's words (clear, fill clipped, line, copy with and without a key, overlapping, text opaque, transparent and UTF-8, rect pixel by pixel, put and get, both scales) and 120 random commands a seed; the FIFO rule (no word into a full FIFO or past the room read, with a slow engine that fills it); a stopped engine and a FIFO that stays full (said, ABORT, after 2 s of simulated time and not much more; a lost word's OVERFLOW said and cleared with it), recovery, Ctrl+C in a wait (an ABORT named only when there was one); BAD_OPCODE, BAD_PARAM and OVERFLOW said in words, cleared, ABORTed, the target and clip sent again; fences on the interrupt and with sleeps; two claimants (the second waits, saying who has the GPU, its words all after the first's; Ctrl+C gives up the wait); the probe (ABORT before FENCE is read; no GPU said with the value read); the frame latch rule (the four stores within 1.3 ms after a blanking, all in the frame after; a 2 ms stall's pair not used; no frames: written at once and said); FRAME_CRC the model's, of a frame begun after the command; the palettes; the demo (no drawing into the page shown, about 60 flips a second, VRAM, GPAL, display, target and clip back, Ctrl+C, no memory; every CPU write of VRAM and GPAL and every engine word made while the screen shows the console or the demo's other page; an engine stopped 0.5 s in: said once, everything back without a second timeout); the bench at four sizes (each rate's pixels those the engine wrote, each string the generator's x, y, fg, bg); reset; 33 usage errors refused with nothing sent, among them a line the shell may have cut; numbers in decimal and hex; GPU1 and GPU2; every line printed within 80 columns, each message also at its widest (t_columns). Then gfxref: its model's VRAM equal to its directly computed pixels, its CRCs to the model's display path |
| `make test-gpuos` (boards/arty_a7/tests/gpuos_test.py) | 25 of 25 checks: first gfxref, which works the test's pictures out from this document alone: its 37 gfx lines are the test's, crc 1 is 15.6's 039d507c, and gfxscene (the same lines through gfx.c and gpu.c in the model) gives its modes, CRCs and pictures; then SurfBoot boots the kernel from a card in build/sim/surfsim_gpu, the gpu.ID expected the one gpu.h gives (GPU1 here); 39 commands typed: the seven `gfx crc` FRAME_CRCs are each the CRC of a frame captured at the VGA pins, and the five in mode 2 gfxref's; the GPU's interrupt: 156 taken for the 156 fences sent between the first `gfx` and the last; the picture's CRC after `gfx demo 1`, and after a `gfx demo 5` that the Ctrl+C typed after its first second ended, is the one before them; the demo's 60 frames in about 1.06 s of simulated time; the bench's four rates; the last frame at the pins in mode 3: the console's text decoded from the pixels over the picture, every blank cell gfxref's picture exactly; side by side, build/sim/surfsim (HAS_GPU = 0): `+GPU: none`, `gfx` and `drivers` say there is no GPU. About 385 million cycles (15.4 s simulated) in 153 s |
| `make test-os-qemu` | 136 of 136 checks at -O2 and at -O0, the new one `gfx: no GPU on this board (qemu_virt)` |
| `make check` (big) | "check: all automated SurfCPU checks passed" at 2310338, the review round's last commit with code (the one after it adds this record), in about 34 minutes; before the round, at 4aa4c8f ([evidence](evidence/gpu-g3/g3-sim.txt)) |

What the RTL run measured (simulation, 25 MHz; the board's numbers may differ): FILL_RECT about
88 million pixels a second (the engine's 4 pixels a cycle, 100 million, less the commands' own
time), COPY_RECT about 43 million (2 cycles a word: 50 million), lines about 1.2 million and text
about 0.7 million pixels a second: those two are the CPU's, which builds every command word (and
the font's bitmap) fetching each instruction from DDR (no cache, about 2 million instructions a
second). The demo draws about 300 command words a frame and keeps up with 59.94 Hz in most frames.

Not tested: anything on the board (15.6); two consoles at once on the RTL system (the host test's
claimants only); `gfx` typed on the PS/2 keyboard (the UART was used); a real monitor.

### 15.5 Pending

The physical test, the integrator's with the owner (15.6; for GPU2's commands and `gfx cube`, step
7, on a DDR+GPU bitstream with GPU2 once 16.11's proof is done). Nothing else of G3 is open.

### 15.6 Trying it on the board (for the owner)

You need the board wired as for G1 and G2 (the Pmod VGA on JB and JC, a monitor, WIRING.md), the
card with SurfOS, and the UART console. Nothing here writes the card except step 2's deploy, and
nothing programs the flash.

1. **The bitstream**: the DDR+GPU design of section 14 (G2's table), its sha256 checked as step 6
   there says, then loaded:

       make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr_gpu

2. **The kernel with the driver** (from the branch the integrator merged G3 into): build it and put
   it on the card (OWNER-GUIDE.md section 7; the old kernel is kept as /BOOT/BACKUP.BIN):

       make -C surfcpu build-kernel
       make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS

3. **Boot**: the card in the Pmod MicroSD, `make -C surfcpu uart-console`, press RESET. Among the
   boot lines: `+GPU: SurfGPU GPU1 (gpu.ID 0x31555047), VRAM 300 KiB at 0x30000000` (GPU2 and
   0x32555047 with G4's bitstream) and `Drivers: serial gpio ps2kbd sdcard gpu`.
4. **A picture whose CRC is known**: type these at the prompt (the first line switches the monitor
   to the picture; the console is then on the UART only):

       gfx mode gfx 1
       gfx pal rgb332
       gfx clear 0x25
       gfx fill 16 16 200 120 0xe0
       gfx rect 8 8 216 136 0xff
       gfx line 0 479 639 0 0x1c
       gfx line 0 0 639 479 0xfc
       gfx copy 16 16 400 300 200 120
       gfx copy 0 0 240 160 64 64 0x25
       gfx text 24 200 0xff - Hello from SurfOS on SurfGPU
       gfx text 24 220 0x03 0xfc opaque text in the console font
       gfx put 639 479 0x92
       gfx crc

   It must print `gfx crc: frame N, crc 039d507c` (any N): the CRC of that picture as this
   document alone gives it (gfxref: the command words written from section 5 and fed to the C
   model, and the pixels computed directly, without SurfOS's code), which SurfOS printed on the
   RTL system and the frame at its VGA pins had. The
   monitor shows a dark blue screen, a red box in a white frame with its copy lower right and part
   of it again in the middle, a green and a yellow diagonal, white text and blue text on yellow,
   and one light pixel in the bottom right corner.
5. **The demo**: `gfx demo 10` (Ctrl+C ends it sooner): bands of colour, a turning fan, bouncing
   boxes and balls at 320x240, without tearing; the UART prints a line a second and the frames a
   second at the end, and the picture of step 4 comes back (`gfx crc` prints 039d507c again).
   `gfxscene` (`make -C surfcpu gfxscene`) prints the model's CRC for any other list of gfx lines,
   one per line in a file of the repository: `surfcpu/devenv/run.sh surfcpu/build/gpuos/gfxscene
   FILE` from the repository's root (it is built for the container, not the Mac).
6. **Text over the picture**: `gfx mode overlay` shows the console over the picture (type `help`,
   say); `gfx mode text` gives the plain console back. `gfx` prints the GPU's state; `gfx help` the
   commands.

7. **GPU2's commands and the cube** (15.8), with a DDR+GPU bitstream that carries GPU2 (G4's,
   section 16.11, once its proof is done: the boot line then says `+GPU: SurfGPU GPU2 (gpu.ID
   0x32555047), ...`; on G2's GPU1 bitstream these commands say `needs GPU2`). Type:

       gfx mode gfx 2
       gfx pal ramps
       gfx zbuf 153600 640
       gfx zclear
       gfx clear 0
       gfx tri 8 8 120 16 40 100 0x3f
       gfx tri cullcw 200 20 300 20 250 60 0xff
       gfx tri cullccw 200 20 300 20 250 60 0x7f
       gfx tri 20 140 60 140 60 180 0x5f
       gfx tri 20 140 60 180 20 180 0x9f
       gfx tri gouraud,ztest,zwrite 150.5 70.25 1000 0x20 300.0625 100 60000 0x3f 200 230.75 30000 0x28
       gfx tri gouraud,zle,zwrite 140 230 30000 0x40 310 220 30000 0x5f 250 60 30000 0x50
       gfx tri gouraud 80 200 0 0xc0 140 236 0 0xdf 70 238.5 0 0xd0
       gfx crc

   It must print `gfx crc: frame N, crc 43e2d407`: the CRC of that picture as section 16 alone gives
   it (gfxref), which SurfOS printed on the RTL system. The monitor shows, on black: a red triangle
   top left, a blue one top right (the second of two with the same corners: the first, clockwise,
   culled), a square split along its diagonal into green and yellow with no gap, two shaded
   triangles, red and green, passing through each other, and a shaded magenta one at the bottom.
   Then `gfx cube 10`: the cube spins for ten seconds; the UART says a line a second, `cube N s: 60
   drawn, 60 shown, 0 missed: all whole, in order` (59 or 61 here and there: the display makes
   59.94 frames a second, and a frame can fall by the second's edge), and at the end the frames
   drawn, the rate (59.9 a second or a little less, never more) and `display back as it was`, then
   `N shown, 0 missed: all whole, in order`; `gfx crc` then prints 43e2d407 again.

What to send back: the UART capture (with the boot lines, the `gfx crc` lines, the demo's summary
and, with GPU2, the cube's lines) and what the monitor showed at steps 4 to 7. A CRC other than
039d507c (steps 4 and 5) or 43e2d407 (step 7), a line saying `a stopped engine` or `the GPU
reports`, a cube line with a frame missed or that is not `all whole, in order`, or a picture that
tears is a FAIL to keep and look into, as section 14 says of the G1 and G2 tests.

### 15.7 The review (2026-10-25)

An independent review of G3 at 484f1f7 found six medium and seven low findings, and found sound
the FIFO credit, ABORT with the target and clip queued, the wrap-safe fence comparison and the
interrupt's acknowledgement, the frame latch sampler, the page flips, the claim and Ctrl+C,
textcon's interrupts-off read-modify-writes, the bounds and the board side. The fixes are
84296c3..2310338 and this record (2310338 mends two slips of this round's own test-gpuos commits
that its first run found: a FAIL_ON entry surfsim could not take as a regular expression, and the
GPU's name overwritten by a loop variable). Each host-test fix was checked by putting its bug back
into today's code alone and running test_gpu (seeds 1, 7 and 123):

| Finding | Fix | Test: the bug put back / after |
|---|---|---|
| 1 medium: `gfx text` lost every word after the tenth without a word: the kernel shell hands a command 16 words and drops the rest | a line that fills the 16 is refused, saying why (CMD_USAGE); the stand-in splits a line as kshell does (c4121fe) | t_usage: a 12-word text refused, 9 words of text drawing all 17 cells: 2 checks fail / pass |
| 2 medium: the demo put the display back before VRAM and GPAL, and on the way in loaded GPAL and copied VRAM under the display from before | the console on the screen whenever VRAM or GPAL changes, on the way in and out (336b58a; VRAM kept and put back four words a loop) | t_demo_order and t_demo_stop log every CPU write of VRAM and GPAL and every engine word with what the screen shows then: 484f1f7's way in and out, 51 of 678 checks fail / all pass |
| 3 medium: an engine that stopped during the demo left graphics on the screen and the target at 320x496 (the put-back's own wait timed out) | after a stopped engine, said and ABORTed once, nothing waits for it again: display, palette, VRAM, target and clip back at once (391cae5) | t_demo_stop, the engine stopped 0.5 s in (from text, and from a picture): 484f1f7's end of the demo alone, 42 checks fail (a second timeout; display, target and clip not back) / pass |
| 4 medium: test-gpuos passed with the GPU's interrupt dead | `gfx` typed again after the demos and the bench; between the first `gfx` and that one, the interrupts taken must be more than none and at least half the fences sent (each FENCE asks for one); irq.c's line for a source switched off for staying pending fails the run (fa736af, 2310338) | a trial kernel whose probe writes IE = 0 (not kept), test-gpuos: 24 of 25, `FAIL the GPU interrupt (intc source 5, IE.FENCE): 0 taken for the 156 fences sent between the first gfx and the last`; the old test passed such a kernel (the reviewer's p_noirq) / 25 of 25, 156 taken for 156 |
| 5 medium: the mode-2 CRCs expected, and 039d507c, came from gfxscene, which runs gpu.c and gfx.c | gfxref works the test's pictures out from this document alone; its lines are checked to be the test's, and gfxscene must agree with it (1537776) | a gfx.c that joins the words of `gfx text` without their space: SurfOS and gfxscene agree on its CRCs (814667fb for crc 1), so the old test passed; now its gfxscene check fails before the RTL run / gfxref gives 039d507c, f345a76e and 309c14df, what SurfOS printed before: 039d507c stands |
| 6 medium: test-gpuos expected GPU1, and G4's surf_gpu.v reads GPU2 | the ID it expects is gpu.h's GPU_ID_VALUE (6a9c411, 2310338) | its reading of this branch's gpu.h gives GPU1, 0x31555047, of sc/gpu-g4's GPU2, 0x32555047 (G4 not merged) |
| 7 low: after a submission ended early (a FIFO that stayed full), the next command reported its sticky error | the errors are said and cleared at the stopped engine and at Ctrl+C (f575b07) | t_fifo, a lost word then a FIFO that stays full: 2 checks fail / pass |
| 8 low: lines of 81 to 92 columns (usage lines, the stopped engine's, drivers' gpu line, gfx's fences) | every line laid out for its widest case: the counts at 10 digits, FENCE, FIFO_FREE's and FRAME_COUNT's widest, the widest target and clip, the longest names; drivers' line from gpu_describe() (6d2bd5a) | every test measures each line printed, t_columns prints each message at its widest: with the old gfx.c and gpu.c, 15 lines past 80 in t_columns alone, and t_usage, t_fifo and t_demo_stop fail / none |
| 9 low: strtol's base 0 read 010 as octal | numbers are decimal (a leading zero too) or 0x hex (702ddd5) | t_usage, 010, 08, 012, -0x2, 0XfF: 1 check fails / passes |
| 10 low: the bench counted text pixels past the target's right edge, and took a call's four arguments from its generator in no set order | each string wholly inside the target; the four numbers taken one statement at a time (85bae18) | t_bench at 640x480, 320x240, 200x100 and 64x32, every rate against the pixels the engine wrote for its opcode, every string the generator's x, y, fg, bg: 228,800 written of 262,144 counted at 640x480 / equal at every size |
| 11 low: the probe read FENCE before its ABORT | ABORT first (0fd2047) | t_probe: 1 check fails / passes |
| 12 low: in kernel/sync.c's event_wait_timeout() | not changed (15.3) | none |
| 13 low: (a) Ctrl+C in `gfx crc` said "the engine stopped (ABORT)"; (b) at boot, gpuhw.c printed the ID after its complaint about INFO; (c) two steps labelled crc 5; (d) test_gpu.c's `vb` read uninitialised (clang) | (a) an ABORT is named only after one (f575b07); (b) one line, `+GPU: none:` with the value read (0fd2047); (c) crc 6 and crc 7 (8e850ee); (d) set (84296c3) | (a) t_fifo: 1 check fails / passes; (b) t_probe checks the line; (c) none; (d) test_gpu.c builds with clang -Werror, as with gcc |

The stand-in grew with them: its CPU stalls come between atomic stretches, as an interrupt does
(4418ea7, after the probe's ABORT moved a stall into one), and it measures every line printed,
counts the pixels the engine writes by opcode, logs the CPU's VRAM and GPAL writes and the engine's
words with the display shown, and names a holder of any length.

### 15.8 The cube in SurfOS, GPU2's commands and the re-review's lows (2026-10-26)

The round after G3 and G4 merged (the brief's "After both: the cube in SurfOS"), on `sc/gpu-cube`
from `sc/gpu-integ` 9b9b7b8, with G4's fix round (`sc/gpu-integ` 687b57e) merged in at c82a629:
`gfx cube`, GPU2's commands in `gfx`, and the three lows the re-review of G3 left. It is done in
simulation and on the host; nothing has run on a board.

| File | What changed |
|---|---|
| `boards/arty_a7/gpu.c`, `gpu.h` | GPU2 in the driver: `gpu2` (gpu.ID "GPU2"), the depth buffer kept as the target is (an accepted SET_TARGET drops it; `gpu_abort()` sends it again after the target and clip), `gpu_zbuf_set/off/clear()`, `gpu_tri()`, `gpu_flip()` (FLIP; the CPU's store of GFX_BASE is now `gpu_set_base()`), `gpu_pump()`, `gpu_fence_next()`, `gpu_frame_next()` and the port's tick (`gpu_tick_start()`, `gpu_tick_stop()`) for the cube; on a GPU1 each GPU2 call is GPU_ENOGPU2 and sends nothing; N1, N2, N3 (below) |
| `boards/arty_a7/gfx.c` | `gfx target`, `zbuf`, `zclear`, `tri`, `flip`, `cube`; the demo's way in and out as `gfx_keep_begin()` / `gfx_keep_end()`, which keep the depth buffer too; `gfx` prints the depth buffer, from a copy of the driver's state taken under the claim |
| `boards/arty_a7/gfxcube.c` | the cube's run, its sink and its check of the frames shown |
| `boards/arty_a7/gen/gfxcube_expect.c` | the expected table's generator, a host tool of the build |
| `arch/riscv32/kernel.mk` | `surfcpu/firmware/diagnostics/gpucube.c` compiled into the Arty kernel from where it is; the generator built and run (`build/riscv32/arty_a7/gen/gfxcube_expect.h`) |
| `boards/arty_a7/gpuhw.c` | `irq_wait()` says whether the interrupt came (N3); the port's tick, a periodic ktimer, for the cube's looks |
| `boards/arty_a7/tests/gpu/` | the stand-in (GPU2 and a GPU1 that refuses its opcodes; FLIP at the blanking; no blanking without a pixel clock; FRAME_CRC of each frame as it ended; the port's tick every 10 ms; and knobs: another console's claim at an ABORT or a release, a handler that wakes nobody, a slow console, a spoiled byte of the picture shown, the pixel clock stopping), test_gpu.c's new tests, gfxref (section 16 from this document alone), gfxscene (the GPU gpu.h describes) |
| `boards/arty_a7/tests/gpuos_test.py`, `surfcpu/mk/area-gpuos.mk` | GPU2's pictures and `gfx cube 1` on the RTL SoC; the host build of the cube and its table |
| `boards/qemu_virt/test_os.py` | `gfx tri`, `flip` and `cube` say there is no GPU (as G3 added `gfx`'s check) |

No contract and no shared kernel file changed (gpu.h of the firmware, spec/soc.toml, kernel/, fs/,
driver/, mm/, lib/, shell/ and include/ outside include/arch/riscv32/ are as they were), and
gpucube.c, the model and the RTL are G4's, used unchanged.

**GPU2's commands** (GPU.md section 16), on a GPU2; on a GPU1 each says `needs GPU2 (SET_ZBUF,
TRIANGLE, FLIP); this engine is GPU1` and fails (CMD_FAIL) with nothing sent. Every argument is
checked first (a usage line, CMD_USAGE), as for G3's commands.

| | |
|---|---|
| `gfx target base stride w h` | SET_TARGET (GPU1's command, which `gfx` lacked: without it nothing could be drawn into picture B to flip to): VRAM bytes, section 5's rule checked first; the clip becomes the whole target, and on a GPU2 the depth buffer goes |
| `gfx zbuf base stride` | SET_ZBUF for the target the driver has, 16.2's rule checked first (refused with the target's size said) |
| `gfx zbuf off` | SET_TARGET to the same target, which drops the depth buffer, then the clip as it was |
| `gfx zclear [b]` | every depth b * 257 (0xff when not given: 65535, the farthest): 16.6's FILL_RECT of b over the depth bytes with a target set to them (2W bytes a line at the buffer's stride), then the target, the clip and SET_ZBUF again; without a depth buffer, or for a target 16,384 or more wide (whose depth bytes are no target), it says so and fails |
| `gfx tri [how] x0 y0 x1 y1 x2 y2 c` | a flat triangle of colour c, depths 0 |
| `gfx tri [how] x0 y0 z0 c0 x1 y1 z1 c1 x2 y2 z2 c2` | each vertex with its depth (0 near to 65535 far) and colour; without `gouraud` the triangle is c0's (the header's colour) |
| `gfx flip [base]` | FLIP to base (a multiple of 4 up to 0x7fffc; the target's base when not given), done once the engine wrote GFX_BASE at the next blanking |
| `gfx cube [seconds]` | below; 10 s when not given, 1 to 3600 |

`how` is words joined by commas, in any order: `flat` (the default) or `gouraud`; `ztest` (draw where
nearer: ZTEST, ZFUNC less) or `zle` (nearer or as near: ZTEST, ZFUNC less or equal); `zwrite`;
`cullcw`, `cullccw`. A word twice, `flat` with `gouraud` or `ztest` with `zle` is refused, and the
flat form refuses `gouraud`, `ztest`, `zle` and `zwrite` (they need each vertex's depth and colour).
`ztest`, `zle` and `zwrite` without a depth buffer say so and fail. x and y are pixels with
sixteenths, 12.4's range -2048 to 2047.9375: decimal with a fraction that is a whole number of
sixteenths (`12`, `12.5`, `-3.0625`, `.25`; `0.3` is refused), or `0x` hex pixels. The 15-word
limit of a `gfx` line (15.2) holds the longest form exactly.

**`gfx cube [seconds]`** (`boards/arty_a7/gfxcube.c`) runs gpudiag's cube (16.9) in SurfOS:

* **The frames** are gpucube.c's own: `gpu_cube_frame()`, compiled into the kernel from
  `surfcpu/firmware/diagnostics/` unchanged, records each frame's 105 words through a `gpu_sink`
  whose `cmd()` appends to the run's record; the run sends them through the driver with
  `gpu_pump()`, the FIFO_FREE rule without waiting (between the words it empties the looks' ring,
  below, and prints its lines). Frame k goes into picture A (k even) or B (k odd) of 16.6's layout
  (A, B and the depth buffer: all of VRAM): the depth buffer cleared, the picture cleared,
  SET_ZBUF, twelve Gouraud triangles with ZTEST and ZWRITE, a FLIP to it, and a FENCE whose value
  is the driver's next (`gpu_fence_next()`), so that FENCE counts the frames drawn and the
  driver's own fences stay after them.
* **The display** goes through the driver and textcon, never through gpucube.c's writes:
  CTRL.MODE 2 through textcon's copy of CTRL, GFX_STRIDE 320 and GFX_SCALE 2 at the frame latch
  (15.1), GFX_BASE by the FLIPs, and the palette that `gpu_cube_display()` makes (six ramps of 32
  and a dark blue background), taken from the GPAL writes it hands the sink (which reach nothing)
  and loaded by the driver. Text RAM, the text palette, SCROLL and the cursor stay the console's:
  the console keeps working (its output goes on to the UART and to text RAM, which mode 2 does not
  show) and comes back whole. Mode 2 rather than 3, since the console's text, which changes every
  second, would be part of every frame's CRC.
* **The way in and out** is the demo's (`gfx_keep_begin()`, `gfx_keep_end()`): the display, the
  palette, the target, the clip, the depth buffer and all 307,200 bytes of VRAM kept first (a copy
  in the kernel heap; without memory it says so and runs anyway) and put back after, with the
  console on the screen whenever VRAM or the palette changes. Frame 0 is drawn into A and its FLIP
  waited for while the console is shown (with `gpu_sync()`, whose FENCE asks for the interrupt:
  gpucube.c's does not); then the display changes, at the latch (since 15.9 the run's first lines
  are said before that, with the console still on the screen).
* **The sink** has no context pointer and no error return: it only records, into the run's own
  record on its stack, found through a static pointer that only the GPU's claimant sets and that
  it clears before letting go (one `gfx cube` at a time, as heaptest and selftest claim theirs).
  The driver's errors and Ctrl+C are handled where the run sends the words.
* **The looks.** FRAME_COUNT is read from the timer interrupt, every tick (10 ms, less than a
  frame's 16.7: no frame can end and the next begin between two looks), through the port's tick
  (a periodic ktimer in gpuhw.c); a count that moved goes into a ring of 16 with that frame's
  FRAME_CRC, which the run empties as it goes. A first version looked from the run's loop, between
  its words: on the RTL SoC it missed two of 66 frames, after the line it printed at the end of
  its first second, since a console line costs SurfCPU about a frame (the console compares its
  whole screen of 2,400 cells from DDR). The stand-in shows the same with each line costing 40 ms
  (`t_cube_look`: with the tick 184 of 184 frames checked, without it 183 of 186). On a port
  without the tick the run still looks from its loop (and since 15.9 fails when frames pass unseen).
  Since 15.9 a look also reads FENCE just before and just after the pair, and keeps its time.
* **The check.** Each FRAME_COUNT that moved has its FRAME_CRC (section 3's consistent pair: that
  frame's) looked up in `gfx_cube_expect[]`. A CRC that is none of the turn's 128 frames is a torn
  frame: not a whole drawn frame. When the turn's pictures all differ, their order is checked too:
  frame f shown d frames after the last whole frame l must have (f - l) mod 128 <= d, since each
  FLIP waits for a blanking (the same frame again, f = l, is a frame the CPU or the engine was late
  for, and is in order); this is gpudiag's rule for its free run (16.9). Since 15.9 (the review's
  M2) the frame must also be in step with the FLIPs done, and the frames shown must go on as many
  frames as the FLIPs; a frame shown that no look saw is missed and fails the run (L1). Before
  G4's fix round frames 9 and 73 of a turn were one picture (GFX_CUBE_PICTURES 127), and gfx cube
  said that it checked only that each frame shown was one of the turn's; since that round the cube
  stands on a corner and the 128 pictures differ: the table followed by itself (its frames 0 and
  15 are 62bae657 and 7e872f01, gpudiag's checked frames), and the order is checked, with no change
  here.
  The frames shown in a second are that second's video frames, as gpudiag counts them; the rate is
  printed and not judged (SurfOS shares the CPU; test-gpuos wants 30 a second or more, and since
  15.9 at most the display's 59.94).
* **The expected table** is worked out at build time by `boards/arty_a7/gen/gfxcube_expect.c`: the
  same `gpu_cube_frame()` through the C model, each frame rendered as gfx cube shows it (MODE 2,
  GFX_STRIDE 320, GFX_SCALE 2, the base its FLIP gives, `gpu_cube_display()`'s GPAL) and its
  FRAME_CRC taken; nothing of the driver or gfx. `arch/riscv32/kernel.mk` builds and runs it for
  the kernel (`build/riscv32/arty_a7/gen/gfxcube_expect.h`), `area-gpuos.mk` for the host test
  (the two are the same file). It fails the build when a frame leaves the engine inside a command,
  with an error, with its FLIP to the wrong picture or its FENCE not done, or when the model's clamp
  or wide values were needed. Its first 16 CRCs are gpudiag's own MODE 2 frames
  (`gpuscenes_expect.h`'s `gpu_cube_expect_check`), which the host test checks: the same display.
* **What it says** (as since 15.9): a line a second, printed just after frames were looked at,
  `    cube 1 s: 60 drawn, 60 shown, 0 missed: all whole, in order`, with drawn the FLIPs done after
  frame 0's, shown the video frames, missed those that no look saw; `all whole` alone when the
  order is not checked; else `N torn, M out of order`. At the end the frames drawn, the time (from
  just before the blanking at which the cube's display began to the first sighting of the last
  FLIP done) and the rate, whether the display went back, the frames shown and missed for the whole
  run, and the first torn frame's FRAME_COUNT and FRAME_CRC if there was one; `ended by Ctrl+C`
  after Ctrl+C. It fails (CMD_FAIL) on a torn, out-of-order or missed frame, Ctrl+C, a stopped
  engine or an error. Until 15.9 the lines said `N checked` (those looked at) in place of missed,
  and drawn counted one FENCE too many.
* **Ending it**: the time running out (the last frame's FLIP done and the frame showing it looked
  at, 200 ms at most), Ctrl+C, a stopped engine (`gpu_pump()`: two seconds without a word taken or
  FENCE moving, which a FLIP with no blanking also comes to) and an error after a frame (STATUS read
  as FENCE moves) each end it with the engine idle and everything put back as the demo puts it back.

**The re-review's three lows** (of G3 at 258182f, line numbers there), each fixed and tested; each
test was run with the fix and with the bug put back (the driver's files at the commit before the
fix, the new test kept):

| Finding | Fix | Test: the bug put back / after |
|---|---|---|
| N1: gfx.c:677-680, Ctrl+C on a dead engine still cost an uncuttable 2 s `gpu_sync()`: the demo's way out waits through Ctrl+C (`keep_going`), so nothing could cut those two seconds | once Ctrl+C has come to a run with `keep_going`, a stopped engine is one that made no progress for GPU_QUICK_US, 50 ms (its ABORT emptied the FIFO; what was sent since takes a live engine microseconds); the message says which span (`for 2 s`, `for 50 ms`); a wait that Ctrl+C cuts short at once yields rather than spins (5a0a75e) | t_ctrlc_dead: the engine stops 0.5 s into `gfx demo 5`, Ctrl+C 0.1 s later: all over 2065 ms after the Ctrl+C, 705 of 711 / 113 ms, the stopped engine said once after 50 ms, everything back, 711 of 711; t_cube_end the same for `gfx cube` |
| N2: gpu.c:104 and gfx.c:124, `intr_aborted` was cleared before the trylock (while another console's run may hold the GPU) and read after the release | the flag is gone: the run's result says it, GPU_EINTR when Ctrl+C ABORTed the engine, GPU_ECANCEL (new) when there was nothing to stop (the claim given up, gfx crc's wait for frames, the bench between steps); `gfx` prints the driver's state from a copy taken under the claim, and `gfx mode` the target it read under it (c70d5b7, 320ab49) | t_ctrlc_said: the stand-in runs an "other console" at an ABORT (its claim gives up) and at the release (its claim taken; in the second case its own Ctrl+C ABORTs): 687 of 693, both checks failing on each seed / 693 of 693 |
| N3: gpuos_test.py:307-313, the interrupt check counted the handler's entries, not wake-ups: a handler that ran without waking anyone passed, every fence wait ending by its 10 ms step | the port's `irq_wait()` says whether the interrupt came; the driver counts the fence waits that slept on the interrupt and those it woke, a wait once however many 10 ms steps it slept (a FLIP waits up to a frame: on the RTL SoC with GPU2's commands, counting steps gave 6 and 2 woken); `gfx` says `fence waits that slept on it: W, woken by it: K`; test-gpuos wants, between the first `gfx` and the last, K > 0 and 2K >= W, as well as interrupts for half the fences (e7207f6, 662c14c). The one wait the interrupt still did not wake was `gfx cube`'s on its frame 0, whose FENCE (gpucube.c's) asks for no interrupt: it waits with `gpu_sync()` now (7cadace; t_cube: every fence wait of the cube woken, 1013 of 1014 with the old wait) | a trial kernel whose `gpu_isr` counts and does not call `event_set` (not kept), at e7207f6: `156 taken for the 156 fences sent ...; 1 waits on it, 0 of them woken by it`, 24 of 25 (the old check passes it: 156 taken for 156) / 25 of 25, `1 waits on it, 1 of them woken by it`; host: t_fences, three full-screen fills 3 waits 3 woken, a handler that wakes nobody 2 more waits none woken (`gfx` says so), a wait of two steps that the interrupt ends one wait woken |

**Decisions, and where this deviates from the brief**

| Point | Done | Why |
|---|---|---|
| The sink | records a frame's words into the run's record; the run sends them through `gpu_pump()` | the sink has no context pointer and no error return: recording needs neither, and the driver's errors and Ctrl+C are handled where the words are sent, under the claim |
| Sending a frame | `gpu_pump()`, never waiting, not `gpu_put()` | a full FIFO behind a FLIP that waits for a blanking would hold the run for up to a frame; never waiting, it empties the looks' ring and prints its lines on time |
| The looks | from the timer interrupt every tick, through the port (not asked) | looking from the run's loop lost frames while it printed (above); a tick (10 ms) comes more often than a frame (16.7 ms), whatever the task does |
| The palette | `gpu_cube_display()`'s, taken from its GPAL writes and loaded by the driver | the cube's colours as gpudiag shows them, from gpucube.c itself rather than a copy; nothing else of `gpu_cube_display()` is used |
| The cube's display | mode 2, not 3 | the console's text changes every second and would be part of every frame's CRC |
| The order check | only when the turn's 128 pictures differ (the brief), which they do since G4's fix round; measured from the last whole frame shown; since 15.9 also in step with the FLIPs | before that round frames 9 and 73 were one picture; after a torn frame the cube's place is not known, so the next whole frame may be two on |
| `gfx target` | added | without it nothing could be drawn into picture B for `gfx flip` to show |
| `gfx flip` without a base | the target's base | draw into a picture with `gfx target`, then show it |
| `gfx zclear [b]` | any byte, 0xff by default | the brief's FILL_RECT 0xFF is the default; another byte gives depth b * 257, which the tests use |
| `gfx tri`'s two forms | 7 numbers (flat, no depth) or 12 (each vertex's depth and colour) | a 2D triangle needs no depths; the 12-number form with a `how` word is 15 words, the most a `gfx` line holds |
| N2 | the flag removed, Ctrl+C said by the result code | no state of the struct says what one run did, so no claimant can change another's |
| The cube's host tests | run once, not once a seed | nothing in them depends on the seed: three runs would be the same run |
| `boards/qemu_virt/test_os.py` | three more `gfx` lines checked | the brief asks QEMU virt to say there is no GPU for the new commands; G3 added `gfx`'s check to the same file |

**What was tested** ([evidence](evidence/gpu-g3/cube-sim.txt)); simulation and the host only:

| Test | Result |
|---|---|
| `make test-gpuos-host` | 1014 of 1014 checks, 31 tests (G3's 18 and 13 new), seeds 1, 7 and 123 (the cube's tests run once: nothing in them depends on the seed). New: t_ctrlc_said (N2), t_ctrlc_dead (N1), t_fences' wake-ups (N3); t_gpu2: each GPU2 command's VRAM, depth buffer included, equal to a second model fed section 16's words (zbuf, zclear with 0xff and 0x80, flat, culled both ways, the top-left rule's square with each of its 1,600 pixels drawn once, Gouraud with depth in front and behind, sixteenths, zbuf off, target, FLIP at the blanking); t_gpu2_random: 120 commands a seed, every flag, the whole 12.4 range, depth clears, against the reference; t_gpu2_rules: a full FIFO (no word into it), a stopped engine (the depth buffer sent again after the ABORT), a FLIP that never comes (no pixel clock: a stopped engine after 2 s), Ctrl+C while a FLIP waits, a refused SET_ZBUF (BAD_PARAM said, cleared, the depth buffer sent again), a GPU1 (each says it needs GPU2 and sends nothing); t_usage2: 46 usage errors with nothing sent, and what is taken (hex, fractions, the ends of 12.4, how's words); t_columns2; t_cube_table: 128 frames, 128 pictures, the first 16 gpudiag's own; t_cube_check: the check alone; t_cube: `gfx cube 2`, 122 drawn (as it then said: 121 FLIPs after frame 0's, 15.9's M1), 122 shown, 122 checked, all whole and in order, every frame logged at the stand-in's pins between the cube's first and last one of the turn's and in step, nothing drawn into the picture shown, VRAM, GPAL, display, target, clip and depth buffer back, every fence wait woken; t_cube_end: Ctrl+C, a stopped engine (said once), Ctrl+C on one (50 ms), the pixel clock stopping (a FLIP that never comes: bounded), a spoiled byte (1 torn, the run fails), no memory; t_cube_order: VRAM and GPAL written only with text on the screen; t_cube_look: the tick's looks with a slow console. Then gfxref: 11 `gfx crc`, its model's VRAM and its pixels computed directly (section 16's triangles too) equal, and the two frame CRCs |
| `make test-gpuos` | 32 of 32 in 225 s (it was 156 s): the G3 steps as before, then GPU2 (15.6's step 7): `gfx zbuf`, `zclear` and eight `gfx tri` lines (flat, culled clockwise and drawn counter-clockwise, the split square, Gouraud with depth through each other with ztest and zle, Gouraud without depth, sixteenths), crc 6 43e2d407; picture B drawn with `gfx target` and a ninth `gfx tri`, and shown by `gfx flip`, crc 7 651474fa; `gfx flip 0`, crc 8 = crc 6; each a frame at the VGA pins and gfxref's, which works them out from section 16 alone; `gfx cube 1`: 62 drawn in 1.07 s, 57.4 a second, as it then said (61 FLIPs after frame 0's: one FENCE too many was counted, and the time began after the display change and its line, 15.9's M1), 66 shown, 66 checked, all whole and in order, its line a second `61 drawn, 60 shown, 60 checked: all whole, in order` (60 FLIPs), and at the VGA pins 71 frames of the turn in one run with no other frame between, each showing the cube's frame of the one before it or the next; crc 9 = crc 8 after it; the interrupt: 181 taken for the 243 fences sent (62 of them the cube's frames', which ask for none: 181 for the 181 that asked, 15.9's L3), 4 fence waits slept on it and 4 woken; without a GPU, `gfx tri`, `flip` and `cube` say there is none. 507 million cycles (20.3 s simulated) in 224 s |
| `make test-os-qemu` | 137 of 137 at -O2 and at -O0: `gfx tri`, `gfx flip` and `gfx cube` say there is no GPU on QEMU virt |
| `make check` (big) | passed on 9160aad, the commit that added this record (the code of 7cadace), 10:55 to 11:34 with the wait for Docker's slots: `check: all automated SurfCPU checks passed`. In it: test-gpuos-host 1014 of 1014 and gfxref's 11 CRCs; test-gpuos 32 of 32 in 224 s, `gfx cube 1` 62 drawn in 1.01 s (60.9 a second, as it then said: 61 FLIPs, 15.9's M1), 64 shown, 64 checked, all whole and in order, 70 frames of the turn at the pins, 0 out of step, the interrupt's 4 fence waits all woken; test-os-qemu 137 of 137 at -O2 and -O0; G4's gpudiag_check PASS |

### 15.9 The review of 58ac7b3 (2026-10-26)

An independent review of the cube round (58ac7b3, merged into `sc/gpu-integ` as a84d299) found
nothing high, two mediums and four lows, all fixed on `sc/gpu-cube`, each with a test that fails
with the bug put back (or with the review's mutant) and passes with the fix. The host runs are the
host test built as `make test-gpuos-host` builds it, in the toolchain container (gcc 14.2), seeds
1, 7 and 123, each with its commit's test and the driver's files as named (in
`surfcpu/build/review-fix/`, not kept); the Mac's clang gave the same counts. Line numbers are
58ac7b3's ([evidence](evidence/gpu-g3/cube-review.txt)).

| Finding | Fix (commit) | Test: the bug put back / the fix |
|---|---|---|
| **M1**: `drawn` and the rate counted one frame too many (gfxcube.c:195, 215, 238, 280). Since 7cadace frame 0 is waited for with `gpu_sync()`, whose FENCE follows frame 0's, and drawn, FENCE less frame 0's value, counted it: `gfx cube 1` said `62 drawn in 1.01 s, 60.9 a second`, above the display's 59.94. The rate's time began after the display change and its line | drawn is FENCE less the sync's value (frame k's is that + k): the FLIPs done after frame 0's. The time counts from just before the blanking at which the cube's display began (`gpu_screen_set()` keeps the latch's last sample before STATUS.FRAME moved, `latch_us`), so FLIP k comes at the k-th blanking after it or later; the run's first lines are said before the display changes (ba925bc). The time ends at the first sighting of the last FLIP done, by a look or by the run (f3f5767: the run's own sighting came after the line it printed as its second ended, and a FLIP every blanking read 56.6 a second) | t_cube: drawn equal to the FLIPs the stand-in did after frame 0's, fewer than the video frames shown, 59.0 to 59.9 a second (its summary parser had read `N shown` as drawn: sscanf counts conversions only); with the driver's files of 6ecf03a 1013 of 1015 (`122 drawn: the FLIPs the engine did after frame 0's (121); 122 shown; 60.4 a second`) / 1015 of 1015. t_cube_look: `gfx cube 1` with each console line costing 40 ms, 59.0 to 59.9 a second; with fba05d5's gfxcube.c 1042 of 1043 (`61 drawn, 58.2 a second`) / 1043 of 1043. test-gpuos: drawn fewer than shown, 30 to 59.94 a second (below) |
| **M2**: nothing checked that the cube turns (gfxcube.c:145; gpuos_test.py:402-413; test_gpu.c:1515-1533): the same frame again was in order without limit; with the review's mutant (`gpu_cube_frame(&sink, 0, ...)`: every frame frame 0) gfx cube said `all whole, in order` and the host test passed | each look reads FENCE just before and just after the FRAME_COUNT and FRAME_CRC pair (`gpu_fence_now()`). A whole frame is in order when, besides being at most d on from the last, it is one of fb - 1 to fa + 1 as FLIPs counted after frame 0's: the frame a look counts showed what the latch took after the blanking before it, the frame FLIPped last by then, so the last FLIP done at the look or the one before; a FLIP whose FENCE the CPU has not yet sent allows one more. So the same frame again is in order only while at most one FLIP came since. A run that ran its time must have seen its frames go on as many frames as the FLIPs done (`the frames shown went A on for D FLIPs`). At the stand-in's pins and test-gpuos's VGA pins the cube must turn: 110 and 120 of the turn's pictures in the host test's 2 s and 3 s, 50 in test-gpuos's 1 s, each new one within two video frames of the one before from frame 2 on, gpudiag's rate rule (c484e14; bb60fe3 lets test-gpuos read that line) | t_cube_check: a cube standing still while its FLIPs go on, one two behind its FLIPs, one two ahead of the FENCEs read, a FLIP between the two reads; with `in_step()` always true and no end check 1018 of 1027 / 1027 of 1027. M2's mutant before the fix (ba925bc's files and test) 1015 of 1015, on the fix 1017 of 1027 (the run fails itself, `the frames shown went 0 on for 121 FLIPs`, `122 shown, 0 missed: 0 torn, 122 out of order`; 1 picture at the pins). test-gpuos on a kernel with the mutant: it fails at its model replay (`gfx cube 1` returns 1 in gfxscene); past the replay (its exit status ignored), on the RTL SoC, `1 pictures` at the VGA pins and SurfOS's own `the frames shown 0 on for 61 FLIPs, 66 shown, 0 missed: 0 torn, 66 out of order` |
| **L1**: the verdict ignored frames never looked at (gfxcube.c:151-155, 295): t_cube_look's run without the tick said `186 shown, 183 checked: all whole, in order`, CMD_OK | frames shown that no look saw are missed: each line says `N drawn, N shown, M missed`, as gpudiag counts them, `all whole` only with none missed, and the run fails with any (6ecf03a) | t_cube_look's run without the tick must fail and say them; with the verdict ignoring them 1012 of 1014 (`186 shown, 3 missed`, CMD_OK) / 1014 of 1014 |
| **L2**: gfx cube's error ending untested (gfxcube.c:272): with the review's M3 (no STATUS check after each frame) the host test passed | a test (ebd1b53) | t_cube_end: frame 30's SET_ZBUF stride arriving as 642 (BAD_PARAM): said once, CMD_FAIL, the run ending at that frame (29 to 31 drawn, nothing torn), the engine idle, VRAM, GPAL, the display, the target and the clip back, no depth buffer left; with M3 1035 of 1036 (`61 drawn`: the run went on, a torn frame was shown, the error was said only at the end) / 1036 of 1036 |
| **L3**: test-gpuos's interrupt rule counted FENCEs that cannot interrupt (gpu.c:282-285; gpuos_test.py:22, 360-367): `gfx cube 1` made it 181 for 243, and a `gfx cube 3` step would have failed a correct kernel | the fences sent with IRQ are counted apart (`irq_fences`); `gfx` says `fences that asked for it: N (gfx cube's frames' do not)` under the interrupts, and test-gpuos wants interrupts for half of those (fe100cf; its state step's pattern followed in fba05d5) | t_cube_look around its `gfx cube 3`: `4 taken for the 4 fences that asked for one (of 184 sent)`; with the cube's FENCEs counted as asking 1036 of 1037 (`4 taken for the 184`) / 1037 of 1037 |
| **L4**: 15.8 gave t_usage2 47 usage errors (46); gfx.c:88's help said `cullcw\|cullccw`, where both are taken, and gfx.c:578 `cullcw,cullccw` | the help and `gfx tri`'s usage both say `flat\|gouraud,ztest\|zle,zwrite,cullcw,cullccw` (54a1451); 15.8's count, and the numbers M1 changed in 15.6 and 15.8, corrected here | t_usage2: the two agree; with the previous gfx.c 1037 of 1040 / 1040 of 1040 |

**Decisions, and where this goes beyond the review's suggestions**

| Point | Done | Why |
|---|---|---|
| Frames missed (L1) | no tolerance: one fails the run | the looks come from the timer interrupt every 10 ms and a frame lasts 16.7, so a frame passes unseen only when a tick is held off more than 6.7 ms or lost, which nothing does while gfx cube runs on an idle SurfOS (0 missed in every run with the tick, the RTL SoC's included). One missed on the board is a FAIL to look into, as gpudiag's is |
| The rate's start (M1) | just before the blanking at which the cube's display began | frame 0's FLIP comes before the display change, whose own wait (up to a frame) would count; the first FRAME_COUNT step after the change can be the blanking of FLIP 1 and would overstate. From the latch, FLIP k is at the k-th blanking after it or later |
| The rate's end (M1) | the first sighting of the last FLIP done | the run's own sighting can come a line later (above); a look's is within a tick of the FLIP and never before it |
| The start lines (M1) | said before the display changes | a line costs SurfCPU a few frames: at 9160aad frame 0 stood for five video frames while the first line printed; now for one |
| The in-step window (M2) | fb - 1 to fa + 1 | fb - 1 to fa is exact when each FLIP's FENCE is in the FIFO behind it; the CPU may still hold a frame's last words while its FLIP is done (a line printing), so one more |
| The cube turning at the pins (M2) | 50 of the turn's pictures in test-gpuos's 1 s, each new one within two video frames of the one before | gpudiag's rate rule (30 a second); on the RTL SoC each came one video frame after the one before |

**What was tested** ([evidence](evidence/gpu-g3/cube-review.txt)); simulation and the host only:

| Test | Result |
|---|---|
| the host test, each fix and its bug put back | the table above: 16 variants in the toolchain container, and the same counts on the Mac |
| `make test-gpuos` | at f3f5767, 32 of 32 in 229 s ([evidence](evidence/gpu-g3/cube-review-test-gpuos.txt)): `gfx cube 1` 61 drawn in 1.02 s, 59.5 a second, 65 shown, 0 missed, all whole and in order, its line a second `60 drawn, 60 shown, 0 missed: all whole, in order`; at the VGA pins 66 frames of the turn in one run, 62 pictures (frame 0 for one video frame, each of 1 to 60 for one, 61 for five, until the display went back), 0 out of step, none more than two video frames after the one before; the interrupt 181 taken for the 181 fences that asked for one (243 sent), 4 fence waits slept on it, 4 woken. At fba05d5, before f3f5767: `61 drawn in 1.07 s (56.6 a second)`, and 4 waits slept, 3 woken: one wait ended by its 10 ms step, which kernel/sync.c's event_wait_timeout() gap allows (15.3, finding 12; a trial kernel that reports a wait timed out with its event set, a few instructions longer, had 4 of 4 and none: the timing moves with the code). On a kernel with M2's mutant: above ([evidence](evidence/gpu-g3/cube-review-test-gpuos-m2-mutant.txt)) |
| `make check` (big) | passed on b884c0b, the commit that added this record (the code of f3f5767), 14:06 to 14:43: `check: all automated SurfCPU checks passed`. In it: test-gpuos-host 1043 of 1043 and gfxref's 11 CRCs; test-gpuos 32 of 32 in 232 s, the same run as at f3f5767 to the cycle (509,781,324): `gfx cube 1` 61 drawn in 1.02 s, 59.5 a second, 65 shown, 0 missed, all whole and in order, 66 frames of the turn at the pins, 62 pictures, the interrupt's 4 fence waits all woken; test-os-qemu 137 of 137 at -O2 and -O0; G4's gpudiag_check PASS |

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
rules were left as they were here; the integration branch settled them in 5fd27ce (BOARD.md
section 8): tools/fpga-build.sh now reads the build identity that make compiled into the design's
own boot ROM (gen/fw_build.h), refuses a ROM of another build or one older than that file, and
counts a ROM built from a dirty tree as a dirty bitstream (BUILD_FLAGS bit 0 and the manifest's
source_dirty), so a build like these first two can no longer pass as clean.

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
at bfaac48 (146 checks, scene 13 bbb6a4cb, the first cube), and a capture of it is judged with
bfaac48's checker and bfaac48's expected values (section 14 step 4: today's fail it, by its check
count, its CRCs and its cube lines, which have no frames apart or out of order). Of this round's
checks it lacks: on the SoC, the top-left rule's top cases of edges 0, 1 and 2 and the left case of
edge 1 (the last would show in its free run's cube lines as torn frames); the VRAM halves of the
depth refusals and the refused SET_ZBUF's own BAD_PARAM, so a TRIANGLE drawn without a depth buffer
passes it; the frame by frame rate rule, so a cube at 28 frames a second passes its checks; frames
out of order in the free run; and distinct cube frames (frame 73 stands for frame 9). Its checker
lacks the rules for the cube's lines that came after it: 58 to 61 frames shown (L2: a line of 60
drawn and 0 shown, nothing checked, passes it) and the frame by frame rule in the free run (the
re-review's low 2, below: it wants 30 drawn in each second instead, which the cube's 60 a second in
simulation meet). No RTL changed, so its 146 checks pass on today's RTL SoC as they did.

**The re-review of b2c765d** (2026-10-26) found the five findings above fixed and three new lows.
The fixes are 3b9c8d0 to f14638e and this addendum; again no RTL and no model changed, and each fix
was checked by putting its bug back ([evidence](evidence/gpu-g4/review-fix.txt), its last part).

| Finding | Fix | Test: the bug put back / after |
|---|---|---|
| low 1: section 14 step 4's command ran the tree's gpudiag_check.py and expected values, which give a false FAIL on a capture of the bfaac48 bitstream (the re-review's probe: with bfaac48's header, `FAIL: 0 of the cube's lines after the last run, 10 wanted`); only a note under it said to take bfaac48's checker | the command takes bfaac48's checker with `git show`, and bfaac48's expected values, a generated file and so not in git at bfaac48, from f6e14c4, which keeps it byte for byte as bfaac48's build wrote it (f6e14c4, 6b58c41); the list of what the bfaac48 bitstream lacks names the checker's rules that came after it (6b58c41) | bfaac48's gpudiag ROM, built from a git archive of bfaac48 (gpudiag.mem aaee817d..., the manifest's rom_sha256), on the RTL SoC to the cube's tenth line: b2c765d's step 4 FAIL four times (scene 13's bbb6a4cb against 9368fe97; the cube's lines not the tree's, so 0 of the 10 wanted; 146 checks taken for a rerun) / step 4 pasted as it is: `PASS (146 checks; ...; the cube 10 seconds, 60 to 60 frames drawn a second, none torn or missed)` |
| low 2: the checks take a frame at most two video frames after the one before (29.97 a second), but gpudiag_check wanted 30 frames drawn in each second of the free run, which that rate misses in about one second of 32 | the claim stated as the rule: a frame at least every second video frame, 29.97 a second on this 59.94 Hz display, the brief's 30+; the free run's line prints apart (16.9), and gpudiag_check judges it by the checks' rule, more than 2 apart failing, with fewer than 29 drawn (what the rule makes of a second) kept as a backstop; gpudiag-half.mem and gpudiag-slowfree.mem in test-gpu-soc (50a152c, f14638e) | test_gpudiag_check's six new rule cases with the same numbers in b2c765d's line form, against b2c765d's checker: 29 drawn and 2 apart fails (twice), 3 apart passes with 30 and with 59 drawn / today's: 100 of 100 as wanted; gpudiag-half.mem at 50a152c with 40 s of free run: the checks `(30 video frames, 30 shown, 2 apart at most) ok`, `GPUDIAG: PASS (147 checks)`, every line `2 apart` with 30 drawn, but 31 in the first second (frame 0's FENCE falls in it) and 29 in the 16th: its lines in b2c765d's form fail b2c765d's checker (`the cube's second 16: 29 frames drawn, 59 shown, ... (fewer than 30 frames a second)`) / today's checker passes them (`29 to 31 frames drawn ..., 2 apart at most`); gpudiag-slowfree.mem, 10 s: `GPUDIAG: PASS (147 checks)`, then every line `3 apart` with 30 or 29 drawn: b2c765d's checker (its form) fails it only by second 3's 29 drawn, seconds 1 and 2 (30 drawn, 3 apart) passing it / today's fails the first second by its `3 apart` |
| low 3: refused_view's comment called its depth words those of pixels 0 and 1 of both rows; the fourth, 0x20000 + 20 * 128 + 40, holds pixels 20 and 21 of row 20 | the comment (3b9c8d0) | by reading: byte 40 of a row of 16-bit depths |
