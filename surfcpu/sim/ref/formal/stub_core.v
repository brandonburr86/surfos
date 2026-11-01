/*
SurfCPU riscv-formal: a stand-in with surfcpu_core's ports (elaboration check only)
--------------------
File: stub_core.v Date: 10/15/26 (SurfCPU)
--------------------
The port list of docs/ISA.md section 4 with nothing behind it, so that tools/rvformal.py
smoke can check, before the core exists, that the formal wrapper elaborates against the
contract's ports. It retires nothing; it is never used in a real check.
*/
`include "surfcpu_rvfi.vh"

module surfcpu_core #(
    parameter [31:0] RESET_PC = 32'h0000_0000,
    parameter [31:0] MIMPID   = 32'h0000_0100
) (
    input  wire        clk,
    input  wire        rst,
    output wire        ibus_req_valid,
    input  wire        ibus_req_ready,
    output wire [31:0] ibus_req_addr,
    output wire        ibus_req_write,
    output wire [3:0]  ibus_req_be,
    output wire [31:0] ibus_req_wdata,
    output wire        ibus_req_fetch,
    input  wire        ibus_rsp_valid,
    input  wire [31:0] ibus_rsp_rdata,
    input  wire        ibus_rsp_err,
    output wire        dbus_req_valid,
    input  wire        dbus_req_ready,
    output wire [31:0] dbus_req_addr,
    output wire        dbus_req_write,
    output wire [3:0]  dbus_req_be,
    output wire [31:0] dbus_req_wdata,
    output wire        dbus_req_fetch,
    input  wire        dbus_rsp_valid,
    input  wire [31:0] dbus_rsp_rdata,
    input  wire        dbus_rsp_err,
    input  wire        irq_external,
    input  wire        irq_timer,
    input  wire        irq_software,
    input  wire [63:0] mtime
`ifdef SURFCPU_RVFI
    , `SURFCPU_RVFI_OUTPUTS
`endif
);
    assign ibus_req_valid = 1'b0;
    assign ibus_req_addr = RESET_PC;
    assign ibus_req_write = 1'b0;
    assign ibus_req_be = 4'b1111;
    assign ibus_req_wdata = 32'h0;
    assign ibus_req_fetch = 1'b1;
    assign dbus_req_valid = 1'b0;
    assign dbus_req_addr = 32'h0;
    assign dbus_req_write = 1'b0;
    assign dbus_req_be = 4'b0000;
    assign dbus_req_wdata = 32'h0;
    assign dbus_req_fetch = 1'b0;
`ifdef SURFCPU_RVFI
    assign rvfi_valid = 1'b0;
    assign rvfi_order = 64'd0;
    assign rvfi_insn = 32'h0;
    assign rvfi_trap = 1'b0;
    assign rvfi_halt = 1'b0;
    assign rvfi_intr = 1'b0;
    assign rvfi_mode = 2'd3;
    assign rvfi_ixl = 2'd1;
    assign rvfi_rs1_addr = 5'd0;
    assign rvfi_rs2_addr = 5'd0;
    assign rvfi_rs1_rdata = 32'h0;
    assign rvfi_rs2_rdata = 32'h0;
    assign rvfi_rd_addr = 5'd0;
    assign rvfi_rd_wdata = 32'h0;
    assign rvfi_pc_rdata = 32'h0;
    assign rvfi_pc_wdata = 32'h0;
    assign rvfi_mem_addr = 32'h0;
    assign rvfi_mem_rmask = 4'h0;
    assign rvfi_mem_wmask = 4'h0;
    assign rvfi_mem_rdata = 32'h0;
    assign rvfi_mem_wdata = 32'h0;
`endif
endmodule
