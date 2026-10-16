/*
SurfCPU RVFI port macros
--------------------
File: surfcpu_rvfi.vh Date: 10/15/26 (SurfCPU)
--------------------
The RISC-V Formal Interface outputs of surfcpu_core (docs/ISA.md section 5), as macros so
every module that passes the trace through declares and connects the same list. Only used
with SURFCPU_RVFI defined (simulation and formal); synthesis never sees these ports.

  module m ( ..., input wire [63:0] mtime `ifdef SURFCPU_RVFI , `SURFCPU_RVFI_OUTPUTS `endif );
  `SURFCPU_RVFI_WIRES                       declares rvfi_* wires in a parent
  surfcpu_core core ( ..., .mtime(mtime) `ifdef SURFCPU_RVFI , `SURFCPU_RVFI_CONNECT `endif );
*/
`ifndef SURFCPU_RVFI_VH
`define SURFCPU_RVFI_VH

`define SURFCPU_RVFI_OUTPUTS \
    output wire        rvfi_valid, \
    output wire [63:0] rvfi_order, \
    output wire [31:0] rvfi_insn, \
    output wire        rvfi_trap, \
    output wire        rvfi_halt, \
    output wire        rvfi_intr, \
    output wire [1:0]  rvfi_mode, \
    output wire [1:0]  rvfi_ixl, \
    output wire [4:0]  rvfi_rs1_addr, \
    output wire [4:0]  rvfi_rs2_addr, \
    output wire [31:0] rvfi_rs1_rdata, \
    output wire [31:0] rvfi_rs2_rdata, \
    output wire [4:0]  rvfi_rd_addr, \
    output wire [31:0] rvfi_rd_wdata, \
    output wire [31:0] rvfi_pc_rdata, \
    output wire [31:0] rvfi_pc_wdata, \
    output wire [31:0] rvfi_mem_addr, \
    output wire [3:0]  rvfi_mem_rmask, \
    output wire [3:0]  rvfi_mem_wmask, \
    output wire [31:0] rvfi_mem_rdata, \
    output wire [31:0] rvfi_mem_wdata

`define SURFCPU_RVFI_WIRES \
    wire        rvfi_valid; \
    wire [63:0] rvfi_order; \
    wire [31:0] rvfi_insn; \
    wire        rvfi_trap; \
    wire        rvfi_halt; \
    wire        rvfi_intr; \
    wire [1:0]  rvfi_mode; \
    wire [1:0]  rvfi_ixl; \
    wire [4:0]  rvfi_rs1_addr; \
    wire [4:0]  rvfi_rs2_addr; \
    wire [31:0] rvfi_rs1_rdata; \
    wire [31:0] rvfi_rs2_rdata; \
    wire [4:0]  rvfi_rd_addr; \
    wire [31:0] rvfi_rd_wdata; \
    wire [31:0] rvfi_pc_rdata; \
    wire [31:0] rvfi_pc_wdata; \
    wire [31:0] rvfi_mem_addr; \
    wire [3:0]  rvfi_mem_rmask; \
    wire [3:0]  rvfi_mem_wmask; \
    wire [31:0] rvfi_mem_rdata; \
    wire [31:0] rvfi_mem_wdata;

`define SURFCPU_RVFI_CONNECT \
    .rvfi_valid(rvfi_valid), \
    .rvfi_order(rvfi_order), \
    .rvfi_insn(rvfi_insn), \
    .rvfi_trap(rvfi_trap), \
    .rvfi_halt(rvfi_halt), \
    .rvfi_intr(rvfi_intr), \
    .rvfi_mode(rvfi_mode), \
    .rvfi_ixl(rvfi_ixl), \
    .rvfi_rs1_addr(rvfi_rs1_addr), \
    .rvfi_rs2_addr(rvfi_rs2_addr), \
    .rvfi_rs1_rdata(rvfi_rs1_rdata), \
    .rvfi_rs2_rdata(rvfi_rs2_rdata), \
    .rvfi_rd_addr(rvfi_rd_addr), \
    .rvfi_rd_wdata(rvfi_rd_wdata), \
    .rvfi_pc_rdata(rvfi_pc_rdata), \
    .rvfi_pc_wdata(rvfi_pc_wdata), \
    .rvfi_mem_addr(rvfi_mem_addr), \
    .rvfi_mem_rmask(rvfi_mem_rmask), \
    .rvfi_mem_wmask(rvfi_mem_wmask), \
    .rvfi_mem_rdata(rvfi_mem_rdata), \
    .rvfi_mem_wdata(rvfi_mem_wdata)

`endif
