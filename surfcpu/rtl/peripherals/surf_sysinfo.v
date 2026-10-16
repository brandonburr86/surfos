/*
SurfCPU system information
--------------------
File: surf_sysinfo.v Date: 10/15/26 (SurfCPU)
--------------------
sysinfo at 0x1000_7000 (docs/MEMORY-MAP.md): the identity of the board and the build, the
clock rates and the DDR state, all read only except SCRATCH. MAGIC reads 0x46525553 ("SURF"
in memory order); the versions come from spec/soc.toml; the build values are parameters set
by the board top or the simulation. DDR_STATUS shows the bridge's calibration levels
(CALIB_DONE, CALIB_FAIL), PRESENT = FEATURES.DDR, and TIMEOUT, which latches any
ddr_timeout from the bridge's watchdog and stays set until reset; it reads 1 already in the
cycle of the pulse, so a read right behind the abandoned access (which the bridge answers in
that same cycle) sees it. BUILD_FLAGS and FEATURES
show only their defined bits. A SurfBus slave (docs/SURFBUS.md) that accepts every request
and answers in the next cycle; an unknown offset, a non-word access or a fetch gets an error
and has no effect; writes to read-only registers are ignored.
*/
module surf_sysinfo #(
    parameter [31:0]  BOARD_ID    = 32'h0000_00F0,
    parameter integer SYS_HZ      = 25000000,
    parameter integer PIX_HZ      = 25173611,
    parameter [31:0]  BUILD_ID    = 32'h0,
    parameter [31:0]  BUILD_FLAGS = 32'h0,
    parameter [31:0]  BUILD_TIME  = 32'h0,
    parameter [31:0]  FEATURES    = 32'h0000_00FF,
    parameter [31:0]  DDR_SIZE    = 32'h1000_0000
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
    input  wire        ddr_calib_done,
    input  wire        ddr_calib_fail,
    input  wire        ddr_timeout
);
    `include "surfcpu_soc.vh"

    // MEMORY-MAP.md gives the magic only as text: the bytes "SURF" at increasing addresses
    localparam [31:0] MAGIC = 32'h4652_5553;

    localparam [31:0] FLAGS_DEFINED = SURF_SYSINFO_BUILD_FLAGS_DIRTY_MASK |
                                      SURF_SYSINFO_BUILD_FLAGS_SIM_MASK;
    localparam [31:0] FEATURES_DEFINED = SURF_SYSINFO_FEATURES_UART_MASK |
                                         SURF_SYSINFO_FEATURES_TIMER_MASK |
                                         SURF_SYSINFO_FEATURES_IRQ_MASK |
                                         SURF_SYSINFO_FEATURES_GPIO_MASK |
                                         SURF_SYSINFO_FEATURES_SPI_MASK |
                                         SURF_SYSINFO_FEATURES_PS2_MASK |
                                         SURF_SYSINFO_FEATURES_VIDEO_MASK |
                                         SURF_SYSINFO_FEATURES_DDR_MASK;
    localparam [31:0] SYS_HZ_W  = SYS_HZ;
    localparam [31:0] PIX_HZ_W  = PIX_HZ;
    localparam [31:0] MAP_VER_W = SURF_MAP_VERSION;

    reg        timeout_q;   // sticky until reset
    reg [31:0] scratch_q;

    reg [31:0] ddr_status;
    always @* begin
        ddr_status = 32'h0;
        ddr_status[SURF_SYSINFO_DDR_STATUS_CALIB_DONE_LSB] = ddr_calib_done;
        ddr_status[SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_LSB] = ddr_calib_fail;
        ddr_status[SURF_SYSINFO_DDR_STATUS_PRESENT_LSB]    = FEATURES[SURF_SYSINFO_FEATURES_DDR_LSB];
        ddr_status[SURF_SYSINFO_DDR_STATUS_TIMEOUT_LSB]    = timeout_q | ddr_timeout;
    end

    // ---------------------------------------------------------------- SurfBus slave
    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    reg        known;
    reg [31:0] rd_mux;
    always @* begin
        known  = 1'b1;
        rd_mux = 32'h0;
        case (woff)
            SURF_SYSINFO_MAGIC[11:2]:       rd_mux = MAGIC;
            SURF_SYSINFO_SOC_VERSION[11:2]: rd_mux = SURF_SOC_VERSION;
            SURF_SYSINFO_MAP_VERSION[11:2]: rd_mux = MAP_VER_W;
            SURF_SYSINFO_BOARD_ID[11:2]:    rd_mux = BOARD_ID;
            SURF_SYSINFO_SYS_HZ[11:2]:      rd_mux = SYS_HZ_W;
            SURF_SYSINFO_BUILD_ID[11:2]:    rd_mux = BUILD_ID;
            SURF_SYSINFO_BUILD_FLAGS[11:2]: rd_mux = BUILD_FLAGS & FLAGS_DEFINED;
            SURF_SYSINFO_FEATURES[11:2]:    rd_mux = FEATURES & FEATURES_DEFINED;
            SURF_SYSINFO_DDR_STATUS[11:2]:  rd_mux = ddr_status;
            SURF_SYSINFO_DDR_SIZE[11:2]:    rd_mux = DDR_SIZE;
            SURF_SYSINFO_PIX_HZ[11:2]:      rd_mux = PIX_HZ_W;
            SURF_SYSINFO_SCRATCH[11:2]:     rd_mux = scratch_q;
            SURF_SYSINFO_BUILD_TIME[11:2]:  rd_mux = BUILD_TIME;
            default:                        known  = 1'b0;
        endcase
    end

    wire ok    = s_req_valid && word_acc && known;
    wire do_wr = ok && s_req_write;

    reg        rsp_valid_q, rsp_err_q;
    reg [31:0] rsp_rdata_q;
    always @(posedge clk) begin
        if (rst) begin
            timeout_q   <= 1'b0;
            scratch_q   <= 32'h0;
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'h0;
        end else begin
            if (ddr_timeout) timeout_q <= 1'b1;
            rsp_valid_q <= s_req_valid;
            rsp_err_q   <= s_req_valid && !(word_acc && known);
            rsp_rdata_q <= (ok && !s_req_write) ? rd_mux : 32'h0;
            if (do_wr && woff == SURF_SYSINFO_SCRATCH[11:2]) scratch_q <= s_req_wdata;
        end
    end

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_err   = rsp_err_q;
    assign s_rsp_rdata = rsp_rdata_q;

    // the decoder has already matched the window (address bits 31:12)
    wire unused_ok = &{1'b0, s_req_addr[31:12]};
endmodule
