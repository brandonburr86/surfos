/*
SurfCPU linker script for riscv-arch-test 4.x (ACT4)
--------------------
File: link.ld Date: 10/15/26 (SurfCPU)
--------------------
ACT4's standard layout (the link.ld of its core configurations) for the CPU testbench's memory:
4 MiB of RAM at 0x80000000, the test starting at its base (the testbench's RESET_PC and
Spike's entry). Only RAM_LENGTH and the stack differ from ACT4's Spike configuration.
*/
RAM_ORIGIN = 0x80000000;
RAM_LENGTH = 0x400000;
TEST_BASE = RAM_ORIGIN;
NUM_HARTS = 1;
STACK_SIZE = 0x10000;

OUTPUT_ARCH( "riscv" )
ENTRY(rvtest_entry_point)

MEMORY
{
  ram (rwx) : ORIGIN = RAM_ORIGIN, LENGTH = RAM_LENGTH
}

PROVIDE(__stack_size = STACK_SIZE);
PROVIDE(__num_harts = NUM_HARTS);

SECTIONS
{
  .text.init   TEST_BASE : { *(.text.init) } > ram
  .text.rvtest . : { *(.text.rvtest) *(.text.rvtest.*) } > ram

  . = ALIGN(0x4000);
  .rodata . : { *(.rodata) *(.rodata.*) *(.srodata) *(.srodata.*) } > ram
  .data   . : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) } > ram

  . = ALIGN(16);
  .bss . : {
    __bss_start = .;
    *(.sbss) *(.sbss.*) *(.bss) *(.bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > ram

  . = ALIGN(16);
  __stack_bottom = .;
  . += __stack_size * __num_harts;
  __stack_top = .;

  . = ALIGN(0x1000);
  .text.rvmodel . : { *(.text.rvmodel) *(.text.rvmodel.*) *(.text) *(.text.*) } > ram

  . = ALIGN(0x1000);
  _end = .;

  ASSERT(_end <= ORIGIN(ram) + LENGTH(ram), "ACT ELF exceeds the testbench's 4 MiB of RAM")
}
