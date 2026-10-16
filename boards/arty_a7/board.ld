/* SurfOS on SurfCPU (Arty A7): where the kernel lives (10/15/26).
 * Every address comes from surfcpu/firmware/ld/surfcpu_soc.ld, generated from
 * surfcpu/spec/soc.toml: the image loads at the `kernel` reservation (the start of DDR),
 * and the kernel heap stops below `ddr_top`, the 16 MB SurfBoot keeps free.
 */
INCLUDE surfcpu_soc.ld

KERNEL_LOAD = SURF_KERNEL_BASE;
RAM_START   = SURF_DDR_BASE;
RAM_END     = SURF_DDR_BASE + SURF_DDR_SIZE;
HEAP_END    = SURF_DDR_TOP_BASE;
REGION_ALIAS("kernel_ram", ddr);
