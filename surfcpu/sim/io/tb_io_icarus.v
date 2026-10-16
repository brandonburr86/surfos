/*
SurfCPU io blocks in Icarus
--------------------
File: tb_io_icarus.v Date: 10/15/26 (SurfCPU)
--------------------
A Verilog testbench that runs surf_spi, surf_ps2 and surf_video together in Icarus (the
detailed tests are the Verilator ones in sim/io/test_*.cpp): one SurfBus master in front
of a small decoder, the SPI port in loopback, a behavioural keyboard sending one frame on
open-drain lines, a send with nobody answering (TX_NAK after 15 ms), and the video
timing measured over a frame. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
module tb_io_icarus;
    reg clk = 0, pix_clk = 0;
    always #20 clk = ~clk;                // 25 MHz
    always #19.859 pix_clk = ~pix_clk;    // 25.1773 MHz
    reg rst = 1, pix_rst = 1;
    integer errors = 0;

    // ---------------------------------------------------------------- one master, three slaves
    reg         req_valid = 0, req_write = 0, req_fetch = 0;
    reg  [31:0] req_addr = 0, req_wdata = 0;
    reg  [3:0]  req_be = 4'hF;
    wire [2:0]  sel = (req_addr[31:16] == 16'h1000) ? ((req_addr[15:12] == 4'h4) ? 3'd1 :
                                                       (req_addr[15:12] == 4'h5) ? 3'd2 :
                                                       (req_addr[15:12] == 4'h6) ? 3'd3 : 3'd0) : 3'd0;
    wire        spi_ready, ps2_ready, vid_ready;
    wire        spi_rv, ps2_rv, vid_rv, spi_re, ps2_re, vid_re;
    wire [31:0] spi_rd, ps2_rd, vid_rd;
    wire        ready = (sel == 3'd1) ? spi_ready : (sel == 3'd2) ? ps2_ready : (sel == 3'd3) ? vid_ready : 1'b0;
    wire        rsp_valid = spi_rv | ps2_rv | vid_rv;
    wire [31:0] rsp_rdata = spi_rd | ps2_rd | vid_rd;
    wire        rsp_err = spi_re | ps2_re | vid_re;

    // ---------------------------------------------------------------- SPI in loopback
    wire sclk, mosi, cs_n, spi_irq;
    surf_spi #(.SYS_HZ(25000000)) spi (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid && sel == 3'd1), .s_req_ready(spi_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata), .s_req_fetch(req_fetch),
        .s_rsp_valid(spi_rv), .s_rsp_rdata(spi_rd), .s_rsp_err(spi_re),
        .irq(spi_irq), .sclk(sclk), .mosi(mosi), .miso(mosi), .cs_n(cs_n), .cd_n(1'b0));

    // ---------------------------------------------------------------- PS/2 with open-drain lines
    reg  kb_clk = 1, kb_dat = 1;          // the keyboard pulls low when 0
    wire ps2_clk_oe, ps2_data_oe, ps2_irq;
    wire ps2_clk_line  = kb_clk & ~ps2_clk_oe;
    wire ps2_data_line = kb_dat & ~ps2_data_oe;
    surf_ps2 #(.SYS_HZ(25000000)) ps2 (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid && sel == 3'd2), .s_req_ready(ps2_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata), .s_req_fetch(req_fetch),
        .s_rsp_valid(ps2_rv), .s_rsp_rdata(ps2_rd), .s_rsp_err(ps2_re),
        .irq(ps2_irq), .ps2_clk_i(ps2_clk_line), .ps2_data_i(ps2_data_line),
        .ps2_clk_oe(ps2_clk_oe), .ps2_data_oe(ps2_data_oe));

    // ---------------------------------------------------------------- video with a text RAM
    wire [11:0] text_addr;
    reg  [31:0] text_data = 32'd0;
    reg  [31:0] tram [0:4095];
    integer k;
    initial for (k = 0; k < 4096; k = k + 1) tram[k] = 32'h0000_1F41;   // 'A' white on blue
    always @(posedge pix_clk) text_data <= tram[text_addr];
    wire [3:0] vr, vg, vb;
    wire       hs, vs, vid_irq;
    surf_video #(.PIX_HZ(25177305)) video (
        .clk(clk), .rst(rst),
        .s_req_valid(req_valid && sel == 3'd3), .s_req_ready(vid_ready), .s_req_addr(req_addr),
        .s_req_write(req_write), .s_req_be(req_be), .s_req_wdata(req_wdata), .s_req_fetch(req_fetch),
        .s_rsp_valid(vid_rv), .s_rsp_rdata(vid_rd), .s_rsp_err(vid_re), .irq(vid_irq),
        .pix_clk(pix_clk), .pix_rst(pix_rst), .text_addr(text_addr), .text_data(text_data),
        .vga_r(vr), .vga_g(vg), .vga_b(vb), .vga_hs(hs), .vga_vs(vs));

    // ---------------------------------------------------------------- bus access
    reg [31:0] rdata;
    reg        rerr;
    task bus(input wr, input [31:0] a, input [31:0] wd);
        begin
            @(negedge clk);
            req_valid = 1; req_write = wr; req_addr = a; req_wdata = wd; req_be = 4'hF;
            while (!ready) @(negedge clk);
            @(negedge clk);                // accepted at the rising edge just passed
            req_valid = 0;
            while (!rsp_valid) @(negedge clk);
            rdata = rsp_rdata;
            rerr  = rsp_err;
        end
    endtask
    task expect(input [31:0] a, input [31:0] want, input [31:0] mask);
        begin
            bus(0, a, 0);
            if (rerr || ((rdata & mask) !== (want & mask))) begin
                $display("FAIL read %h: %h (err %b), want %h", a, rdata, rerr, want);
                errors = errors + 1;
            end
        end
    endtask

    // ---------------------------------------------------------------- the keyboard sends one byte
    task kb_send(input [7:0] b);
        reg [10:0] f;
        integer i;
        begin
            f = {1'b1, ~^b, b, 1'b0};      // stop, odd parity, data, start
            for (i = 0; i < 11; i = i + 1) begin
                kb_dat = f[i];
                #20000 kb_clk = 0;         // 12.5 kHz: 20 us setup, 40 us low, 20 us high
                #40000 kb_clk = 1;
                #20000;
            end
            kb_dat = 1;
        end
    endtask

    // ---------------------------------------------------------------- video timing
    integer pix_n = 0, hs_fall = 0, hs_rise = 0, vs_fall = 0, line_len = 0, hs_width = 0, frame_len = 0;
    integer first_lit = -1, vs_falls = 0;
    reg hs_d = 1, vs_d = 1;
    always @(posedge pix_clk) begin
        pix_n = pix_n + 1;
        if (!hs && hs_d) begin line_len = pix_n - hs_fall; hs_fall = pix_n; end
        if (hs && !hs_d) begin hs_width = pix_n - hs_fall; hs_rise = pix_n; end
        if (!vs && vs_d) begin frame_len = pix_n - vs_fall; vs_fall = pix_n; vs_falls = vs_falls + 1; end
        if (vs_falls >= 1 && first_lit < 0 && hs_fall > vs_fall + 40 * 800 && (vr | vg | vb) != 0)
            first_lit = pix_n - hs_fall;   // the border's first pixel after the hsync edge
        hs_d <= hs;
        vs_d <= vs;
    end

    initial begin
        repeat (6) @(posedge clk);
        rst = 0;
        pix_rst = 0;
        // SPI: reset DIV, loopback bytes, chip select
        expect(32'h1000_400C, 32'd31, 32'hFF);
        bus(1, 32'h1000_400C, 32'd1);
        bus(1, 32'h1000_4000, 32'hA5);
        expect(32'h1000_4000, 32'hA5, 32'hFFFF_FFFF);
        bus(1, 32'h1000_4000, 32'h3C);
        expect(32'h1000_4000, 32'h3C, 32'hFFFF_FFFF);
        bus(1, 32'h1000_4008, 32'd1);
        if (cs_n !== 1'b0) begin $display("FAIL cs_n"); errors = errors + 1; end
        bus(0, 32'h1000_4010, 0);
        if (!rerr) begin $display("FAIL no error for an unknown SPI offset"); errors = errors + 1; end
        // PS/2: a frame from the keyboard, then a send nobody answers
        expect(32'h1000_5008, 32'h1, 32'hFFFF_FFFF);
        kb_send(8'h5A);
        repeat (200) @(posedge clk);
        expect(32'h1000_5004, 32'h1, 32'hFF);          // RX_AVAIL only
        expect(32'h1000_5000, 32'h5A, 32'hFFFF_FFFF);
        expect(32'h1000_5000, 32'h8000_0000, 32'hFFFF_FFFF);
        bus(1, 32'h1000_500C, 32'hED);
        repeat (100) @(posedge clk);
        if (ps2_clk_oe !== 1'b1) begin $display("FAIL the send did not inhibit the clock"); errors = errors + 1; end
        #16000000;                                     // 16 ms: TX_NAK after 15 ms without a device
        expect(32'h1000_5004, 32'h80, 32'hFF);
        if (ps2_clk_oe || ps2_data_oe) begin $display("FAIL lines still pulled"); errors = errors + 1; end
        // video: geometry registers, then the timing of the test pattern over a whole frame
        expect(32'h1000_6014, 32'h1008_1E50, 32'hFFFF_FFFF);
        expect(32'h1000_6018, 32'd25177305, 32'hFFFF_FFFF);
        wait (vs_falls >= 2);
        if (line_len != 800 || hs_width != 96 || frame_len != 420000 || first_lit != 144) begin
            $display("FAIL video: line %0d, hsync %0d, frame %0d, first lit pixel %0d after hsync",
                     line_len, hs_width, frame_len, first_lit);
            errors = errors + 1;
        end
        if (errors) $fatal(1, "tb_io_icarus: %0d errors", errors);
        $display("tb_io_icarus: PASS (SPI loopback, PS/2 receive and TX_NAK, video 800 x 525 = %0d pixels per frame)",
                 frame_len);
        $finish;
    end
endmodule
