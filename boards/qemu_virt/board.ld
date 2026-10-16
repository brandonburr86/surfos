/* SurfOS on QEMU's virt machine: where the kernel lives (10/15/26).
 * QEMU's RAM starts at 0x80000000 (-m 256M: to 0x90000000); with -bios none its reset
 * vector jumps to the start of RAM, where the image is. The kernel heap stops at the RAM
 * disk window (boards/qemu_virt/qemu_virt.h: QV_RAMDISK_BASE), where the test harness
 * loads a FAT image with -device loader; QEMU puts its device tree at the top of RAM.
 */
KERNEL_LOAD = 0x80000000;
RAM_START   = 0x80000000;
RAM_END     = 0x90000000;
HEAP_END    = 0x88000000;
MEMORY
{
  qv_ram (rwx) : ORIGIN = 0x80000000, LENGTH = 0x10000000
}
REGION_ALIAS("kernel_ram", qv_ram);
