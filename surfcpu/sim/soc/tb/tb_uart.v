/*
SurfCPU surf_uart testbench
--------------------
File: tb_uart.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for the UART at its real reset divisor (217 cycles per bit at 25 MHz and
115200 baud) through the SurfBus BFM. A serial monitor decodes every frame on tx and checks
its timing to the cycle (each bit exactly DIV cycles, back-to-back frames exactly 10 bits
apart); a serial driver sends frames into rx (good, bad stop bit, break, glitch, +-3 % baud),
or rx is looped back from tx. Checks: reset values; TX order and the 16-byte TX FIFO with
overflow (TX_OVF, the byte dropped); loopback; the 64-byte RX FIFO with overflow (RX_OVF, the
later bytes lost, the first 64 kept in order); framing errors and breaks (dropped, sticky
bits, one event per break, recovery); glitches ignored; baud tolerance; DIV writes (the
minimum of 16) and traffic at other divisors; w1c behaviour; the three interrupt enables;
the error rules (a byte write to TXDATA sends nothing, a byte read of RXDATA pops nothing);
and a long random loopback run. +seed=N. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_uart;
    `include "surfcpu_soc.vh"

    parameter integer SYS_HZ = 25000000;
    localparam integer DIV0  = (SYS_HZ + 115200 / 2) / 115200;
    localparam integer CLK_NS = 40;

    reg clk = 1'b0;
    always #(CLK_NS / 2) clk = ~clk;
    reg rst = 1'b1;
    integer seed = 1;
    integer errors = 0;

    wire        req_valid, req_ready, req_write, req_fetch, rsp_valid, rsp_err;
    wire [31:0] req_addr, req_wdata, rsp_rdata;
    wire [3:0]  req_be;
    wire        irq, tx;
    reg         loop = 1'b1;        // rx = tx
    reg         tb_rx = 1'b1;       // the serial driver's line
    wire        rx = loop ? tx : tb_rx;

    surfbus_bfm #(.NAME("bfm")) bfm (
        .clk(clk), .m_req_valid(req_valid), .m_req_ready(req_ready), .m_req_addr(req_addr),
        .m_req_write(req_write), .m_req_be(req_be), .m_req_wdata(req_wdata),
        .m_req_fetch(req_fetch), .m_rsp_valid(rsp_valid), .m_rsp_rdata(rsp_rdata),
        .m_rsp_err(rsp_err));
    surfbus_monitor #(.NAME("mon"), .MAX_LAT(1)) mon (
        .clk(clk), .rst(rst), .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_write(req_write), .req_be(req_be), .req_wdata(req_wdata), .req_fetch(req_fetch),
        .rsp_valid(rsp_valid), .rsp_rdata(rsp_rdata), .rsp_err(rsp_err));
    surf_uart #(.SYS_HZ(SYS_HZ)) dut (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid), .s_req_ready(req_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata),
        .s_req_fetch(req_fetch), .s_rsp_valid(rsp_valid), .s_rsp_rdata(rsp_rdata),
        .s_rsp_err(rsp_err), .irq(irq), .rx(rx), .tx(tx));

    localparam [31:0] B   = SURF_UART_BASE;
    localparam [31:0] ALL = 32'hFFFF_FFFF;
    localparam [31:0] ST_TX_FULL  = SURF_UART_STATUS_TX_FULL_MASK;
    localparam [31:0] ST_TX_IDLE  = SURF_UART_STATUS_TX_IDLE_MASK;
    localparam [31:0] ST_RX_AVAIL = SURF_UART_STATUS_RX_AVAIL_MASK;
    localparam [31:0] ST_RX_FULL  = SURF_UART_STATUS_RX_FULL_MASK;
    localparam [31:0] ST_RX_OVF   = SURF_UART_STATUS_RX_OVF_MASK;
    localparam [31:0] ST_RX_FRAME = SURF_UART_STATUS_RX_FRAME_MASK;
    localparam [31:0] ST_TX_OVF   = SURF_UART_STATUS_TX_OVF_MASK;
    localparam [31:0] ST_RX_BREAK = SURF_UART_STATUS_RX_BREAK_MASK;
    localparam [31:0] ST_STICKY   = ST_RX_OVF | ST_RX_FRAME | ST_TX_OVF | ST_RX_BREAK;

    // ---------------------------------------------------------------- expected streams
    reg [7:0] txexp [0:8191];        // bytes the UART must send, in order
    integer   txexp_w = 0;
    reg [7:0] rxexp [0:8191];        // bytes that must come out of RXDATA, in order
    integer   rxexp_w = 0, rxexp_r = 0;

    // ---------------------------------------------------------------- serial monitor on tx
    integer mon_div = DIV0;
    integer tx_frames = 0, tx_b2b = 0;
    time    last_start = 0;
    initial begin : txmon
        integer k, c, bad;
        reg [7:0] d;
        reg       s;
        forever begin
            @(negedge tx);
            if (last_start != 0 && ($time - last_start) == 10 * mon_div * CLK_NS)
                tx_b2b = tx_b2b + 1;
            last_start = $time;
            bad = 0;
            for (k = 0; k < 10; k = k + 1) begin
                for (c = 0; c < mon_div; c = c + 1) begin
                    @(negedge clk);
                    if (c == 0) s = tx;
                    else if (tx !== s) bad = bad + 1;
                end
                if (k >= 1 && k <= 8) d[k - 1] = s;
                if (k == 0 && s !== 1'b0) bad = bad + 1;
                if (k == 9 && s !== 1'b1) bad = bad + 1;
            end
            if (bad) begin
                $display("ERROR tx frame %0d: %0d samples off the %0d-cycle bit grid", tx_frames,
                         bad, mon_div);
                errors = errors + 1;
            end
            if (tx_frames >= txexp_w) begin
                $display("ERROR tx sent an unexpected byte %h", d);
                errors = errors + 1;
            end else if (d !== txexp[tx_frames]) begin
                $display("ERROR tx frame %0d is %h, expected %h", tx_frames, d, txexp[tx_frames]);
                errors = errors + 1;
            end
            tx_frames = tx_frames + 1;
        end
    end

    // ---------------------------------------------------------------- serial driver on rx
    task send_frame;
        input [7:0]   data;
        input         stop;
        input integer bit_cycles;
        integer b;
        begin
            tb_rx = 1'b0;
            repeat (bit_cycles) @(posedge clk);
            for (b = 0; b < 8; b = b + 1) begin
                tb_rx = data[b];
                repeat (bit_cycles) @(posedge clk);
            end
            tb_rx = stop;
            repeat (bit_cycles) @(posedge clk);
            tb_rx = 1'b1;
        end
    endtask

    task send_good;             // a frame the UART must receive
        input [7:0]   data;
        input integer bit_cycles;
        begin
            rxexp[rxexp_w] = data;
            rxexp_w = rxexp_w + 1;
            send_frame(data, 1'b1, bit_cycles);
        end
    endtask

    // ---------------------------------------------------------------- bus helpers
    task tx_write;              // a byte the UART accepts
        input [7:0] data;
        begin
            txexp[txexp_w] = data;
            txexp_w = txexp_w + 1;
            if (loop) begin
                rxexp[rxexp_w] = data;
                rxexp_w = rxexp_w + 1;
            end
            bfm.write_ok(B + SURF_UART_TXDATA, 4'hF, {24'h0, data});
        end
    endtask

    task tx_write_wait;         // as software does: wait while TXDATA says FULL, then write
        input [7:0] data;
        integer w;
        begin
            w = 0;
            bfm.read(B + SURF_UART_TXDATA, 4'hF);
            while (bfm.rdata & SURF_UART_TXDATA_FULL_MASK) begin
                w = w + 1;
                if (w > 100000) $fatal(1, "TX FIFO stays full");
                bfm.idle(5);
                bfm.read(B + SURF_UART_TXDATA, 4'hF);
            end
            tx_write(data);
        end
    endtask

    task rx_read_expect;        // pop one byte, which must be the next expected one
        begin
            if (rxexp_r >= rxexp_w) $fatal(1, "rx_read_expect with nothing expected");
            bfm.read_check(B + SURF_UART_RXDATA, 4'hF, {24'h0, rxexp[rxexp_r]}, ALL);
            rxexp_r = rxexp_r + 1;
        end
    endtask

    task rx_read_empty;
        begin
            bfm.read_check(B + SURF_UART_RXDATA, 4'hF, SURF_UART_RXDATA_EMPTY_MASK, ALL);
        end
    endtask

    task wait_status;           // poll STATUS until (STATUS & mask) == value
        input [31:0]  mask;
        input [31:0]  value;
        input integer limit;
        integer w;
        begin
            w = 0;
            bfm.read(B + SURF_UART_STATUS, 4'hF);
            while ((bfm.rdata & mask) != value) begin
                w = w + 1;
                if (w > limit) $fatal(1, "STATUS %h never matched %h under mask %h", bfm.rdata,
                                      value, mask);
                bfm.idle(20);
                bfm.read(B + SURF_UART_STATUS, 4'hF);
            end
        end
    endtask

    task wait_tx_done;          // everything written has been sent and decoded
        integer w;
        begin
            wait_status(ST_TX_IDLE, ST_TX_IDLE, 100000);
            w = 0;
            while (tx_frames < txexp_w) begin
                bfm.idle(10);
                w = w + 1;
                if (w > 100000) $fatal(1, "tx monitor stuck");
            end
            bfm.idle(4 * mon_div);   // the receiver samples the stop bit in its middle
        end
    endtask

    task drain_rx;              // read every expected byte, then expect EMPTY
        begin
            while (rxexp_r < rxexp_w) rx_read_expect;
            rx_read_empty;
        end
    endtask

    task check_level;
        input integer ntx;
        input integer nrx;
        begin
            bfm.read_check(B + SURF_UART_LEVEL, 4'hF, (nrx << SURF_UART_LEVEL_RX_LSB) |
                           (ntx << SURF_UART_LEVEL_TX_LSB), ALL);
        end
    endtask

    task clear_sticky;
        begin
            bfm.write_ok(B + SURF_UART_STATUS, 4'hF, ALL);
            bfm.read_check(B + SURF_UART_STATUS, 4'hF, 32'h0, ST_STICKY);
        end
    endtask

    task set_div;
        input integer d;
        begin
            bfm.write_ok(B + SURF_UART_DIV, 4'hF, d);
            bfm.read_check(B + SURF_UART_DIV, 4'hF, (d < 16) ? 16 : d, ALL);
            mon_div = (d < 16) ? 16 : d;
        end
    endtask

    task check_irq;
        input expected;
        begin
            #1;
            if (irq !== expected) begin
                $display("ERROR irq %b, expected %b", irq, expected);
                errors = errors + 1;
            end
        end
    endtask

    integer n, k, nerr_checks, sent, budget;
    reg [7:0] byte_v;
    reg [3:0] be;

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        bfm.seed = seed;
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;

        // ------------------------------------------------ reset values
        $display("[%0t] reset values", $time);
        bfm.read_check(B + SURF_UART_DIV, 4'hF, DIV0, ALL);
        bfm.read_check(B + SURF_UART_CTRL, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_IDLE, ALL);
        bfm.read_check(B + SURF_UART_LEVEL, 4'hF, 32'h0, ALL);
        bfm.read_check(B + SURF_UART_TXDATA, 4'hF, 32'h0, ALL);
        rx_read_empty;
        check_irq(1'b0);
        if (tx !== 1'b1) begin $display("ERROR tx not idle after reset"); errors = errors + 1; end

        // ------------------------------------------------ one byte out and back
        $display("[%0t] one byte out and back", $time);
        tx_write(8'h55);
        wait_tx_done;
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_IDLE | ST_RX_AVAIL, ALL);
        check_level(0, 1);
        drain_rx;

        // ------------------------------------------------ TX FIFO: 1 in the shifter + 16 queued, the 18th dropped
        $display("[%0t] TX FIFO: 1 in the shifter + 16 queued, the 18th dropped", $time);
        for (n = 0; n < 17; n = n + 1) tx_write(8'hA0 + n);
        check_level(16, 0);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_FULL, ST_TX_FULL | ST_TX_IDLE | ST_TX_OVF);
        bfm.read_check(B + SURF_UART_TXDATA, 4'hF, SURF_UART_TXDATA_FULL_MASK, ALL);
        bfm.write_ok(B + SURF_UART_TXDATA, 4'hF, 32'h0000_00EE);       // dropped
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_OVF, ST_TX_OVF);
        k = tx_b2b;
        wait_tx_done;
        if (tx_b2b - k != 16) begin
            $display("ERROR %0d of 16 frames back to back", tx_b2b - k); errors = errors + 1;
        end
        bfm.write_ok(B + SURF_UART_STATUS, 4'hF, ~ST_TX_OVF);          // 0 in TX_OVF: stays
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_OVF, ST_TX_OVF);
        bfm.write_ok(B + SURF_UART_STATUS, 4'hF, ST_TX_OVF);           // w1c
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_IDLE | ST_RX_AVAIL, ALL);
        drain_rx;

        // ------------------------------------------------ RX FIFO overflow: 66 frames, 64 kept
        $display("[%0t] RX FIFO overflow: 66 frames, 64 kept", $time);
        loop = 1'b0;
        for (n = 0; n < 66; n = n + 1) begin
            byte_v = $random(seed);
            if (n < 64) begin
                send_good(byte_v, DIV0);
            end else begin
                send_frame(byte_v, 1'b1, DIV0);    // lost
            end
        end
        bfm.idle(DIV0);
        check_level(0, 64);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_IDLE | ST_RX_AVAIL | ST_RX_FULL | ST_RX_OVF,
                       ALL);
        drain_rx;
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_RX_OVF, ST_RX_OVF | ST_RX_AVAIL | ST_RX_FULL);
        clear_sticky;

        // ------------------------------------------------ framing error: the byte is dropped, the next one is fine
        $display("[%0t] framing error: the byte is dropped, the next one is fine", $time);
        send_good(8'h3C, DIV0);
        send_frame(8'h5A, 1'b0, DIV0);
        repeat (DIV0) @(posedge clk);
        send_good(8'hC3, DIV0);
        bfm.idle(DIV0);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_RX_FRAME, ST_STICKY);
        drain_rx;
        clear_sticky;

        // ------------------------------------------------ break: 25 bit times low = one frame error + break, no bytes
        $display("[%0t] break: 25 bit times low = one frame error + break, no bytes", $time);
        @(negedge clk) tb_rx = 1'b0;
        repeat (25 * DIV0) @(posedge clk);
        tb_rx = 1'b1;
        repeat (2 * DIV0) @(posedge clk);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_RX_FRAME | ST_RX_BREAK, ST_STICKY | ST_RX_AVAIL);
        send_good(8'h81, DIV0);
        bfm.idle(DIV0);
        drain_rx;
        clear_sticky;

        // ------------------------------------------------ a glitch shorter than half a bit is not a start bit
        $display("[%0t] a glitch shorter than half a bit is not a start bit", $time);
        @(negedge clk) tb_rx = 1'b0;
        repeat (DIV0 / 4) @(posedge clk);
        tb_rx = 1'b1;
        repeat (12 * DIV0) @(posedge clk);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, 32'h0, ST_STICKY | ST_RX_AVAIL);
        rx_read_empty;

        // ------------------------------------------------ baud tolerance: +-3 %
        $display("[%0t] baud tolerance: +-3 percent", $time);
        for (n = 0; n < 20; n = n + 1) send_good($random(seed), (DIV0 * 97) / 100);
        for (n = 0; n < 20; n = n + 1) send_good($random(seed), (DIV0 * 103) / 100);
        bfm.idle(DIV0);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, 32'h0, ST_STICKY);
        drain_rx;

        // ------------------------------------------------ DIV: the minimum, and traffic at other divisors
        $display("[%0t] DIV: the minimum, and traffic at other divisors", $time);
        loop = 1'b1;
        set_div(5);              // stored as 16
        set_div(0);
        set_div(16);
        for (n = 0; n < 30; n = n + 1) tx_write_wait($random(seed));
        wait_tx_done;
        drain_rx;
        set_div(1000);
        for (n = 0; n < 3; n = n + 1) tx_write($random(seed));
        wait_tx_done;
        drain_rx;
        set_div(DIV0 + 13);
        for (n = 0; n < 5; n = n + 1) tx_write($random(seed));
        wait_tx_done;
        drain_rx;
        set_div(DIV0);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, 32'h0, ST_STICKY);

        // ------------------------------------------------ interrupts
        $display("[%0t] interrupts", $time);
        bfm.write_ok(B + SURF_UART_CTRL, 4'hF, SURF_UART_CTRL_RX_IE_MASK);
        check_irq(1'b0);
        tx_write(8'h11);
        wait_tx_done;
        check_irq(1'b1);                          // RX_AVAIL
        rx_read_expect;
        check_irq(1'b0);                          // popped by the time the response came
        bfm.write_ok(B + SURF_UART_CTRL, 4'hF, SURF_UART_CTRL_TX_IE_MASK);
        check_irq(1'b1);                          // TX FIFO empty
        tx_write(8'h22);
        tx_write(8'h33);                          // the second waits in the FIFO
        check_irq(1'b0);
        wait_tx_done;
        check_irq(1'b1);
        tx_write(8'h44);                          // taken by the shifter at once: FIFO empty,
        bfm.idle(3);                              // not idle, and TX_IE is about the FIFO
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, 32'h0, ST_TX_IDLE);
        check_irq(1'b1);
        wait_tx_done;
        drain_rx;
        bfm.write_ok(B + SURF_UART_CTRL, 4'hF, SURF_UART_CTRL_ERR_IE_MASK);
        check_irq(1'b0);
        loop = 1'b0;
        send_frame(8'h77, 1'b0, DIV0);            // framing error
        bfm.idle(DIV0);
        check_irq(1'b1);
        clear_sticky;
        check_irq(1'b0);
        bfm.write_ok(B + SURF_UART_CTRL, 4'hF, ALL);
        bfm.read_check(B + SURF_UART_CTRL, 4'hF, SURF_UART_CTRL_RX_IE_MASK |
                       SURF_UART_CTRL_TX_IE_MASK | SURF_UART_CTRL_ERR_IE_MASK, ALL);
        check_irq(1'b1);                          // TX empty
        bfm.write_ok(B + SURF_UART_CTRL, 4'hF, 32'h0);
        check_irq(1'b0);
        loop = 1'b1;

        // ------------------------------------------------ error rules: no push, no pop, no change
        $display("[%0t] error rules: no push, no pop, no change", $time);
        nerr_checks = 0;
        tx_write(8'h99);
        wait_tx_done;                             // one byte waiting in RX
        for (n = 0; n < 6; n = n + 1) begin
            case (n)
                0: be = 4'b0001; 1: be = 4'b0010; 2: be = 4'b0100;
                3: be = 4'b1000; 4: be = 4'b0011; default: be = 4'b1100;
            endcase
            bfm.write_err(B + SURF_UART_TXDATA, be, 32'h4242_4242);   // nothing sent
            bfm.read_err(B + SURF_UART_RXDATA, be);                   // nothing popped
            bfm.write_err(B + SURF_UART_DIV, be, 32'h0000_0020);
            bfm.write_err(B + SURF_UART_STATUS, be, ALL);
            nerr_checks = nerr_checks + 4;
        end
        for (n = 0; n < 200; n = n + 1) begin
            k = {$random(seed)} & 12'hFFC;
            if (k > SURF_UART_LEVEL) begin
                bfm.read_err(B + k, 4'hF);
                bfm.write_err(B + k, 4'hF, 32'h41);
                nerr_checks = nerr_checks + 2;
            end
        end
        bfm.write_err(B + SURF_UART_TXDATA + 1, 4'hF, 32'h41);
        bfm.fetch_flag = 1'b1;
        bfm.read_err(B + SURF_UART_RXDATA, 4'hF);
        bfm.write_err(B + SURF_UART_TXDATA, 4'hF, 32'h41);
        bfm.fetch_flag = 1'b0;
        nerr_checks = nerr_checks + 3;
        bfm.write_ok(B + SURF_UART_RXDATA, 4'hF, 32'h41);   // read only: ignored
        bfm.write_ok(B + SURF_UART_LEVEL, 4'hF, ALL);
        check_level(0, 1);
        bfm.read_check(B + SURF_UART_DIV, 4'hF, DIV0, ALL);
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_IDLE | ST_RX_AVAIL, ALL);
        drain_rx;
        bfm.idle(12 * DIV0);                      // and nothing was sent

        // ------------------------------------------------ random loopback traffic
        $display("[%0t] random loopback traffic", $time);
        sent = 0;
        budget = 260;
        while (sent < budget || rxexp_r < rxexp_w) begin
            case ({$random(seed)} % 4)
                0, 1: if (sent < budget) begin
                          bfm.read(B + SURF_UART_LEVEL, 4'hF);
                          if (bfm.rdata[SURF_UART_LEVEL_TX_LSB +: 8] < 16 &&
                              rxexp_w - rxexp_r < 40) begin
                              tx_write($random(seed));
                              sent = sent + 1;
                          end
                      end
                2:    begin
                          bfm.read(B + SURF_UART_STATUS, 4'hF);
                          if (bfm.rdata & ST_RX_AVAIL) rx_read_expect;
                      end
                default: bfm.idle({$random(seed)} % (2 * DIV0));
            endcase
        end
        wait_tx_done;            // the last stop bit ends after its byte was received
        rx_read_empty;
        bfm.read_check(B + SURF_UART_STATUS, 4'hF, ST_TX_IDLE, ALL);

        repeat (2) @(posedge clk);   // let the monitor see the last response
        if (tx_frames != txexp_w) begin
            $display("ERROR %0d frames sent, %0d expected", tx_frames, txexp_w); errors = errors + 1;
        end
        errors = errors + bfm.errors + mon.errors;
        if (mon.accepts != mon.responses) begin
            $display("ERROR %0d requests, %0d responses", mon.accepts, mon.responses);
            errors = errors + 1;
        end
        if (errors) $fatal(1, "tb_uart: %0d errors", errors);
        $display("tb_uart: PASS (DIV %0d, %0d frames sent and timed, %0d back to back, %0d bytes received, %0d transfers, %0d error responses checked)",
                 DIV0, tx_frames, tx_b2b, rxexp_r, mon.accepts, nerr_checks);
        $finish;
    end
endmodule
