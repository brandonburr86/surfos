# SurfOS documentation (branch `ai-dev`)

`ai-dev` is the development fork of the restored 2004 SurfOS kernel. `master` stays
as the restoration; new work happens here.

| Document | What it is for |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the kernel works today: boot sequence, memory map, scheduler, interrupts, drivers, shell, build system |
| [CODE-AUDIT.md](CODE-AUDIT.md) | Every bug and risk found in the source, with file:line references, and what was verified by booting under QEMU |
| [ROADMAP.md](ROADMAP.md) | The plan: ground rules, the Phase 0 toolchain recipe, a catalog of kernel modules with acceptance tests, and a recommended milestone order |

## Building and running

```
make              # build/O0/surfos.bin  (make O=2 for an optimised build)
make run          # boot in QEMU on this terminal (serial console; Ctrl-A x quits)
make test-all     # headless smoke test over the serial console at -O0 and -O2
make iso          # bootable GRUB ISO (needs grub-pc-bin, xorriso, mtools)
```

Needs `gcc` with 32-bit support, `binutils`, `nasm`, `qemu-system-x86` and `python3`.
`CLAUDE.md` at the repository root has the longer version plus the conventions.

Status in one paragraph: the kernel builds with a 2026 toolchain, boots to its shell
under QEMU (`-kernel` and GRUB ISO) and can be driven entirely over the serial console.
Multitasking, demand paging, the PIT, keyboard, VGA console, PCI scan and most shell
commands work. Nothing blocks, the allocator leaks, several exception vectors mishandle
error codes, user mode and storage never worked, and the only NIC driver targets a card
QEMU does not emulate. The roadmap starts from there.
