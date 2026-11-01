/*
SurfCPU Arty A7: the synthesized full top, run from its pins
--------------------
File: tb_netlist.v Date: 10/16/26 (SurfCPU)
--------------------
The post-synthesis simulation (sim/board/netlist_sim.sh, make board-netlist-sim): the open
flow's Yosys netlist of surfcpu_arty_a7_top, with Yosys's cell models and the block RAM and
clock tile models of sim/board/prims, clocked at 100 MHz on its clk100 pin as the board's
oscillator does. Nothing inside the netlist is touched; only its pins are seen. With
ARTY_HAS_DDR it takes the DDR top's pins too (the DDR designs, and the RTL simulation of the
DDR top, sim/board/ddr_top_sim.sh): no memory is on the DDR3L pins (DQ and DQS weakly pulled
low, nothing answers).
  * uart_tx is received at 115200 8N1 (the start bit checked at its middle, each bit sampled
    at its middle, the stop bit checked); each character goes to +transcript=FILE as it came
    and each line is printed as "tb_netlist: <time> uart| <line>";
  * the run ends at the first line that begins with +stop=TEXT (default "BRAMDIAG:") or after
    +lines=N lines, and prints the LEDs, the characters, the framing errors and the VGA sync
    periods seen; it stops with $fatal after +max_ms=N milliseconds of simulated time
    (default 300).
The SD socket is empty (sd_cd_n high), the switches, buttons and UART input are idle and the
PS/2 lines are released (the adapter's pull-ups). Runs in Verilator --timing and in Icarus.
*/
`timescale 1ps/1ps
module tb_netlist;
    reg clk100 = 1'b0;
    always #5000 clk100 = ~clk100;

    reg        ck_rst_n = 1'b1;
    reg  [3:0] sw = 4'h0, btn = 4'h0;
    reg        uart_rx = 1'b1, sd_miso = 1'b1, sd_cd_n = 1'b1, sd_dat1 = 1'b1, sd_dat2 = 1'b1;
    reg        ps2_clk2 = 1'b1, ps2_data2 = 1'b1;
    wire [3:0] led;
    wire [11:0] rgb;
    wire       uart_tx, sd_cs_n, sd_mosi, sd_sclk;
    wire [3:0] vga_r, vga_g, vga_b;
    wire       vga_hs, vga_vs;
    wire       ps2_clk, ps2_data;
    pullup (ps2_clk);
    pullup (ps2_data);
`ifdef ARTY_HAS_DDR
    wire [13:0] ddr3_addr;
    wire [2:0]  ddr3_ba;
    wire        ddr3_ras_n, ddr3_cas_n, ddr3_we_n, ddr3_reset_n, ddr3_ck_p, ddr3_ck_n;
    wire        ddr3_cke, ddr3_cs_n, ddr3_odt;
    wire [1:0]  ddr3_dm;
    wire [15:0] ddr3_dq;
    wire [1:0]  ddr3_dqs_p, ddr3_dqs_n;
    genvar gq;
    generate
        for (gq = 0; gq < 16; gq = gq + 1) begin : g_dq
            pulldown (ddr3_dq[gq]);
        end
        for (gq = 0; gq < 2; gq = gq + 1) begin : g_dqs
            pulldown (ddr3_dqs_p[gq]);
            pulldown (ddr3_dqs_n[gq]);
        end
    endgenerate
`endif

    surfcpu_arty_a7_top dut (
        .clk100(clk100), .ck_rst_n(ck_rst_n), .sw(sw), .btn(btn), .led(led), .rgb(rgb),
        .uart_rx(uart_rx), .uart_tx(uart_tx),
        .sd_cs_n(sd_cs_n), .sd_mosi(sd_mosi), .sd_miso(sd_miso), .sd_sclk(sd_sclk),
        .sd_cd_n(sd_cd_n), .sd_dat1(sd_dat1), .sd_dat2(sd_dat2),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs),
        .ps2_clk(ps2_clk), .ps2_data(ps2_data), .ps2_clk2(ps2_clk2), .ps2_data2(ps2_data2)
`ifdef ARTY_HAS_DDR
        ,
        .ddr3_addr(ddr3_addr), .ddr3_ba(ddr3_ba), .ddr3_ras_n(ddr3_ras_n), .ddr3_cas_n(ddr3_cas_n),
        .ddr3_we_n(ddr3_we_n), .ddr3_reset_n(ddr3_reset_n), .ddr3_ck_p(ddr3_ck_p), .ddr3_ck_n(ddr3_ck_n),
        .ddr3_cke(ddr3_cke), .ddr3_cs_n(ddr3_cs_n), .ddr3_dm(ddr3_dm), .ddr3_odt(ddr3_odt),
        .ddr3_dq(ddr3_dq), .ddr3_dqs_p(ddr3_dqs_p), .ddr3_dqs_n(ddr3_dqs_n)
`endif
    );

    // ------------------------------------------------------------------ settings
    localparam real BIT_PS = 1.0e12 / 115200.0;
    localparam real TEN_MS = 1.0e10;
    reg  [8*64-1:0]   stop;
    reg  [8*1024-1:0] tfile;
    integer stop_len, max_lines, max_ms, fd, i;
    reg  [7:0] line [0:255];
    reg  [7:0] ch;
    integer len = 0, nlines = 0, nchars = 0, nframing = 0, k, j;
    reg match;
    initial begin
        stop = "BRAMDIAG:";
        if ($value$plusargs("stop=%s", stop)) begin end
        stop_len = 0;
        for (i = 0; i < 64; i = i + 1) if (stop[8*i +: 8] != 8'h00) stop_len = i + 1;
        max_lines = 0;
        if ($value$plusargs("lines=%d", max_lines)) begin end
        max_ms = 300;
        if ($value$plusargs("max_ms=%d", max_ms)) begin end
        fd = 0;
        if ($value$plusargs("transcript=%s", tfile)) fd = $fopen(tfile, "wb");
    end

    // ------------------------------------------------------------------ the UART receiver
    task finish_line;
        begin
            $write("tb_netlist: %0d us uart| ", $time / 1000000);
            for (j = 0; j < len; j = j + 1) $write("%c", line[j]);
            $write("\n");
            $fflush;
            nlines = nlines + 1;
            match = len >= stop_len && stop_len > 0;
            for (j = 0; j < stop_len && j < len; j = j + 1)
                if (line[j] != stop[8*(stop_len-1-j) +: 8]) match = 1'b0;
            len = 0;
            if (match || (max_lines > 0 && nlines >= max_lines)) begin
                summary;
                $finish;
            end
        end
    endtask

    initial begin
        wait (uart_tx === 1'b1);                  // the line is idle once the SoC is out of reset
        forever begin
            @(negedge uart_tx);
            #(BIT_PS / 2.0);
            if (uart_tx === 1'b0) begin           // a start bit, not a glitch
                for (k = 0; k < 8; k = k + 1) begin
                    #(BIT_PS);
                    ch[k] = uart_tx;
                end
                #(BIT_PS);
                if (uart_tx !== 1'b1) nframing = nframing + 1;
                nchars = nchars + 1;
                if (fd != 0) $fwrite(fd, "%c", ch);
                if (ch == 8'h0a) finish_line;
                else if (ch != 8'h0d && len < 256) begin
                    line[len] = ch;
                    len = len + 1;
                end
            end
        end
    end

    // ------------------------------------------------------------------ the VGA syncs
    realtime hs_last = 0, hs_period = 0, vs_last = 0, vs_period = 0;
    integer  hs_edges = 0, vs_edges = 0;
    always @(negedge vga_hs) begin
        if (hs_edges > 0) hs_period = $realtime - hs_last;
        hs_last = $realtime;
        hs_edges = hs_edges + 1;
    end
    always @(negedge vga_vs) begin
        if (vs_edges > 0) vs_period = $realtime - vs_last;
        vs_last = $realtime;
        vs_edges = vs_edges + 1;
    end

    task summary;
        begin
            $display("tb_netlist: %0d lines, %0d characters, %0d framing errors, at %0d us of simulated time",
                     nlines, nchars, nframing, $time / 1000000);
            $display("tb_netlist: LEDs %b, RGB LEDs %h", led, rgb);
            if (hs_edges > 1 && vs_edges > 1)
                $display("tb_netlist: VGA hsync every %0.3f us (%0d), vsync every %0.3f ms (%0d)",
                         hs_period / 1.0e6, hs_edges, vs_period / 1.0e9, vs_edges);
            else
                $display("tb_netlist: VGA syncs: %0d hsync and %0d vsync edges, too few to measure both periods",
                         hs_edges, vs_edges);
            if (fd != 0) $fclose(fd);
        end
    endtask

    // ------------------------------------------------------------------ progress and the limit
    integer ms = 0;
    initial begin
        forever begin
            #(TEN_MS);
            ms = ms + 10;
            $display("tb_netlist: %0d ms simulated, %0d lines, %0d characters", ms, nlines, nchars);
            $fflush;
            if (ms >= max_ms) begin
                summary;
                $fatal(1, "tb_netlist: no line beginning with the stop text after %0d ms", max_ms);
            end
        end
    end
endmodule
