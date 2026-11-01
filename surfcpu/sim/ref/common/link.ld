/*
SurfCPU verification programs: linker script
--------------------
File: link.ld Date: 10/15/26 (SurfCPU)
--------------------
For the programs of sim/ref (directed reference tests, random programs, the CSR probe): code
from 0x80000000, where build/cpu/surfcpu_tb starts the core and where Spike's boot ROM jumps;
tohost/fromhost for the end of the run; everything inside the testbench's 4 MiB of RAM.
*/
OUTPUT_ARCH("riscv")
ENTRY(_start)
MEMORY { ram (rwx) : ORIGIN = 0x80000000, LENGTH = 0x400000 }
SECTIONS
{
  .text.init 0x80000000 : { *(.text.init) } > ram
  .text : { *(.text) *(.text.*) } > ram
  .tohost ALIGN(0x1000) : { *(.tohost) } > ram
  .data ALIGN(0x1000) : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) *(.rodata) *(.rodata.*) } > ram
  .bss : { *(.bss) *(.bss.*) *(.sbss) *(COMMON) } > ram
  _end = .;
  ASSERT(_end <= 0x80400000, "program does not fit the testbench's 4 MiB of RAM")
}
