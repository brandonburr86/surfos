/*
SurfOS riscv32 entry
--------------------
File: start.S   Date: 10/15/26 (SurfCPU)
--------------------
The first instruction of the image, at its load address (0x80000000). SurfBoot
jumps here as surfcpu/docs/BOOT-ABI.md section 4 says: machine mode, mstatus.MIE = 0,
mie = 0, a0 the hart ID, a1 the address of struct surf_boot_info, a2
SURF_BOOTINFO_MAGIC, every other register undefined. QEMU's virt machine (-bios none)
also lands here, from its reset vector, with a0 the hart ID, a1 a device tree and a2
something else, which is how kmain() tells the two apart.

Before any C: interrupts stay off, mtvec points at our own trap entry (not SurfBoot's
ROM fault reporter), gp and sp are ours, tp is zero (the kernel has no thread-local
storage), the BSS is cleared, and the frame-pointer chain ends at kmain(). The boot
information is still where SurfBoot left it (scratch RAM, outside the image): kmain()
copies it before anything else runs.
*/

#include <riscv/csr.h>

#define BOOT_STACK_SIZE 0x4000          /* kernel/task.c: the idle task keeps this stack */
#define STACK_CANARY    0x5354434B      /* "STCK" at the bottom of every kernel stack (arch/riscv32/trap.c) */

        .section .text.entry, "ax"
        .globl  _start
_start:
        csrci   mstatus, MSTATUS_MIE
        csrw    mie, zero
        .option push
        .option norelax
        la      gp, __global_pointer$   /* not relaxed: gp is not set yet */
        .option pop
        li      tp, 0
        la      sp, stack + BOOT_STACK_SIZE
        la      t0, trap_entry
        csrw    mtvec, t0
        bnez    a0, park                /* one hart runs the kernel (SurfCPU has one) */

        mv      s1, a0                  /* the handoff, kept across the BSS clear */
        mv      s2, a1
        mv      s3, a2
        la      t0, __bss_start
        la      t1, __bss_end
1:      bgeu    t0, t1, 2f
        sw      zero, 0(t0)
        addi    t0, t0, 4
        j       1b
2:
        la      t0, stack               /* the boot stack's canary, as task stacks have */
        li      t1, STACK_CANARY
        sw      t1, 0(t0)
        li      s0, 0                   /* backtraces end here */
        li      ra, 0
        mv      a0, s1
        mv      a1, s2
        mv      a2, s3
        call    kmain
park:   wfi                             /* kmain() never returns */
        j       park

        .section .bss.stack, "aw", @nobits
        .balign 16
        .globl  stack
stack:  .space  BOOT_STACK_SIZE
