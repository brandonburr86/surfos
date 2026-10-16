# SurfCPU module and model interfaces

The interfaces between the parts that are built in parallel: RTL module port lists, the
simulation models' C++ APIs, file formats and command lines. Each part is free inside; these
edges are fixed (version 1, 2026-10-15; clarifications in section 8) so the parts plug together
without rework. A needed
change is proposed to the integrator, not made unilaterally.

## 1. RTL conventions

* Verilog-2005, synthesizable, one module per file, file name = module name. Testbenches may
  use SystemVerilog (Verilator) or C++.
* Clocks are `clk` (the SoC clock, `clocks.sys_hz`) and `pix_clk`; resets are synchronous and
  active high (`rst`, `pix_rst`), each made by `rtl/lib/surf_reset_sync.v` in its own domain.
* Vendor primitives (MMCM, IOBUF, MIG) appear only under `boards/`. `rtl/cpu`, `rtl/soc`,
  `rtl/peripherals` and `rtl/lib` are portable and simulate in Verilator and Icarus.
* Simulation-only code sits behind `` `ifndef SYNTHESIS `` (or `` `ifdef SURFCPU_RVFI ``).
* Addresses and register layouts come from `rtl/generated/surfcpu_soc.vh`; include it inside
  the module body. Never type an address or register offset by hand.
* Lint: `verilator --lint-only -Wall` clean, with a comment justifying each waiver.

Shared building blocks in `rtl/lib/` (tested by `make test-lib`): `surf_sync` (one-bit level
synchronizer), `surf_reset_sync`, `surf_fifo` (synchronous, first-word fall-through),
`surf_pulse_sync` (event pulses across clocks), `surf_cdc_word` (a multi-bit value across
clocks, never torn).

## 2. SurfBus slave port

Every MMIO peripheral and memory has this port, in the `clk` domain (protocol: SURFBUS.md).
The decoder passes the full 32-bit address; a peripheral decodes `s_req_addr[11:0]` and
answers an unknown offset or a non-word access with `s_rsp_err`. Fetches never reach MMIO.

```verilog
    input  wire        s_req_valid,
    output wire        s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output wire        s_rsp_valid,
    output wire [31:0] s_rsp_rdata,
    output wire        s_rsp_err,
```

A master port is the same list with `m_` and directions reversed.

## 3. Modules

`<slave>` stands for the port list in section 2. Parameters default to the spec's values.

| Module | Owner | Parameters | Ports besides `clk`, `rst`, `<slave>` |
|---|---|---|---|
| `surfcpu_core` | cpu | `RESET_PC`, `MIMPID` | docs/ISA.md section 4 |
| `surf_uart` | soc | `SYS_HZ`, `BAUD` = 115200 | `output irq`, `input rx`, `output tx` |
| `surf_timer` | soc | `SYS_HZ`, `TIMEBASE_HZ` | `output [63:0] mtime`, `output irq_timer`, `output irq_software` |
| `surf_intc` | soc | | `input [31:0] sources`, `output irq_external` |
| `surf_gpio` | soc | `SYS_HZ` | `output irq`, `output [3:0] led`, `output [11:0] rgb`, `input [3:0] sw`, `input [3:0] btn` (raw pins; synchronized and debounced inside) |
| `surf_sysinfo` | soc | `BOARD_ID`, `SYS_HZ`, `PIX_HZ`, `BUILD_ID`, `BUILD_FLAGS`, `BUILD_TIME`, `FEATURES`, `DDR_SIZE` | `input ddr_calib_done`, `input ddr_calib_fail`, `input ddr_timeout` |
| `surf_textram` | soc | | `input pix_clk`, `input [11:0] pix_addr`, `output [31:0] pix_data` (registered: the word for `pix_addr` appears after one `pix_clk` edge) |
| `surf_spi` | io | `SYS_HZ` | `output irq`, `output sclk`, `output mosi`, `input miso`, `output cs_n`, `input cd_n` (card detect, low = card present; tie 0 without one) |
| `surf_ps2` | io | `SYS_HZ` | `output irq`, `input ps2_clk_i`, `input ps2_data_i`, `output ps2_clk_oe`, `output ps2_data_oe` (open drain: `_oe` = 1 pulls the line low, 0 releases it) |
| `surf_video` | io | `PIX_HZ` | `output irq`; `input pix_clk`, `input pix_rst`; `output [11:0] text_addr`, `input [31:0] text_data` (to `surf_textram`'s pixel port); `output [3:0] vga_r`, `vga_g`, `vga_b`, `output vga_hs`, `vga_vs` (registered in the `pix_clk` domain) |
| `surf_ddr_bridge` | ddr | `TIMEOUT_CYCLES`, `CALIB_TIMEOUT_CYCLES`, `APP_ADDR_WIDTH`, `APP_DATA_WIDTH`, `DQ_WIDTH` | `output calib_done`, `calib_fail`, `timeout` (sys domain); controller side in the `ui_clk` domain, as the ddr track documents in docs/DDR.md |
| `surfcpu_soc` | soc | section 4 | section 4 |
| `surfcpu_arty_a7_top` | board | `BOARD`, ROM file, build identity | the board's pins (WIRING.md) |

The text RAM's `pix_addr` is the cell number, `row * 80 + col` (0..2399; 2400..4095 are
ordinary RAM). `surf_video` owns the font ROM and the palette.

## 4. `surfcpu_soc`

```verilog
module surfcpu_soc #(
    parameter         BOOTROM_INIT = "",           // $readmemh file for the boot ROM; "" = all zero
    parameter [31:0]  BOARD_ID     = 32'h0000_00F0,
    parameter integer SYS_HZ       = 25000000,
    parameter integer PIX_HZ       = 25173611,
    parameter [31:0]  BUILD_ID     = 32'h0,
    parameter [31:0]  BUILD_FLAGS  = 32'h0,
    parameter [31:0]  BUILD_TIME   = 32'h0,
    parameter integer HAS_DDR      = 1,
    parameter [31:0]  DDR_SIZE     = 32'h1000_0000
) (
    input  wire        clk, rst, pix_clk, pix_rst,
    input  wire        uart_rx,  output wire uart_tx,
    output wire [3:0]  led,      output wire [11:0] rgb,  input wire [3:0] sw, input wire [3:0] btn,
    output wire        sd_sclk, sd_mosi, sd_cs_n,  input wire sd_miso, sd_cd_n,
    input  wire        ps2_clk_i, ps2_data_i,      output wire ps2_clk_oe, ps2_data_oe,
    output wire [3:0]  vga_r, vga_g, vga_b,        output wire vga_hs, vga_vs,
    // DDR: a SurfBus master port toward surf_ddr_bridge (or the simulation's DDR model)
    output wire        ddr_req_valid, input wire ddr_req_ready, output wire [31:0] ddr_req_addr,
    output wire        ddr_req_write, output wire [3:0] ddr_req_be, output wire [31:0] ddr_req_wdata,
    output wire        ddr_req_fetch,
    input  wire        ddr_rsp_valid, input wire [31:0] ddr_rsp_rdata, input wire ddr_rsp_err,
    input  wire        ddr_calib_done, ddr_calib_fail, ddr_timeout
`ifdef SURFCPU_RVFI
    , `SURFCPU_RVFI_OUTPUTS
`endif
);
```

The SoC itself answers every DDR-window access with an error while `HAS_DDR` is 0 or
`ddr_calib_done` is 0, so nothing reaches the bridge before calibration. In simulation the
boot ROM can also be loaded at run time with `+bootrom=<file>`.

## 5. File formats

**ROM image (`*.mem`).** For `$readmemh`: one 32-bit word per line, 8 hex digits (written in lower case;
readers accept either), no address markers; line *i* is bytes 4*i*..4*i*+3 of the binary, little-endian. The boot ROM's file has
exactly 16384 lines (64 KiB, zero padded). Made by `tools/bin2mem.py`.

**Retirement trace (`*.trace`).** One line per `rvfi_valid`, written by every CPU and SoC
testbench that has RVFI; `#` starts a comment line. Fields, space separated, hex without
`0x` except `order` (decimal):

```
# surfcpu-trace v1: order pc insn trap intr rd rd_wdata mem_addr rmask wmask mem_rdata mem_wdata pc_next
0 80000000 00000297 0 0 5 80000000 00000000 0 0 00000000 00000000 80000004
```

`rd` is 0 when no register is written; `rmask`/`wmask` are one hex digit; `pc_next` is
`rvfi_pc_wdata`. Memory fields follow riscv-formal's aligned convention
(`RISCV_FORMAL_ALIGNED_MEM`): `mem_addr` is word aligned, the masks select byte lanes and unused
lanes are 0. A trapping instruction gives a record with `trap` = 1, no register write, no
memory access and `pc_next` = the handler (`mtvec`). An interrupt has no record of its own: the
handler's first record has `intr` = 1, as has the first record after any trap. A test's trace
ends with its store to `tohost`. `sim/README.md` (cpu track) has the details.

**Card images.** Raw files of 512-byte sectors (MBR + FAT32), as `tools/mkcard.py` makes them;
the SD card model and the host firmware tests read them; nothing writes a real device.

## 6. CPU testbench (cpu track)

`build/cpu/surfcpu_tb` (Verilator) runs one program on the core alone:

```
surfcpu_tb --elf FILE [--trace FILE] [--seed N] [--max-cycles N]
           [--ilat MIN:MAX] [--dlat MIN:MAX] [--signature FILE --sig-begin SYM --sig-end SYM]
```

Memory: 4 MiB of RAM at `0x80000000` loaded from the ELF; the core starts at `0x80000000`
(`RESET_PC`); every other address answers with a bus error, except a CLINT-compatible timer at
`0x02000000` (`msip` +0, `mtimecmp` +0x4000, `mtime` +0xBFF8, as in Spike) and the testbench's
interrupt and console registers that docs/CPU.md lists. With `--ilat`/`--dlat MIN:MAX` each request
waits 0..MAX-MIN cycles for `req_ready` and its response comes MIN..MAX cycles after acceptance
(half the draws take the shortest value), from `--seed`. The program ends with its first non-zero store to
its `tohost` symbol: 1 = pass (exit 0); any other value = fail, test number `value >> 1` (exit 1); running
past `--max-cycles` exits 2; a testbench error (bad arguments or ELF, no `tohost`, a bus protocol
violation by the core) exits 3. `--signature` dumps the words between the two symbols, one per
line (the riscv-arch-test format).

## 7. Simulation models (C++17, no Verilator dependency inside the model)

| Model | Owner | Interface |
|---|---|---|
| `sim/models/uart_model.h` | soc | `UartModel(clks_per_bit)`; `void tick(bool tx_pin)` and `bool rx_pin()` every `clk`; `send(std::string)`; `std::string take_output()` |
| `sim/models/ddr_model.h` | soc | a SurfBus slave behaving like `surf_ddr_bridge`: sparse 256 MiB, seeded random latency, calibration done after N cycles, error injection |
| `sim/models/sdcard_model.h` | io | `SdCardModel(image_path, options)`; `bool tick(bool cs_n, bool sclk, bool mosi)` every `clk`, returns MISO; SPI-mode commands and data tokens, SDHC and SDSC, card absent, injected errors; writes go to the image file only when asked |
| `sim/models/ps2_keyboard_model.h` | io | `Ps2KeyboardModel(sys_hz, ps2_hz)`; `type_text(std::string)` (scan code set 2 make/break), `queue_bytes(...)`, error injection; `void tick(bool clk_oe, bool data_oe, bool &clk_line, bool &data_line)` every `clk` (open-drain lines, so host and device both pull low); answers host commands with `0xFA` |
| `sim/models/vga_capture.h` | io | `void tick(r4, g4, b4, hs, vs)` every `pix_clk`; measured timing (totals, sync widths, polarity); `write_ppm(path)` of the last whole frame; `decode_text(font)` returns the 80x30 characters by matching glyph bitmaps |

`build/sim/surfsim` (soc track) is the whole SoC with these models, its clocks at `SURF_SYS_HZ`
and `SURF_PIX_HZ`: `--rom FILE --sd IMAGE --uart-in FILE --ps2-keys TEXT --vga-dir DIR --trace
FILE --max-cycles N --expect REGEX` (repeatable: all must match the UART output in order; exit
0), `--send REGEX=TEXT` / `--ps2-send REGEX=TEXT` (type on the UART or the keyboard once REGEX
has matched, each list in order), `--fail-on REGEX` (exit 1), `--settle N`, `--transcript
FILE`, `--sd-flush`; exit 2 when `--max-cycles` runs out first. `--vga-dir` writes `screen.ppm`
and `screen.txt` (timing, the decoded 80x30 text, the cursor). docs/SOC.md section 9 has the
details.

## 8. Clarifications (version 1 unchanged)

* 2026-10-15: `.mem` files are written in lower case (section 5); the trace's memory, trap and
  interrupt conventions (section 5); the testbench's exit code 3 and latency ranges (section 6); `surf_ddr_bridge`'s parameters
  (section 3); the surfsim command line as built (section 7).
* 2026-10-16: the program ends with its first non-zero store to `tohost` (section 6), as the
  testbench, Spike and the trace comparator already treat it.
