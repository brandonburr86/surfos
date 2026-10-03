# SurfOS documentation (branch `ai-dev`)

`ai-dev` is the development fork of the restored 2004 SurfOS kernel. `master` stays
as the restoration; new work happens here.

| Document | What it is for |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the kernel works today: boot sequence, memory map, scheduler, interrupts, drivers, shell, build system |
| [CODE-AUDIT.md](CODE-AUDIT.md) | Every bug and risk found in the source, with file:line references, and what was verified by booting under QEMU |
| [ROADMAP.md](ROADMAP.md) | The plan: ground rules, the Phase 0 toolchain recipe, a catalog of kernel modules with acceptance tests, and a recommended milestone order |

Status in one paragraph: with a modern GCC, binutils and NASM the tree needs three
mechanical source fixes and new compiler flags, after which it links to a 69 KB
Multiboot ELF and boots to its shell under `qemu-system-i386 -m 64 -kernel surfos.bin`.
Multitasking, demand paging, the PIT, keyboard, VGA console, PCI scan and most shell
commands work. Nothing blocks, the allocator leaks, several exception vectors mishandle
error codes, user mode and storage never worked, and the only NIC driver targets a card
QEMU does not emulate. The roadmap starts from there.
