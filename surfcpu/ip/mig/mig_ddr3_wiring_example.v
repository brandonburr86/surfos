/*
SurfCPU DDR wiring example
--------------------
File: mig_ddr3_wiring_example.v Date: 10/15/26 (SurfCPU)
--------------------
How the board top (surfcpu_arty_a7_top, board track) connects the MIG and surf_ddr_bridge;
docs/DDR.md explains each choice. Lint-checked against ip/mig/mig_ddr3_stub.v and the bridge
(make check-mig); never part of a design. The board's clocking (an IBUF and a BUFG on the
100 MHz oscillator pin E3, a PLL or MMCM for sys_clk, pix_clk and the 200 MHz reference) is
the board track's and appears here only as inputs.

  clk100 (BUFG) ---> mig.sys_clk_i                     (MIG SystemClock: No Buffer)
  clk_ref_200   ---> mig.clk_ref_i                     (ReferenceClock: No Buffer; IDELAYCTRL)
  ck_rst_n & clocks_locked ---> mig.sys_rst (active low)
  mig.ui_clk, ui_clk_sync_rst, init_calib_complete, app_* <---> bridge controller side
  bridge SurfBus slave <---> surfcpu_soc's ddr_* master port; calib_done/calib_fail/timeout
                         ---> surfcpu_soc's ddr_calib_done/ddr_calib_fail/ddr_timeout
*/
module mig_ddr3_wiring_example (
    // from the board's clocking
    input  wire         clk100,             // the oscillator through IBUF + BUFG
    input  wire         clk_ref_200,        // 200 MHz from the board's PLL
    input  wire         sys_clk,            // 25 MHz SoC clock from the same PLL
    input  wire         clocks_locked,      // that PLL is locked
    input  wire         ck_rst_n,           // the RESET button, active low
    // DDR3L pins
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
    // the SoC's DDR master port (MODULES.md section 4)
    input  wire         ddr_req_valid,
    output wire         ddr_req_ready,
    input  wire [31:0]  ddr_req_addr,
    input  wire         ddr_req_write,
    input  wire [3:0]   ddr_req_be,
    input  wire [31:0]  ddr_req_wdata,
    input  wire         ddr_req_fetch,
    output wire         ddr_rsp_valid,
    output wire [31:0]  ddr_rsp_rdata,
    output wire         ddr_rsp_err,
    output wire         ddr_calib_done,
    output wire         ddr_calib_fail,
    output wire         ddr_timeout,
    output wire         sys_rst             // the SoC's reset (shown here because the bridge uses it)
);
    // The SoC's reset: asynchronous assertion, synchronous release in the sys_clk domain.
    surf_reset_sync sys_rst_sync (.clk(sys_clk), .arst(!ck_rst_n || !clocks_locked), .rst(sys_rst));

    // The MIG leaves reset only once its reference clock is stable.
    wire mig_sys_rst_n = ck_rst_n && clocks_locked;

    wire         ui_clk, ui_clk_sync_rst, init_calib_complete;
    wire [27:0]  app_addr;
    wire [2:0]   app_cmd;
    wire         app_en, app_rdy, app_wdf_wren, app_wdf_end, app_wdf_rdy;
    wire [127:0] app_wdf_data, app_rd_data;
    wire [15:0]  app_wdf_mask;
    wire         app_rd_data_valid, app_rd_data_end;
    wire         app_sr_req, app_ref_req, app_zq_req;
    wire         app_sr_active, app_ref_ack, app_zq_ack;
    wire         ui_addn_clk_0, ui_addn_clk_1, ui_addn_clk_2, ui_addn_clk_3, ui_addn_clk_4, mmcm_locked;
    wire [11:0]  device_temp;

    mig_ddr3 mig (
        .ddr3_dq(ddr3_dq), .ddr3_dqs_n(ddr3_dqs_n), .ddr3_dqs_p(ddr3_dqs_p),
        .ddr3_addr(ddr3_addr), .ddr3_ba(ddr3_ba), .ddr3_ras_n(ddr3_ras_n), .ddr3_cas_n(ddr3_cas_n),
        .ddr3_we_n(ddr3_we_n), .ddr3_reset_n(ddr3_reset_n), .ddr3_ck_p(ddr3_ck_p), .ddr3_ck_n(ddr3_ck_n),
        .ddr3_cke(ddr3_cke), .ddr3_cs_n(ddr3_cs_n), .ddr3_dm(ddr3_dm), .ddr3_odt(ddr3_odt),
        .sys_clk_i(clk100), .clk_ref_i(clk_ref_200), .sys_rst(mig_sys_rst_n),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en),
        .app_wdf_data(app_wdf_data), .app_wdf_end(app_wdf_end), .app_wdf_mask(app_wdf_mask),
        .app_wdf_wren(app_wdf_wren), .app_rd_data(app_rd_data), .app_rd_data_end(app_rd_data_end),
        .app_rd_data_valid(app_rd_data_valid), .app_rdy(app_rdy), .app_wdf_rdy(app_wdf_rdy),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req),
        .app_sr_active(app_sr_active), .app_ref_ack(app_ref_ack), .app_zq_ack(app_zq_ack),
        .ui_clk(ui_clk), .ui_clk_sync_rst(ui_clk_sync_rst),
        .ui_addn_clk_0(ui_addn_clk_0), .ui_addn_clk_1(ui_addn_clk_1), .ui_addn_clk_2(ui_addn_clk_2),
        .ui_addn_clk_3(ui_addn_clk_3), .ui_addn_clk_4(ui_addn_clk_4), .mmcm_locked(mmcm_locked),
        .init_calib_complete(init_calib_complete), .device_temp(device_temp));

    surf_ddr_bridge bridge (
        .clk(sys_clk), .rst(sys_rst),
        .s_req_valid(ddr_req_valid), .s_req_ready(ddr_req_ready), .s_req_addr(ddr_req_addr),
        .s_req_write(ddr_req_write), .s_req_be(ddr_req_be), .s_req_wdata(ddr_req_wdata),
        .s_req_fetch(ddr_req_fetch), .s_rsp_valid(ddr_rsp_valid), .s_rsp_rdata(ddr_rsp_rdata),
        .s_rsp_err(ddr_rsp_err), .calib_done(ddr_calib_done), .calib_fail(ddr_calib_fail),
        .timeout(ddr_timeout),
        .ui_clk(ui_clk), .ui_rst(ui_clk_sync_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req));

    // Left unused: the self-refresh/refresh/ZQ acknowledgements (never requested), the extra
    // ui clocks (clk_ref_i comes from the board's PLL; ui_addn_clk_0, about 200 MHz, is the
    // alternative Digilent's configuration provides) and the MIG's temperature reading.
    wire unused = &{1'b0, app_sr_active, app_ref_ack, app_zq_ack, ui_addn_clk_0, ui_addn_clk_1,
                    ui_addn_clk_2, ui_addn_clk_3, ui_addn_clk_4, mmcm_locked, device_temp};
endmodule
