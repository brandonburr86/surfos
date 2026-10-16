/*
SurfCPU SoC
--------------------
File: surfcpu_soc.v Date: 10/15/26 (SurfCPU)
--------------------
The system around SurfCPU (docs/MODULES.md section 4, docs/SOC.md): the core, the SurfBus
interconnect of docs/SURFBUS.md section 6, the boot ROM, scratch RAM and text RAM, and the
peripherals of docs/MEMORY-MAP.md (uart, timer, intc, gpio, spi, ps2, video, sysinfo), all in
the clk domain except the video scan-out and the text RAM's pixel port (pix_clk). The DDR
window leaves as a SurfBus master port toward surf_ddr_bridge (or a simulation model); the
SoC answers every DDR-window access with an error while HAS_DDR is 0 or ddr_calib_done is 0,
so nothing reaches the bridge before calibration. rst and pix_rst come from a
surf_reset_sync in each domain (the board top); ddr_calib_done, ddr_calib_fail and
ddr_timeout come from the bridge, already in the clk domain. In simulation the boot ROM can
also be loaded with +bootrom=<file>.
Interrupts: mip.MTIP and mip.MSIP from the timer, mip.MEIP from the interrupt controller,
whose sources are numbered by spec/soc.toml [irqs].
*/
`include "surf_bus_targets.vh"
`include "surfcpu_rvfi.vh"

module surfcpu_soc #(
    parameter         BOOTROM_INIT = "",           // $readmemh file for the boot ROM; "" = all zero
    parameter [31:0]  BOARD_ID     = 32'h0000_00F0,
    parameter integer SYS_HZ       = 25000000,
    parameter integer PIX_HZ       = 25173611,
    parameter [31:0]  BUILD_ID     = 32'h0,
    parameter [31:0]  BUILD_FLAGS  = 32'h0,
    parameter [31:0]  BUILD_TIME   = 32'h0,
    parameter integer HAS_DDR      = 1,
    parameter [31:0]  DDR_SIZE     = 32'h1000_0000
) (
    input  wire        clk, rst, pix_clk, pix_rst,
    input  wire        uart_rx,  output wire uart_tx,
    output wire [3:0]  led,      output wire [11:0] rgb,  input wire [3:0] sw, input wire [3:0] btn,
    output wire        sd_sclk, sd_mosi, sd_cs_n,  input wire sd_miso, sd_cd_n,
    input  wire        ps2_clk_i, ps2_data_i,      output wire ps2_clk_oe, ps2_data_oe,
    output wire [3:0]  vga_r, vga_g, vga_b,        output wire vga_hs, vga_vs,
    // DDR: a SurfBus master port toward surf_ddr_bridge (or the simulation's DDR model)
    output wire        ddr_req_valid, input wire ddr_req_ready, output wire [31:0] ddr_req_addr,
    output wire        ddr_req_write, output wire [3:0] ddr_req_be, output wire [31:0] ddr_req_wdata,
    output wire        ddr_req_fetch,
    input  wire        ddr_rsp_valid, input wire [31:0] ddr_rsp_rdata, input wire ddr_rsp_err,
    input  wire        ddr_calib_done, ddr_calib_fail, ddr_timeout
`ifdef SURFCPU_RVFI
    , `SURFCPU_RVFI_OUTPUTS
`endif
);
    `include "surfcpu_soc.vh"

    localparam integer KI = `SURF_IT_DDR;     // instruction-side targets besides DDR
    localparam integer KD = `SURF_DT_DDR;     // data-side targets besides DDR

    localparam [31:0] FEATURES  = SURF_SYSINFO_FEATURES_UART_MASK | SURF_SYSINFO_FEATURES_TIMER_MASK |
                                  SURF_SYSINFO_FEATURES_IRQ_MASK  | SURF_SYSINFO_FEATURES_GPIO_MASK |
                                  SURF_SYSINFO_FEATURES_SPI_MASK  | SURF_SYSINFO_FEATURES_PS2_MASK |
                                  SURF_SYSINFO_FEATURES_VIDEO_MASK |
                                  ((HAS_DDR != 0) ? SURF_SYSINFO_FEATURES_DDR_MASK : 32'h0);
    localparam [31:0] DDR_BYTES = (HAS_DDR != 0) ? DDR_SIZE : 32'h0;

    // ---------------------------------------------------------------- the core
    wire        ibus_req_valid, ibus_req_ready, ibus_req_write, ibus_req_fetch;
    wire        ibus_rsp_valid, ibus_rsp_err;
    wire [31:0] ibus_req_addr, ibus_req_wdata, ibus_rsp_rdata;
    wire [3:0]  ibus_req_be;
    wire        dbus_req_valid, dbus_req_ready, dbus_req_write, dbus_req_fetch;
    wire        dbus_rsp_valid, dbus_rsp_err;
    wire [31:0] dbus_req_addr, dbus_req_wdata, dbus_rsp_rdata;
    wire [3:0]  dbus_req_be;
    wire        irq_external, irq_timer, irq_software;
    wire [63:0] mtime;

    surfcpu_core #(.RESET_PC(SURF_RESET_PC), .MIMPID(SURF_SOC_VERSION)) u_core (
        .clk(clk), .rst(rst),
        .ibus_req_valid(ibus_req_valid), .ibus_req_ready(ibus_req_ready),
        .ibus_req_addr(ibus_req_addr), .ibus_req_write(ibus_req_write),
        .ibus_req_be(ibus_req_be), .ibus_req_wdata(ibus_req_wdata),
        .ibus_req_fetch(ibus_req_fetch), .ibus_rsp_valid(ibus_rsp_valid),
        .ibus_rsp_rdata(ibus_rsp_rdata), .ibus_rsp_err(ibus_rsp_err),
        .dbus_req_valid(dbus_req_valid), .dbus_req_ready(dbus_req_ready),
        .dbus_req_addr(dbus_req_addr), .dbus_req_write(dbus_req_write),
        .dbus_req_be(dbus_req_be), .dbus_req_wdata(dbus_req_wdata),
        .dbus_req_fetch(dbus_req_fetch), .dbus_rsp_valid(dbus_rsp_valid),
        .dbus_rsp_rdata(dbus_rsp_rdata), .dbus_rsp_err(dbus_rsp_err),
        .irq_external(irq_external), .irq_timer(irq_timer), .irq_software(irq_software),
        .mtime(mtime)
`ifdef SURFCPU_RVFI
        , `SURFCPU_RVFI_CONNECT
`endif
    );

    // ---------------------------------------------------------------- the interconnect
    wire [KI-1:0]    it_req_valid, it_req_ready, it_rsp_valid, it_rsp_err;
    wire [31:0]      it_req_addr, it_req_wdata;
    wire             it_req_write, it_req_fetch;
    wire [3:0]       it_req_be;
    wire [32*KI-1:0] it_rsp_rdata;
    wire [KD-1:0]    dt_req_valid, dt_req_ready, dt_rsp_valid, dt_rsp_err;
    wire [31:0]      dt_req_addr, dt_req_wdata;
    wire             dt_req_write, dt_req_fetch;
    wire [3:0]       dt_req_be;
    wire [32*KD-1:0] dt_rsp_rdata;

    surf_interconnect #(.HAS_DDR(HAS_DDR)) u_bus (
        .clk(clk), .rst(rst), .ddr_calib_done(ddr_calib_done),
        .i_req_valid(ibus_req_valid), .i_req_ready(ibus_req_ready), .i_req_addr(ibus_req_addr),
        .i_req_write(ibus_req_write), .i_req_be(ibus_req_be), .i_req_wdata(ibus_req_wdata),
        .i_req_fetch(ibus_req_fetch), .i_rsp_valid(ibus_rsp_valid), .i_rsp_rdata(ibus_rsp_rdata),
        .i_rsp_err(ibus_rsp_err),
        .d_req_valid(dbus_req_valid), .d_req_ready(dbus_req_ready), .d_req_addr(dbus_req_addr),
        .d_req_write(dbus_req_write), .d_req_be(dbus_req_be), .d_req_wdata(dbus_req_wdata),
        .d_req_fetch(dbus_req_fetch), .d_rsp_valid(dbus_rsp_valid), .d_rsp_rdata(dbus_rsp_rdata),
        .d_rsp_err(dbus_rsp_err),
        .it_req_valid(it_req_valid), .it_req_ready(it_req_ready), .it_req_addr(it_req_addr),
        .it_req_write(it_req_write), .it_req_be(it_req_be), .it_req_wdata(it_req_wdata),
        .it_req_fetch(it_req_fetch), .it_rsp_valid(it_rsp_valid), .it_rsp_rdata(it_rsp_rdata),
        .it_rsp_err(it_rsp_err),
        .dt_req_valid(dt_req_valid), .dt_req_ready(dt_req_ready), .dt_req_addr(dt_req_addr),
        .dt_req_write(dt_req_write), .dt_req_be(dt_req_be), .dt_req_wdata(dt_req_wdata),
        .dt_req_fetch(dt_req_fetch), .dt_rsp_valid(dt_rsp_valid), .dt_rsp_rdata(dt_rsp_rdata),
        .dt_rsp_err(dt_rsp_err),
        .ddr_req_valid(ddr_req_valid), .ddr_req_ready(ddr_req_ready),
        .ddr_req_addr(ddr_req_addr), .ddr_req_write(ddr_req_write), .ddr_req_be(ddr_req_be),
        .ddr_req_wdata(ddr_req_wdata), .ddr_req_fetch(ddr_req_fetch),
        .ddr_rsp_valid(ddr_rsp_valid), .ddr_rsp_rdata(ddr_rsp_rdata), .ddr_rsp_err(ddr_rsp_err));

    // target ports of the interconnect, by number (surf_bus_targets.vh): a memory's port A on
    // the instruction side, a memory's port B and a peripheral's slave port on the data side
`define SURF_IT_A(T) \
    .a_req_valid(it_req_valid[T]), .a_req_ready(it_req_ready[T]), .a_req_addr(it_req_addr), \
    .a_req_write(it_req_write), .a_req_be(it_req_be), .a_req_wdata(it_req_wdata), \
    .a_req_fetch(it_req_fetch), .a_rsp_valid(it_rsp_valid[T]), \
    .a_rsp_rdata(it_rsp_rdata[32*(T) +: 32]), .a_rsp_err(it_rsp_err[T])
`define SURF_DT_B(T) \
    .b_req_valid(dt_req_valid[T]), .b_req_ready(dt_req_ready[T]), .b_req_addr(dt_req_addr), \
    .b_req_write(dt_req_write), .b_req_be(dt_req_be), .b_req_wdata(dt_req_wdata), \
    .b_req_fetch(dt_req_fetch), .b_rsp_valid(dt_rsp_valid[T]), \
    .b_rsp_rdata(dt_rsp_rdata[32*(T) +: 32]), .b_rsp_err(dt_rsp_err[T])
`define SURF_DT_S(T) \
    .s_req_valid(dt_req_valid[T]), .s_req_ready(dt_req_ready[T]), .s_req_addr(dt_req_addr), \
    .s_req_write(dt_req_write), .s_req_be(dt_req_be), .s_req_wdata(dt_req_wdata), \
    .s_req_fetch(dt_req_fetch), .s_rsp_valid(dt_rsp_valid[T]), \
    .s_rsp_rdata(dt_rsp_rdata[32*(T) +: 32]), .s_rsp_err(dt_rsp_err[T])

    // ---------------------------------------------------------------- memories
    surf_bootrom #(.INIT_FILE(BOOTROM_INIT)) u_bootrom (
        .clk(clk), .rst(rst), `SURF_IT_A(`SURF_IT_ROM), `SURF_DT_B(`SURF_DT_ROM));

    surf_scratch u_scratch (
        .clk(clk), .rst(rst), `SURF_IT_A(`SURF_IT_SCRATCH), `SURF_DT_B(`SURF_DT_SCRATCH));

    wire [11:0] text_addr;
    wire [31:0] text_data;
    surf_textram u_textram (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_TEXTRAM),
        .pix_clk(pix_clk), .pix_addr(text_addr), .pix_data(text_data));

    // ---------------------------------------------------------------- peripherals
    wire uart_irq, gpio_irq, spi_irq, ps2_irq, video_irq;

    surf_uart #(.SYS_HZ(SYS_HZ)) u_uart (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_UART),
        .irq(uart_irq), .rx(uart_rx), .tx(uart_tx));

    surf_timer #(.SYS_HZ(SYS_HZ), .TIMEBASE_HZ(SURF_TIMEBASE_HZ)) u_timer (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_TIMER),
        .mtime(mtime), .irq_timer(irq_timer), .irq_software(irq_software));

    reg [31:0] irq_sources;
    always @* begin
        irq_sources = 32'h0;
        irq_sources[SURF_IRQ_UART]  = uart_irq;
        irq_sources[SURF_IRQ_PS2]   = ps2_irq;
        irq_sources[SURF_IRQ_GPIO]  = gpio_irq;
        irq_sources[SURF_IRQ_VIDEO] = video_irq;
        irq_sources[SURF_IRQ_SPI]   = spi_irq;
    end

    surf_intc u_intc (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_INTC),
        .sources(irq_sources), .irq_external(irq_external));

    surf_gpio #(.SYS_HZ(SYS_HZ)) u_gpio (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_GPIO),
        .irq(gpio_irq), .led(led), .rgb(rgb), .sw(sw), .btn(btn));

    surf_spi #(.SYS_HZ(SYS_HZ)) u_spi (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_SPI),
        .irq(spi_irq), .sclk(sd_sclk), .mosi(sd_mosi), .miso(sd_miso), .cs_n(sd_cs_n),
        .cd_n(sd_cd_n));

    surf_ps2 #(.SYS_HZ(SYS_HZ)) u_ps2 (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_PS2),
        .irq(ps2_irq), .ps2_clk_i(ps2_clk_i), .ps2_data_i(ps2_data_i),
        .ps2_clk_oe(ps2_clk_oe), .ps2_data_oe(ps2_data_oe));

    surf_video #(.PIX_HZ(PIX_HZ)) u_video (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_VIDEO),
        .irq(video_irq), .pix_clk(pix_clk), .pix_rst(pix_rst),
        .text_addr(text_addr), .text_data(text_data),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs));

    surf_sysinfo #(
        .BOARD_ID(BOARD_ID), .SYS_HZ(SYS_HZ), .PIX_HZ(PIX_HZ), .BUILD_ID(BUILD_ID),
        .BUILD_FLAGS(BUILD_FLAGS), .BUILD_TIME(BUILD_TIME), .FEATURES(FEATURES),
        .DDR_SIZE(DDR_BYTES)
    ) u_sysinfo (
        .clk(clk), .rst(rst), `SURF_DT_S(`SURF_DT_SYSINFO),
        .ddr_calib_done((HAS_DDR != 0) && ddr_calib_done),
        .ddr_calib_fail((HAS_DDR != 0) && ddr_calib_fail),
        .ddr_timeout((HAS_DDR != 0) && ddr_timeout));

`undef SURF_IT_A
`undef SURF_DT_B
`undef SURF_DT_S
endmodule
