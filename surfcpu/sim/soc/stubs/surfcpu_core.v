/*
SurfCPU core stand-in: a scripted SurfBus master
--------------------
File: surfcpu_core.v Date: 10/15/26 (SurfCPU)
--------------------
Simulation stand-in for the cpu track's core with the exact module name, parameters and
ports of docs/ISA.md section 4. mk/area-soc.mk builds the SoC's lint and surfsim with it only
while rtl/cpu/surfcpu_core.v does not exist; the SoC's interconnect tests always use it. It is
never part of a synthesized design.
Instead of executing instructions it runs a bus script (+script=<file>, compiled by
sim/soc/tools/surfscript.py) on both ports at once: two surf_script_port engines, one on the
instruction port (fetches only) and one on the data port, which issue the scripted reads,
writes and polls, wait for interrupt levels, meet at barriers, and check every response.
When both have reached their END it prints "SCRIPT PASS" and calls $finish, or prints
"SCRIPT FAIL" and stops with $fatal; with +script_nofinish it only prints the verdict and
raises `reported`, so a testbench can finish the run itself. Without +script it issues
nothing. The RVFI outputs are present (with SURFCPU_RVFI) and never valid.
*/
`include "surfcpu_rvfi.vh"

module surfcpu_core #(
    parameter [31:0] RESET_PC = 32'h0000_0000,
    parameter [31:0] MIMPID   = 32'h0000_0100
) (
    input  wire        clk,
    input  wire        rst,             // synchronous, active high
    // instruction port (SurfBus master, SURFBUS.md)
    output wire        ibus_req_valid,
    input  wire        ibus_req_ready,
    output wire [31:0] ibus_req_addr,
    output wire        ibus_req_write,  // 0
    output wire [3:0]  ibus_req_be,     // 4'b1111
    output wire [31:0] ibus_req_wdata,  // 0
    output wire        ibus_req_fetch,  // 1
    input  wire        ibus_rsp_valid,
    input  wire [31:0] ibus_rsp_rdata,
    input  wire        ibus_rsp_err,
    // data port (SurfBus master)
    output wire        dbus_req_valid,
    input  wire        dbus_req_ready,
    output wire [31:0] dbus_req_addr,
    output wire        dbus_req_write,
    output wire [3:0]  dbus_req_be,
    output wire [31:0] dbus_req_wdata,
    output wire        dbus_req_fetch,  // 0
    input  wire        dbus_rsp_valid,
    input  wire [31:0] dbus_rsp_rdata,
    input  wire        dbus_rsp_err,
    // interrupts: levels, synchronous to clk
    input  wire        irq_external,    // mip.MEIP
    input  wire        irq_timer,       // mip.MTIP
    input  wire        irq_software,    // mip.MSIP
    input  wire [63:0] mtime            // for the time/timeh CSRs
`ifdef SURFCPU_RVFI
    , `SURFCPU_RVFI_OUTPUTS             // section 5
`endif
);
    wire [2:0]  irqs = {irq_software, irq_timer, irq_external};
    wire [31:0] i_syncs, d_syncs, i_fails, d_fails, i_bus, d_bus, i_ops, d_ops;
    wire        i_done, d_done;

    surf_script_port #(.PORT(1)) u_iport (
        .clk(clk), .rst(rst),
        .m_req_valid(ibus_req_valid), .m_req_ready(ibus_req_ready), .m_req_addr(ibus_req_addr),
        .m_req_write(ibus_req_write), .m_req_be(ibus_req_be), .m_req_wdata(ibus_req_wdata),
        .m_req_fetch(ibus_req_fetch), .m_rsp_valid(ibus_rsp_valid), .m_rsp_rdata(ibus_rsp_rdata),
        .m_rsp_err(ibus_rsp_err), .irqs(irqs), .mtime(mtime),
        .other_syncs(d_syncs), .my_syncs(i_syncs), .done(i_done), .fails(i_fails),
        .n_bus(i_bus), .n_ops(i_ops));

    surf_script_port #(.PORT(0)) u_dport (
        .clk(clk), .rst(rst),
        .m_req_valid(dbus_req_valid), .m_req_ready(dbus_req_ready), .m_req_addr(dbus_req_addr),
        .m_req_write(dbus_req_write), .m_req_be(dbus_req_be), .m_req_wdata(dbus_req_wdata),
        .m_req_fetch(dbus_req_fetch), .m_rsp_valid(dbus_rsp_valid), .m_rsp_rdata(dbus_rsp_rdata),
        .m_rsp_err(dbus_rsp_err), .irqs(irqs), .mtime(mtime),
        .other_syncs(i_syncs), .my_syncs(d_syncs), .done(d_done), .fails(d_fails),
        .n_bus(d_bus), .n_ops(d_ops));

    // the verdict, once both ports are done
    reg     reported;
    reg     nofinish;
    integer cycles;
    initial begin
        reported = 1'b0;
        cycles   = 0;
        nofinish = $test$plusargs("script_nofinish");
        $display("surfcpu_core stand-in: scripted SurfBus master (RESET_PC %h, MIMPID %h)",
                 RESET_PC, MIMPID);
    end
    always @(posedge clk) begin
        if (!rst) cycles <= cycles + 1;
        if (!rst && i_done && d_done && !reported) begin
            reported <= 1'b1;
            if (i_fails == 32'h0 && d_fails == 32'h0) begin
                $display("SCRIPT PASS (%0d data + %0d fetch transfers, %0d + %0d operations, %0d cycles)",
                         d_bus, i_bus, d_ops, i_ops, cycles);
                if (!nofinish) $finish;
            end else begin
                $display("SCRIPT FAIL (%0d data-port and %0d fetch-port checks failed)", d_fails,
                         i_fails);
                if (!nofinish) $fatal(1, "bus script failed");
            end
        end
    end

`ifdef SURFCPU_RVFI
    assign rvfi_valid     = 1'b0;
    assign rvfi_order     = 64'h0;
    assign rvfi_insn      = 32'h0;
    assign rvfi_trap      = 1'b0;
    assign rvfi_halt      = 1'b0;
    assign rvfi_intr      = 1'b0;
    assign rvfi_mode      = 2'd3;
    assign rvfi_ixl       = 2'd1;
    assign rvfi_rs1_addr  = 5'd0;
    assign rvfi_rs2_addr  = 5'd0;
    assign rvfi_rs1_rdata = 32'h0;
    assign rvfi_rs2_rdata = 32'h0;
    assign rvfi_rd_addr   = 5'd0;
    assign rvfi_rd_wdata  = 32'h0;
    assign rvfi_pc_rdata  = 32'h0;
    assign rvfi_pc_wdata  = 32'h0;
    assign rvfi_mem_addr  = 32'h0;
    assign rvfi_mem_rmask = 4'h0;
    assign rvfi_mem_wmask = 4'h0;
    assign rvfi_mem_rdata = 32'h0;
    assign rvfi_mem_wdata = 32'h0;
`endif
endmodule
