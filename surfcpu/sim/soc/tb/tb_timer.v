/*
SurfCPU surf_timer testbench
--------------------
File: tb_timer.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for the machine timer through the SurfBus BFM: reset values; the tick rate
(every gap between mtime increments, and the count over a long window, against SYS_HZ /
TIMEBASE_HZ, integer or fractional); 64-bit reads with the hi/lo/hi retry across carries into
the high word (retries must happen and every value must be consistent); the mtimecmp write
sequence lo = all ones, hi, lo with MTIP checked low on every cycle of it; MTIP rising exactly
when mtime reaches mtimecmp and tracking mtime >= mtimecmp on every cycle; the unsigned 64-bit
compare (bit 63) and the wrap of mtime; clearing MTIP by a write; MSIP; writes to mtime
halves; TIMEBASE read only; and the error rules. Parameters SYS_HZ and TB_HZ pick the ratio.
+seed=N. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_timer;
    `include "surfcpu_soc.vh"

    parameter integer SYS_HZ = 25000000;
    parameter integer TB_HZ  = 1000000;

    reg clk = 1'b0;
    always #20 clk = ~clk;
    reg rst = 1'b1;
    integer seed = 1;
    integer errors = 0;

    wire        req_valid, req_ready, req_write, req_fetch, rsp_valid, rsp_err;
    wire [31:0] req_addr, req_wdata, rsp_rdata;
    wire [3:0]  req_be;
    wire [63:0] mtime;
    wire        mtip, msip;

    surfbus_bfm #(.NAME("bfm")) bfm (
        .clk(clk), .m_req_valid(req_valid), .m_req_ready(req_ready), .m_req_addr(req_addr),
        .m_req_write(req_write), .m_req_be(req_be), .m_req_wdata(req_wdata),
        .m_req_fetch(req_fetch), .m_rsp_valid(rsp_valid), .m_rsp_rdata(rsp_rdata),
        .m_rsp_err(rsp_err));
    surfbus_monitor #(.NAME("mon"), .MAX_LAT(1)) mon (
        .clk(clk), .rst(rst), .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_write(req_write), .req_be(req_be), .req_wdata(req_wdata), .req_fetch(req_fetch),
        .rsp_valid(rsp_valid), .rsp_rdata(rsp_rdata), .rsp_err(rsp_err));
    surf_timer #(.SYS_HZ(SYS_HZ), .TIMEBASE_HZ(TB_HZ)) dut (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid), .s_req_ready(req_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata),
        .s_req_fetch(req_fetch), .s_rsp_valid(rsp_valid), .s_rsp_rdata(rsp_rdata),
        .s_rsp_err(rsp_err), .mtime(mtime), .irq_timer(mtip), .irq_software(msip));

    localparam [31:0] B   = SURF_TIMER_BASE;
    localparam [31:0] ALL = 32'hFFFF_FFFF;
    localparam integer GAP_LO = SYS_HZ / TB_HZ;                       // cycles between ticks
    localparam integer GAP_HI = (SYS_HZ % TB_HZ == 0) ? GAP_LO : GAP_LO + 1;

    // ---------------------------------------------------------------- tick measurement
    reg        measuring = 1'b0;
    reg [63:0] last_mtime = 64'h0;
    integer    since = 0, gap_min = 1 << 30, gap_max = 0, n_ticks = 0;
    always @(posedge clk) begin
        if (measuring) begin
            if (mtime != last_mtime) begin
                if (mtime != last_mtime + 64'd1) begin
                    $display("ERROR mtime jumped from %h to %h", last_mtime, mtime);
                    errors = errors + 1;
                end
                if (n_ticks > 0) begin
                    if (since + 1 < gap_min) gap_min = since + 1;
                    if (since + 1 > gap_max) gap_max = since + 1;
                end
                n_ticks = n_ticks + 1;
                since = 0;
            end else begin
                since = since + 1;
            end
        end
        last_mtime = mtime;
    end

    // ---------------------------------------------------------------- MTIP checker
    // while check_mtip is set, MTIP must equal (mtime >= cmp_model) on every cycle; while
    // quiet_mtip is set it must stay 0
    reg        check_mtip = 1'b0, quiet_mtip = 1'b0;
    reg [63:0] cmp_model = 64'hFFFF_FFFF_FFFF_FFFF;
    integer    n_mtip_checks = 0;
    always @(posedge clk) begin
        if (check_mtip) begin
            n_mtip_checks = n_mtip_checks + 1;
            if (mtip !== (mtime >= cmp_model)) begin
                $display("ERROR MTIP %b with mtime %h, mtimecmp %h", mtip, mtime, cmp_model);
                errors = errors + 1;
            end
        end
        if (quiet_mtip && mtip !== 1'b0) begin
            $display("ERROR spurious MTIP with mtime %h during the mtimecmp write sequence", mtime);
            errors = errors + 1;
        end
    end

    // ---------------------------------------------------------------- helpers
    integer retries_total = 0;

    task read64;
        input  [31:0] off_lo;
        output [63:0] v;
        reg    [31:0] hi, lo, hi2;
        begin
            bfm.read(B + off_lo + 4, 4'hF); hi  = bfm.rdata;
            bfm.read(B + off_lo,     4'hF); lo  = bfm.rdata;
            bfm.read(B + off_lo + 4, 4'hF); hi2 = bfm.rdata;
            while (hi2 != hi) begin
                retries_total = retries_total + 1;
                hi = hi2;
                bfm.read(B + off_lo,     4'hF); lo  = bfm.rdata;
                bfm.read(B + off_lo + 4, 4'hF); hi2 = bfm.rdata;
            end
            v = {hi, lo};
        end
    endtask

    task set_mtime;             // high half first, then the low half
        input [63:0] v;
        begin
            bfm.write_ok(B + SURF_TIMER_MTIME_HI, 4'hF, v[63:32]);
            bfm.write_ok(B + SURF_TIMER_MTIME_LO, 4'hF, v[31:0]);
        end
    endtask

    // The no-spurious-interrupt sequence of MEMORY-MAP.md, for a v in the future: if MTIP is
    // low at the start it stays low all through; if the old mtimecmp had already made it
    // high, it may stay high until the high word is written, and is low from then on.
    task set_cmp;
        input [63:0] v;
        begin
            check_mtip = 1'b0;
            #1 quiet_mtip = !mtip;
            bfm.write_ok(B + SURF_TIMER_MTIMECMP_LO, 4'hF, 32'hFFFF_FFFF);
            bfm.write_ok(B + SURF_TIMER_MTIMECMP_HI, 4'hF, v[63:32]);
            quiet_mtip = 1'b1;
            bfm.write_ok(B + SURF_TIMER_MTIMECMP_LO, 4'hF, v[31:0]);
            quiet_mtip = 1'b0;
            cmp_model  = v;
            check_mtip = 1'b1;
        end
    endtask

    task wait_mtip;             // wait for MTIP to rise; it must rise exactly at mtime == cmp
        input integer limit;
        integer w;
        begin
            w = 0;
            @(posedge clk);
            while (mtip !== 1'b1) begin
                w = w + 1;
                if (w > limit) $fatal(1, "MTIP never rose (mtime %h, cmp %h)", mtime, cmp_model);
                @(posedge clk);
            end
            if (mtime !== cmp_model) begin
                $display("ERROR MTIP rose at mtime %h, mtimecmp %h", mtime, cmp_model);
                errors = errors + 1;
            end
            @(negedge clk);
        end
    endtask

    function [11:0] noreg_off;   // a random offset with no register
        input integer r;
        reg [11:0] o;
        begin
            o = r & 12'hFFC;
            if (o <= SURF_TIMER_TIMEBASE[11:0]) o = o + 12'h018;
            noreg_off = o;
        end
    endfunction

    integer n, k, nerr_checks;
    reg [63:0] v, vprev, expect_ticks;
    reg [31:0] r;
    reg [3:0]  be;
    integer    t0;

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        bfm.seed = seed;
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;

        // ------------------------------------------------ reset values
        bfm.read_check(B + SURF_TIMER_MTIMECMP_LO, 4'hF, ALL, ALL);
        bfm.read_check(B + SURF_TIMER_MTIMECMP_HI, 4'hF, ALL, ALL);
        bfm.read_check(B + SURF_TIMER_MSIP, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_TIMER_TIMEBASE, 4'hF, TB_HZ, ALL);
        bfm.read_check(B + SURF_TIMER_MTIME_HI, 4'hF, 32'h0, ALL);
        bfm.read(B + SURF_TIMER_MTIME_LO, 4'hF);
        if (bfm.rdata > 2) begin
            $display("ERROR mtime %0d just after reset", bfm.rdata); errors = errors + 1;
        end
        if (mtip !== 1'b0 || msip !== 1'b0) begin
            $display("ERROR interrupts after reset: MTIP %b MSIP %b", mtip, msip); errors = errors + 1;
        end

        // ------------------------------------------------ tick rate
        measuring = 1'b1;
        t0 = $time;
        v = mtime;
        repeat (GAP_HI * 400 + 7) @(posedge clk);
        expect_ticks = ((GAP_HI * 400 + 7) * TB_HZ) / SYS_HZ;
        if (mtime - v < expect_ticks || mtime - v > expect_ticks + 1) begin
            $display("ERROR %0d ticks in %0d cycles, expected %0d", mtime - v, GAP_HI * 400 + 7,
                     expect_ticks);
            errors = errors + 1;
        end
        measuring = 1'b0;
        if (gap_min != GAP_LO || gap_max != GAP_HI) begin
            $display("ERROR tick gaps %0d..%0d cycles, expected %0d..%0d", gap_min, gap_max,
                     GAP_LO, GAP_HI);
            errors = errors + 1;
        end
        @(negedge clk);

        // ------------------------------------------------ hi/lo/hi across carries
        for (n = 0; n < 60; n = n + 1) begin
            r = 32'hFFFF_FFFF - ({$random(seed)} % 3);
            set_mtime({32'h0000_0005 + n, r});
            vprev = 64'h0;
            for (k = 0; k < 6; k = k + 1) begin
                read64(SURF_TIMER_MTIME_LO, v);
                if (v < vprev || v > mtime || v + 64'd4 < mtime ||
                    v[63:32] < 32'h5 + n || v[63:32] > 32'h6 + n) begin
                    $display("ERROR 64-bit read %h (previous %h, mtime now %h)", v, vprev, mtime);
                    errors = errors + 1;
                end
                vprev = v;
            end
        end
        if (retries_total == 0) begin
            $display("ERROR the hi/lo/hi retry never happened"); errors = errors + 1;
        end
        read64(SURF_TIMER_MTIMECMP_LO, v);
        if (v !== 64'hFFFF_FFFF_FFFF_FFFF) begin
            $display("ERROR mtimecmp %h", v); errors = errors + 1;
        end

        // ------------------------------------------------ MTIP: write sequence, rise, clear
        for (n = 0; n < 20; n = n + 1) begin
            read64(SURF_TIMER_MTIME_LO, v);
            set_cmp(v + 20 + ({$random(seed)} % 40));
            wait_mtip(GAP_HI * 100);
            repeat (GAP_HI * 3) @(negedge clk);           // stays high while mtime >= mtimecmp
            check_mtip = 1'b0;
            bfm.write_ok(B + SURF_TIMER_MTIMECMP_HI, 4'hF, ALL);
            if (mtip !== 1'b0) begin                      // cleared by the time the response came
                $display("ERROR MTIP still high after mtimecmp was moved"); errors = errors + 1;
            end
            cmp_model = {32'hFFFF_FFFF, cmp_model[31:0]};
            check_mtip = 1'b1;
        end

        // unsigned compare: bit 63 set in mtimecmp, mtime just below it
        check_mtip = 1'b0;
        set_mtime(64'h7FFF_FFFF_FFFF_FFF0);
        set_cmp(64'h8000_0000_0000_0000);
        if (mtip !== 1'b0) begin $display("ERROR MTIP compared signed"); errors = errors + 1; end
        wait_mtip(GAP_HI * 40);
        // the wrap of mtime: MTIP falls when mtime goes from all ones to 0
        check_mtip = 1'b0;
        set_cmp(64'hFFFF_FFFF_FFFF_FFFC);
        set_mtime(64'hFFFF_FFFF_FFFF_FFF8);
        wait_mtip(GAP_HI * 10);
        k = 0;
        while (mtime != 64'h0) begin
            @(negedge clk);
            k = k + 1;
            if (k > GAP_HI * 10) $fatal(1, "mtime did not wrap");
        end
        #1;
        if (mtip !== 1'b0) begin $display("ERROR MTIP after mtime wrapped"); errors = errors + 1; end
        check_mtip = 1'b0;
        set_cmp(64'hFFFF_FFFF_FFFF_FFFF);

        // writing one half keeps the other
        set_mtime(64'h1234_5678_0000_0100);
        bfm.write_ok(B + SURF_TIMER_MTIME_LO, 4'hF, 32'h0000_0000);
        read64(SURF_TIMER_MTIME_LO, v);
        if (v[63:32] !== 32'h1234_5678 || v[31:0] > 4) begin
            $display("ERROR mtime %h after writing the low half", v); errors = errors + 1;
        end
        bfm.write_ok(B + SURF_TIMER_MTIME_HI, 4'hF, 32'h0000_0042);
        read64(SURF_TIMER_MTIME_LO, v);
        if (v[63:32] !== 32'h42 || v[31:0] > 8) begin
            $display("ERROR mtime %h after writing the high half", v); errors = errors + 1;
        end
        if (mtime[63:32] !== 32'h42) begin
            $display("ERROR mtime output %h", mtime); errors = errors + 1;
        end

        // ------------------------------------------------ MSIP
        bfm.write_ok(B + SURF_TIMER_MSIP, 4'hF, 32'h1);
        if (msip !== 1'b1) begin $display("ERROR MSIP not set"); errors = errors + 1; end
        bfm.read_check(B + SURF_TIMER_MSIP, 4'hF, 32'h1, ALL);
        bfm.write_ok(B + SURF_TIMER_MSIP, 4'hF, 32'hFFFF_FFFE);
        if (msip !== 1'b0) begin $display("ERROR MSIP not cleared"); errors = errors + 1; end
        bfm.read_check(B + SURF_TIMER_MSIP, 4'hF, 32'h0, ALL);
        bfm.write_ok(B + SURF_TIMER_MSIP, 4'hF, 32'hFFFF_FFFF);
        bfm.read_check(B + SURF_TIMER_MSIP, 4'hF, 32'h1, ALL);
        bfm.write_ok(B + SURF_TIMER_MSIP, 4'hF, 32'h0);

        // ------------------------------------------------ read only, errors
        check_mtip = 1'b0;
        bfm.write_ok(B + SURF_TIMER_TIMEBASE, 4'hF, 32'h1234);
        bfm.read_check(B + SURF_TIMER_TIMEBASE, 4'hF, TB_HZ, ALL);
        nerr_checks = 0;
        bfm.write_ok(B + SURF_TIMER_MTIMECMP_LO, 4'hF, 32'hA5A5_0000);
        for (n = 0; n < 300; n = n + 1) begin
            r = noreg_off($random(seed));
            bfm.read_err(B + r, 4'hF);
            bfm.write_err(B + r, 4'hF, 32'h0);
            nerr_checks = nerr_checks + 2;
        end
        for (n = 0; n < 6; n = n + 1) begin
            case (n)
                0: be = 4'b0001; 1: be = 4'b0010; 2: be = 4'b0100;
                3: be = 4'b1000; 4: be = 4'b0011; default: be = 4'b1100;
            endcase
            bfm.write_err(B + SURF_TIMER_MTIMECMP_LO, be, 32'h0);
            bfm.write_err(B + SURF_TIMER_MSIP, be, 32'hFFFF_FFFF);
            bfm.read_err(B + SURF_TIMER_MTIME_LO, be);
            nerr_checks = nerr_checks + 3;
        end
        bfm.write_err(B + SURF_TIMER_MTIMECMP_LO + 1, 4'hF, 32'h0);
        bfm.fetch_flag = 1'b1;
        bfm.read_err(B + SURF_TIMER_MTIME_LO, 4'hF);
        bfm.write_err(B + SURF_TIMER_MSIP, 4'hF, 32'h1);
        bfm.fetch_flag = 1'b0;
        nerr_checks = nerr_checks + 3;
        bfm.read_check(B + SURF_TIMER_MTIMECMP_LO, 4'hF, 32'hA5A5_0000, ALL);
        bfm.read_check(B + SURF_TIMER_MSIP, 4'hF, 32'h0, ALL);
        if (msip !== 1'b0) begin $display("ERROR MSIP set by an error access"); errors = errors + 1; end

        repeat (2) @(posedge clk);   // let the monitor see the last response
        errors = errors + bfm.errors + mon.errors;
        if (mon.accepts != mon.responses) begin
            $display("ERROR %0d requests, %0d responses", mon.accepts, mon.responses);
            errors = errors + 1;
        end
        if (errors) $fatal(1, "tb_timer: %0d errors", errors);
        $display("tb_timer: PASS (%0d/%0d Hz, %0d ticks with gaps %0d..%0d, %0d hi/lo/hi retries, %0d MTIP cycle checks, %0d transfers, %0d error responses checked)",
                 SYS_HZ, TB_HZ, n_ticks, gap_min, gap_max, retries_total, n_mtip_checks,
                 mon.accepts, nerr_checks);
        $finish;
    end
endmodule
