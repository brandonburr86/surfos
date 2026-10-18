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

**Status (2026-10-18).** G0 (this document and the contract changes) is done; G1 and G2 are in
progress. Section 12 has the record, section 11 the milestones. Nothing has run on the board.

## 1. Decisions and where this deviates from the brief

The architecture was decided by the integrator (the SurfCPU GPU brief of 2026-10-17); this
section records the refinements and the deviations.

| Point | Decided | Done | Why |
|---|---|---|---|
| VRAM size, ports | 300 KiB TDP BRAM, A 32-bit sys (CPU first), B 32-bit read pix | as decided; 19 banks of 16 KiB (the last 12 KiB); CPU and engine answer one cycle after the access, the pixel port two pix_clk edges after the address | one-cycle answers keep SurfBus rule 7 ("block RAM answers in 1 cycle"); the pixel port has a register after the bank multiplexer, where timing is free |
| CPU window | 0x3000_0000, decodes 512 KiB, error above 300 KiB, byte enables, no fetch | as decided | |
| Engine on port A | CPU priority, bounded short CPU latency | the CPU's request takes port A in the cycle it is offered; the engine waits that cycle | the CPU never waits, so its latency is exactly the scratch RAM's |
| Graphics modes | MODE 2 graphics, MODE 3 graphics + text | as decided, with the proposed overlay rule | |
| GFX_BASE, GFX_STRIDE | byte offset; stride | both must be multiples of 4 (bits 1:0 read 0) | the scan-out reads aligned words; an unaligned base would need a byte rotator for no use |
| GFX_SCALE | 1 or 2 | 2 doubles; 0 and 3 act as 1 | every value has a defined meaning |
| GPAL | 256 x RGB444 | block RAM (a RAMB18): RGB332 when the FPGA is configured, not changed by a reset | a 3,072-bit register file would cost about 3,000 flip-flops; the text palette keeps its reset |
| FRAME_CRC | CRC-32 of the composited visible pixels | zlib's crc32 over each pixel as a little-endian 16-bit word, plus FRAME_COUNT (frames counted with the CRC, crossing with it) | FRAME_COUNT tells software which frame a CRC belongs to (section 4.4) |
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

**Banks.** Nineteen banks of 4,096 words (16 KiB), the last one 3,072 words (12 KiB); word
address bits 16:12 choose the bank and 11:0 the word in it. Each bank is written like
`surf_textram` (its proven dual-clock form): port A in sys_clk with byte write enables, reading
only when it does not write (the block RAM's NO_CHANGE mode: READ_FIRST is unsafe with the
other port on another clock, UG473), port B in pix_clk, read only. A pixel read at the instant
the same word is written may show either value in that one frame; memory is never corrupted.
The expected mapping is 4 RAMB36 per full bank (4Kx9, one byte lane each, as Yosys maps the text
RAM) and 3 for the last: about 75 RAMB36.

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
`surf_cdc_word` and are taken at the same point as CTRL, CURSOR and SCROLL: the start of the last
blanking line (line 524). A frame is drawn with one set of values, so software that writes
GFX_BASE after STATUS.VBLANK_EV, or any time, flips pages without tearing: the new base shows
from the next frame whose latch it reaches. VRAM itself is not latched: what is written while a
frame is shown shows in that frame from that point down.

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
ended, and FRAME_COUNT says which one it was.

**Checking a still picture** (the diagnostic's method, section 10). After the last VRAM or
register change, read c0 = FRAME_COUNT, then wait until FRAME_COUNT - c0 >= 2: the frame counted
c0 + 2 started after the change, whatever moment within a frame it came at, so its CRC is that
of the final picture. Two frames at 59.94 Hz: at most 33.4 ms.

## 4. Registers

The tables are in [MEMORY-MAP.md](MEMORY-MAP.md) (from spec/soc.toml); this section explains
them.

### 4.1 video (new registers, only with FEATURES.GPU)

| Offset | Register | Meaning |
|---|---|---|
| 0x080 | GFX_BASE | VRAM byte offset of the top-left pixel; bits 18:2 (multiple of 4); reset 0 |
| 0x084 | GFX_STRIDE | bytes per VRAM line; bits 15:2 (multiple of 4); reset 640 |
| 0x088 | GFX_SCALE | 1 = 640x480, 2 = 320x240 doubled, 0 and 3 act as 1; reset 1 |
| 0x08C | FRAME_CRC | CRC-32 of the last complete frame (section 3) |
| 0x090 | FRAME_COUNT | frames whose CRC was taken since the pixel domain's reset (16 bits, wraps) |
| 0x400..0x7FC | GPAL | graphics palette, RGB444 |

CTRL.MODE 2 and 3 are the graphics modes. Reads of GPAL take the same single cycle as every other
register (the block RAM's registered output is the answer).

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
starts. ABORT stops the running command between two accesses.

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

Every new crossing uses rtl/lib blocks whose scoped constraints the designs already apply. The
longest new paths are the CPU's VRAM read (block RAM, the 19-way bank multiplexer, the response
multiplexer, the core's load path, in one 40 ns sys_clk cycle) and the engine's address and mask
arithmetic.

## 9. Resources (estimates before synthesis; section 12 has the results)

| Part | LUT | FF | RAMB36 | DSP |
|---|---|---|---|---|
| surf_vram | ~250 (bank multiplexers) | ~80 | 75 | 0 |
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
ends with `GPUDIAG: PASS (n checks)` or `GPUDIAG: FAIL (...)`, then shows the last scene and
echoes the UART. The scenes are written once and run in three places: on the CPU (writing the
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

(filled in as G1 and G2 land)

### 12.2 Synthesis

(filled in as G1 and G2 land)

### 12.3 Placement: "another part of the chip"

(filled in after the gpudiag build: where nextpnr put VRAM and the engine, and whether the open
flow can constrain a block to a region)

## 13. Risks

* **Block RAM 83 %.** VRAM's 75 RAMB36 spread over most of the 100T's block RAM columns, so its
  multiplexers and the CPU's read path span the die. At 25 MHz there is room; the DDR design's
  80 MHz dram_clk had +1.13 ns and placed in three tries of four, and the added block RAM and
  routing may cost it placements. The DDR+GPU design is built last and its margin recorded.
* **0x3000_0000 was a "known unmapped" address** in older tests: bramdiag's "load from an
  unmapped address" probe, SurfBoot's monitor test (`md 0x30000000`) and the hello test kernel use
  it. On the GPU designs that address is VRAM, so those probes would see memory. They run on the
  existing designs, where nothing changed; bramdiag on a GPU design would report that one check
  (moving its probe to 0x4000_0000 is a change for the integrator to decide).
* **Pixel reads during writes.** A word the CPU or the engine writes at the instant the scan-out
  reads it may show either value for that frame (the NO_CHANGE mode keeps memory safe). FRAME_CRC
  of a picture that changes while shown is therefore not reproducible; the diagnostic only takes
  CRCs of still pictures (section 3).
* **The engine's complexity** (COPY_RECT's alignment and overlap, EXPAND's stream) is covered by
  the randomized comparison with the model and by mutants; the model itself is checked by
  directed tests written from section 5, not from the RTL.
* **Open-flow sign-off.** As for every SurfCPU bitstream, nextpnr's timing is an estimate and the
  CDC constraints are Vivado's only (OPEN-FLOW.md).
