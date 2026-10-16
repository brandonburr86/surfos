/*
SurfCPU DDR model (Verilog)
--------------------
File: surfbus_ddr_model.v Date: 10/15/26 (SurfCPU)
--------------------
Testbench-only SurfBus slave standing in for surf_ddr_bridge in the Icarus SoC testbench (the
C++ sim/models/ddr_model plays the same part in surfsim). Calibration completes +ddr_calib=N
cycles after reset (default 3000; a negative N never completes, and with +ddr_fail
calib_fail rises instead). While idle it raises req_ready on +ddr_ready=PCT percent of cycles
(default 70); an access is answered after +ddr_lat_min..+ddr_lat_max cycles (default 2..12),
from +ddr_seed. An access to the address given by +ddr_stall=<hex> hangs like a stuck
controller: after WDOG cycles it is answered with an error and `timeout` pulses for one cycle,
as the bridge's watchdog does. Memory: two 1 MiB windows, the first and the last MiB of the
DDR region, zero at start, with byte enables; an access elsewhere is reported (the scripts keep
to the windows). A request that arrives before calibration is reported too: the SoC's gate
must keep them away.
*/
`timescale 1ns/1ps
module surfbus_ddr_model #(
    parameter integer WDOG = 300
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        s_req_valid,
    output reg         s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output reg         s_rsp_valid,
    output reg  [31:0] s_rsp_rdata,
    output reg         s_rsp_err,
    output reg         calib_done,
    output reg         calib_fail,
    output reg         timeout
);
    `include "surfcpu_soc.vh"

    localparam integer WIN   = 1 << 18;                       // words in a window (1 MiB)
    localparam [31:0]  HI_LO = SURF_DDR_BASE + SURF_DDR_SIZE - 32'h0010_0000;

    integer calib_cycles = 3000, lat_min = 2, lat_max = 12, ready_pct = 70, seed = 99;
    reg     fail_mode = 1'b0;
    reg     have_stall = 1'b0;
    reg [31:0] stall_addr = 32'h0;
    integer errors = 0, n_reads = 0, n_writes = 0, n_fetches = 0, n_stalls = 0;

    reg [31:0] mem [0:2*WIN-1];
    integer k;
    initial begin
        for (k = 0; k < 2 * WIN; k = k + 1) mem[k] = 32'h0;
        if ($value$plusargs("ddr_calib=%d", calib_cycles)) begin end
        if ($value$plusargs("ddr_lat_min=%d", lat_min)) begin end
        if ($value$plusargs("ddr_lat_max=%d", lat_max)) begin end
        if ($value$plusargs("ddr_ready=%d", ready_pct)) begin end
        if ($value$plusargs("ddr_seed=%d", seed)) begin end
        if ($test$plusargs("ddr_fail")) fail_mode = 1'b1;
        if ($value$plusargs("ddr_stall=%h", stall_addr)) have_stall = 1'b1;
        s_req_ready = 1'b0;
        s_rsp_valid = 1'b0;
        s_rsp_rdata = 32'h0;
        s_rsp_err   = 1'b0;
        calib_done  = 1'b0;
        calib_fail  = 1'b0;
        timeout     = 1'b0;
    end

    function integer index_of;      // word index in mem, or -1 outside the windows
        input [31:0] a;
        begin
            if (a - SURF_DDR_BASE < 32'h0010_0000)  index_of = (a - SURF_DDR_BASE) >> 2;
            else if (a - HI_LO < 32'h0010_0000)    index_of = WIN + ((a - HI_LO) >> 2);
            else                                   index_of = -1;
        end
    endfunction

    integer    cycles = 0;
    reg        busy = 1'b0;
    integer    left = 0;
    reg [31:0] r_data;
    reg        r_err;
    integer    idx, lat;

    always @(posedge clk) begin
        if (rst) begin
            cycles      <= 0;
            calib_done  <= 1'b0;
            calib_fail  <= 1'b0;
            timeout     <= 1'b0;
            s_req_ready <= 1'b0;
            s_rsp_valid <= 1'b0;
            busy        <= 1'b0;
        end else begin
            cycles  <= cycles + 1;
            timeout <= 1'b0;
            if (calib_cycles >= 0 && cycles == calib_cycles) begin
                if (fail_mode) calib_fail <= 1'b1;
                else           calib_done <= 1'b1;
            end
            s_rsp_valid <= 1'b0;
            if (busy) begin
                if (left <= 1) begin
                    s_rsp_valid <= 1'b1;
                    s_rsp_rdata <= r_data;
                    s_rsp_err   <= r_err;
                    if (r_err) timeout <= 1'b1;     // only stalls end in an error here
                    busy        <= 1'b0;
                    s_req_ready <= ({$random(seed)} % 100) < ready_pct;
                end else begin
                    left <= left - 1;
                end
            end else if (s_req_valid && s_req_ready) begin
                if (!calib_done) begin
                    $display("ddr model: ERROR request %h before calibration", s_req_addr);
                    errors = errors + 1;
                end
                idx = index_of(s_req_addr);
                if (idx < 0) begin
                    $display("ddr model: ERROR access %h outside the test windows", s_req_addr);
                    errors = errors + 1;
                end
                r_err  = 1'b0;
                r_data = (idx >= 0) ? mem[idx] : 32'hDEAD_0000;
                if (s_req_write && idx >= 0) begin
                    if (s_req_be[0]) mem[idx][7:0]   = s_req_wdata[7:0];
                    if (s_req_be[1]) mem[idx][15:8]  = s_req_wdata[15:8];
                    if (s_req_be[2]) mem[idx][23:16] = s_req_wdata[23:16];
                    if (s_req_be[3]) mem[idx][31:24] = s_req_wdata[31:24];
                end
                if (s_req_fetch) n_fetches = n_fetches + 1;
                else if (s_req_write) n_writes = n_writes + 1;
                else n_reads = n_reads + 1;
                lat = lat_min + ({$random(seed)} % (lat_max - lat_min + 1));
                if (have_stall && {s_req_addr[31:2], 2'b00} == stall_addr) begin
                    lat = WDOG;
                    r_err = 1'b1;
                    n_stalls = n_stalls + 1;
                    if (s_req_write && idx >= 0) mem[idx] = r_data;   // a stuck write changes nothing
                end
                if (lat <= 1) begin
                    s_rsp_valid <= 1'b1;
                    s_rsp_rdata <= r_data;
                    s_rsp_err   <= r_err;
                    if (r_err) timeout <= 1'b1;
                    s_req_ready <= ({$random(seed)} % 100) < ready_pct;
                end else begin
                    busy        <= 1'b1;
                    left        <= lat - 1;
                    s_req_ready <= 1'b0;
                end
            end else begin
                s_req_ready <= ({$random(seed)} % 100) < ready_pct;
            end
        end
    end
endmodule
