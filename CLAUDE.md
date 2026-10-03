# SurfOS: notes for AI sessions

SurfOS is a 2004 hobby kernel for 32-bit x86 (Multiboot, ring-0 shell, pre-emptive
tasks, demand-paged kernel heaps) that was restored from printouts in 2026. Branch
`master` is the restoration; `ai-dev` is where the kernel grows. Read `docs/README.md`
first; it points to the architecture map, the code audit and the roadmap.

## Build, run, test

Requirements: `gcc` with 32-bit support (`gcc-multilib` on Debian/Ubuntu), `binutils`,
`nasm`, `qemu-system-x86`, `python3`. For `make iso`: `grub-pc-bin`, `xorriso`, `mtools`.

```
make              # build/O0/surfos.bin (O=2 for an optimised build)
make run          # boot in QEMU on this terminal over the serial console; Ctrl-A x quits
make test         # headless boot + smoke test over the serial console; exit code 0 = pass
make test-all     # the smoke test at -O0 and -O2
make debug        # QEMU paused with the gdb stub; gdb build/O0/surfos.bin -ex 'target remote :1234'
make iso          # GRUB ISO for real hardware or other emulators
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --shell ps memstat     # ad hoc commands
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --screen --keys 'help\n'  # VGA text dump
```

Every change must build with `make` and pass `make test-all` before it is committed.
Add a smoke-test step in `tools/qemu-run.py` (the `SMOKE` table) when you add a
shell command or a kernel feature that the shell can exercise.

## Layout

```
boot/      Multiboot entry (boot.S), grub.cfg for the ISO
kernel/    gdt, interrupts + ISR stubs (assem.asm), scheduler (task.c), console, keyboard, timer, panic
mm/        physical page stack, paging, kernel heap (kalloc), 1:1 DMA heap (palloc)
driver/    serial console, PCI, parallel port, DMA, floppy, 3c905B NIC
lib/blibc/ the in-tree libc (printf, strings, getch, ctype, CMOS time)
shell/     the ring-0 shell and demos
include/   surfos/ kernel headers, mm/, sys/ driver headers, net/, libc headers
tools/     qemu-run.py test harness
docs/      ARCHITECTURE.md, CODE-AUDIT.md, ROADMAP.md
```

## Conventions

* Stay inside the architecture described in `docs/ROADMAP.md` section 1: 32-bit,
  monolithic, Multiboot 1, 8259 PIC + PIT, software task switching.
* Fix things the way the audit describes them and cite the audit ID (for example
  "A6", "T1") in the commit message; update `docs/CODE-AUDIT.md` when an item is closed.
* Console output goes through `kputch()` (so the serial mirror sees it); use
  `kprintf()` in the kernel and `printf()` in task code.
* Keep the 2004 file headers and style where you touch old code; new files use the
  same header block with the current date.
* Small commits, one topic each. Do not rewrite `master`.

## Debugging tips

* `tools/qemu-run.py --int-log file.log ...` records every interrupt and exception
  QEMU delivers (`-d int`); `grep "v=0d"` finds general protection faults.
* `build/O<level>/surfos.sym` is the sorted symbol table; look up a faulting EIP there.
* The screen dump works even when the serial console does not.
