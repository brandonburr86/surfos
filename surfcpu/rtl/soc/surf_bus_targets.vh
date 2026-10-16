/*
SurfCPU interconnect target numbers
--------------------
File: surf_bus_targets.vh Date: 10/15/26 (SurfCPU)
--------------------
The slave ports of the I and D decoders (docs/SURFBUS.md section 6, docs/SOC.md), as macros so
they can size port lists. `include it at the top of a file, outside the module. The DDR target
is always the last one: the interconnect keeps it inside (arbiter and calibration gate) and
exposes the others. Each decoder also has its own error slave, which is not numbered here.
*/
`ifndef SURF_BUS_TARGETS_VH
`define SURF_BUS_TARGETS_VH

// I decoder (instruction fetch): only regions with fetch
`define SURF_IT_ROM      0
`define SURF_IT_SCRATCH  1
`define SURF_IT_DDR      2
`define SURF_IT_COUNT    3

// D decoder (data): every region
`define SURF_DT_ROM      0
`define SURF_DT_SCRATCH  1
`define SURF_DT_UART     2
`define SURF_DT_TIMER    3
`define SURF_DT_INTC     4
`define SURF_DT_GPIO     5
`define SURF_DT_SPI      6
`define SURF_DT_PS2      7
`define SURF_DT_VIDEO    8
`define SURF_DT_SYSINFO  9
`define SURF_DT_TEXTRAM  10
`define SURF_DT_DDR      11
`define SURF_DT_COUNT    12

`endif
