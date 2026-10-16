/*
SurfCPU on the Arty A7 (top level)
--------------------
File: surfcpu_arty_a7_top.v Date: 10/16/26 (SurfCPU)
--------------------
The whole SoC on the board: arty_a7_clocks makes sys_clk and pix_clk and their resets, and
surfcpu_soc (MODULES.md section 4) gets the board's pins. Every pin is in WIRING.md and in
boards/arty_a7/xdc/arty_a7_{base,pmod,ddr3}_pins.xdc (tools/check-wiring.py checks they agree):

  base     clk100, ck_rst_n, sw, btn, led, rgb, uart_rx/uart_tx (the FT2232's UART)
  Pmod JA  Pmod MicroSD: sd_cs_n, sd_mosi, sd_miso, sd_sclk (SPI mode), sd_cd_n (card detect);
           sd_dat1, sd_dat2 are unused in SPI mode and only parked (inputs with pull-ups)
  Pmod JB  Pmod VGA J1: vga_r[3:0], vga_b[3:0]
  Pmod JC  Pmod VGA J2: vga_g[3:0], vga_hs, vga_vs
  Pmod JD  Pmod PS2 (6-pin, top row): ps2_data, ps2_clk (open drain through IOBUFs);
           ps2_data2, ps2_clk2 (the connector's spare pair) are only parked
  DDR3L    with ARTY_HAS_DDR defined only: ddr3_* (bank 34)

PS/2 is open drain: an IOBUF per line drives 0 while the SoC's _oe is 1 and otherwise lets go,
so the adapter's and the keyboard's pull-ups make the high level (never more than the PS/2
supply: see WIRING.md, the adapter must be powered from the Pmod's 3.3 V). The FPGA never
drives a PS/2 line high.

DDR3L (docs/DDR.md, docs/BOARD.md section 7). Without ARTY_HAS_DDR the top has no DDR3L pins,
HAS_DDR is 0, the SoC answers every DDR-window access with an error and its DDR master port is
idle. With ARTY_HAS_DDR (the design lists' "define" line) HAS_DDR is 1 and the SoC's DDR port
goes through surf_ddr_bridge (sys_clk to the controller's clock) and surf_ddr_litedram_adapter
to litedram_core, the LiteDRAM controller and Artix-7 PHY generated from ip/litedram/arty.yml
(make litedram-gen; ip/litedram/arty.sha256 holds the generated files' hashes). The core has no
CPU of its own: SurfCPU initializes and calibrates the DDR3L itself (firmware/common/ddrinit.c,
in SurfBoot and the diagnostics), through the SoC's ddrc port, surf_ddr_csr (sys_clk to the
controller's clock) and LiteDRAM's control bus (wb_ctrl), and writes LiteDRAM's init_done (and
init_error on a failure) there; those reach sysinfo.DDR_STATUS through the adapter
(init_calib_complete = init_done and not init_error) and the bridge (calib_done, or calib_fail
when calibration has not succeeded within DDR_CALIB_LIMIT sys_clk cycles of reset).
sysinfo.DDR_STATUS.SWINIT (DDR_SWINIT = 1 here) tells the firmware to do so.

Clocks: the oscillator pin has one input buffer (inserted by synthesis on clk100). It drives
arty_a7_clocks' two tiles directly, as without DDR, and, through u_dram_clkbuf (a BUFG),
LiteDRAM's PLL in a third clock tile: no second buffer on the pin, and no dedicated route to a
clock tile in another clock region. LiteDRAM's PLL makes its 100 MHz (core, user port, the
bridge's ui_clk), 400 MHz and 400 MHz at 90 degrees (DDR3 at 800 MT/s) and 200 MHz (IDELAYCTRL
reference), each through its own BUFG. The RESET button resets every clock tile, LiteDRAM's too,
so the controller calibrates again after it. Vendor primitives (IOBUF, BUFG, and the clock
tiles in arty_a7_clocks): this file belongs under boards/ only.
*/
// The build sets BOARD and the build identity. Vivado takes them as generics (build.tcl);
// the open flow has no top-level parameter option, so tools/fpga-build.sh passes the same
// values to Yosys as these defines (FLOW=openxc7), which become the defaults. With neither,
// they are the defaults below (simulation, lint).
`ifndef ARTY_BUILD_BOARD
`define ARTY_BUILD_BOARD "arty_a7_100t"
`endif
`ifndef ARTY_BUILD_ID
`define ARTY_BUILD_ID 32'h0
`endif
`ifndef ARTY_BUILD_FLAGS
`define ARTY_BUILD_FLAGS 32'h0
`endif
`ifndef ARTY_BUILD_TIME
`define ARTY_BUILD_TIME 32'h0
`endif
`ifndef ARTY_BOOTROM_INIT
`define ARTY_BOOTROM_INIT ""
`endif
// HAS_DDR follows the port list: the DDR3L ports and the memory controller exist only with
// ARTY_HAS_DDR (Yosys -D, Vivado -verilog_define; the design lists' "define" line).
`ifdef ARTY_HAS_DDR
`define ARTY_HAS_DDR_VALUE 1
`else
`define ARTY_HAS_DDR_VALUE 0
`endif
module surfcpu_arty_a7_top #(
    parameter         BOARD        = `ARTY_BUILD_BOARD,    // spec/soc.toml [boards] name
    parameter         BOOTROM_INIT = `ARTY_BOOTROM_INIT,   // $readmemh file for the boot ROM
    parameter [31:0]  BUILD_ID     = `ARTY_BUILD_ID,
    parameter [31:0]  BUILD_FLAGS  = `ARTY_BUILD_FLAGS,
    parameter [31:0]  BUILD_TIME   = `ARTY_BUILD_TIME
) (
    input  wire        clk100,
    input  wire        ck_rst_n,
    input  wire [3:0]  sw,
    input  wire [3:0]  btn,
    output wire [3:0]  led,
    output wire [11:0] rgb,
    input  wire        uart_rx,
    output wire        uart_tx,
    // Pmod JA: microSD
    output wire        sd_cs_n,
    output wire        sd_mosi,
    input  wire        sd_miso,
    output wire        sd_sclk,
    input  wire        sd_cd_n,
    input  wire        sd_dat1,
    input  wire        sd_dat2,
    // Pmod JB + JC: VGA
    output wire [3:0]  vga_r,
    output wire [3:0]  vga_g,
    output wire [3:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs,
    // Pmod JD: PS/2
    inout  wire        ps2_clk,
    inout  wire        ps2_data,
    input  wire        ps2_clk2,
    input  wire        ps2_data2
`ifdef ARTY_HAS_DDR
    ,
    // DDR3L, bank 34 (SSTL135): Micron MT41K128M16, x16, the names of Digilent's MIG project
    output wire [13:0] ddr3_addr,
    output wire [2:0]  ddr3_ba,
    output wire        ddr3_ras_n,
    output wire        ddr3_cas_n,
    output wire        ddr3_we_n,
    output wire        ddr3_reset_n,
    output wire        ddr3_ck_p,
    output wire        ddr3_ck_n,
    output wire        ddr3_cke,
    output wire        ddr3_cs_n,
    output wire [1:0]  ddr3_dm,
    output wire        ddr3_odt,
    inout  wire [15:0] ddr3_dq,
    inout  wire [1:0]  ddr3_dqs_p,
    inout  wire [1:0]  ddr3_dqs_n
`endif
);
    `include "surfcpu_soc.vh"

    localparam integer HAS_DDR = `ARTY_HAS_DDR_VALUE;

    // The waiver covers the string compare: BOARD and the names differ in length on purpose.
    /* verilator lint_off WIDTHEXPAND */
    localparam [31:0] BOARD_ID = (BOARD == "arty_a7_35t")  ? SURF_BOARD_ARTY_A7_35T :
                                 (BOARD == "arty_a7_100t") ? SURF_BOARD_ARTY_A7_100T : SURF_BOARD_SIM;
    /* verilator lint_on WIDTHEXPAND */

    wire sys_clk, sys_rst, pix_clk, pix_rst;

    /* verilator lint_off PINCONNECTEMPTY */   // the lock outputs are only for status
    arty_a7_clocks u_clocks (
        .clk100(clk100), .rst_btn(!ck_rst_n),
        .sys_clk(sys_clk), .sys_rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .sys_locked(), .pix_locked()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // ---------------------------------------------------------------- PS/2: open drain
    wire ps2_clk_i, ps2_data_i, ps2_clk_oe, ps2_data_oe;
    // (I/O standard, drive and slew are set in the XDC, the one place for pin properties)
    IOBUF u_ps2_clk  (.IO(ps2_clk),  .I(1'b0), .T(!ps2_clk_oe),  .O(ps2_clk_i));
    IOBUF u_ps2_data (.IO(ps2_data), .I(1'b0), .T(!ps2_data_oe), .O(ps2_data_i));

    // ---------------------------------------------------------------- the SoC
    wire        ddr_req_valid, ddr_req_ready, ddr_req_write, ddr_req_fetch;
    wire [31:0] ddr_req_addr, ddr_req_wdata, ddr_rsp_rdata;
    wire [3:0]  ddr_req_be;
    wire        ddr_rsp_valid, ddr_rsp_err, ddr_calib_done, ddr_calib_fail, ddr_timeout;
    wire        ddrc_req_valid, ddrc_req_ready, ddrc_req_write, ddrc_rsp_valid, ddrc_rsp_err;
    wire [31:0] ddrc_req_addr, ddrc_req_wdata, ddrc_rsp_rdata;
    wire [3:0]  ddrc_req_be;

    surfcpu_soc #(
        .BOOTROM_INIT (BOOTROM_INIT),
        .BOARD_ID     (BOARD_ID),
        .SYS_HZ       (SURF_SYS_HZ),
        .PIX_HZ       (SURF_PIX_HZ),
        .BUILD_ID     (BUILD_ID),
        .BUILD_FLAGS  (BUILD_FLAGS),
        .BUILD_TIME   (BUILD_TIME),
        .HAS_DDR      (HAS_DDR),
        .DDR_SIZE     (SURF_DDR_SIZE),
        .DDR_SWINIT   (HAS_DDR)          // LiteDRAM without a CPU: SurfCPU initializes it
    ) u_soc (
        .clk(sys_clk), .rst(sys_rst), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .uart_rx(uart_rx), .uart_tx(uart_tx),
        .led(led), .rgb(rgb), .sw(sw), .btn(btn),
        .sd_sclk(sd_sclk), .sd_mosi(sd_mosi), .sd_cs_n(sd_cs_n), .sd_miso(sd_miso), .sd_cd_n(sd_cd_n),
        .ps2_clk_i(ps2_clk_i), .ps2_data_i(ps2_data_i), .ps2_clk_oe(ps2_clk_oe), .ps2_data_oe(ps2_data_oe),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs),
        .ddr_req_valid(ddr_req_valid), .ddr_req_ready(ddr_req_ready), .ddr_req_addr(ddr_req_addr),
        .ddr_req_write(ddr_req_write), .ddr_req_be(ddr_req_be), .ddr_req_wdata(ddr_req_wdata),
        .ddr_req_fetch(ddr_req_fetch),
        .ddr_rsp_valid(ddr_rsp_valid), .ddr_rsp_rdata(ddr_rsp_rdata), .ddr_rsp_err(ddr_rsp_err),
        .ddr_calib_done(ddr_calib_done), .ddr_calib_fail(ddr_calib_fail), .ddr_timeout(ddr_timeout),
        .ddrc_req_valid(ddrc_req_valid), .ddrc_req_ready(ddrc_req_ready), .ddrc_req_addr(ddrc_req_addr),
        .ddrc_req_write(ddrc_req_write), .ddrc_req_be(ddrc_req_be), .ddrc_req_wdata(ddrc_req_wdata),
        .ddrc_rsp_valid(ddrc_rsp_valid), .ddrc_rsp_rdata(ddrc_rsp_rdata), .ddrc_rsp_err(ddrc_rsp_err)
    );

`ifdef ARTY_HAS_DDR
    // ---------------------------------------------------------------- DDR3L: bridge, adapter, LiteDRAM
    // How long calibration may take before DDR_STATUS.CALIB_FAIL, counted from reset and
    // including the firmware's start and its initialization of the controller: 1.5 s, the
    // bridge's default (the BRAM diagnostics start it after their 0.15 s of BRAM tests, and the
    // initialization takes a fraction of a second; docs/DDR.md section 10), under SurfBoot's and
    // the diagnostics' own 2 s, so they see CALIB_FAIL first; and the watchdog's limit for one
    // access, 4096 cycles (164 us). The register bridge's own limits: an access answered within
    // 1024 sys_clk cycles (41 us; one takes about half a microsecond), a bus answer within 256
    // controller cycles.
    localparam integer DDR_CALIB_LIMIT = SURF_SYS_HZ / 2 * 3;
    localparam integer DDR_WATCHDOG    = 4096;

    wire clk100_g;                          // the oscillator for LiteDRAM's PLL (see above)
    BUFG u_dram_clkbuf (.I(clk100), .O(clk100_g));

    wire         dram_clk, dram_rst, dram_init_done, dram_init_error, dram_pll_locked;
    wire [29:0]  dram_wb_adr;
    wire [31:0]  dram_wb_dat_w, dram_wb_dat_r;
    wire [3:0]   dram_wb_sel;
    wire [2:0]   dram_wb_cti;
    wire [1:0]   dram_wb_bte;
    wire         dram_wb_cyc, dram_wb_stb, dram_wb_we, dram_wb_ack, dram_wb_err;
    wire [23:0]  dram_cmd_addr;
    wire         dram_cmd_valid, dram_cmd_ready, dram_cmd_we;
    wire [127:0] dram_wdata, dram_rdata;
    wire [15:0]  dram_wdata_we;
    wire         dram_wdata_valid, dram_wdata_ready, dram_rdata_valid, dram_rdata_ready;

    litedram_core u_dram (
        .clk(clk100_g), .rst(!ck_rst_n), .pll_locked(dram_pll_locked),
        .ddram_a(ddr3_addr), .ddram_ba(ddr3_ba), .ddram_ras_n(ddr3_ras_n), .ddram_cas_n(ddr3_cas_n),
        .ddram_we_n(ddr3_we_n), .ddram_reset_n(ddr3_reset_n), .ddram_clk_p(ddr3_ck_p),
        .ddram_clk_n(ddr3_ck_n), .ddram_cke(ddr3_cke), .ddram_cs_n(ddr3_cs_n), .ddram_dm(ddr3_dm),
        .ddram_odt(ddr3_odt), .ddram_dq(ddr3_dq), .ddram_dqs_p(ddr3_dqs_p), .ddram_dqs_n(ddr3_dqs_n),
        .init_done(dram_init_done), .init_error(dram_init_error),
        .wb_ctrl_adr(dram_wb_adr), .wb_ctrl_dat_w(dram_wb_dat_w), .wb_ctrl_dat_r(dram_wb_dat_r),
        .wb_ctrl_sel(dram_wb_sel), .wb_ctrl_cyc(dram_wb_cyc), .wb_ctrl_stb(dram_wb_stb),
        .wb_ctrl_we(dram_wb_we), .wb_ctrl_cti(dram_wb_cti), .wb_ctrl_bte(dram_wb_bte),
        .wb_ctrl_ack(dram_wb_ack), .wb_ctrl_err(dram_wb_err),
        .user_clk(dram_clk), .user_rst(dram_rst),
        .user_port_native_0_cmd_addr(dram_cmd_addr), .user_port_native_0_cmd_ready(dram_cmd_ready),
        .user_port_native_0_cmd_valid(dram_cmd_valid), .user_port_native_0_cmd_we(dram_cmd_we),
        .user_port_native_0_rdata_data(dram_rdata), .user_port_native_0_rdata_ready(dram_rdata_ready),
        .user_port_native_0_rdata_valid(dram_rdata_valid), .user_port_native_0_wdata_data(dram_wdata),
        .user_port_native_0_wdata_ready(dram_wdata_ready), .user_port_native_0_wdata_valid(dram_wdata_valid),
        .user_port_native_0_wdata_we(dram_wdata_we)
    );

    wire         ui_rst, init_calib_complete;
    wire [27:0]  app_addr;
    wire [2:0]   app_cmd;
    wire         app_en, app_rdy, app_wdf_wren, app_wdf_end, app_wdf_rdy;
    wire [127:0] app_wdf_data, app_rd_data;
    wire [15:0]  app_wdf_mask;
    wire         app_rd_data_valid, app_rd_data_end, app_sr_req, app_ref_req, app_zq_req;

    surf_ddr_litedram_adapter u_dram_adapter (
        .clk(dram_clk), .rst(dram_rst), .ui_rst(ui_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req),
        .init_done(dram_init_done), .init_error(dram_init_error),
        .cmd_valid(dram_cmd_valid), .cmd_ready(dram_cmd_ready), .cmd_we(dram_cmd_we), .cmd_addr(dram_cmd_addr),
        .wdata_valid(dram_wdata_valid), .wdata_ready(dram_wdata_ready), .wdata_data(dram_wdata),
        .wdata_we(dram_wdata_we), .rdata_valid(dram_rdata_valid), .rdata_ready(dram_rdata_ready),
        .rdata_data(dram_rdata)
    );

    surf_ddr_bridge #(
        .TIMEOUT_CYCLES       (DDR_WATCHDOG),
        .CALIB_TIMEOUT_CYCLES (DDR_CALIB_LIMIT)
    ) u_ddr (
        .clk(sys_clk), .rst(sys_rst),
        .s_req_valid(ddr_req_valid), .s_req_ready(ddr_req_ready), .s_req_addr(ddr_req_addr),
        .s_req_write(ddr_req_write), .s_req_be(ddr_req_be), .s_req_wdata(ddr_req_wdata),
        .s_req_fetch(ddr_req_fetch), .s_rsp_valid(ddr_rsp_valid), .s_rsp_rdata(ddr_rsp_rdata),
        .s_rsp_err(ddr_rsp_err), .calib_done(ddr_calib_done), .calib_fail(ddr_calib_fail),
        .timeout(ddr_timeout),
        .ui_clk(dram_clk), .ui_rst(ui_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req)
    );

    // The SoC's ddrc port: LiteDRAM's control and status registers, for the firmware's
    // initialization and calibration (surf_ddr_csr: sys_clk to the controller's clock).
    surf_ddr_csr #(
        .ADDR_BITS         ($clog2(SURF_DDRC_SIZE)),
        .TIMEOUT_CYCLES    (1024),
        .WB_TIMEOUT_CYCLES (256)
    ) u_ddrc (
        .clk(sys_clk), .rst(sys_rst),
        .s_req_valid(ddrc_req_valid), .s_req_ready(ddrc_req_ready), .s_req_addr(ddrc_req_addr),
        .s_req_write(ddrc_req_write), .s_req_be(ddrc_req_be), .s_req_wdata(ddrc_req_wdata),
        .s_req_fetch(1'b0), .s_rsp_valid(ddrc_rsp_valid), .s_rsp_rdata(ddrc_rsp_rdata),
        .s_rsp_err(ddrc_rsp_err),
        .wb_clk(dram_clk), .wb_rst(dram_rst),
        .wb_adr(dram_wb_adr), .wb_dat_w(dram_wb_dat_w), .wb_dat_r(dram_wb_dat_r), .wb_sel(dram_wb_sel),
        .wb_cyc(dram_wb_cyc), .wb_stb(dram_wb_stb), .wb_we(dram_wb_we), .wb_cti(dram_wb_cti),
        .wb_bte(dram_wb_bte), .wb_ack(dram_wb_ack), .wb_err(dram_wb_err)
    );

    // pll_locked is LiteDRAM's own business (its resets follow it); nothing else reads it.
    wire unused_dram = &{1'b0, dram_pll_locked};
`else
    // No DDR: the SoC errors the window itself and its DDR master port stays idle.
    assign ddr_req_ready  = 1'b0;
    assign ddr_rsp_valid  = 1'b0;
    assign ddr_rsp_rdata  = 32'h0;
    assign ddr_rsp_err    = 1'b0;
    assign ddr_calib_done = 1'b0;
    assign ddr_calib_fail = 1'b0;
    assign ddr_timeout    = 1'b0;
    assign ddrc_req_ready = 1'b0;
    assign ddrc_rsp_valid = 1'b0;
    assign ddrc_rsp_rdata = 32'h0;
    assign ddrc_rsp_err   = 1'b0;
    wire unused_ddr = &{1'b0, ddr_req_valid, ddr_req_addr, ddr_req_write, ddr_req_be, ddr_req_wdata,
                        ddr_req_fetch, ddrc_req_valid, ddrc_req_addr, ddrc_req_write, ddrc_req_be,
                        ddrc_req_wdata};
`endif

    // The parked inputs (pulled up in the XDC) have no logic behind them.
    wire unused_ok = &{1'b0, sd_dat1, sd_dat2, ps2_clk2, ps2_data2};
endmodule
