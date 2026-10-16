/*
SurfCPU memory testbench
--------------------
File: tb_mem.v Date: 10/15/26 (SurfCPU)
--------------------
Icarus testbench for the three block RAMs through SurfBus BFMs, one per port:
* surf_bootrom: random reads on both ports at once against the image (ROMFILE, or the file
  given with +bootrom=, which must win; an empty ROMFILE means all zero); writes on either
  port answer with an error and change nothing.
* surf_scratch: random byte, halfword, word and arbitrary byte-enable writes on port B against
  a model, reads on both ports at once (port A on words port B is not writing), port A reading
  the word port B writes in the same cycle gets the old word, and a write on port A answers
  with an error and changes nothing.
* surf_textram: random byte-enabled writes and reads on the bus port; the pixel port (its own
  25.18 MHz clock) reads every cell one pix_clk edge later, both in a sweep and while the bus
  writes other cells; a fetch answers with an error and does not write.
A protocol monitor on every port checks the next-cycle answer. With SURF_GATE defined it runs
on the synthesized netlists of the three memories (make test-soc-gate). +seed=N. Prints PASS or
stops with $fatal.
*/
`timescale 1ns/1ps
module tb_mem;
    `include "surfcpu_soc.vh"

    parameter ROMFILE = "";

    localparam integer ROM_WORDS = SURF_BOOTROM_SIZE / 4;
    localparam integer SCR_WORDS = SURF_SCRATCH_SIZE / 4;
    localparam integer TXT_WORDS = SURF_TEXTRAM_SIZE / 4;

    reg clk = 1'b0, pix_clk = 1'b0;
    always #20 clk = ~clk;
    always #19.859 pix_clk = ~pix_clk;
    reg rst = 1'b1;
    integer seed = 1;
    integer errors = 0;

    // ---------------------------------------------------------------- one BFM + monitor per port
    `define PORT(P) \
    wire        P``_req_valid, P``_req_ready, P``_req_write, P``_req_fetch, P``_rsp_valid, P``_rsp_err; \
    wire [31:0] P``_req_addr, P``_req_wdata, P``_rsp_rdata; \
    wire [3:0]  P``_req_be;

    `PORT(ra) `PORT(rb) `PORT(sa) `PORT(sb) `PORT(tx)

    `define BFM_MON(P, F) \
    surfbus_bfm #(.NAME(`"P`"), .FETCH(F)) P``_bfm ( \
        .clk(clk), .m_req_valid(P``_req_valid), .m_req_ready(P``_req_ready), \
        .m_req_addr(P``_req_addr), .m_req_write(P``_req_write), .m_req_be(P``_req_be), \
        .m_req_wdata(P``_req_wdata), .m_req_fetch(P``_req_fetch), .m_rsp_valid(P``_rsp_valid), \
        .m_rsp_rdata(P``_rsp_rdata), .m_rsp_err(P``_rsp_err)); \
    surfbus_monitor #(.NAME(`"P``_mon`"), .MAX_LAT(1)) P``_mon ( \
        .clk(clk), .rst(rst), .req_valid(P``_req_valid), .req_ready(P``_req_ready), \
        .req_addr(P``_req_addr), .req_write(P``_req_write), .req_be(P``_req_be), \
        .req_wdata(P``_req_wdata), .req_fetch(P``_req_fetch), .rsp_valid(P``_rsp_valid), \
        .rsp_rdata(P``_rsp_rdata), .rsp_err(P``_rsp_err));

    `BFM_MON(ra, 1) `BFM_MON(rb, 0) `BFM_MON(sa, 1) `BFM_MON(sb, 0) `BFM_MON(tx, 0)

    `define SLAVE(PFX, P) \
        .PFX``_req_valid(P``_req_valid), .PFX``_req_ready(P``_req_ready), \
        .PFX``_req_addr(P``_req_addr), .PFX``_req_write(P``_req_write), .PFX``_req_be(P``_req_be), \
        .PFX``_req_wdata(P``_req_wdata), .PFX``_req_fetch(P``_req_fetch), \
        .PFX``_rsp_valid(P``_rsp_valid), .PFX``_rsp_rdata(P``_rsp_rdata), .PFX``_rsp_err(P``_rsp_err)

`ifdef SURF_GATE   // a synthesized netlist: the image is already in the block RAMs' INIT
    surf_bootrom u_rom (.clk(clk), .rst(rst), `SLAVE(a, ra), `SLAVE(b, rb));
`else
    surf_bootrom #(.INIT_FILE(ROMFILE)) u_rom (.clk(clk), .rst(rst), `SLAVE(a, ra), `SLAVE(b, rb));
`endif
    surf_scratch u_scr (.clk(clk), .rst(rst), `SLAVE(a, sa), `SLAVE(b, sb));

    reg  [11:0] pix_addr = 12'h0;
    wire [31:0] pix_data;
    surf_textram u_txt (.clk(clk), .rst(rst), `SLAVE(s, tx), .pix_clk(pix_clk), .pix_addr(pix_addr),
                        .pix_data(pix_data));

    // ---------------------------------------------------------------- reference contents
    reg [31:0] rom_ref [0:ROM_WORDS-1];
    reg [31:0] scr_ref [0:SCR_WORDS-1];
    reg [31:0] txt_ref [0:TXT_WORDS-1];
    reg [8*1024-1:0] plusarg_file;
    integer i;
    initial begin
        for (i = 0; i < ROM_WORDS; i = i + 1) rom_ref[i] = 32'h0;
        for (i = 0; i < SCR_WORDS; i = i + 1) scr_ref[i] = 32'h0;
        for (i = 0; i < TXT_WORDS; i = i + 1) txt_ref[i] = 32'h0;
        if (ROMFILE != "") $readmemh(ROMFILE, rom_ref);
        if ($value$plusargs("bootrom=%s", plusarg_file)) $readmemh(plusarg_file, rom_ref);
    end

    function [31:0] merge;      // a word with the enabled bytes replaced
        input [31:0] old;
        input [31:0] data;
        input [3:0]  be;
        begin
            merge = {be[3] ? data[31:24] : old[31:24], be[2] ? data[23:16] : old[23:16],
                     be[1] ? data[15:8]  : old[15:8],  be[0] ? data[7:0]   : old[7:0]};
        end
    endfunction

    function [3:0] rand_be;     // byte, halfword or word lanes for the address, or any pattern
        input [1:0]   a;
        input integer r;
        begin
            case (r % 4)
                0: rand_be = 4'b0001 << a;
                1: rand_be = a[1] ? 4'b1100 : 4'b0011;
                2: rand_be = 4'b1111;
                default: rand_be = (r >> 4) & 4'hF;
            endcase
        end
    endfunction

    // ---------------------------------------------------------------- port processes
    integer n_rom_a = 0, n_rom_b = 0, n_scr_a = 0, n_scr_b = 0, n_txt = 0, n_pix = 0;

    task rom_reads_a;
        input integer count;
        integer k, w;
        begin
            for (k = 0; k < count; k = k + 1) begin
                w = {$random(seed)} % ROM_WORDS;
                ra_bfm.read_check(SURF_BOOTROM_BASE + 4 * w, 4'hF, rom_ref[w], 32'hFFFF_FFFF);
                n_rom_a = n_rom_a + 1;
            end
        end
    endtask

    task rom_reads_b;
        input integer count;
        integer k, w, r;
        begin
            for (k = 0; k < count; k = k + 1) begin
                w = {$random(seed)} % ROM_WORDS;
                r = $random(seed);
                rb_bfm.read_check(SURF_BOOTROM_BASE + 4 * w + (r & 3), rand_be(r[1:0], r >>> 2),
                                  rom_ref[w], 32'hFFFF_FFFF);   // the whole word comes back
                n_rom_b = n_rom_b + 1;
            end
        end
    endtask

    task scr_traffic_b;         // port B: random writes and reads in the upper half
        input integer count;
        integer k, w, r;
        reg [3:0]  be;
        reg [31:0] d;
        begin
            for (k = 0; k < count; k = k + 1) begin
                w = SCR_WORDS / 2 + {$random(seed)} % (SCR_WORDS / 2);
                r = $random(seed);
                be = rand_be(r[1:0], r >>> 2);
                if (r[30]) begin
                    d = $random(seed);
                    sb_bfm.write_ok(SURF_SCRATCH_BASE + 4 * w + (r & 3), be, d);
                    scr_ref[w] = merge(scr_ref[w], d, be);
                end else begin
                    sb_bfm.read_check(SURF_SCRATCH_BASE + 4 * w + (r & 3), be, scr_ref[w],
                                      32'hFFFF_FFFF);
                end
                n_scr_b = n_scr_b + 1;
            end
        end
    endtask

    task scr_reads_a;           // port A: reads in the lower half (not being written)
        input integer count;
        integer k, w;
        begin
            for (k = 0; k < count; k = k + 1) begin
                w = {$random(seed)} % (SCR_WORDS / 2);
                sa_bfm.read_check(SURF_SCRATCH_BASE + 4 * w, 4'hF, scr_ref[w], 32'hFFFF_FFFF);
                n_scr_a = n_scr_a + 1;
            end
        end
    endtask

    task txt_traffic;           // random writes and reads in cells 2048..4095
        input integer count;
        integer k, w, r;
        reg [3:0]  be;
        reg [31:0] d;
        begin
            for (k = 0; k < count; k = k + 1) begin
                w = TXT_WORDS / 2 + {$random(seed)} % (TXT_WORDS / 2);
                r = $random(seed);
                be = rand_be(r[1:0], r >>> 2);
                if (r[30]) begin
                    d = $random(seed);
                    tx_bfm.write_ok(SURF_TEXTRAM_BASE + 4 * w + (r & 3), be, d);
                    txt_ref[w] = merge(txt_ref[w], d, be);
                end else begin
                    tx_bfm.read_check(SURF_TEXTRAM_BASE + 4 * w + (r & 3), be, txt_ref[w],
                                      32'hFFFF_FFFF);
                end
                n_txt = n_txt + 1;
            end
        end
    endtask

    task pix_read;              // the cell appears one pix_clk edge after the address
        input integer idx;
        begin
            @(negedge pix_clk) pix_addr = idx;
            @(posedge pix_clk) #1;
            if (pix_data !== txt_ref[idx]) begin
                $display("ERROR pixel port cell %0d: %h, expected %h", idx, pix_data, txt_ref[idx]);
                errors = errors + 1;
            end
            n_pix = n_pix + 1;
        end
    endtask

    task pix_sweep;             // cells lo..hi-1
        input integer lo;
        input integer hi;
        integer c;
        begin
            for (c = lo; c < hi; c = c + 1) pix_read(c);
        end
    endtask

    integer n, k, w, nerr_checks;
    reg [31:0] d, old;

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        ra_bfm.seed = seed; rb_bfm.seed = seed + 1; sa_bfm.seed = seed + 2;
        sb_bfm.seed = seed + 3; tx_bfm.seed = seed + 4;
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;
        nerr_checks = 0;

        // ------------------------------------------------ boot ROM
        ra_bfm.read_check(SURF_BOOTROM_BASE, 4'hF, rom_ref[0], 32'hFFFF_FFFF);
        rb_bfm.read_check(SURF_BOOTROM_BASE + SURF_BOOTROM_SIZE - 4, 4'hF, rom_ref[ROM_WORDS - 1],
                          32'hFFFF_FFFF);
        fork
            rom_reads_a(3000);
            rom_reads_b(3000);
        join
        for (n = 0; n < 200; n = n + 1) begin
            w = {$random(seed)} % ROM_WORDS;
            rb_bfm.write_err(SURF_BOOTROM_BASE + 4 * w, rand_be(2'b00, n), $random(seed));
            ra_bfm.xfer(1'b1, SURF_BOOTROM_BASE + 4 * w, 4'hF, 32'h0);
            if (ra_bfm.err !== 1'b1) begin
                $display("ERROR boot ROM port A took a write"); errors = errors + 1;
            end
            rb_bfm.read_check(SURF_BOOTROM_BASE + 4 * w, 4'hF, rom_ref[w], 32'hFFFF_FFFF);
            ra_bfm.read_check(SURF_BOOTROM_BASE + 4 * w, 4'hF, rom_ref[w], 32'hFFFF_FFFF);
            nerr_checks = nerr_checks + 2;
        end

        // ------------------------------------------------ scratch RAM
        // fill the lower half through port B, then traffic on both ports at once
        for (w = 0; w < SCR_WORDS / 2; w = w + 37) begin
            d = $random(seed);
            sb_bfm.write_ok(SURF_SCRATCH_BASE + 4 * w, 4'hF, d);
            scr_ref[w] = d;
        end
        fork
            scr_traffic_b(4000);
            scr_reads_a(4000);
        join
        // port B: read back everything it wrote, and the start and end words
        for (w = SCR_WORDS / 2; w < SCR_WORDS; w = w + 1)
            if (scr_ref[w] !== 32'h0) sb_bfm.read_check(SURF_SCRATCH_BASE + 4 * w, 4'hF, scr_ref[w],
                                                        32'hFFFF_FFFF);
        sb_bfm.write_ok(SURF_SCRATCH_BASE, 4'hF, 32'h0102_0304);
        scr_ref[0] = 32'h0102_0304;
        sb_bfm.write_ok(SURF_SCRATCH_BASE + SURF_SCRATCH_SIZE - 1, 4'b1000, 32'hAB00_0000);
        scr_ref[SCR_WORDS - 1] = merge(scr_ref[SCR_WORDS - 1], 32'hAB00_0000, 4'b1000);
        sa_bfm.read_check(SURF_SCRATCH_BASE, 4'hF, scr_ref[0], 32'hFFFF_FFFF);
        sa_bfm.read_check(SURF_SCRATCH_BASE + SURF_SCRATCH_SIZE - 4, 4'hF, scr_ref[SCR_WORDS - 1],
                          32'hFFFF_FFFF);
        // same word, same cycle: port A reads while port B writes -> the old word
        for (n = 0; n < 50; n = n + 1) begin
            w = {$random(seed)} % SCR_WORDS;
            d = $random(seed);
            old = scr_ref[w];
            sa_bfm.max_gap = 0; sb_bfm.max_gap = 0;
            fork
                sa_bfm.xfer(1'b0, SURF_SCRATCH_BASE + 4 * w, 4'hF, 32'h0);
                sb_bfm.xfer(1'b1, SURF_SCRATCH_BASE + 4 * w, 4'hF, d);
            join
            sa_bfm.max_gap = 2; sb_bfm.max_gap = 2;
            scr_ref[w] = d;
            if (sa_bfm.err !== 1'b0 || sa_bfm.rdata !== old) begin
                $display("ERROR scratch collision: port A read %h, expected the old word %h",
                         sa_bfm.rdata, old);
                errors = errors + 1;
            end
            sa_bfm.read_check(SURF_SCRATCH_BASE + 4 * w, 4'hF, d, 32'hFFFF_FFFF);
        end
        // a write on port A is an error and changes nothing
        for (n = 0; n < 50; n = n + 1) begin
            w = {$random(seed)} % SCR_WORDS;
            sa_bfm.xfer(1'b1, SURF_SCRATCH_BASE + 4 * w, 4'hF, ~scr_ref[w]);
            if (sa_bfm.err !== 1'b1) begin
                $display("ERROR scratch port A took a write"); errors = errors + 1;
            end
            sb_bfm.read_check(SURF_SCRATCH_BASE + 4 * w, 4'hF, scr_ref[w], 32'hFFFF_FFFF);
            nerr_checks = nerr_checks + 1;
        end

        // ------------------------------------------------ text RAM
        pix_sweep(0, TXT_WORDS);                              // all zero at start
        for (w = 0; w < TXT_WORDS / 2; w = w + 1) begin       // fill the screen half
            d = $random(seed);
            tx_bfm.write_ok(SURF_TEXTRAM_BASE + 4 * w, 4'hF, d);
            txt_ref[w] = d;
        end
        pix_sweep(0, TXT_WORDS / 2);
        fork                                                   // bus on the upper half,
            txt_traffic(4000);                                 // pixel port on the lower
            begin
                for (n = 0; n < 6000; n = n + 1) pix_read({$random(seed)} % (TXT_WORDS / 2));
            end
        join
        pix_sweep(0, TXT_WORDS);
        // a fetch is an error and writes nothing
        for (n = 0; n < 50; n = n + 1) begin
            w = {$random(seed)} % TXT_WORDS;
            tx_bfm.fetch_flag = 1'b1;
            tx_bfm.write_err(SURF_TEXTRAM_BASE + 4 * w, 4'hF, ~txt_ref[w]);
            tx_bfm.read_err(SURF_TEXTRAM_BASE + 4 * w, 4'hF);
            tx_bfm.fetch_flag = 1'b0;
            tx_bfm.read_check(SURF_TEXTRAM_BASE + 4 * w, 4'hF, txt_ref[w], 32'hFFFF_FFFF);
            nerr_checks = nerr_checks + 2;
        end
        pix_sweep(0, TXT_WORDS);

        repeat (2) @(posedge clk);   // let the monitors see the last responses
        errors = errors + ra_bfm.errors + rb_bfm.errors + sa_bfm.errors + sb_bfm.errors +
                 tx_bfm.errors + ra_mon.errors + rb_mon.errors + sa_mon.errors + sb_mon.errors +
                 tx_mon.errors;
        if (errors) $fatal(1, "tb_mem: %0d errors", errors);
        $display("tb_mem: PASS (ROM %0s: %0d port A + %0d port B reads; scratch %0d port A + %0d port B; text RAM %0d bus + %0d pixel reads; %0d error responses checked; %0d+%0d back-to-back)",
                 (ROMFILE != "") ? "image" : "empty", n_rom_a, n_rom_b, n_scr_a, n_scr_b, n_txt, n_pix,
                 nerr_checks, ra_bfm.n_b2b, sb_bfm.n_b2b);
        $finish;
    end
endmodule
