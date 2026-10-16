/*
SurfOS on QEMU virt: a stand-in for SurfBoot's last step
--------------------
File: surfboot_stub.S   Date: 10/15/26 (SurfCPU)
--------------------
For testing the boot ABI and the kernel package under QEMU (boards/qemu_virt/test_os.py
--package). QEMU (-bios this ELF) enters here in machine mode. The harness has loaded
SURFOS.BIN at STAGE and a struct surf_boot_info at BOOTINFO (-device loader), after
checking the package with arch/riscv32/surfimg_tmp.py verify. Like SurfBoot's handoff
(surfcpu/docs/BOOT-ABI.md section 4) this turns interrupts off, checks the header's magic,
copies the payload to load_addr, executes FENCE.I and jumps to entry with a0 = 0 (the
hart), a1 = BOOTINFO and a2 = SURF_BOOTINFO_MAGIC. It is not SurfBoot (the fw track's):
no SD card, no FAT, no CRC; a bad magic ends QEMU with status 0xbad.
*/

#define STAGE        0x8D000000             /* where the harness loads SURFOS.BIN */
#define BOOTINFO     0x8C000000             /* and the boot information */
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
        bne     t1, t2, bad
        lw      a3, 0x10(t0)                /* load_addr */
        lw      a4, 0x14(t0)                /* payload_size */
        lw      a5, 0x1C(t0)                /* entry */
        addi    t3, t0, 64                  /* the payload follows the 64-byte header */
1:      beqz    a4, 2f
        lbu     t5, 0(t3)
        sb      t5, 0(a3)
        addi    t3, t3, 1
        addi    a3, a3, 1
        addi    a4, a4, -1
        j       1b
2:      fence.i
        li      a0, 0
        li      a1, BOOTINFO
        li      a2, INFO_MAGIC
        jr      a5
bad:    li      t0, TEST_DEVICE
        li      t1, (0xbad << 16) | 0x3333
        sw      t1, 0(t0)
        j       bad
