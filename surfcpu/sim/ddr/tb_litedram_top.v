/*
SurfCPU DDR bridge with LiteDRAM (simulation top)
--------------------
File: tb_litedram_top.v Date: 10/16/26 (SurfCPU)
--------------------
surf_ddr_bridge -> surf_ddr_litedram_adapter -> litedram_sim, the LiteDRAM core generated from
the board's own configuration ip/litedram/arty.yml with --sim (ip/litedram/generate.sh: the same
controller settings, command buffers and native port as the bitstream's core, LiteDRAM's
behavioural DRAM model in place of the Artix-7 PHY; like the board's core, no CPU). The SurfBus
side has the bridge's sys-side ports, so tb_ddr_litedram.cpp drives it with the same bench as
the MIG model. ui_clk is LiteDRAM's clock (its user_clk follows it). Where the firmware
initializes the board's core (firmware/common/ddrinit.c, through surf_ddr_csr), this top writes
the initialization's last step over the core's Wishbone control port when init_go is 1
(control-bus addresses from the generated csr.csv: 0 ddrctrl_init_done, 4 ddrctrl_init_error):
init_done = 1 after a good calibration, or with init_fail init_error = 1 and then init_done = 1,
the order in which ddrinit.c (as liblitedram's sdram_init) reports a failed leveling. As on the
board, the native port stays blocked until init_done is 1 with init_error 0. Simulation only.
*/
module tb_litedram_top #(
    parameter integer TIMEOUT_CYCLES       = 512,
    parameter integer CALIB_TIMEOUT_CYCLES = 20000
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        s_req_valid,
    output wire        s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output wire        s_rsp_valid,
    output wire [31:0] s_rsp_rdata,
    output wire        s_rsp_err,
    output wire        calib_done,
    output wire        calib_fail,
    output wire        timeout,
    input  wire        ui_clk,
    input  wire        init_go,
    input  wire        init_fail
);
    wire         user_clk, user_rst, init_done, init_error;
    wire [23:0]  cmd_addr;
    wire         cmd_valid, cmd_ready, cmd_we;
    wire [127:0] wdata_data, rdata_data;
    wire [15:0]  wdata_we;
    wire         wdata_valid, wdata_ready, rdata_valid, rdata_ready;

    // the BIOS's last step: init_done <= 1, or init_error <= 1 then init_done <= 1
    reg          wb_cyc  = 1'b0;
    reg          wb_adr2 = 1'b0;        // 0: init_done (0x0), 1: init_error (0x4)
    reg  [1:0]   wb_left = 2'd0;        // writes still to do once init_go comes
    reg          wb_started = 1'b0;
    wire         wb_ack, wb_err;
    wire [31:0]  wb_dat_r;
    always @(posedge user_clk) begin
        if (user_rst) begin
            wb_cyc     <= 1'b0;
            wb_adr2    <= 1'b0;
            wb_left    <= 2'd0;
            wb_started <= 1'b0;
        end else if (wb_cyc) begin
            if (wb_ack || wb_err) begin
                wb_cyc  <= 1'b0;
                wb_adr2 <= 1'b0;
                wb_left <= wb_left - 2'd1;
            end
        end else if (init_go && !wb_started) begin
            wb_started <= 1'b1;
            wb_cyc     <= 1'b1;
            wb_adr2    <= init_fail;
            wb_left    <= init_fail ? 2'd2 : 2'd1;
        end else if (wb_left != 2'd0) begin
            wb_cyc <= 1'b1;                 // init_done after init_error
        end
    end

    litedram_sim ram (
        .clk(ui_clk), .sim_trace(1'b0),
        .init_done(init_done), .init_error(init_error),
        .user_clk(user_clk), .user_rst(user_rst),
        .user_port_native_0_cmd_addr(cmd_addr), .user_port_native_0_cmd_ready(cmd_ready),
        .user_port_native_0_cmd_valid(cmd_valid), .user_port_native_0_cmd_we(cmd_we),
        .user_port_native_0_rdata_data(rdata_data), .user_port_native_0_rdata_ready(rdata_ready),
        .user_port_native_0_rdata_valid(rdata_valid), .user_port_native_0_wdata_data(wdata_data),
        .user_port_native_0_wdata_ready(wdata_ready), .user_port_native_0_wdata_valid(wdata_valid),
        .user_port_native_0_wdata_we(wdata_we),
        .wb_ctrl_adr({29'd0, wb_adr2}), .wb_ctrl_dat_w(32'd1), .wb_ctrl_sel(4'hF), .wb_ctrl_cyc(wb_cyc),
        .wb_ctrl_stb(wb_cyc), .wb_ctrl_we(1'b1), .wb_ctrl_cti(3'd0), .wb_ctrl_bte(2'd0),
        .wb_ctrl_ack(wb_ack), .wb_ctrl_dat_r(wb_dat_r), .wb_ctrl_err(wb_err));

    wire         ui_rst, init_calib_complete;
    wire [27:0]  app_addr;
    wire [2:0]   app_cmd;
    wire         app_en, app_rdy, app_wdf_wren, app_wdf_end, app_wdf_rdy;
    wire [127:0] app_wdf_data, app_rd_data;
    wire [15:0]  app_wdf_mask;
    wire         app_rd_data_valid, app_rd_data_end, app_sr_req, app_ref_req, app_zq_req;

    surf_ddr_litedram_adapter adapter (
        .clk(user_clk), .rst(user_rst), .ui_rst(ui_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req),
        .init_done(init_done), .init_error(init_error),
        .cmd_valid(cmd_valid), .cmd_ready(cmd_ready), .cmd_we(cmd_we), .cmd_addr(cmd_addr),
        .wdata_valid(wdata_valid), .wdata_ready(wdata_ready), .wdata_data(wdata_data), .wdata_we(wdata_we),
        .rdata_valid(rdata_valid), .rdata_ready(rdata_ready), .rdata_data(rdata_data));

    surf_ddr_bridge #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES), .CALIB_TIMEOUT_CYCLES(CALIB_TIMEOUT_CYCLES)) bridge (
        .clk(clk), .rst(rst), .s_req_valid(s_req_valid), .s_req_ready(s_req_ready),
        .s_req_addr(s_req_addr), .s_req_write(s_req_write), .s_req_be(s_req_be),
        .s_req_wdata(s_req_wdata), .s_req_fetch(s_req_fetch), .s_rsp_valid(s_rsp_valid),
        .s_rsp_rdata(s_rsp_rdata), .s_rsp_err(s_rsp_err), .calib_done(calib_done),
        .calib_fail(calib_fail), .timeout(timeout),
        .ui_clk(user_clk), .ui_rst(ui_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req));

    wire unused = &{1'b0, wb_dat_r};
endmodule
