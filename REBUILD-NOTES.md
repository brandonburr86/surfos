# Rebuild notes

SurfOS was restored in September 2026 from a 2004 printout of the source. Files that
were not on the printout were reconstructed from the headers, the Makefiles, the way
the rest of the code calls them, and memory. They carry the comment

    Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

Reconstructed files: `driver/drivers.c`, `driver/pci.c`, `lib/blibc/time.c`,
`lib/blibc/memory.c`, `include/stdio.h`, `include/stdlib.h`, `include/string.h`,
`include/memory.h`, `include/ctype.h`, `include/time.h`, `include/surfos/interrupts.h`
(since removed) and the `mm/` and `driver/` Makefiles (since replaced by the
top-level Makefile). Treat them as the least original code when something looks
odd; the PCI scanner in particular was written from the 2004 header, not recovered.

Everything after commit `cd518b5` on the `ai-dev` branch is new work; see
`docs/ROADMAP.md`.
