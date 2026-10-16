/*
SurfCPU DDR bridge smoke test (Icarus)
--------------------
File: tb_ddr_bridge_icarus.v Date: 10/15/26 (SurfCPU)
--------------------
surf_ddr_bridge in a second, four-state simulator, with the small Verilog MIG stand-in
(mig_ui_bfm.v) on its own ui_clk. The thorough tests are the Verilator ones (tb_ddr_bridge.cpp);
this one shows the RTL behaves the same in Icarus and drives no X: outputs known right after
reset, errors before calibration, every byte lane of a beat, walking address bits, random
traffic against a reference, the watchdog with a stuck controller and recovery by reset.
Prints PASS or stops with $fatal.

    vvp tb_ddr_bridge_icarus.vvp +seed=N
*/
`timescale 1ns/1ps
module tb_ddr_bridge_icarus;
    localparam integer TIMEOUT = 64;
    integer seed = 1, seed0 = 1;
    reg clk = 0, ui_clk = 0;
    always #20      clk = ~clk;          // 25 MHz
    always #6.1538  ui_clk = ~ui_clk;    // 81.25 MHz

    reg         rst = 1;
    reg         s_req_valid = 0, s_req_write = 0;
    reg  [31:0] s_req_addr = 0, s_req_wdata = 0;
    reg  [3:0]  s_req_be = 0;
    wire        s_req_ready, s_rsp_valid, s_rsp_err, calib_done, calib_fail, timeout;
    wire [31:0] s_rsp_rdata;
    wire        ui_rst, init_calib_complete, app_en, app_rdy, app_wdf_wren, app_wdf_end, app_wdf_rdy;
    wire        app_rd_data_valid, app_rd_data_end, app_sr_req, app_ref_req, app_zq_req;
    wire [27:0] app_addr;
    wire [2:0]  app_cmd;
    wire [127:0] app_wdf_data, app_rd_data;
    wire [15:0] app_wdf_mask;
    reg         stuck = 0;

    surf_ddr_bridge #(.TIMEOUT_CYCLES(TIMEOUT), .CALIB_TIMEOUT_CYCLES(3000)) dut (
        .clk(clk), .rst(rst), .s_req_valid(s_req_valid), .s_req_ready(s_req_ready),
        .s_req_addr(s_req_addr), .s_req_write(s_req_write), .s_req_be(s_req_be),
        .s_req_wdata(s_req_wdata), .s_req_fetch(1'b0), .s_rsp_valid(s_rsp_valid),
        .s_rsp_rdata(s_rsp_rdata), .s_rsp_err(s_rsp_err), .calib_done(calib_done),
        .calib_fail(calib_fail), .timeout(timeout),
        .ui_clk(ui_clk), .ui_rst(ui_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req));

    mig_ui_bfm #(.CALIB_CYCLES(300), .SEED(5)) mig (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end), .stuck(stuck));

    integer errors = 0, accesses = 0;

    // ---- reference: up to 1024 words, found by address; untouched words follow the BFM's pattern
    reg  [27:0] ref_addr [0:1023];
    reg  [31:0] ref_val  [0:1023];
    reg         ref_maybe[0:1023];       // a write that may or may not have happened
    reg  [31:0] ref_alt  [0:1023];
    integer     ref_used = 0;

    function integer ref_find;
        input [27:0] off;
        integer i;
        begin
            ref_find = -1;
            for (i = 0; i < ref_used; i = i + 1) if (ref_addr[i] == off) ref_find = i;
        end
    endfunction
    function [31:0] init_word;
        input [27:0] off;
        init_word = {8'h00, off[27:4]} ^ (32'h1111_1111 * (off[3:2] + 1));
    endfunction
    function [31:0] merge;
        input [31:0] old, data;
        input [3:0] be;
        integer k;
        begin
            merge = old;
            for (k = 0; k < 4; k = k + 1) if (be[k]) merge[8*k +: 8] = data[8*k +: 8];
        end
    endfunction
    task ref_write;
        input [27:0] off; input [3:0] be; input [31:0] data; input certain;
        integer i;
        begin
            i = ref_find({off[27:2], 2'b00});
            if (i < 0) begin
                i = ref_used; ref_used = ref_used + 1;
                ref_addr[i] = {off[27:2], 2'b00}; ref_val[i] = init_word(off); ref_maybe[i] = 0;
            end
            if (certain) begin ref_val[i] = merge(ref_val[i], data, be); ref_maybe[i] = 0; end
            else begin ref_alt[i] = merge(ref_val[i], data, be); ref_maybe[i] = 1; end
        end
    endtask
    task ref_check;
        input [27:0] off; input [31:0] got;
        integer i;
        reg [31:0] want;
        begin
            i = ref_find({off[27:2], 2'b00});
            want = (i < 0) ? init_word(off) : ref_val[i];
            if (i >= 0 && ref_maybe[i] && got === ref_alt[i]) begin ref_val[i] = got; ref_maybe[i] = 0; end
            else if (got !== want) begin
                $display("read %h returned %h, expected %h", off, got, want); errors = errors + 1;
            end else if (i >= 0) ref_maybe[i] = 0;
        end
    endtask

    // ---- one SurfBus access: drive and sample at falling edges (no races with the rising edge).
    // r_lat counts rising edges after the accepting one: 0 = response in the very next cycle.
    reg  [31:0] r_data;
    reg         r_err;
    integer     r_lat;
    task access;
        input w; input [31:0] addr; input [3:0] be; input [31:0] wdata;
        integer t0;
        begin
            @(negedge clk);
            s_req_valid = 1; s_req_write = w; s_req_addr = addr; s_req_be = be; s_req_wdata = wdata;
            while (s_req_ready !== 1'b1) @(negedge clk);
            @(posedge clk);                  // accepted at this edge
            t0 = $time; accesses = accesses + 1;
            #1 s_req_valid = 0;
            @(negedge clk);
            while (s_rsp_valid !== 1'b1) begin
                if ($time - t0 > (TIMEOUT + 8) * 40) begin $display("no response to %h", addr); errors = errors + 1; disable access; end
                @(negedge clk);
            end
            r_data = s_rsp_rdata; r_err = s_rsp_err; r_lat = ($time - t0 - 20) / 40;
            if ((^{r_err, r_data}) === 1'bx) begin $display("X in the response to %h", addr); errors = errors + 1; end
            if (!r_err && w) ref_write(addr[27:0], be, wdata, 1);
            if (r_err && w && r_lat >= TIMEOUT) ref_write(addr[27:0], be, wdata, 0);
            if (!r_err && !w) ref_check(addr[27:0], r_data);
        end
    endtask
    task expect_ok;  begin if (r_err) begin $display("unexpected error response"); errors = errors + 1; end end endtask
    task expect_err; begin if (!r_err) begin $display("missing error response"); errors = errors + 1; end end endtask

    integer i, lane, k, n;
    reg [3:0] bes [0:6];
    reg [31:0] a;

    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        seed0 = seed;
        bes[0] = 4'h1; bes[1] = 4'h2; bes[2] = 4'h4; bes[3] = 4'h8; bes[4] = 4'h3; bes[5] = 4'hC; bes[6] = 4'hF;
        repeat (4) @(posedge clk);
        #1 rst = 0;
        @(posedge clk); #1;
        if ((^{s_req_ready, s_rsp_valid, calib_done, calib_fail, timeout, app_sr_req, app_ref_req, app_zq_req}) === 1'bx) begin
            $display("X on the bridge's outputs after reset"); errors = errors + 1;
        end

        // before calibration: refused at once, nothing reaches the controller
        for (i = 0; i < 8; i = i + 1) begin
            access(i[0], 32'h8000_0000 + 4 * i, 4'hF, i); expect_err;
            if (r_lat != 0) begin $display("refusal latency %0d", r_lat); errors = errors + 1; end
        end
        if (mig.reads + mig.writes != 0) begin $display("controller saw an access before calibration"); errors = errors + 1; end
        wait (calib_done === 1'b1);

        // every lane with every byte-enable pattern, two beats
        for (k = 0; k < 2; k = k + 1)
            for (lane = 0; lane < 4; lane = lane + 1)
                for (i = 0; i < 7; i = i + 1) begin
                    a = (k ? 32'h8FFF_FFF0 : 32'h8000_0100) + 4 * lane;
                    access(1, a, bes[i], $random(seed)); expect_ok;
                    for (n = 0; n < 4; n = n + 1) begin
                        access(0, (a & 32'hFFFF_FFF0) + 4 * n, 4'hF, 0); expect_ok;
                    end
                end

        // walking ones and zeros across the 28-bit offset
        for (i = 2; i < 28; i = i + 1) begin
            access(1, 32'h8000_0000 | (32'h1 << i), 4'hF, 32'hA500_0000 | i); expect_ok;
            access(1, 32'h8FFF_FFFC ^ (32'h1 << i), 4'hF, 32'h5A00_0000 | i); expect_ok;
        end
        for (i = 2; i < 28; i = i + 1) begin
            access(0, 32'h8000_0000 | (32'h1 << i), 4'hF, 0); expect_ok;
            access(0, 32'h8FFF_FFFC ^ (32'h1 << i), 4'hF, 0); expect_ok;
        end

        // random traffic on 64 words of 16 beats
        for (i = 0; i < 600; i = i + 1) begin
            k = $random(seed) & 6;
            a = 32'h8123_4000 | (($random(seed) & 63) << 2);
            access($random(seed) & 1, a, bes[(k == 6) ? 6 : k], $random(seed)); expect_ok;
        end

        // the watchdog: a stuck controller, fail-stop, recovery by reset
        stuck = 1;
        access(0, 32'h8123_4000, 4'hF, 0); expect_err;
        if (r_lat != TIMEOUT || timeout !== 1'b1) begin
            $display("watchdog: latency %0d (want %0d), timeout %b", r_lat, TIMEOUT, timeout); errors = errors + 1;
        end
        access(1, 32'h8123_4004, 4'hF, 32'h1234_5678); expect_err;
        if (r_lat != 0) begin $display("no fail-stop after the watchdog"); errors = errors + 1; end
        stuck = 0;
        repeat (100) @(posedge clk);
        @(negedge clk) rst = 1; repeat (2) @(posedge clk); #1 rst = 0;
        wait (calib_done === 1'b1);
        if (timeout !== 1'b0) begin $display("timeout survived the reset"); errors = errors + 1; end
        for (i = 0; i < 100; i = i + 1) begin
            a = 32'h8123_4000 | (($random(seed) & 63) << 2);
            access($random(seed) & 1, a, 4'hF, $random(seed)); expect_ok;
        end

        repeat (20) @(posedge clk);
        if (mig.errors) begin $display("the MIG stand-in counted %0d protocol errors", mig.errors); errors = errors + mig.errors; end
        if (calib_fail) begin $display("calib_fail set"); errors = errors + 1; end
        if (errors) $fatal(1, "tb_ddr_bridge_icarus: %0d errors", errors);
        $display("tb_ddr_bridge_icarus: PASS seed=%0d accesses=%0d controller reads=%0d writes=%0d", seed0, accesses,
                 mig.reads, mig.writes);
        $finish;
    end
    initial begin #20_000_000; $fatal(1, "tb_ddr_bridge_icarus: time limit"); end
endmodule
