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
  SET_TARGET, SET_CLIP, FILL_RECT, COPY_RECT, LINE, EXPAND, WRITE_RECT, FENCE, WAIT_VBLANK.

The GPU exists only in designs built with it (`HAS_GPU = 1`: sysinfo.FEATURES.GPU). Every other
design is unchanged: without it the vram and gpu windows are unmapped, MODE 2 and 3 are black
and the new video registers answer with errors, as before. In this version the GPU is a SurfBus
slave only (SURFBUS.md is unchanged).

**Status (2026-10-18).** G0 (this document and the contract changes) is done. G1 (VRAM, the
graphics scan-out, GPAL, FRAME_CRC, the CPU window, FEATURES.GPU, gpudiag's CPU-drawn scenes) and
G2 (the engine) are done in simulation and in synthesis: the gpudiag design passes every gate of
the open flow and its netlist runs gpudiag to PASS (section 12). Two independent reviews'
findings are fixed (sections 12.4 and 12.5), and the bitstreams of section 12.5 can be rebuilt to
their sha256 (section 14). Their physical tests (section 14) are open: nothing has run on a board.
Section 11 has the milestones.

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
| 0x000 | ID | 0x31555047 ("GPU1"): this command set |
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

Any other opcode sets STATUS.BAD_OPCODE; that word is dropped and the next word is read as a
command header (so after an error software should ABORT and start again).

**The target** is a rectangle of pixels in VRAM: pixel (x, y), 0 <= x < W, 0 <= y < H, is the
byte at base + y * stride + x. SET_TARGET sets base, stride, W and H and the clip to (0, 0, W, H).
It is refused (BAD_PARAM, nothing changes) unless: base and stride are multiples of 4, base <
307,200, stride < 65,536, W and H < 32,768, and either W = 0, H = 0, or (W <= stride or H = 1) and
base + (H - 1) * stride + W <= 307,200. So every pixel of a target is in VRAM and two lines never
share a word. Reset: base 0, stride 640, 640 x 480.

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
sysinfo.FEATURES has GPU (bit 8). With 0 both regions go to the decoder's error slave and nothing
of the GPU is built. The I decoder never maps them (no fetch). The board top takes `HAS_GPU` from
the define `ARTY_HAS_GPU`, as it takes `HAS_DDR` from `ARTY_HAS_DDR`.

## 8. Clocks, crossings and timing

| Signal | Crossing | Block |
|---|---|---|
| VRAM | port A sys_clk, port B pix_clk | dual-clock block RAM (no logic crosses) |
| GPAL | port A sys_clk, port B pix_clk | dual-clock block RAM |
| GFX_BASE, GFX_STRIDE, GFX_SCALE (32 bits) | sys -> pix | `surf_cdc_word` (its constraints: BOARD.md) |
| FRAME_CRC, FRAME_COUNT (48 bits) | pix -> sys | `surf_cdc_word` |
| vertical blanking start (to the engine's WAIT_VBLANK) | pix -> sys | the existing `surf_pulse_sync` |

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
errors, then draws a list of scenes (`firmware/diagnostics/gpuscenes.c`) and, for each, prints
the FRAME_CRC of a frame that started after the scene was complete and compares it with the
value the C model computed for that scene at build time (`build/fw/gen/gpuscenes_expect.h`). It
ends with `GPUDIAG: PASS (124 checks)` or `GPUDIAG: FAIL (...)`, then shows the last scene and
echoes the UART; Ctrl-R runs it again. A rerun cannot see the GFX_ values a reset leaves (the run
before changed them): it says so in a line under its banner ("a rerun (Ctrl-R): ...") and makes
123 checks, passing none it does not make. PASS means every check of the run's kind ran and
passed: a missing FEATURES.GPU, a gpu window that traps or reads another ID than "GPU1", each
engine scene that could not run, and a run with another number of checks than a complete one of
its kind (GPUDIAG_CHECKS 124, GPUDIAG_RERUN_CHECKS 123) all fail it, and `sim/gpu/gpudiag_check.py`
wants exactly that total from the capture's final run, which must have ended (sections 12.4, 12.5).

**What gpudiag checks of the engine, and what it does not.** On the board it checks: gpu.ID and
INFO; idle state; a full FIFO behind a WAIT_VBLANK, OVERFLOW, and the drain at the next blanking;
an unknown opcode and a refused SET_TARGET (sticky, and the refused target not taken); the error
interrupt (through intc with IE.ERR only); a fence and its interrupt (IE.FENCE), cleared; ABORT (the
FIFO empty and the engine idle at once, nothing queued running later); BUSY held while a WRITE_RECT
waits for its last data word; the FRAME pair (sections 3, 4.1); and through the scenes' CRCs
FILL_RECT at every alignment and edge with a clip, LINE in every octant, overlapping COPY_RECT in
eight directions with the key, EXPAND opaque and transparent, WRITE_RECT, WAIT_VBLANK with a page
flip, 320x240 doubling, the overlay. It does not check, and only the RTL tests do: OVERFLOW through
the error interrupt; BAD_PARAM from SET_CLIP or a drawing command; ABORT in the middle of a drawing
command (gpudiag aborts behind a WAIT_VBLANK); COPY_RECT from sources outside the target; random
command streams against the model; the engine waiting behind the CPU on VRAM's port A; BUSY at
cycle precision (it can only see BUSY while the engine waits for data); GFX_ placements that wrap at
512 KiB, stride 0 and the scale values 0 and 3; GPAL written in mid-frame; a reset of the pixel
domain alone. The scenes are written once and run in three places: on the CPU (writing the
gpu, video and vram registers), in the host model (the expected CRCs), and in the RTL tests.

## 11. Milestones

Each milestone lists its simulation, synthesis and physical tests. The physical tests are the
integrator's, run with the owner; a milestone is not done while its physical test is open.

| Milestone | Work | Simulation | Synthesis | Physical (integrator, owner) |
|---|---|---|---|---|
| G0 | this document, spec/soc.toml (soc_version 0.3.0), make gen, MODULES.md, SOC.md | `make check-generated` | | |
| G1 | surf_vram, the graphics scan-out, GPAL, FRAME_CRC, the CPU window, FEATURES.GPU, the C display model, gpudiag's window and CPU-drawn scenes, the gpudiag design | VRAM unit test with random CPU and engine traffic; surf_video's graphics modes captured and compared with the model, FRAME_CRC equal to the model's and to the captured pins' CRC; the old video tests unchanged; gpudiag on the RTL SoC (surfsim) prints the model's CRCs | gpudiag through the open flow, every gate; its netlist runs gpudiag as the RTL does | load the gpudiag bitstream: its UART transcript ends in PASS with the expected CRCs; the monitor shows the scenes (the owner looks) |
| G2 | surf_gpu, its C model, the engine's scenes in gpudiag, both designs | directed tests of every command and edge case, randomized command streams compared with the model at every FENCE, mutants caught; gpudiag's engine scenes in surfsim | gpudiag and ddr_gpu through the open flow, every gate; dram_clk's margin recorded | gpudiag PASS with the engine scenes; ddr_gpu boots SurfOS from the card as the DDR design does |
| G3 | a SurfOS driver (boards/arty_a7) with shell commands (`gfx mode`, `gfx fill`, `gfx line`, `gfx text`, `gfx crc`) and a demo | the kernel's driver against the RTL SoC with HAS_GPU (surfsim), its commands' FRAME_CRCs against the model | ddr_gpu with the driver's kernel | the demo on the monitor; `gfx crc` equal to the model's |
| G4 | triangles (flat, then Gouraud in the palette), the 320x240 double-buffered mode with page flips at vertical blanking, a z-buffer in VRAM (16-bit, 150 KiB at 320x240), perhaps a 16-bit colour format (RGB565 at 320x240) | model-first tests for every new command; tearing checked frame by frame with FRAME_COUNT | the designs with the new engine; timing of the wider data paths | a spinning cube at 30+ frames a second without tearing |
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
test-release 53 tests), and test-gpu-mutants catches 33 of 33 in 5 minutes; the same mutants
against 589223c's tests and gpudiag, and the flows of 589223c and of now side by side, are in the
[evidence](evidence/gpu/review2-sim.txt).

## 13. Risks

* **Block RAM 83 %.** VRAM's 76 RAMB36 spread over most of the 100T's block RAM columns, so its
  multiplexers and the CPU's read path span the die. At 25 MHz there is room; the DDR design's
  80 MHz dram_clk had +1.13 ns and placed in three tries of four, and the added block RAM and
  routing may cost it placements. The DDR+GPU design met it with +1.24 ns (section 12.2), +0.84 ns
  (12.4) and +0.087 ns (12.5): nextpnr's estimate passes, but the margin is almost gone. If that
  bitstream fails its memory test on the board, its other seeds (2 to 8) are the first thing to
  try, and registering VRAM's port signals next to each bank (G5) would give the router room.
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

## 14. The physical tests (the integrator's, with the owner)

Nothing here has run on a board. These are the steps for G1 and G2 on the owner's A7-100T, with
the Pmod VGA on JB and JC and a monitor (WIRING.md); no card or keyboard is needed for gpudiag.

1. Build and load the diagnostic: `make -C surfcpu build-rom`, then on the Mac
   `make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_gpudiag` and
   `make -C surfcpu program-volatile DESIGN=surfcpu_arty_a7_gpudiag` (volatile: the flash is not
   touched). Both GPU designs try the placer seeds 1 to 8 in turn, each for at most 30 minutes
   (pnr-timeout), so a placement router2 cannot route no longer stops the build. The bitstreams
   of section 12.5 are in the sc/gpu worktree's `build/fpga/arty_a7_100t/<design>/`. They rebuild
   to the same sha256 only from a clean checkout of 72ba06c (the build id is the commit's, the
   build time the epoch's), with the same images, on the same kind of host:

       git checkout 72ba06c        # sc/gpu; no tracked file changed
       export SOURCE_DATE_EPOCH=1792346703 LC_ALL=C TZ=UTC   # git log -1 --format=%ct 72ba06c
       make -C surfcpu build-rom
       make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_gpudiag
       make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr_gpu

   on an Apple Silicon Mac (Darwin-arm64, the containers aarch64) with surfcpu-toolchain:2026-10-15
   (image sha256:e4389c3c6d93...) and surfcpu-openxc7:2026-10-15 (sha256:b802ed1bac37...; its chip
   database's sha256 is checked against devenv/openxc7/chipdb.sha256). Seed 1 comes first in both
   design lists and routed; `PLACER_SEEDS=1` pins it. Expected: gpudiag.mem 117f4bea...,
   surfboot.mem be8aa3df..., `surfcpu_arty_a7_gpudiag.bit` 359b44e0...6a85 and
   `surfcpu_arty_a7_ddr_gpu.bit` 0eaca076...92c3 (the full values in section 12.5). A second build
   to compare was not made.
2. Watch the UART (`make -C surfcpu uart-console`) and press RESET: gpudiag prints its identity
   (board Arty A7-100T (0x100), SoC 0.3.0, features 17f: no DDR), its sections, one line per scene
   with its FRAME_CRC, and `GPUDIAG: PASS (124 checks)`, as the synthesized netlist did in
   simulation (section 12.5). If the port opened after that run, type Ctrl-R: the rerun says so
   under its banner and ends `GPUDIAG: PASS (123 checks)`. Every scene's CRC must be the one in
   section 14.1; `sim/gpu/gpudiag_check.py --uart <capture> --frames - --expect
   build/fw/gen/gpuscenes_expect.h` checks a capture (its last run, which must have ended).
3. The monitor: each scene shows for about a twentieth of a second while it is checked; the last
   picture (the demo: colour bands, boxes, rays, "SURFGPU" in large letters, a yellow frame) stays,
   with a green bar on the top text row saying `GPUDIAG: PASS (...)` (red with FAIL). The owner
   confirms the picture: square corners, the frame on all four edges, nothing cut off.
4. G2 also: the DDR+GPU design boots SurfOS from the card exactly as `surfcpu_arty_a7_ddr` does
   (`make build-fpga FLOW=openxc7 DESIGN=surfcpu_arty_a7_ddr_gpu`, then the m7 steps of
   PHYSICAL-TESTS.md with `DESIGN=surfcpu_arty_a7_ddr_gpu`); SurfBoot's monitor command `info`
   shows features 1ff (its label list predates the GPU bit).

### 14.1 Expected FRAME_CRCs

The C model's values for gpudiag's scenes (`build/fw/gen/gpuscenes_expect.h`, written by `make
build-rom`; `make gpu-scenes` writes each scene's frame as `build/fw/gpuscenes/scene-NN.ppm`). They
do not depend on the build: the same numbers on every bitstream with this gpudiag.

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
| 12 | engine: the demo (stays on the screen with the result bar) | f501eb46 |

In simulation (2026-10-18, `make test-gpu-soc`) every one of these was also the CRC of a frame the
VGA capture took at the RTL SoC's pins.
