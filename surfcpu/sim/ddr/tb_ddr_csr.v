/*
SurfCPU DDR controller register bridge test
--------------------
File: tb_ddr_csr.v Date: 10/16/26 (SurfCPU)
--------------------
Icarus testbench for surf_ddr_csr between two unrelated clocks (periods from plusargs, in ps,
random phase, optional jitter of every half period): the SoC side is the soc track's SurfBus
BFM with a protocol monitor; the controller side is a Wishbone classic slave model holding the
8 KiB register window (2048 words) that answers after a random number of cycles, with an error
when told to, or never when muted, and checks the master: one cycle at a time, wb_sel 1111,
wb_cti and wb_bte 0, the request held until answered. Phases:
  1. random whole-word reads and writes, every read checked against a reference copy;
  2. accesses that are not whole aligned words, and fetches: an error at once, nothing on the
     Wishbone side;
  3. errors from the slave (30%): the access answered with an error, a write not done;
  4. a muted slave: an error after WB_TIMEOUT_CYCLES controller cycles (or after the SoC side's
     TIMEOUT_CYCLES, when that is the shorter time: then the write may still be done later),
     then normal again;
  5. the controller's clock stopped: an error after TIMEOUT_CYCLES SoC cycles, the next access
     waits for the first (and errs too), and once the clock runs again the first may still be
     carried out (its word becomes unknown) while every later answer belongs to its own access;
  6. resets: the SoC side's in the middle of an access (no answer to it, nothing stale after),
     the controller side's (the access in flight answered with an error), both at random;
  7. random accesses again, everything read back.
+seed=N +sys_ps=P +wb_ps=P +jitter=PCT (of each half period) +count=N. TIMEOUT and WB_TIMEOUT
are the DUT's parameters (the make target builds the board's values and short ones). Prints
PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_ddr_csr;
    parameter integer TIMEOUT    = 1024;
    parameter integer WB_TIMEOUT = 256;
    localparam integer WORDS = 2048;                // ADDR_BITS = 13

    integer seed = 1, sys_ps = 40000, wb_ps = 12500, jitter = 0, count = 3000;
    integer errors = 0;
    real    sys_half, wb_half;
    reg     clk = 1'b0, wb_clk = 1'b0, wb_run = 1'b1;
    reg     rst = 1'b1, wb_rst = 1'b1;

    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        if ($value$plusargs("sys_ps=%d", sys_ps)) ;
        if ($value$plusargs("wb_ps=%d", wb_ps)) ;
        if ($value$plusargs("jitter=%d", jitter)) ;
        if ($value$plusargs("count=%d", count)) ;
        sys_half = sys_ps / 2000.0;
        wb_half  = wb_ps / 2000.0;
    end
    integer jseed = 7;
    function real half;                             // a half period, jittered
        input real h;
        begin
            half = h;
            if (jitter > 0) half = h * (1.0 + (($random(jseed) % (jitter + 1)) / 100.0));
        end
    endfunction
    initial begin #0.001; forever begin #(half(sys_half)); clk = ~clk; end end
    initial begin #(0.003 + ($random(jseed) & 255) / 100.0); forever begin #(half(wb_half)); if (wb_run) wb_clk = ~wb_clk; end end

    // ---------------------------------------------------------------- the DUT
    wire        req_valid, req_ready, req_write, req_fetch, rsp_valid, rsp_err;
    wire [31:0] req_addr, req_wdata, rsp_rdata;
    wire [3:0]  req_be;
    wire [29:0] wb_adr;
    wire [31:0] wb_dat_w;
    reg  [31:0] wb_dat_r = 32'h0;
    wire [3:0]  wb_sel;
    wire        wb_cyc, wb_stb, wb_we;
    wire [2:0]  wb_cti;
    wire [1:0]  wb_bte;
    reg         wb_ack = 1'b0, wb_err = 1'b0;

    surf_ddr_csr #(.ADDR_BITS(13), .TIMEOUT_CYCLES(TIMEOUT), .WB_TIMEOUT_CYCLES(WB_TIMEOUT)) dut (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid), .s_req_ready(req_ready), .s_req_addr(req_addr), .s_req_write(req_write),
        .s_req_be(req_be), .s_req_wdata(req_wdata), .s_req_fetch(req_fetch),
        .s_rsp_valid(rsp_valid), .s_rsp_rdata(rsp_rdata), .s_rsp_err(rsp_err),
        .wb_clk(wb_clk), .wb_rst(wb_rst),
        .wb_adr(wb_adr), .wb_dat_w(wb_dat_w), .wb_dat_r(wb_dat_r), .wb_sel(wb_sel), .wb_cyc(wb_cyc),
        .wb_stb(wb_stb), .wb_we(wb_we), .wb_cti(wb_cti), .wb_bte(wb_bte), .wb_ack(wb_ack), .wb_err(wb_err));

    surfbus_bfm #(.NAME("bfm")) bfm (
        .clk(clk), .m_req_valid(req_valid), .m_req_ready(req_ready), .m_req_addr(req_addr),
        .m_req_write(req_write), .m_req_be(req_be), .m_req_wdata(req_wdata), .m_req_fetch(req_fetch),
        .m_rsp_valid(rsp_valid), .m_rsp_rdata(rsp_rdata), .m_rsp_err(rsp_err));
    surfbus_monitor #(.NAME("mon"), .MAX_LAT(2 * TIMEOUT + 8)) mon (
        .clk(clk), .rst(rst), .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_write(req_write), .req_be(req_be), .req_wdata(req_wdata), .req_fetch(req_fetch),
        .rsp_valid(rsp_valid), .rsp_rdata(rsp_rdata), .rsp_err(rsp_err));

    // ---------------------------------------------------------------- the Wishbone slave
    reg  [31:0] regs [0:WORDS-1];
    integer     lat_max = 3, err_pct = 0, wait_left = -1, n_wb = 0, n_wb_err = 0;
    reg         mute = 1'b0;
    reg  [29:0] held_adr;
    reg  [31:0] held_dat;
    reg         held_we;
    integer     i;
    initial for (i = 0; i < WORDS; i = i + 1) regs[i] = 32'h0;

    always @(posedge wb_clk) begin
        wb_ack <= 1'b0;
        wb_err <= 1'b0;
        if (wb_cyc !== wb_stb) begin
            $display("ERROR wb_cyc and wb_stb differ"); errors = errors + 1;
        end
        if (wb_cyc && !wb_ack && !wb_err) begin
            if (wb_sel !== 4'hF || wb_cti !== 3'd0 || wb_bte !== 2'd0 || wb_adr >= WORDS) begin
                $display("ERROR Wishbone request: sel %h cti %h bte %h adr %h", wb_sel, wb_cti, wb_bte, wb_adr);
                errors = errors + 1;
            end
            if (wait_left < 0) begin                // a new request: hold it, pick its latency
                wait_left = {$random(seed)} % (lat_max + 1);
                held_adr = wb_adr; held_dat = wb_dat_w; held_we = wb_we;
            end else if (wb_adr !== held_adr || wb_we !== held_we || (wb_we && wb_dat_w !== held_dat)) begin
                $display("ERROR the Wishbone request changed before its answer"); errors = errors + 1;
            end
            if (wait_left == 0 && !mute) begin
                n_wb = n_wb + 1;
                if (({$random(seed)} % 100) < err_pct) begin
                    wb_err <= 1'b1;
                    n_wb_err = n_wb_err + 1;
                end else begin
                    wb_ack <= 1'b1;
                    if (wb_we) regs[wb_adr[10:0]] = wb_dat_w;
                    wb_dat_r <= wb_we ? $random(seed) : regs[wb_adr[10:0]];   // junk for a write
                end
                wait_left = -1;
            end else if (wait_left > 0) begin
                wait_left = wait_left - 1;
            end
        end else if (!wb_cyc) begin
            wait_left = -1;                         // a request withdrawn (a reset) is forgotten
        end
    end

    // ---------------------------------------------------------------- the reference copy
    reg [31:0] ref_data [0:WORDS-1];
    reg        ref_known [0:WORDS-1];
    initial for (i = 0; i < WORDS; i = i + 1) begin ref_data[i] = 32'h0; ref_known[i] = 1'b1; end
    integer n_reads = 0, n_writes = 0, n_err = 0, n_refused = 0;

    // a random whole-word access, checked; returns at a falling edge
    task access;
        input integer err_ok;                       // 1: an error answer is allowed
        reg   [10:0] w;
        reg   [31:0] d;
        begin
            w = {$random(seed)} % WORDS;
            if ({$random(seed)} % 2) begin
                d = $random(seed);
                bfm.xfer(1'b1, {19'h0, w, 2'b00}, 4'hF, d);
                if (bfm.err) begin
                    if (!err_ok) begin $display("ERROR write %h: error answer", w); errors = errors + 1; end
                    n_err = n_err + 1;
                end else begin
                    ref_data[w] = d; ref_known[w] = 1'b1;
                end
                n_writes = n_writes + 1;
            end else begin
                bfm.xfer(1'b0, {19'h0, w, 2'b00}, 4'hF, 32'h0);
                if (bfm.err) begin
                    if (!err_ok) begin $display("ERROR read %h: error answer", w); errors = errors + 1; end
                    n_err = n_err + 1;
                end else if (ref_known[w] && bfm.rdata !== ref_data[w]) begin
                    $display("ERROR read %h: %h, expected %h", w, bfm.rdata, ref_data[w]); errors = errors + 1;
                end else if (!ref_known[w]) begin
                    ref_data[w] = bfm.rdata; ref_known[w] = 1'b1;     // what the cut write left
                end
                n_reads = n_reads + 1;
            end
        end
    endtask

    // the SoC side's reset in the middle of an access: offered, accepted, then rst for a while
    task cut_by_reset;
        input integer after;                        // SoC cycles between acceptance and reset
        reg   [10:0] w;
        begin
            w = {$random(seed)} % WORDS;
            @(negedge clk);                         // the BFM's outputs, driven from here
            bfm.m_req_valid = 1'b1; bfm.m_req_addr = {19'h0, w, 2'b00}; bfm.m_req_write = 1'b1;
            bfm.m_req_be = 4'hF; bfm.m_req_wdata = $random(seed); bfm.m_req_fetch = 1'b0;
            @(posedge clk);
            while (req_ready !== 1'b1) @(posedge clk);
            @(negedge clk);
            bfm.m_req_valid = 1'b0;
            repeat (after) @(negedge clk);
            rst = 1'b1;
            ref_known[w] = 1'b0;                    // the write may or may not happen
            repeat (2 + {$random(seed)} % 4) @(negedge clk);
            rst = 1'b0;
        end
    endtask

    integer n, k, words_unknown, t0;
    initial begin
        #1;
        bfm.seed = seed + 1;
        bfm.timeout = 4 * TIMEOUT + 100;
        repeat (4) @(negedge clk);
        rst = 1'b0;
        @(negedge wb_clk);
        wb_rst = 1'b0;
        repeat (4) @(negedge clk);

        // 1. random reads and writes
        for (n = 0; n < count; n = n + 1) access(0);

        // 2. not whole words, and fetches: refused at once, nothing on the Wishbone side
        k = n_wb;
        for (n = 0; n < 200; n = n + 1) begin
            case (n % 4)
                0: bfm.xfer($random(seed), 32'h4 * ({$random(seed)} % WORDS), 4'h1 << (n % 3), $random(seed));
                1: bfm.xfer($random(seed), 32'h4 * ({$random(seed)} % WORDS) + 1 + (n % 3), 4'hF, $random(seed));
                2: bfm.xfer($random(seed), 32'h4 * ({$random(seed)} % WORDS), 4'h3, $random(seed));
                default: begin
                    bfm.fetch_flag = 1'b1;
                    bfm.xfer(1'b0, 32'h4 * ({$random(seed)} % WORDS), 4'hF, 32'h0);
                    bfm.fetch_flag = 1'b0;
                end
            endcase
            if (!bfm.err || bfm.rdata !== 32'h0) begin
                $display("ERROR access %0d that is not a whole word: err %b rdata %h", n, bfm.err, bfm.rdata);
                errors = errors + 1;
            end
            n_refused = n_refused + 1;
        end
        repeat (20) @(negedge wb_clk);
        if (n_wb != k) begin $display("ERROR %0d refused accesses reached the Wishbone side", n_wb - k); errors = errors + 1; end

        // 3. errors from the slave
        err_pct = 30;
        k = n_wb_err;
        for (n = 0; n < count / 4; n = n + 1) access(1);
        err_pct = 0;
        if (n_wb_err == k) begin $display("ERROR no slave error was injected"); errors = errors + 1; end

        // 4. a muted slave: WB_TIMEOUT_CYCLES, then an error
        for (n = 0; n < 4; n = n + 1) begin
            mute = 1'b1;
            bfm.xfer(1'b0, 32'h4 * n, 4'hF, 32'h0);
            if (!bfm.err) begin $display("ERROR an access to the muted slave was not answered with an error"); errors = errors + 1; end
            if (TIMEOUT * sys_ps > WB_TIMEOUT * wb_ps * 2 && bfm.lat >= TIMEOUT) begin
                $display("ERROR the muted slave's access was answered by the SoC side's watchdog (%0d cycles), not the Wishbone limit", bfm.lat);
                errors = errors + 1;
            end
            bfm.xfer(1'b1, 32'h4 * n, 4'hF, 32'hBAD0_0000 | n);
            if (!bfm.err) begin $display("ERROR a write to the muted slave was not answered with an error"); errors = errors + 1; end
            // answered by the controller side's limit, the write was never done; by the SoC side's
            // watchdog (when it is the shorter one), it may still be done once the slave answers
            ref_known[n] = (TIMEOUT * sys_ps > WB_TIMEOUT * wb_ps * 2) ? ref_known[n] : 1'b0;
            mute = 1'b0;
            for (k = 0; k < 20; k = k + 1) access(0);
        end

        // 5. the controller's clock stopped: the SoC side's watchdog, then recovery
        for (n = 0; n < 6; n = n + 1) begin
            @(negedge wb_clk);
            wb_run = 1'b0;
            k = {$random(seed)} % WORDS;
            t0 = 0;
            bfm.xfer(1'b1, {19'h0, k[10:0], 2'b00}, 4'hF, 32'hC10C_0000 | n);
            if (!bfm.err || bfm.lat < TIMEOUT) begin
                $display("ERROR clock stopped: write answered after %0d cycles, err %b", bfm.lat, bfm.err);
                errors = errors + 1;
            end
            ref_known[k] = 1'b0;                    // carried out later, or not at all
            bfm.xfer(1'b0, 32'h0, 4'hF, 32'h0);     // waits for the first, then its own watchdog
            if (!bfm.err) begin $display("ERROR clock stopped: the second access did not err"); errors = errors + 1; end
            wb_run = 1'b1;
            for (k = 0; k < 30; k = k + 1) access(0);
        end

        // 6. resets
        for (n = 0; n < 20; n = n + 1) begin
            cut_by_reset({$random(seed)} % 12);
            for (k = 0; k < 10; k = k + 1) access(0);
            // the controller side's reset while an access waits on it: answered with an error
            lat_max = 30;
            fork
                begin
                    k = {$random(seed)} % WORDS;
                    bfm.xfer(1'b1, {19'h0, k[10:0], 2'b00}, 4'hF, 32'hEE00_0000 | n);
                    if (!bfm.err) begin ref_data[k] = 32'hEE00_0000 | n; ref_known[k] = 1'b1; end
                    else ref_known[k] = 1'b0;
                end
                begin
                    repeat (2 + {$random(seed)} % 20) @(negedge wb_clk);
                    wb_rst = 1'b1;
                    repeat (1 + {$random(seed)} % 3) @(negedge wb_clk);
                    wb_rst = 1'b0;
                end
            join
            lat_max = 3;
            for (k = 0; k < 10; k = k + 1) access(0);
        end

        // 7. random accesses again, then every word read back
        for (n = 0; n < count; n = n + 1) access(0);
        words_unknown = 0;
        for (n = 0; n < WORDS; n = n + 1) begin
            bfm.xfer(1'b0, 32'h4 * n, 4'hF, 32'h0);
            if (bfm.err) begin $display("ERROR final read %h: error", n); errors = errors + 1; end
            else if (ref_known[n] && bfm.rdata !== ref_data[n]) begin
                $display("ERROR final read %h: %h, expected %h", n, bfm.rdata, ref_data[n]); errors = errors + 1;
            end else if (!ref_known[n]) words_unknown = words_unknown + 1;
        end

        repeat (4) @(negedge clk);
        errors = errors + bfm.errors + mon.errors;
        if (errors) $fatal(1, "tb_ddr_csr: %0d errors", errors);
        $display("tb_ddr_csr: PASS (sys %0d ps, wb %0d ps, jitter %0d%%, TIMEOUT %0d, WB_TIMEOUT %0d: %0d reads, %0d writes, %0d error answers, %0d refused at once, %0d Wishbone cycles with %0d slave errors, %0d words left unknown by cut accesses)",
                 sys_ps, wb_ps, jitter, TIMEOUT, WB_TIMEOUT, n_reads, n_writes, n_err, n_refused, n_wb, n_wb_err, words_unknown);
        $finish;
    end
endmodule
