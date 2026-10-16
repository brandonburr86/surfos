# SurfBus

SurfBus connects SurfCPU's instruction and data ports to the memories and the MMIO
peripherals. It is a single-clock, request/response bus in the style of OBI (the OpenHW
Group's Open Bus Interface): one request channel with a valid/ready handshake, one response
channel without backpressure, byte enables, and an error response. This document is the
contract: the CPU, the interconnect, every slave and the DDR bridge implement exactly this.
Frozen at version 1 on 2026-10-15; a change needs a new version number here and in every user.

## 1. Signals

All signals are synchronous to the SoC clock `clk` (`sys_clk`, `clocks.sys_hz`). `rst` is
active high, synchronous, and comes from the reset synchronizer. A port is named by a prefix:
`ibus_` (instruction fetch), `dbus_` (data), or `s_`/`m_` inside the interconnect.

| Signal | Width | Direction | Meaning |
|---|---|---|---|
| `req_valid` | 1 | master → slave | A request is offered this cycle. |
| `req_ready` | 1 | slave → master | The slave accepts the offered request this cycle. |
| `req_addr` | 32 | master → slave | Byte address of the access. |
| `req_write` | 1 | master → slave | 1 = write, 0 = read. |
| `req_be` | 4 | master → slave | Byte enables for the aligned word `req_addr[31:2]`, for reads and writes. |
| `req_wdata` | 32 | master → slave | Write data, lane-aligned (byte at offset k is in bits 8k+7:8k). |
| `req_fetch` | 1 | master → slave | The access is an instruction fetch. |
| `rsp_valid` | 1 | slave → master | A response is delivered this cycle. |
| `rsp_rdata` | 32 | slave → master | The whole aligned word for reads (lanes as in memory); don't-care for writes. |
| `rsp_err` | 1 | slave → master | The access failed and had no effect. Valid with `rsp_valid`. |

The instruction port always drives `req_write = 0`, `req_be = 4'b1111`, `req_wdata = 0`,
`req_fetch = 1`. The data port drives `req_fetch = 0`.

## 2. Handshake rules

1. **Transfer.** A request is transferred at a rising edge of `clk` where `req_valid` and
   `req_ready` are both 1.
2. **No withdrawal.** Once a master raises `req_valid`, it keeps it and all `req_*` fields
   unchanged until the request is accepted. A master that no longer wants the result (for
   example, the fetch unit after a branch) still lets the request complete and drops the
   response.
3. **No combinational loops.** `req_valid` and the request fields never depend on `req_ready`
   in the same cycle. `req_ready` may depend on `req_valid` and the request fields.
4. **One response per request, in order.** Every accepted request gets exactly one response,
   in acceptance order, no earlier than the cycle after acceptance (`rsp_valid` is never
   combinational from the request).
5. **No response backpressure.** A master must take a response in the cycle it arrives.
6. **One outstanding request per master (version 1).** After an accepted request, a master
   offers its next request no earlier than the cycle in which the previous response arrives
   (it may offer it in that same cycle). With single-cycle memories this still gives one
   access per cycle.
7. **Bounded latency.** Every slave answers within a bounded number of cycles. MMIO and block
   RAM answer in 1 cycle (an MMIO register may take longer when its peripheral says so: the SPI
   `DATA` read waits for a running transfer, at most 16 x (DIV + 1) cycles). The DDR bridge has
   a watchdog that turns a stuck access into an error response (`sysinfo.DDR_STATUS.TIMEOUT`).
8. **Reset.** In reset, masters drive `req_valid = 0` and slaves drive `rsp_valid = 0`. The
   whole SoC resets together; no request survives a reset.

## 3. Errors

`rsp_err = 1` means the access was not performed: a write with an error changed nothing, and a
read with an error had no side effect (no FIFO was popped). The CPU turns an error into an
access fault (instruction, load or store) with `mtval` = the access address. Errors are given
for:

* an address in no region of [MEMORY-MAP.md](MEMORY-MAP.md) (the interconnect's error slave);
* an instruction fetch from a region without *fetch* (MMIO, text RAM);
* a write to a region without *write* (the boot ROM);
* an MMIO offset with no register, or a non-word access to an MMIO register (byte enables
  other than `4'b1111`);
* any DDR access before calibration has succeeded, or one abandoned by the bridge's watchdog.

## 4. Alignment and widths

The CPU never issues a misaligned access: a misaligned load or store traps before it reaches
the bus (ISA.md). So `req_be` is one of `0001 0010 0100 1000` (byte), `0011 1100` (halfword) or
`1111` (word), matching `req_addr[1:0]`. Memories honour the byte enables on writes. For reads
a memory returns the whole word; the CPU picks and extends the bytes it asked for.

## 5. MMIO registers

Peripheral registers are 32-bit words at word-aligned offsets inside a 4 KiB window. They
are accessed with `req_be = 4'b1111` only (anything else is an error, section 3), so a
read-side effect such as popping a FIFO happens exactly once per load. Reading or writing
an offset that has no register is an error.

## 6. Topology (version 1)

```
                         +----------------------+
 SurfCPU ibus ---------->| I decoder            |--> bootrom port A
                         |  (fetch regions only) |--> scratch port A
                         |                      |--> DDR arbiter
                         |                      |--> error (everything else: access fault)
                         +----------------------+
                         +----------------------+
 SurfCPU dbus ---------->| D decoder            |--> bootrom port B (reads; writes → error)
                         |                      |--> scratch port B
                         |                      |--> MMIO: uart timer intc gpio spi ps2 video sysinfo
                         |                      |--> textram port A (port B is the video scan-out)
                         |                      |--> DDR arbiter
                         |                      |--> error
                         +----------------------+
 DDR arbiter: data port first, then fetch; one request at a time; responses go back to the
 master that issued the request.
```

The boot ROM and scratch RAM are true dual-port block RAMs, one port per master, so fetch
and data accesses to them never wait for each other. The data port reaches everything; the
fetch port only regions with *fetch*. Text RAM is dual-ported between the data port and the
video clock domain.

## 7. What the CPU guarantees

* A load or store goes to the bus only when it will certainly complete: no older
  instruction can still trap, and an interrupt waits until it is done. So a flushed (wrong
  path) instruction never reaches the data bus, and an MMIO write or a FIFO pop happens
  exactly once however long the bus stalls.
* Instruction fetches have no side effects, so wrong-path fetches are harmless; their
  responses are dropped.
