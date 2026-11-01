/*
SurfCPU MIG user-interface BFM (Icarus)
--------------------
File: mig_ui_bfm.v Date: 10/15/26 (SurfCPU)
--------------------
A small Verilog stand-in for the MIG 7-series native user interface, for the Icarus smoke
test of surf_ddr_bridge (the thorough model is the C++ one, mig_ui_model.h). Simulation only.
ui_clk_sync_rst for 16 cycles, init_calib_complete CALIB_CYCLES later; app_rdy and
app_wdf_rdy drop at random; one command at a time (as the bridge issues them), a write
taking the data already given, a read answering 8..23 cycles later; up to 256 beats of
storage, found by address, with a fixed pattern in beats never written. It counts protocol
errors: X on the control outputs after reset, app_en before calibration, a command or write
data withdrawn or changed before it was taken, app_wdf_end apart from app_wdf_wren, an
unaligned or out-of-range address, a write command without its data, a second command while
one is pending, a full store.
*/
`timescale 1ns/1ps
module mig_ui_bfm #(
    parameter integer CALIB_CYCLES = 200,
    parameter integer SEED         = 1
) (
    input  wire         ui_clk,
    output reg          ui_rst              = 1'b1,
    output reg          init_calib_complete = 1'b0,
    input  wire [27:0]  app_addr,
    input  wire [2:0]   app_cmd,
    input  wire         app_en,
    output reg          app_rdy             = 1'b0,
    input  wire [127:0] app_wdf_data,
    input  wire [15:0]  app_wdf_mask,
    input  wire         app_wdf_wren,
    input  wire         app_wdf_end,
    output reg          app_wdf_rdy         = 1'b0,
    output reg  [127:0] app_rd_data         = 128'h0,
    output reg          app_rd_data_valid   = 1'b0,
    output reg          app_rd_data_end     = 1'b0,
    input  wire         stuck               // hold app_rdy low (watchdog tests)
);
    integer seed = SEED;
    integer errors = 0, reads = 0, writes = 0;
    integer cycle = 0, rst_left = 16, cal_left = CALIB_CYCLES;

    // storage: beat index (byte offset / 16) -> 128 bits
    reg  [23:0]  tag  [0:255];
    reg  [127:0] data [0:255];
    integer      used = 0;

    function [31:0] init_word;           // word w of a never-written beat (the testbench knows it)
        input [23:0] beat;
        input integer w;
        init_word = {8'h00, beat} ^ (32'h1111_1111 * (w + 1));
    endfunction

    function integer find;               // slot of a beat, or -1
        input [23:0] beat;
        integer i;
        begin
            find = -1;
            for (i = 0; i < used; i = i + 1)
                if (tag[i] == beat) find = i;
        end
    endfunction

    function [127:0] beat_data;
        input [23:0] beat;
        integer s;
        begin
            s = find(beat);
            if (s >= 0) beat_data = data[s];
            else beat_data = {init_word(beat, 3), init_word(beat, 2), init_word(beat, 1), init_word(beat, 0)};
        end
    endfunction

    task error;
        input [8*64-1:0] what;
        begin
            errors = errors + 1;
            if (errors < 20) $display("mig_ui_bfm: cycle %0d: %0s", cycle, what);
        end
    endtask

    reg         wbuf_valid = 1'b0;
    reg [127:0] wbuf_data  = 128'h0;
    reg [15:0]  wbuf_mask  = 16'h0;
    reg         pending_rd = 1'b0;
    integer     rd_left    = 0;
    reg [23:0]  rd_beat    = 24'h0;
    reg         prev_en = 1'b0, prev_rdy = 1'b0, prev_wren = 1'b0, prev_wrdy = 1'b0;
    reg [27:0]  prev_addr  = 28'h0;
    reg [2:0]   prev_cmd   = 3'h0;
    reg [127:0] prev_wdata = 128'h0;
    reg [15:0]  prev_mask  = 16'h0;

    integer s, k;
    reg [127:0] merged;

    always @(posedge ui_clk) begin
        cycle <= cycle + 1;
        app_rd_data_valid <= 1'b0;
        app_rd_data_end   <= 1'b0;
        app_rd_data       <= {$random(seed), $random(seed), $random(seed), $random(seed)};
        if (ui_rst) begin
            app_rdy     <= 1'b0;
            app_wdf_rdy <= 1'b0;
            if (rst_left > 1) rst_left = rst_left - 1;
            else ui_rst <= 1'b0;
        end else begin
            // ---- checks on the bridge's side
            if ((^{app_en, app_wdf_wren, app_wdf_end}) === 1'bx) error("X on app_en/app_wdf_wren/app_wdf_end");
            if (app_en && !init_calib_complete) error("app_en before init_calib_complete");
            if (prev_en && !prev_rdy && (!app_en || app_addr !== prev_addr || app_cmd !== prev_cmd))
                error("command withdrawn or changed before app_rdy");
            if (prev_wren && !prev_wrdy && (!app_wdf_wren || app_wdf_data !== prev_wdata || app_wdf_mask !== prev_mask))
                error("write data withdrawn or changed before app_wdf_rdy");
            if (app_wdf_end !== app_wdf_wren) error("app_wdf_end differs from app_wdf_wren");

            // ---- transfers at this edge
            if (app_wdf_wren && app_wdf_rdy) begin
                if (wbuf_valid) error("second write beat before its command");
                wbuf_valid = 1'b1;
                wbuf_data  = app_wdf_data;
                wbuf_mask  = app_wdf_mask;
            end
            if (app_en && app_rdy) begin
                if (app_addr[2:0] != 3'b000) error("app_addr not aligned to a burst");
                if (app_addr[27]) error("app_addr rank bit set");
                if (pending_rd) error("second command while a read is pending");
                if (app_cmd == 3'b000) begin
                    if (!wbuf_valid) error("write command before its data");
                    merged = beat_data(app_addr[26:3]);
                    for (k = 0; k < 16; k = k + 1)
                        if (!wbuf_mask[k]) merged[8*k +: 8] = wbuf_data[8*k +: 8];
                    s = find(app_addr[26:3]);
                    if (s < 0) begin
                        if (used == 256) error("store full");
                        else begin s = used; tag[s] = app_addr[26:3]; used = used + 1; end
                    end
                    if (s >= 0) data[s] = merged;
                    wbuf_valid = 1'b0;
                    writes = writes + 1;
                end else if (app_cmd == 3'b001) begin
                    pending_rd = 1'b1;
                    rd_beat    = app_addr[26:3];
                    rd_left    = 8 + ($random(seed) & 15);
                    reads      = reads + 1;
                end else begin
                    error("app_cmd neither read nor write");
                end
            end
            if (pending_rd) begin
                if (rd_left > 0) rd_left = rd_left - 1;
                else begin
                    app_rd_data_valid <= 1'b1;
                    app_rd_data_end   <= 1'b1;
                    app_rd_data       <= beat_data(rd_beat);
                    pending_rd = 1'b0;
                end
            end
            // ---- next outputs
            if (cal_left > 0) cal_left = cal_left - 1;
            else init_calib_complete <= 1'b1;
            app_rdy     <= init_calib_complete && !stuck && !pending_rd && ($random(seed) & 3) != 0;
            app_wdf_rdy <= init_calib_complete && !wbuf_valid && ($random(seed) & 3) != 0;
        end
        prev_en    <= app_en;    prev_rdy  <= app_rdy && !ui_rst;  prev_addr  <= app_addr; prev_cmd  <= app_cmd;
        prev_wren  <= app_wdf_wren; prev_wrdy <= app_wdf_rdy && !ui_rst; prev_wdata <= app_wdf_data; prev_mask <= app_wdf_mask;
    end
endmodule
