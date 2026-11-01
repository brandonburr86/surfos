/*
SurfOS on QEMU virt: a stand-in for SurfBoot's last step
--------------------
File: surfboot_stub.S   Date: 10/15/26 (SurfCPU)
--------------------
For testing the boot ABI under QEMU (boards/qemu_virt/test_os.py). QEMU (-bios this ELF)
enters here in machine mode, with a struct surf_boot_info the harness made at BOOTINFO
(-device loader) and the kernel in one of two forms:

  * SURFOS.BIN at STAGE (the package, packed by the fw track's tools/surfimg.py and checked
    with its verify): like SurfBoot's handoff (surfcpu/docs/BOOT-ABI.md section 4) this
    checks the header's magic, copies the payload to load_addr, executes FENCE.I and jumps
    to entry;
  * nothing at STAGE and surfos.elf already in place at KERNEL_ENTRY (QEMU's loader put its
    segments there): straight to the entry, so the handoff and the boot information can be
    tested on a tree without the packer.

Either way interrupts are off and the kernel starts with a0 = 0 (the hart), a1 = BOOTINFO and
a2 = SURF_BOOTINFO_MAGIC. It is not SurfBoot (the fw track's): no SD card, no FAT, no CRC
check. Anything else at STAGE, or no kernel at all, ends QEMU with status 0xbad.
*/

#define STAGE        0x8D000000             /* where the harness loads SURFOS.BIN */
#define BOOTINFO     0x8C000000             /* and the boot information */
#define KERNEL_ENTRY 0x80000000             /* boards/qemu_virt/board.ld: KERNEL_LOAD, the ELF's entry */
#define IMAGE_MAGIC  0x474D4953             /* SURF_IMAGE_MAGIC */
#define INFO_MAGIC   0x49465253             /* SURF_BOOTINFO_MAGIC */
#define TEST_DEVICE  0x00100000             /* QEMU's sifive_test */

        .section .text.entry, "ax"
        .globl  _start
_start:
        csrw    mie, zero
        csrci   mstatus, 8
        li      t0, STAGE
        lw      t1, 0(t0)
        li      t2, IMAGE_MAGIC
        beq     t1, t2, package
        bnez    t1, bad                     /* something at STAGE that is not a package */
        li      a5, KERNEL_ENTRY            /* the ELF, loaded in place by QEMU */
        lw      t1, 0(a5)
        beqz    t1, bad                     /* (RAM starts zeroed: no kernel there either) */
        j       handoff
package:
        lw      a3, 0x10(t0)                /* load_addr */
        lw      a4, 0x14(t0)                /* payload_size */
        lw      a5, 0x1C(t0)                /* entry */
        addi    t3, t0, 64                  /* the payload follows the 64-byte header */
1:      beqz    a4, handoff
        lbu     t5, 0(t3)
        sb      t5, 0(a3)
        addi    t3, t3, 1
        addi    a3, a3, 1
        addi    a4, a4, -1
        j       1b
handoff:
        fence.i
        li      a0, 0
        li      a1, BOOTINFO
        li      a2, INFO_MAGIC
        jr      a5
bad:    li      t0, TEST_DEVICE
        li      t1, (0xbad << 16) | 0x3333
        sw      t1, 0(t0)
        j       bad
