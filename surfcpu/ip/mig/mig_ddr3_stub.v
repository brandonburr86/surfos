/*
SurfCPU MIG port list (expected)
--------------------
File: mig_ddr3_stub.v Date: 10/15/26 (SurfCPU)
--------------------
The ports that MIG 7 Series 4.2 is expected to give mig_ddr3 when generated from
ip/mig/<board>/mig_ddr3.prj (native interface, one x16 DDR3 controller, 4:1, one rank, data
mask, system and reference clocks without buffers, active-low sys_rst, XADC temperature
monitoring, one extra ui clock). It lets the board top, the example wiring and their lint run
without Vivado; it is a black box (empty) and never part of a bitstream: the real module comes
from the generated IP (mig_ddr3.tcl).

PENDING: written from UG586 and Digilent's configuration, not from generated output. The first
Vivado run must compare it with the generated mig_ddr3.veo (in particular whether the extra
ui_addn_clk_1..4 and mmcm_locked ports exist with UIExtraClocks = 1) and correct this file.
*/
// A black box has nothing inside: the generated IP drives its outputs and reads its inputs.
/* verilator lint_off UNDRIVEN */
/* verilator lint_off UNUSEDSIGNAL */
(* black_box *)
module mig_ddr3 (
    // DDR3L pins (locations and I/O standards come from the MIG's generated constraints)
    inout  wire [15:0]  ddr3_dq,
    inout  wire [1:0]   ddr3_dqs_n,
    inout  wire [1:0]   ddr3_dqs_p,
    output wire [13:0]  ddr3_addr,
    output wire [2:0]   ddr3_ba,
    output wire         ddr3_ras_n,
    output wire         ddr3_cas_n,
    output wire         ddr3_we_n,
    output wire         ddr3_reset_n,
    output wire [0:0]   ddr3_ck_p,
    output wire [0:0]   ddr3_ck_n,
    output wire [0:0]   ddr3_cke,
    output wire [0:0]   ddr3_cs_n,
    output wire [1:0]   ddr3_dm,
    output wire [0:0]   ddr3_odt,
    // clocks and reset
    input  wire         sys_clk_i,          // 100 MHz from a global buffer (SystemClock: No Buffer)
    input  wire         clk_ref_i,          // 200 MHz IDELAYCTRL reference (ReferenceClock: No Buffer)
    input  wire         sys_rst,            // active low, asynchronous
    // native user interface (ui_clk domain)
    input  wire [27:0]  app_addr,
    input  wire [2:0]   app_cmd,
    input  wire         app_en,
    input  wire [127:0] app_wdf_data,
    input  wire         app_wdf_end,
    input  wire [15:0]  app_wdf_mask,
    input  wire         app_wdf_wren,
    output wire [127:0] app_rd_data,
    output wire         app_rd_data_end,
    output wire         app_rd_data_valid,
    output wire         app_rdy,
    output wire         app_wdf_rdy,
    input  wire         app_sr_req,
    input  wire         app_ref_req,
    input  wire         app_zq_req,
    output wire         app_sr_active,
    output wire         app_ref_ack,
    output wire         app_zq_ack,
    output wire         ui_clk,             // 81.25 MHz (memory clock 325 MHz / 4)
    output wire         ui_clk_sync_rst,
    output wire         ui_addn_clk_0,      // about 200 MHz (UIExtraClocks, MMCMClkOut0 = 3.25)
    output wire         ui_addn_clk_1,
    output wire         ui_addn_clk_2,
    output wire         ui_addn_clk_3,
    output wire         ui_addn_clk_4,
    output wire         mmcm_locked,
    output wire         init_calib_complete,
    output wire [11:0]  device_temp         // XADC temperature, as the MIG samples it
);
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNDRIVEN */
