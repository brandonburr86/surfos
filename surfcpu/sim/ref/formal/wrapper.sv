/*
SurfCPU riscv-formal wrapper
--------------------
File: wrapper.sv Date: 10/15/26 (SurfCPU)
--------------------
riscv-formal's rvfi_wrapper for surfcpu_core (docs/ISA.md section 4): the core with
SURFCPU_RVFI defined, its two SurfBus ports on protocol-keeping slaves with unconstrained
answers (surfbus_formal_slave.sv), and unconstrained interrupt lines and mtime. The RVFI
ports are connected with the core's own macro (rtl/cpu/surfcpu_rvfi.vh). tools/rvformal.py
copies this file, the slave, the core's RTL and its headers into riscv-formal's
cores/surfcpu/ and generates the checks from checks.cfg.
*/
`include "surfcpu_rvfi.vh"

module rvfi_wrapper (
    input clock,
    input reset,
    `RVFI_OUTPUTS
);
    wire        ibus_req_valid, ibus_req_ready, ibus_req_write, ibus_req_fetch;
    wire [31:0] ibus_req_addr, ibus_req_wdata, ibus_rsp_rdata;
    wire [3:0]  ibus_req_be;
    wire        ibus_rsp_valid, ibus_rsp_err;
    wire        dbus_req_valid, dbus_req_ready, dbus_req_write, dbus_req_fetch;
    wire [31:0] dbus_req_addr, dbus_req_wdata, dbus_rsp_rdata;
    wire [3:0]  dbus_req_be;
    wire        dbus_rsp_valid, dbus_rsp_err;

    (* keep *) `rvformal_rand_reg        irq_external;
    (* keep *) `rvformal_rand_reg        irq_timer;
    (* keep *) `rvformal_rand_reg        irq_software;
    (* keep *) `rvformal_rand_reg [63:0] mtime;

    surfbus_formal_slave imem (
        .clock(clock), .reset(reset),
        .req_valid(ibus_req_valid), .req_ready(ibus_req_ready), .req_addr(ibus_req_addr),
        .req_write(ibus_req_write), .req_be(ibus_req_be), .req_wdata(ibus_req_wdata),
        .req_fetch(ibus_req_fetch),
        .rsp_valid(ibus_rsp_valid), .rsp_rdata(ibus_rsp_rdata), .rsp_err(ibus_rsp_err));

    surfbus_formal_slave dmem (
        .clock(clock), .reset(reset),
        .req_valid(dbus_req_valid), .req_ready(dbus_req_ready), .req_addr(dbus_req_addr),
        .req_write(dbus_req_write), .req_be(dbus_req_be), .req_wdata(dbus_req_wdata),
        .req_fetch(dbus_req_fetch),
        .rsp_valid(dbus_rsp_valid), .rsp_rdata(dbus_rsp_rdata), .rsp_err(dbus_rsp_err));

    surfcpu_core uut (
        .clk(clock), .rst(reset),
        .ibus_req_valid(ibus_req_valid), .ibus_req_ready(ibus_req_ready),
        .ibus_req_addr(ibus_req_addr), .ibus_req_write(ibus_req_write),
        .ibus_req_be(ibus_req_be), .ibus_req_wdata(ibus_req_wdata),
        .ibus_req_fetch(ibus_req_fetch),
        .ibus_rsp_valid(ibus_rsp_valid), .ibus_rsp_rdata(ibus_rsp_rdata),
        .ibus_rsp_err(ibus_rsp_err),
        .dbus_req_valid(dbus_req_valid), .dbus_req_ready(dbus_req_ready),
        .dbus_req_addr(dbus_req_addr), .dbus_req_write(dbus_req_write),
        .dbus_req_be(dbus_req_be), .dbus_req_wdata(dbus_req_wdata),
        .dbus_req_fetch(dbus_req_fetch),
        .dbus_rsp_valid(dbus_rsp_valid), .dbus_rsp_rdata(dbus_rsp_rdata),
        .dbus_rsp_err(dbus_rsp_err),
        .irq_external(irq_external), .irq_timer(irq_timer), .irq_software(irq_software),
        .mtime(mtime),
        `SURFCPU_RVFI_CONNECT
    );
endmodule
