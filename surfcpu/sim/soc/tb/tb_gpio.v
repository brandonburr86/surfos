/*
SurfCPU surf_gpio testbench
--------------------
File: tb_gpio.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for the GPIO block through the SurfBus BFM: reset values; LED read-back and
pins; RGB read-back and the 1/8 duty cycle measured on every pin (256 of every 2048 cycles,
in one block); switches through the synchronizer; buttons with contact bounce: the debounced
state changes 9 to 10 ms after the last bounce (measured to the cycle), each bouncy press
latches exactly one BTN_EDGE, releases and pulses shorter than the debounce time latch
nothing; BTN_EDGE w1c, with a press in the same cycle as the clear winning; IE and irq; the
error rules and read-only registers. SYS_HZ scales the time base (100 kHz for the long random
run, 25 MHz for the real 10 ms). +seed=N. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_gpio;
    `include "surfcpu_soc.vh"

    parameter integer SYS_HZ = 100000;
    localparam integer MS    = SYS_HZ / 1000;      // cycles per millisecond
    localparam integer FAST  = (SYS_HZ <= 1000000);

    reg clk = 1'b0;
    always #20 clk = ~clk;
    reg rst = 1'b1;
    integer seed = 1;
    integer errors = 0;

    wire        req_valid, req_ready, req_write, req_fetch, rsp_valid, rsp_err;
    wire [31:0] req_addr, req_wdata, rsp_rdata;
    wire [3:0]  req_be;
    wire        irq;
    wire [3:0]  led;
    wire [11:0] rgb;
    reg  [3:0]  sw = 4'h0, btn = 4'h0;

    surfbus_bfm #(.NAME("bfm")) bfm (
        .clk(clk), .m_req_valid(req_valid), .m_req_ready(req_ready), .m_req_addr(req_addr),
        .m_req_write(req_write), .m_req_be(req_be), .m_req_wdata(req_wdata),
        .m_req_fetch(req_fetch), .m_rsp_valid(rsp_valid), .m_rsp_rdata(rsp_rdata),
        .m_rsp_err(rsp_err));
    surfbus_monitor #(.NAME("mon"), .MAX_LAT(1)) mon (
        .clk(clk), .rst(rst), .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_write(req_write), .req_be(req_be), .req_wdata(req_wdata), .req_fetch(req_fetch),
        .rsp_valid(rsp_valid), .rsp_rdata(rsp_rdata), .rsp_err(rsp_err));
    surf_gpio #(.SYS_HZ(SYS_HZ)) dut (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid), .s_req_ready(req_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata),
        .s_req_fetch(req_fetch), .s_rsp_valid(rsp_valid), .s_rsp_rdata(rsp_rdata),
        .s_rsp_err(rsp_err), .irq(irq), .led(led), .rgb(rgb), .sw(sw), .btn(btn));

    localparam [31:0] B   = SURF_GPIO_BASE;
    localparam [31:0] ALL = 32'hFFFF_FFFF;

    // ---------------------------------------------------------------- debounce timing probe
    // records, per button, the cycle of the last pin change and the delay to each change of
    // the debounced state (dut.btn_db, the BTN register's source)
    integer cyc = 0;
    always @(posedge clk) cyc = cyc + 1;
    integer last_pin [0:3];
    integer db_changes [0:3];
    integer dly_min = 1 << 30, dly_max = 0;
    reg [3:0] btn_prev = 4'h0, db_prev = 4'h0;
    integer j;
    initial for (j = 0; j < 4; j = j + 1) begin last_pin[j] = 0; db_changes[j] = 0; end
    always @(posedge clk) begin
        for (j = 0; j < 4 && !rst; j = j + 1) begin
            if (btn[j] !== btn_prev[j]) last_pin[j] = cyc;
            if (dut.btn_db[j] !== db_prev[j]) begin
                db_changes[j] = db_changes[j] + 1;
                if (cyc - last_pin[j] < dly_min) dly_min = cyc - last_pin[j];
                if (cyc - last_pin[j] > dly_max) dly_max = cyc - last_pin[j];
                if (dut.btn_db[j] !== btn[j]) begin
                    $display("ERROR button %0d debounced to %b while the pin is %b", j,
                             dut.btn_db[j], btn[j]);
                    errors = errors + 1;
                end
            end
        end
        btn_prev = btn;
        db_prev  = dut.btn_db;
    end

    // ---------------------------------------------------------------- helpers
    reg [3:0] edge_model = 4'h0, ie_model = 4'h0;

    task check_irq;
        begin
            #1;
            if (irq !== |(edge_model & ie_model)) begin
                $display("ERROR irq %b, BTN_EDGE %b IE %b", irq, edge_model, ie_model);
                errors = errors + 1;
            end
        end
    endtask

    task bounce;                 // contact bounce on button b, ending at level v
        input integer b;
        input         v;
        input integer n;         // number of bounces
        integer k;
        begin
            for (k = 0; k < n; k = k + 1) begin
                btn[b] = ~btn[b];
                repeat (1 + {$random(seed)} % (5 * MS)) @(negedge clk);   // shorter than 9 ms
            end
            btn[b] = v;
        end
    endtask

    task settle;                 // past the debounce time (10 ms) plus the synchronizer
        begin
            bfm.idle(11 * MS + 10);
        end
    endtask

    integer n, k, b, nerr_checks, hi_cnt, runs, presses;
    reg [11:0] rgbv;
    reg [3:0]  v4, be, prev_rgb_bit;
    reg        prev;

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        bfm.seed = seed;
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;

        // ------------------------------------------------ reset values
        bfm.read_check(B + SURF_GPIO_LED, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_GPIO_RGB, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_GPIO_SW, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_GPIO_BTN, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_GPIO_IE, 4'hF, 32'h0, ALL);
        if (led !== 4'h0 || rgb !== 12'h0 || irq !== 1'b0) begin
            $display("ERROR outputs after reset"); errors = errors + 1;
        end

        // ------------------------------------------------ LEDs
        for (n = 0; n < 40; n = n + 1) begin
            v4 = $random(seed);
            bfm.write_ok(B + SURF_GPIO_LED, 4'hF, {$random(seed)} & 32'hFFFF_FFF0 | v4);
            bfm.read_check(B + SURF_GPIO_LED, 4'hF, v4, ALL);
            if (led !== v4) begin $display("ERROR led pins %b, expected %b", led, v4); errors = errors + 1; end
        end

        // ------------------------------------------------ RGB at 1/8 duty
        for (n = 0; n < 4; n = n + 1) begin
            rgbv = (n == 0) ? 12'hFFF : $random(seed);
            bfm.write_ok(B + SURF_GPIO_RGB, 4'hF, {20'hABCDE, rgbv});
            bfm.read_check(B + SURF_GPIO_RGB, 4'hF, rgbv, ALL);
            bfm.idle(3);
            for (b = 0; b < 12; b = b + 1) begin
                hi_cnt = 0; runs = 0; prev = rgb[b];
                for (k = 0; k < 4096; k = k + 1) begin
                    @(negedge clk);
                    if (rgb[b]) hi_cnt = hi_cnt + 1;
                    if (rgb[b] && !prev) runs = runs + 1;
                    prev = rgb[b];
                end
                if (hi_cnt != (rgbv[b] ? 512 : 0) || runs > 2) begin
                    $display("ERROR rgb[%0d] (set %b) was on %0d of 4096 cycles in %0d runs", b,
                             rgbv[b], hi_cnt, runs);
                    errors = errors + 1;
                end
            end
        end
        bfm.write_ok(B + SURF_GPIO_RGB, 4'hF, 32'h0);

        // ------------------------------------------------ switches
        for (n = 0; n < 40; n = n + 1) begin
            @(negedge clk) sw = $random(seed);
            bfm.idle(2);
            bfm.read_check(B + SURF_GPIO_SW, 4'hF, sw, ALL);
        end

        // ------------------------------------------------ buttons: bouncy presses and releases
        ie_model = 4'hF;
        bfm.write_ok(B + SURF_GPIO_IE, 4'hF, ALL);
        bfm.read_check(B + SURF_GPIO_IE, 4'hF, 32'hF, ALL);
        presses = FAST ? 12 : 1;
        for (n = 0; n < presses; n = n + 1) begin
            b = {$random(seed)} % 4;
            bounce(b, 1'b1, {$random(seed)} % 6);
            settle;
            edge_model[b] = 1'b1;
            bfm.read_check(B + SURF_GPIO_BTN, 4'hF, btn, ALL);
            bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, edge_model, ALL);
            check_irq;
            bfm.write_ok(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h1 << b);   // w1c
            edge_model[b] = 1'b0;
            bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, edge_model, ALL);
            check_irq;
            bounce(b, 1'b0, {$random(seed)} % 6);                   // a release latches nothing
            settle;
            bfm.read_check(B + SURF_GPIO_BTN, 4'hF, btn, ALL);
            bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, edge_model, ALL);
        end

        // a pulse shorter than the debounce time is ignored
        @(negedge clk) btn[2] = 1'b1;
        repeat (7 * MS) @(negedge clk);
        btn[2] = 1'b0;
        settle;
        bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, edge_model, ALL);
        bfm.read_check(B + SURF_GPIO_BTN, 4'hF, 32'h0, ALL);

        // several buttons at once, and IE masking the interrupt
        @(negedge clk) btn = 4'b1010;
        settle;
        edge_model = 4'b1010;
        bfm.read_check(B + SURF_GPIO_BTN, 4'hF, 32'hA, ALL);
        bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'hA, ALL);
        check_irq;
        ie_model = 4'b0101;
        bfm.write_ok(B + SURF_GPIO_IE, 4'hF, 32'h5);
        check_irq;                                       // masked
        ie_model = 4'b0010;
        bfm.write_ok(B + SURF_GPIO_IE, 4'hF, 32'h2);
        check_irq;
        bfm.write_ok(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h0);   // writing 0 clears nothing
        bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'hA, ALL);
        bfm.write_ok(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h2);
        edge_model = 4'b1000;
        check_irq;
        bfm.write_ok(B + SURF_GPIO_BTN_EDGE, 4'hF, ALL);
        edge_model = 4'b0000;
        check_irq;
        @(negedge clk) btn = 4'b0000;
        settle;

        // a press in the same cycle as the clear: the press wins
        @(negedge clk) btn[1] = 1'b1;
        k = 0;
        while (!(dut.btn_db[1] === 1'b1 && dut.btn_db_d[1] === 1'b0)) begin
            @(negedge clk);
            k = k + 1;
            if (k > 12 * MS + 10) $fatal(1, "no debounced rise");
        end
        bfm.max_gap = 0;
        bfm.write_ok(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h2);   // accepted at the rising edge
        bfm.max_gap = 2;
        edge_model = 4'b0010;
        bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h2, ALL);
        check_irq;
        bfm.write_ok(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h2);
        edge_model = 4'b0000;
        @(negedge clk) btn[1] = 1'b0;
        settle;

        // ------------------------------------------------ read only, errors
        nerr_checks = 0;
        bfm.write_ok(B + SURF_GPIO_LED, 4'hF, 32'h5);
        @(negedge clk) btn[3] = 1'b1;
        settle;
        edge_model = 4'b1000;
        bfm.write_ok(B + SURF_GPIO_SW, 4'hF, ALL);
        bfm.write_ok(B + SURF_GPIO_BTN, 4'hF, 32'h0);
        bfm.read_check(B + SURF_GPIO_BTN, 4'hF, 32'h8, ALL);
        for (n = 0; n < 6; n = n + 1) begin
            case (n)
                0: be = 4'b0001; 1: be = 4'b0010; 2: be = 4'b0100;
                3: be = 4'b1000; 4: be = 4'b0011; default: be = 4'b1100;
            endcase
            bfm.write_err(B + SURF_GPIO_LED, be, 32'hA);
            bfm.write_err(B + SURF_GPIO_BTN_EDGE, be, ALL);
            bfm.read_err(B + SURF_GPIO_SW, be);
            nerr_checks = nerr_checks + 3;
        end
        for (n = 0; n < 200; n = n + 1) begin
            k = {$random(seed)} & 12'hFFC;
            if (k > SURF_GPIO_IE) begin
                bfm.read_err(B + k, 4'hF);
                bfm.write_err(B + k, 4'hF, ALL);
                nerr_checks = nerr_checks + 2;
            end
        end
        bfm.write_err(B + SURF_GPIO_LED + 2, 4'hF, 32'hA);
        bfm.fetch_flag = 1'b1;
        bfm.read_err(B + SURF_GPIO_LED, 4'hF);
        bfm.write_err(B + SURF_GPIO_LED, 4'hF, 32'hA);
        bfm.fetch_flag = 1'b0;
        nerr_checks = nerr_checks + 3;
        bfm.read_check(B + SURF_GPIO_LED, 4'hF, 32'h5, ALL);
        bfm.read_check(B + SURF_GPIO_BTN_EDGE, 4'hF, 32'h8, ALL);
        if (led !== 4'h5) begin $display("ERROR led pins changed by an error access"); errors = errors + 1; end

        repeat (2) @(posedge clk);   // let the monitor see the last response
        // the debounced state must change between 9 and 10 ms (plus 3 cycles of
        // synchronizer and register) after the last bounce
        if (dly_min < 9 * MS || dly_max > 10 * MS + 3) begin
            $display("ERROR debounce delays %0d..%0d cycles, expected %0d..%0d", dly_min, dly_max,
                     9 * MS, 10 * MS + 3);
            errors = errors + 1;
        end
        errors = errors + bfm.errors + mon.errors;
        if (mon.accepts != mon.responses) begin
            $display("ERROR %0d requests, %0d responses", mon.accepts, mon.responses);
            errors = errors + 1;
        end
        if (errors) $fatal(1, "tb_gpio: %0d errors", errors);
        $display("tb_gpio: PASS (SYS_HZ %0d, debounced changes after %0d..%0d cycles = %0d..%0d us, %0d presses, %0d transfers, %0d error responses checked)",
                 SYS_HZ, dly_min, dly_max, (dly_min * 1000) / MS, (dly_max * 1000) / MS, presses,
                 mon.accepts, nerr_checks);
        $finish;
    end
endmodule
