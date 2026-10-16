/*
SurfCPU surf_sysinfo testbench
--------------------
File: tb_sysinfo.v Date: 10/16/26 (SurfCPU)
--------------------
Icarus testbench for the system information block through the SurfBus BFM: every register
against the parameters given (with junk in the undefined BUILD_FLAGS and FEATURES bits to
check they read 0), DDR_STATUS following the calibration inputs, PRESENT from FEATURES.DDR,
TIMEOUT latched from a one-cycle pulse and cleared only by reset, SCRATCH read/write, writes
to read-only registers ignored, and the error rules. The tb parameter DDR selects whether
FEATURES has the DDR bit, SWINIT the sysinfo's DDR_SWINIT (DDR_STATUS.SWINIT, set only with the
DDR bit). +seed=N. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_sysinfo;
    `include "surfcpu_soc.vh"

    parameter integer DDR = 1;
    parameter integer SWINIT = 0;

    localparam [31:0] P_BOARD   = 32'h0000_0135;
    localparam integer P_SYS    = 12345678;
    localparam integer P_PIX    = 25173611;
    localparam [31:0] P_BUILD   = 32'hDEAD_BEEF;
    localparam [31:0] P_FLAGS   = 32'hFFFF_FFFE;   // SIM set, DIRTY clear, junk above
    localparam [31:0] P_TIME    = 32'h6720_1234;
    localparam [31:0] P_FEAT    = (DDR != 0) ? 32'hFFFF_FFFF : 32'hFFFF_FF7F;
    localparam [31:0] P_DDRSIZE = (DDR != 0) ? 32'h1000_0000 : 32'h0;

    reg clk = 1'b0;
    always #20 clk = ~clk;
    reg rst = 1'b1;
    integer seed = 1;
    integer errors = 0;

    wire        req_valid, req_ready, req_write, req_fetch, rsp_valid, rsp_err;
    wire [31:0] req_addr, req_wdata, rsp_rdata;
    wire [3:0]  req_be;
    reg         calib_done = 1'b0, calib_fail = 1'b0, timeout = 1'b0;

    surfbus_bfm #(.NAME("bfm")) bfm (
        .clk(clk), .m_req_valid(req_valid), .m_req_ready(req_ready), .m_req_addr(req_addr),
        .m_req_write(req_write), .m_req_be(req_be), .m_req_wdata(req_wdata),
        .m_req_fetch(req_fetch), .m_rsp_valid(rsp_valid), .m_rsp_rdata(rsp_rdata),
        .m_rsp_err(rsp_err));
    surfbus_monitor #(.NAME("mon"), .MAX_LAT(1)) mon (
        .clk(clk), .rst(rst), .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_write(req_write), .req_be(req_be), .req_wdata(req_wdata), .req_fetch(req_fetch),
        .rsp_valid(rsp_valid), .rsp_rdata(rsp_rdata), .rsp_err(rsp_err));
    surf_sysinfo #(
        .BOARD_ID(P_BOARD), .SYS_HZ(P_SYS), .PIX_HZ(P_PIX), .BUILD_ID(P_BUILD),
        .BUILD_FLAGS(P_FLAGS), .BUILD_TIME(P_TIME), .FEATURES(P_FEAT), .DDR_SIZE(P_DDRSIZE),
        .DDR_SWINIT(SWINIT)
    ) dut (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid), .s_req_ready(req_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata),
        .s_req_fetch(req_fetch), .s_rsp_valid(rsp_valid), .s_rsp_rdata(rsp_rdata),
        .s_rsp_err(rsp_err),
        .ddr_calib_done(calib_done), .ddr_calib_fail(calib_fail), .ddr_timeout(timeout));

    localparam [31:0] B = SURF_SYSINFO_BASE;
    localparam [31:0] ALL = 32'hFFFF_FFFF;

    reg [31:0] scratch_model = 32'h0;

    function [31:0] ddr_status;
        input done, fail, to;
        begin
            ddr_status = 32'h0;
            ddr_status[SURF_SYSINFO_DDR_STATUS_CALIB_DONE_LSB] = done;
            ddr_status[SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_LSB] = fail;
            ddr_status[SURF_SYSINFO_DDR_STATUS_PRESENT_LSB]    = (DDR != 0);
            ddr_status[SURF_SYSINFO_DDR_STATUS_TIMEOUT_LSB]    = to;
            ddr_status[SURF_SYSINFO_DDR_STATUS_SWINIT_LSB]     = (DDR != 0) && (SWINIT != 0);
        end
    endfunction

    task check_ids;
        begin
            bfm.read_check(B + SURF_SYSINFO_MAGIC, 4'hF, 32'h4652_5553, ALL);
            bfm.read_check(B + SURF_SYSINFO_SOC_VERSION, 4'hF, SURF_SOC_VERSION, ALL);
            bfm.read_check(B + SURF_SYSINFO_MAP_VERSION, 4'hF, SURF_MAP_VERSION, ALL);
            bfm.read_check(B + SURF_SYSINFO_BOARD_ID, 4'hF, P_BOARD, ALL);
            bfm.read_check(B + SURF_SYSINFO_SYS_HZ, 4'hF, P_SYS, ALL);
            bfm.read_check(B + SURF_SYSINFO_BUILD_ID, 4'hF, P_BUILD, ALL);
            bfm.read_check(B + SURF_SYSINFO_BUILD_FLAGS, 4'hF, SURF_SYSINFO_BUILD_FLAGS_SIM_MASK, ALL);
            bfm.read_check(B + SURF_SYSINFO_FEATURES, 4'hF, P_FEAT & 32'hFF, ALL);
            bfm.read_check(B + SURF_SYSINFO_DDR_SIZE, 4'hF, P_DDRSIZE, ALL);
            bfm.read_check(B + SURF_SYSINFO_PIX_HZ, 4'hF, P_PIX, ALL);
            bfm.read_check(B + SURF_SYSINFO_BUILD_TIME, 4'hF, P_TIME, ALL);
            bfm.read_check(B + SURF_SYSINFO_SCRATCH, 4'hF, scratch_model, ALL);
        end
    endtask

    function is_reg;
        input [11:0] off;
        begin
            is_reg = (off <= SURF_SYSINFO_BUILD_TIME[11:0]);   // 0x00..0x30, every word a register
        end
    endfunction

    integer n, k, nerr_checks;
    reg [11:0] off;
    reg [3:0]  be;
    reg        d, f;

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        bfm.seed = seed;
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;

        check_ids;
        bfm.read_check(B + SURF_SYSINFO_DDR_STATUS, 4'hF, ddr_status(0, 0, 0), ALL);

        // DDR_STATUS follows the calibration levels
        for (n = 0; n < 40; n = n + 1) begin
            d = $random(seed); f = $random(seed);
            calib_done = d; calib_fail = f;
            bfm.read_check(B + SURF_SYSINFO_DDR_STATUS, 4'hF, ddr_status(d, f, 0), ALL);
        end

        // TIMEOUT: a one-cycle pulse latches until reset, and a read accepted in the cycle of
        // the pulse already sees it
        bfm.max_gap = 0;
        @(negedge clk) timeout = 1'b1;
        fork
            bfm.read_check(B + SURF_SYSINFO_DDR_STATUS, 4'hF, ddr_status(d, f, 1), ALL);
            @(negedge clk) timeout = 1'b0;
        join
        bfm.max_gap = 2;
        for (n = 0; n < 10; n = n + 1) begin
            d = $random(seed); f = $random(seed);
            calib_done = d; calib_fail = f;
            bfm.read_check(B + SURF_SYSINFO_DDR_STATUS, 4'hF, ddr_status(d, f, 1), ALL);
        end
        bfm.write_ok(B + SURF_SYSINFO_DDR_STATUS, 4'hF, 32'h0);     // read only: still set
        bfm.read_check(B + SURF_SYSINFO_DDR_STATUS, 4'hF, ddr_status(d, f, 1), ALL);

        // SCRATCH is the only writable register
        for (n = 0; n < 200; n = n + 1) begin
            scratch_model = $random(seed);
            bfm.write_ok(B + SURF_SYSINFO_SCRATCH, 4'hF, scratch_model);
            bfm.read_check(B + SURF_SYSINFO_SCRATCH, 4'hF, scratch_model, ALL);
        end
        for (k = 0; k <= SURF_SYSINFO_BUILD_TIME; k = k + 4)
            if (k != SURF_SYSINFO_SCRATCH) bfm.write_ok(B + k, 4'hF, $random(seed));
        check_ids;

        // error rules: nothing changes
        nerr_checks = 0;
        for (n = 0; n < 400; n = n + 1) begin
            off = {$random(seed)} & 12'hFFC;
            if (!is_reg(off)) begin
                bfm.read_err(B + off, 4'hF);
                bfm.write_err(B + off, 4'hF, 32'hFFFF_FFFF);
                nerr_checks = nerr_checks + 2;
            end
        end
        for (n = 0; n < 6; n = n + 1) begin
            case (n)
                0: be = 4'b0001; 1: be = 4'b0010; 2: be = 4'b0100;
                3: be = 4'b1000; 4: be = 4'b0011; default: be = 4'b1100;
            endcase
            bfm.read_err(B + SURF_SYSINFO_MAGIC, be);
            bfm.write_err(B + SURF_SYSINFO_SCRATCH, be, 32'h1234_5678);   // must not change it
            nerr_checks = nerr_checks + 2;
        end
        bfm.write_err(B + SURF_SYSINFO_SCRATCH + 1, 4'hF, 32'h1234_5678);
        bfm.fetch_flag = 1'b1;
        bfm.read_err(B + SURF_SYSINFO_MAGIC, 4'hF);
        bfm.fetch_flag = 1'b0;
        nerr_checks = nerr_checks + 2;
        check_ids;

        // reset clears TIMEOUT and SCRATCH
        @(negedge clk) rst = 1'b1;
        repeat (2) @(negedge clk);
        rst = 1'b0;
        scratch_model = 32'h0;
        calib_done = 1'b1; calib_fail = 1'b0;
        bfm.read_check(B + SURF_SYSINFO_DDR_STATUS, 4'hF, ddr_status(1, 0, 0), ALL);
        check_ids;

        repeat (2) @(posedge clk);   // let the monitor see the last response
        errors = errors + bfm.errors + mon.errors;
        if (mon.accepts != mon.responses) begin
            $display("ERROR %0d requests, %0d responses", mon.accepts, mon.responses);
            errors = errors + 1;
        end
        if (errors) $fatal(1, "tb_sysinfo: %0d errors", errors);
        $display("tb_sysinfo: PASS (DDR=%0d SWINIT=%0d, %0d transfers, %0d error responses checked)",
                 DDR, SWINIT, mon.accepts, nerr_checks);
        $finish;
    end
endmodule
