/*
SurfCPU core testbench top
--------------------
File: surfcpu_tb_top.v Date: 10/15/26 (SurfCPU)
--------------------
The Verilator top of build/cpu/surfcpu_tb (docs/MODULES.md section 6): surfcpu_core alone
with RESET_PC = 0x80000000, every port brought out to the C++ harness (surfcpu_tb.cpp),
which models the memory, the bus timing, the timer and the testbench registers. Built with
SURFCPU_RVFI defined.
*/
`include "surfcpu_rvfi.vh"

module surfcpu_tb_top (
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
    input  wire [63:0] mtime,
    `SURFCPU_RVFI_OUTPUTS
);
    surfcpu_core #(
        .RESET_PC(32'h8000_0000)
    ) core (
        .clk(clk),
        .rst(rst),
        .ibus_req_valid(ibus_req_valid),
        .ibus_req_ready(ibus_req_ready),
        .ibus_req_addr(ibus_req_addr),
        .ibus_req_write(ibus_req_write),
        .ibus_req_be(ibus_req_be),
        .ibus_req_wdata(ibus_req_wdata),
        .ibus_req_fetch(ibus_req_fetch),
        .ibus_rsp_valid(ibus_rsp_valid),
        .ibus_rsp_rdata(ibus_rsp_rdata),
        .ibus_rsp_err(ibus_rsp_err),
        .dbus_req_valid(dbus_req_valid),
        .dbus_req_ready(dbus_req_ready),
        .dbus_req_addr(dbus_req_addr),
        .dbus_req_write(dbus_req_write),
        .dbus_req_be(dbus_req_be),
        .dbus_req_wdata(dbus_req_wdata),
        .dbus_req_fetch(dbus_req_fetch),
        .dbus_rsp_valid(dbus_rsp_valid),
        .dbus_rsp_rdata(dbus_rsp_rdata),
        .dbus_rsp_err(dbus_rsp_err),
        .irq_external(irq_external),
        .irq_timer(irq_timer),
        .irq_software(irq_software),
        .mtime(mtime),
        `SURFCPU_RVFI_CONNECT
    );
endmodule
