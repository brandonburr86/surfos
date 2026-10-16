/*
SurfCPU surf_interconnect testbench
--------------------
File: tb_interconnect.v Date: 10/16/26 (SurfCPU)
--------------------
Icarus testbench for the interconnect of docs/SURFBUS.md section 6. Two BFMs play the core's
instruction and data ports; every target (boot ROM A/B, scratch A/B, the eight MMIO windows,
text RAM, DDR) is a test slave with random ready stalls and random latency that returns its
own number and the address it saw, and checks write data and which requests may reach it.
A reference decoder written from the region table as address ranges (the RTL uses masks)
predicts the target or the error of every access. Protocol monitors watch all 17 links.
Phases: random accesses from each port alone and from both at once, biased to region
boundaries, holes and permission cases (ROM writes, fetches from MMIO and text RAM, writes on
the fetch port, data-port fetches), with per-target request counts checked; DDR before and
after calibration, calibration lost while a request waits at the bridge (it must stay
offered), errors from the DDR slave itself, and the arbiter: data first when both ask in the
same cycle, an already offered fetch not overtaken. The HAS_DDR parameter builds the variant
without DDR, where the window is unmapped and the DDR port never moves; HAS_DDRC the one whose
ddrc window (the DDR controller's registers, a controller initialized by software) reaches its
target, unmapped otherwise. +seed=N. Prints PASS or stops with $fatal.
*/
`timescale 1ns/1ps
`include "surf_bus_targets.vh"
module tb_interconnect;
    `include "surfcpu_soc.vh"

    parameter integer HAS_DDR = 1;
    parameter integer HAS_DDRC = 0;
    localparam integer KI = `SURF_IT_DDR;     // I targets outside the interconnect
    localparam integer KD = `SURF_DT_DDR;     // D targets outside the interconnect
    localparam integer T_DDR = 98;
    localparam integer T_ERR = 99;
    localparam [3:0]   DDR_ID = 4'hE;

    reg clk = 1'b0;
    always #20 clk = ~clk;
    reg rst = 1'b1;
    reg calib = 1'b1;
    integer seed = 1;
    integer errors = 0;

    // ---------------------------------------------------------------- masters
    wire        i_req_valid, i_req_ready, i_req_write, i_req_fetch, i_rsp_valid, i_rsp_err;
    wire [31:0] i_req_addr, i_req_wdata, i_rsp_rdata;
    wire [3:0]  i_req_be;
    wire        d_req_valid, d_req_ready, d_req_write, d_req_fetch, d_rsp_valid, d_rsp_err;
    wire [31:0] d_req_addr, d_req_wdata, d_rsp_rdata;
    wire [3:0]  d_req_be;

    surfbus_bfm #(.NAME("ibus"), .FETCH(1)) ib (
        .clk(clk), .m_req_valid(i_req_valid), .m_req_ready(i_req_ready), .m_req_addr(i_req_addr),
        .m_req_write(i_req_write), .m_req_be(i_req_be), .m_req_wdata(i_req_wdata),
        .m_req_fetch(i_req_fetch), .m_rsp_valid(i_rsp_valid), .m_rsp_rdata(i_rsp_rdata),
        .m_rsp_err(i_rsp_err));
    surfbus_bfm #(.NAME("dbus"), .FETCH(0)) db (
        .clk(clk), .m_req_valid(d_req_valid), .m_req_ready(d_req_ready), .m_req_addr(d_req_addr),
        .m_req_write(d_req_write), .m_req_be(d_req_be), .m_req_wdata(d_req_wdata),
        .m_req_fetch(d_req_fetch), .m_rsp_valid(d_rsp_valid), .m_rsp_rdata(d_rsp_rdata),
        .m_rsp_err(d_rsp_err));
    surfbus_monitor #(.NAME("imon"), .MAX_LAT(2000)) imon (
        .clk(clk), .rst(rst), .req_valid(i_req_valid), .req_ready(i_req_ready),
        .req_addr(i_req_addr), .req_write(i_req_write), .req_be(i_req_be),
        .req_wdata(i_req_wdata), .req_fetch(i_req_fetch), .rsp_valid(i_rsp_valid),
        .rsp_rdata(i_rsp_rdata), .rsp_err(i_rsp_err));
    surfbus_monitor #(.NAME("dmon"), .MAX_LAT(2000)) dmon (
        .clk(clk), .rst(rst), .req_valid(d_req_valid), .req_ready(d_req_ready),
        .req_addr(d_req_addr), .req_write(d_req_write), .req_be(d_req_be),
        .req_wdata(d_req_wdata), .req_fetch(d_req_fetch), .rsp_valid(d_rsp_valid),
        .rsp_rdata(d_rsp_rdata), .rsp_err(d_rsp_err));

    // ---------------------------------------------------------------- targets
    wire [KI-1:0]    it_req_valid, it_req_ready, it_rsp_valid, it_rsp_err;
    wire [31:0]      it_req_addr, it_req_wdata;
    wire             it_req_write, it_req_fetch;
    wire [3:0]       it_req_be;
    wire [32*KI-1:0] it_rsp_rdata;
    wire [KD-1:0]    dt_req_valid, dt_req_ready, dt_rsp_valid, dt_rsp_err;
    wire [31:0]      dt_req_addr, dt_req_wdata;
    wire             dt_req_write, dt_req_fetch;
    wire [3:0]       dt_req_be;
    wire [32*KD-1:0] dt_rsp_rdata;
    wire             ddr_req_valid, ddr_req_ready, ddr_req_write, ddr_req_fetch;
    wire             ddr_rsp_valid, ddr_rsp_err;
    wire [31:0]      ddr_req_addr, ddr_req_wdata, ddr_rsp_rdata;
    wire [3:0]       ddr_req_be;

    surf_interconnect #(.HAS_DDR(HAS_DDR), .HAS_DDRC(HAS_DDRC)) dut (
        .clk(clk), .rst(rst), .ddr_calib_done(calib),
        .i_req_valid(i_req_valid), .i_req_ready(i_req_ready), .i_req_addr(i_req_addr),
        .i_req_write(i_req_write), .i_req_be(i_req_be), .i_req_wdata(i_req_wdata),
        .i_req_fetch(i_req_fetch), .i_rsp_valid(i_rsp_valid), .i_rsp_rdata(i_rsp_rdata),
        .i_rsp_err(i_rsp_err),
        .d_req_valid(d_req_valid), .d_req_ready(d_req_ready), .d_req_addr(d_req_addr),
        .d_req_write(d_req_write), .d_req_be(d_req_be), .d_req_wdata(d_req_wdata),
        .d_req_fetch(d_req_fetch), .d_rsp_valid(d_rsp_valid), .d_rsp_rdata(d_rsp_rdata),
        .d_rsp_err(d_rsp_err),
        .it_req_valid(it_req_valid), .it_req_ready(it_req_ready), .it_req_addr(it_req_addr),
        .it_req_write(it_req_write), .it_req_be(it_req_be), .it_req_wdata(it_req_wdata),
        .it_req_fetch(it_req_fetch), .it_rsp_valid(it_rsp_valid), .it_rsp_rdata(it_rsp_rdata),
        .it_rsp_err(it_rsp_err),
        .dt_req_valid(dt_req_valid), .dt_req_ready(dt_req_ready), .dt_req_addr(dt_req_addr),
        .dt_req_write(dt_req_write), .dt_req_be(dt_req_be), .dt_req_wdata(dt_req_wdata),
        .dt_req_fetch(dt_req_fetch), .dt_rsp_valid(dt_rsp_valid), .dt_rsp_rdata(dt_rsp_rdata),
        .dt_rsp_err(dt_rsp_err),
        .ddr_req_valid(ddr_req_valid), .ddr_req_ready(ddr_req_ready),
        .ddr_req_addr(ddr_req_addr), .ddr_req_write(ddr_req_write), .ddr_req_be(ddr_req_be),
        .ddr_req_wdata(ddr_req_wdata), .ddr_req_fetch(ddr_req_fetch),
        .ddr_rsp_valid(ddr_rsp_valid), .ddr_rsp_rdata(ddr_rsp_rdata), .ddr_rsp_err(ddr_rsp_err));

    integer slave_errors_i [0:KI-1];
    integer slave_errors_d [0:KD-1];
    integer target_mon_errors_i [0:KI-1];
    integer target_mon_errors_d [0:KD-1];

    genvar g;
    generate
        for (g = 0; g < KI; g = g + 1) begin : g_it
            surfbus_slave_model #(.NAME("it"), .ID(1 + g), .ALLOW_FETCH(1), .ALLOW_WRITE(0)) u (
                .clk(clk), .rst(rst), .s_req_valid(it_req_valid[g]), .s_req_ready(it_req_ready[g]),
                .s_req_addr(it_req_addr), .s_req_write(it_req_write), .s_req_be(it_req_be),
                .s_req_wdata(it_req_wdata), .s_req_fetch(it_req_fetch),
                .s_rsp_valid(it_rsp_valid[g]), .s_rsp_rdata(it_rsp_rdata[32*g +: 32]),
                .s_rsp_err(it_rsp_err[g]));
            surfbus_monitor #(.NAME("it_mon"), .MAX_LAT(50)) m (
                .clk(clk), .rst(rst), .req_valid(it_req_valid[g]), .req_ready(it_req_ready[g]),
                .req_addr(it_req_addr), .req_write(it_req_write), .req_be(it_req_be),
                .req_wdata(it_req_wdata), .req_fetch(it_req_fetch), .rsp_valid(it_rsp_valid[g]),
                .rsp_rdata(it_rsp_rdata[32*g +: 32]), .rsp_err(it_rsp_err[g]));
            always @(posedge clk) begin
                slave_errors_i[g]      = u.errors;
                target_mon_errors_i[g] = m.errors;
            end
        end
        for (g = 0; g < KD; g = g + 1) begin : g_dt
            // boot ROM: no writes; MMIO and text RAM: no fetches
            surfbus_slave_model #(.NAME("dt"), .ID(3 + g),
                                  .ALLOW_FETCH(g == `SURF_DT_ROM || g == `SURF_DT_SCRATCH),
                                  .ALLOW_WRITE(g != `SURF_DT_ROM)) u (
                .clk(clk), .rst(rst), .s_req_valid(dt_req_valid[g]), .s_req_ready(dt_req_ready[g]),
                .s_req_addr(dt_req_addr), .s_req_write(dt_req_write), .s_req_be(dt_req_be),
                .s_req_wdata(dt_req_wdata), .s_req_fetch(dt_req_fetch),
                .s_rsp_valid(dt_rsp_valid[g]), .s_rsp_rdata(dt_rsp_rdata[32*g +: 32]),
                .s_rsp_err(dt_rsp_err[g]));
            surfbus_monitor #(.NAME("dt_mon"), .MAX_LAT(50)) m (
                .clk(clk), .rst(rst), .req_valid(dt_req_valid[g]), .req_ready(dt_req_ready[g]),
                .req_addr(dt_req_addr), .req_write(dt_req_write), .req_be(dt_req_be),
                .req_wdata(dt_req_wdata), .req_fetch(dt_req_fetch), .rsp_valid(dt_rsp_valid[g]),
                .rsp_rdata(dt_rsp_rdata[32*g +: 32]), .rsp_err(dt_rsp_err[g]));
            always @(posedge clk) begin
                slave_errors_d[g]      = u.errors;
                target_mon_errors_d[g] = m.errors;
            end
        end
    endgenerate

    surfbus_slave_model #(.NAME("ddr"), .ID(DDR_ID), .ALLOW_FETCH(1), .ALLOW_WRITE(1)) u_ddr (
        .clk(clk), .rst(rst), .s_req_valid(ddr_req_valid), .s_req_ready(ddr_req_ready),
        .s_req_addr(ddr_req_addr), .s_req_write(ddr_req_write), .s_req_be(ddr_req_be),
        .s_req_wdata(ddr_req_wdata), .s_req_fetch(ddr_req_fetch), .s_rsp_valid(ddr_rsp_valid),
        .s_rsp_rdata(ddr_rsp_rdata), .s_rsp_err(ddr_rsp_err));
    surfbus_monitor #(.NAME("ddr_mon"), .MAX_LAT(500)) ddr_mon (
        .clk(clk), .rst(rst), .req_valid(ddr_req_valid), .req_ready(ddr_req_ready),
        .req_addr(ddr_req_addr), .req_write(ddr_req_write), .req_be(ddr_req_be),
        .req_wdata(ddr_req_wdata), .req_fetch(ddr_req_fetch), .rsp_valid(ddr_rsp_valid),
        .rsp_rdata(ddr_rsp_rdata), .rsp_err(ddr_rsp_err));

    // ---------------------------------------------------------------- counts at the targets
    integer acc_i [0:KI-1];
    integer acc_d [0:KD-1];
    integer acc_ddr_f = 0, acc_ddr_d = 0, ddr_offers_uncal = 0;
    integer exp_i [0:KI-1];
    integer exp_d [0:KD-1];
    integer exp_ddr_f = 0, exp_ddr_d = 0;
    reg     ddr_log_fetch [0:15];        // the first DDR acceptances after ddr_log_reset
    integer ddr_log_n = 0;
    integer j;
    initial begin
        for (j = 0; j < KI; j = j + 1) begin acc_i[j] = 0; exp_i[j] = 0; end
        for (j = 0; j < KD; j = j + 1) begin acc_d[j] = 0; exp_d[j] = 0; end
    end
    always @(posedge clk) begin
        if (!rst) begin
            for (j = 0; j < KI; j = j + 1)
                if (it_req_valid[j] && it_req_ready[j]) acc_i[j] = acc_i[j] + 1;
            for (j = 0; j < KD; j = j + 1)
                if (dt_req_valid[j] && dt_req_ready[j]) acc_d[j] = acc_d[j] + 1;
            if (ddr_req_valid && ddr_req_ready) begin
                if (ddr_req_fetch) acc_ddr_f = acc_ddr_f + 1;
                else               acc_ddr_d = acc_ddr_d + 1;
                if (ddr_log_n < 16) ddr_log_fetch[ddr_log_n] = ddr_req_fetch;
                ddr_log_n = ddr_log_n + 1;
            end
            if (ddr_req_valid && !calib) ddr_offers_uncal = ddr_offers_uncal + 1;
            if (HAS_DDR == 0 && ddr_req_valid !== 1'b0) begin
                $display("ERROR the DDR port moved with HAS_DDR = 0");
                errors = errors + 1;
            end
        end
    end

    // ---------------------------------------------------------------- reference decoder
    function in_rgn;            // address ranges, not masks
        input [31:0] a;
        input [31:0] base;
        input [31:0] size;
        begin
            in_rgn = (a - base) < size;
        end
    endfunction

    function integer ref_i;
        input [31:0] a;
        input        wr;
        begin
            if (wr)                                                  ref_i = T_ERR;
            else if (in_rgn(a, SURF_BOOTROM_BASE, SURF_BOOTROM_SIZE)) ref_i = `SURF_IT_ROM;
            else if (in_rgn(a, SURF_SCRATCH_BASE, SURF_SCRATCH_SIZE)) ref_i = `SURF_IT_SCRATCH;
            else if (in_rgn(a, SURF_DDR_BASE, SURF_DDR_SIZE))         ref_i = HAS_DDR ? T_DDR : T_ERR;
            else                                                     ref_i = T_ERR;
        end
    endfunction

    function integer ref_d;
        input [31:0] a;
        input        wr;
        input        fetch;
        begin
            ref_d = T_ERR;
            if (in_rgn(a, SURF_BOOTROM_BASE, SURF_BOOTROM_SIZE))      ref_d = wr ? T_ERR : `SURF_DT_ROM;
            else if (in_rgn(a, SURF_SCRATCH_BASE, SURF_SCRATCH_SIZE)) ref_d = `SURF_DT_SCRATCH;
            else if (in_rgn(a, SURF_DDR_BASE, SURF_DDR_SIZE))         ref_d = HAS_DDR ? T_DDR : T_ERR;
            else if (fetch)                                          ref_d = T_ERR;
            else if (in_rgn(a, SURF_UART_BASE, SURF_UART_SIZE))       ref_d = `SURF_DT_UART;
            else if (in_rgn(a, SURF_TIMER_BASE, SURF_TIMER_SIZE))     ref_d = `SURF_DT_TIMER;
            else if (in_rgn(a, SURF_INTC_BASE, SURF_INTC_SIZE))       ref_d = `SURF_DT_INTC;
            else if (in_rgn(a, SURF_GPIO_BASE, SURF_GPIO_SIZE))       ref_d = `SURF_DT_GPIO;
            else if (in_rgn(a, SURF_SPI_BASE, SURF_SPI_SIZE))         ref_d = `SURF_DT_SPI;
            else if (in_rgn(a, SURF_PS2_BASE, SURF_PS2_SIZE))         ref_d = `SURF_DT_PS2;
            else if (in_rgn(a, SURF_VIDEO_BASE, SURF_VIDEO_SIZE))     ref_d = `SURF_DT_VIDEO;
            else if (in_rgn(a, SURF_SYSINFO_BASE, SURF_SYSINFO_SIZE)) ref_d = `SURF_DT_SYSINFO;
            else if (in_rgn(a, SURF_DDRC_BASE, SURF_DDRC_SIZE))       ref_d = HAS_DDRC ? `SURF_DT_DDRC : T_ERR;
            else if (in_rgn(a, SURF_TEXTRAM_BASE, SURF_TEXTRAM_SIZE)) ref_d = `SURF_DT_TEXTRAM;
        end
    endfunction

    // ---------------------------------------------------------------- address generator
    reg [31:0] rbase [0:13];
    reg [31:0] rsize [0:13];
    initial begin
        rbase[0]  = SURF_BOOTROM_BASE; rsize[0]  = SURF_BOOTROM_SIZE;
        rbase[1]  = SURF_SCRATCH_BASE; rsize[1]  = SURF_SCRATCH_SIZE;
        rbase[2]  = SURF_UART_BASE;    rsize[2]  = SURF_UART_SIZE;
        rbase[3]  = SURF_TIMER_BASE;   rsize[3]  = SURF_TIMER_SIZE;
        rbase[4]  = SURF_INTC_BASE;    rsize[4]  = SURF_INTC_SIZE;
        rbase[5]  = SURF_GPIO_BASE;    rsize[5]  = SURF_GPIO_SIZE;
        rbase[6]  = SURF_SPI_BASE;     rsize[6]  = SURF_SPI_SIZE;
        rbase[7]  = SURF_PS2_BASE;     rsize[7]  = SURF_PS2_SIZE;
        rbase[8]  = SURF_VIDEO_BASE;   rsize[8]  = SURF_VIDEO_SIZE;
        rbase[9]  = SURF_SYSINFO_BASE; rsize[9]  = SURF_SYSINFO_SIZE;
        rbase[10] = SURF_ETH_BASE;     rsize[10] = SURF_ETH_SIZE;
        rbase[11] = SURF_TEXTRAM_BASE; rsize[11] = SURF_TEXTRAM_SIZE;
        rbase[12] = SURF_DDR_BASE;     rsize[12] = SURF_DDR_SIZE;
        rbase[13] = SURF_DDRC_BASE;    rsize[13] = SURF_DDRC_SIZE;
    end

    function [31:0] rand_addr;
        input integer r1;
        input integer r2;
        input integer r3;
        integer k;
        begin
            k = {r2} % 14;
            case ({r1} % 20)
                0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11:
                            case ({r3} % 4)
                                0: rand_addr = rbase[k];
                                1: rand_addr = rbase[k] + rsize[k] - 4;
                                default: rand_addr = rbase[k] + ({r3} % rsize[k]);
                            endcase
                12, 13, 14: case ({r3} % 4)
                          0: rand_addr = rbase[k] - 4;
                          1: rand_addr = rbase[k] + rsize[k];
                          2: rand_addr = rbase[k] - 1;
                          default: rand_addr = rbase[k] + rsize[k] + 3;
                      endcase
                15, 16: rand_addr = r3;
                17, 18: rand_addr = SURF_UART_BASE + ({r3} & 32'h0001_FFFF);   // MMIO, eth, holes
                default: rand_addr = {r3[0] ? 4'h2 : 4'h0, 12'h000, r3[15:0]} ^ (r3[1] ? 32'h0001_0000 : 0);
            endcase
        end
    endfunction

    // ---------------------------------------------------------------- one access with its check
    function [31:0] exp_rdata;
        input [3:0]  id;
        input [31:0] a;
        input        fetch;
        begin
            exp_rdata = u_ddr.rsp_data_for(id, a, fetch);
        end
    endfunction

    integer n_ok = 0, n_err = 0, n_ddr_err = 0;

    task i_access;
        input [31:0] a0;
        input        wr;
        reg   [31:0] a;
        integer      t;
        reg          exp_err;
        reg   [31:0] exp_data;
        reg          slv_err;
        begin
            a = {a0[31:2], 2'b00};
            t = ref_i(a, wr);
            slv_err = (t == T_DDR) ? u_ddr.inject_err : 1'b0;
            exp_err = (t == T_ERR) || (t == T_DDR && !calib) || slv_err;
            exp_data = (t == T_DDR) ? exp_rdata(DDR_ID, a, 1'b1) : exp_rdata(1 + t, a, 1'b1);
            if (t == T_DDR && calib) exp_ddr_f = exp_ddr_f + 1;
            else if (t != T_ERR && t != T_DDR) exp_i[t] = exp_i[t] + 1;
            ib.xfer(wr, a, 4'hF, wr ? u_ddr.wdata_for(a, 4'hF) : 32'h0);
            if (ib.err !== exp_err) begin
                $display("ERROR ibus %0s %h: err %b, expected %b (target %0d)", wr ? "write" : "fetch",
                         a, ib.err, exp_err, t);
                errors = errors + 1;
            end else if (!exp_err && ib.rdata !== exp_data) begin
                $display("ERROR ibus fetch %h: data %h, expected %h", a, ib.rdata, exp_data);
                errors = errors + 1;
            end
            if (exp_err) n_err = n_err + 1; else n_ok = n_ok + 1;
            if (t == T_DDR && !calib) n_ddr_err = n_ddr_err + 1;
        end
    endtask

    task d_access;
        input [31:0]  a0;
        input         wr;
        input integer size;     // 0 byte, 1 halfword, 2 word
        input         fetch;
        reg   [31:0] a;
        reg   [3:0]  be;
        integer      t;
        reg          exp_err, slv_err;
        reg   [31:0] exp_data;
        begin
            case (size)
                0: begin a = a0; be = 4'b0001 << a0[1:0]; end
                1: begin a = {a0[31:1], 1'b0}; be = a0[1] ? 4'b1100 : 4'b0011; end
                default: begin a = {a0[31:2], 2'b00}; be = 4'b1111; end
            endcase
            t = ref_d(a, wr, fetch);
            slv_err = (t == T_DDR) ? u_ddr.inject_err : 1'b0;
            exp_err = (t == T_ERR) || (t == T_DDR && !calib) || slv_err;
            exp_data = (t == T_DDR) ? exp_rdata(DDR_ID, a, fetch) : exp_rdata(3 + t, a, fetch);
            if (t == T_DDR && calib && fetch)  exp_ddr_f = exp_ddr_f + 1;   // counted by req_fetch
            if (t == T_DDR && calib && !fetch) exp_ddr_d = exp_ddr_d + 1;
            if (t != T_ERR && t != T_DDR) exp_d[t] = exp_d[t] + 1;
            db.fetch_flag = fetch;
            db.xfer(wr, a, be, wr ? u_ddr.wdata_for(a, be) : 32'h0);
            db.fetch_flag = 1'b0;
            if (db.err !== exp_err) begin
                $display("ERROR dbus %0s %h be %b fetch %b: err %b, expected %b (target %0d)",
                         wr ? "write" : "read", a, be, fetch, db.err, exp_err, t);
                errors = errors + 1;
            end else if (!exp_err && !wr && db.rdata !== exp_data) begin
                $display("ERROR dbus read %h: data %h, expected %h", a, db.rdata, exp_data);
                errors = errors + 1;
            end
            if (exp_err) n_err = n_err + 1; else n_ok = n_ok + 1;
            if (t == T_DDR && !calib) n_ddr_err = n_ddr_err + 1;
        end
    endtask

    task i_random;
        input integer count;
        integer k;
        begin
            for (k = 0; k < count; k = k + 1)
                i_access(rand_addr($random(seed), $random(seed), $random(seed)),
                         ({$random(seed)} % 20) == 0);
        end
    endtask

    task d_random;
        input integer count;
        integer k, r;
        begin
            for (k = 0; k < count; k = k + 1) begin
                r = $random(seed);
                d_access(rand_addr($random(seed), $random(seed), $random(seed)), r[0] & r[1],
                         {r[5:2]} % 3, ({r} % 23) == 0);
            end
        end
    endtask

    task set_latencies;         // every slave: random ready percentage and latency range
        input integer lo;
        input integer hi;
        input integer pct;
        begin
            g_it[0].u.lat_min = lo; g_it[0].u.lat_max = hi; g_it[0].u.ready_pct = pct;
            g_it[1].u.lat_min = lo; g_it[1].u.lat_max = hi; g_it[1].u.ready_pct = pct;
            `define SETD(k) g_dt[k].u.lat_min = lo; g_dt[k].u.lat_max = hi; g_dt[k].u.ready_pct = pct;
            `SETD(0) `SETD(1) `SETD(2) `SETD(3) `SETD(4) `SETD(5) `SETD(6) `SETD(7) `SETD(8)
            `SETD(9) `SETD(10)
            u_ddr.lat_min = lo; u_ddr.lat_max = hi + 6; u_ddr.ready_pct = pct;
        end
    endtask

    task check_counts;
        input [8*24-1:0] where;
        integer k;
        begin
            repeat (12) @(posedge clk);
            for (k = 0; k < KI; k = k + 1)
                if (acc_i[k] != exp_i[k]) begin
                    $display("ERROR %0s: I target %0d got %0d requests, expected %0d", where, k,
                             acc_i[k], exp_i[k]);
                    errors = errors + 1;
                end
            for (k = 0; k < KD; k = k + 1)
                if (acc_d[k] != exp_d[k]) begin
                    $display("ERROR %0s: D target %0d got %0d requests, expected %0d", where, k,
                             acc_d[k], exp_d[k]);
                    errors = errors + 1;
                end
            if (acc_ddr_f != exp_ddr_f || acc_ddr_d != exp_ddr_d) begin
                $display("ERROR %0s: DDR got %0d fetches and %0d data requests, expected %0d and %0d",
                         where, acc_ddr_f, acc_ddr_d, exp_ddr_f, exp_ddr_d);
                errors = errors + 1;
            end
            @(negedge clk);
        end
    endtask

    integer n, k, t_wait, sum;
    reg [31:0] a_i, a_d;

    initial begin
        if ($value$plusargs("seed=%d", seed)) $display("seed %0d", seed);
        ib.seed = seed; db.seed = seed + 17;
        repeat (3) @(posedge clk);
        @(negedge clk) rst = 1'b0;

        // ------------------------------------------------ each port alone, then both at once
        set_latencies(1, 1, 100);       // single-cycle slaves, always ready
        d_random(1500);
        i_random(800);
        check_counts("single-cycle");
        set_latencies(1, 6, 60);        // random stalls and latencies
        d_random(3000);
        i_random(2000);
        check_counts("one port");
        fork
            d_random(6000);
            i_random(6000);
        join
        check_counts("both ports");

        // ------------------------------------------------ DDR: before calibration, lost, errors, arbitration
        if (HAS_DDR != 0) begin
            calib = 1'b0;
            for (n = 0; n < 200; n = n + 1) begin
                d_access(SURF_DDR_BASE + ({$random(seed)} % SURF_DDR_SIZE), n[0], n % 3, 1'b0);
                i_access(SURF_DDR_BASE + ({$random(seed)} % SURF_DDR_SIZE), 1'b0);
            end
            fork
                d_random(1000);
                i_random(1000);
            join
            check_counts("before calibration");
            if (ddr_offers_uncal != 0) begin
                $display("ERROR %0d DDR requests offered before calibration", ddr_offers_uncal);
                errors = errors + 1;
            end
            calib = 1'b1;
            fork
                d_random(1500);
                i_random(1500);
            join
            check_counts("after calibration");

            // calibration lost while a fetch waits at the bridge: it stays offered and completes
            u_ddr.force_ready = 0;
            repeat (2) @(negedge clk);       // the slave's ready is 0 before the fetch is offered
            ib.max_gap = 0;
            fork
                i_access(SURF_DDR_BASE + 32'h100, 1'b0);
                begin
                    repeat (5) @(negedge clk);
                    calib = 1'b0;              // the gate already passed it on
                    repeat (20) @(negedge clk);
                    u_ddr.force_ready = -1;
                end
            join
            ib.max_gap = 2;
            if (ib.err !== 1'b0 || ib.wait_acc < 5) begin
                $display("ERROR the waiting DDR fetch failed after calibration was lost (err %b, waited %0d cycles)",
                         ib.err, ib.wait_acc);
                errors = errors + 1;
            end
            ddr_offers_uncal = 0;
            for (n = 0; n < 50; n = n + 1) d_access(SURF_DDR_BASE + 4 * n, 1'b1, 2, 1'b0);
            if (ddr_offers_uncal != 0) begin
                $display("ERROR new DDR requests reached the bridge without calibration");
                errors = errors + 1;
            end
            calib = 1'b1;
            check_counts("calibration lost");

            // errors from the DDR slave itself go back to the right master
            u_ddr.inject_err = 1'b1;
            fork
                begin
                    for (n = 0; n < 100; n = n + 1)
                        d_access(SURF_DDR_BASE + ({$random(seed)} % SURF_DDR_SIZE), n[0], 2, 1'b0);
                end
                begin
                    for (n = 0; n < 100; n = n + 1)
                        i_access(SURF_DDR_BASE + ({$random(seed)} % SURF_DDR_SIZE), 1'b0);
                end
            join
            u_ddr.inject_err = 1'b0;
            check_counts("DDR errors");

            // arbiter: both ask in the same cycle -> data first
            for (n = 0; n < 20; n = n + 1) begin
                u_ddr.force_ready = 0;         // the DDR slave not ready, and the arbiter idle
                repeat (3) @(negedge clk);     // (it regrants after the response cycle)
                ib.max_gap = 0; db.max_gap = 0;
                ddr_log_n = 0;
                fork
                    i_access(SURF_DDR_BASE + 32'h1000 + 4 * n, 1'b0);
                    d_access(SURF_DDR_BASE + 32'h2000 + 4 * n, 1'b0, 2, 1'b0);
                    begin repeat (3 + n % 5) @(negedge clk); u_ddr.force_ready = -1; end
                join
                if (ddr_log_n != 2 || ddr_log_fetch[0] !== 1'b0 || ddr_log_fetch[1] !== 1'b1) begin
                    $display("ERROR arbitration: %0d DDR requests, first fetch=%b", ddr_log_n,
                             ddr_log_fetch[0]);
                    errors = errors + 1;
                end
            end
            // an offered fetch is not overtaken by a later data request
            for (n = 0; n < 20; n = n + 1) begin
                u_ddr.force_ready = 0;         // the DDR slave not ready, and the arbiter idle so
                repeat (3) @(negedge clk);     // the fetch is offered (and locked) at once
                ib.max_gap = 0; db.max_gap = 0;
                ddr_log_n = 0;
                fork
                    i_access(SURF_DDR_BASE + 32'h3000 + 4 * n, 1'b0);
                    begin repeat (1 + n % 4) @(negedge clk); d_access(SURF_DDR_BASE + 32'h4000 + 4 * n, 1'b1, 2, 1'b0); end
                    begin repeat (6 + n % 3) @(negedge clk); u_ddr.force_ready = -1; end
                join
                if (ddr_log_n != 2 || ddr_log_fetch[0] !== 1'b1 || ddr_log_fetch[1] !== 1'b0) begin
                    $display("ERROR lock: %0d DDR requests, first fetch=%b", ddr_log_n, ddr_log_fetch[0]);
                    errors = errors + 1;
                end
            end
            ib.max_gap = 2; db.max_gap = 2;
            check_counts("arbitration");
        end else begin
            calib = 1'b0;                     // makes no difference without DDR
            fork
                d_random(1000);
                i_random(1000);
            join
            check_counts("no DDR, calib 0");
            calib = 1'b1;
        end

        repeat (4) @(posedge clk);
        sum = 0;
        for (k = 0; k < KI; k = k + 1) sum = sum + slave_errors_i[k] + target_mon_errors_i[k];
        for (k = 0; k < KD; k = k + 1) sum = sum + slave_errors_d[k] + target_mon_errors_d[k];
        errors = errors + sum + ib.errors + db.errors + imon.errors + dmon.errors + ddr_mon.errors +
                 u_ddr.errors;
        if (imon.accepts != imon.responses || dmon.accepts != dmon.responses) begin
            $display("ERROR unanswered requests"); errors = errors + 1;
        end
        if (errors) $fatal(1, "tb_interconnect: %0d errors", errors);
        $display("tb_interconnect: PASS (HAS_DDR=%0d HAS_DDRC=%0d, %0d accesses: %0d answered by targets, %0d errors expected and seen, %0d DDR errors before calibration; DDR %0d fetches + %0d data; ddrc %0d; %0d+%0d back-to-back)",
                 HAS_DDR, HAS_DDRC, n_ok + n_err, n_ok, n_err, n_ddr_err, acc_ddr_f, acc_ddr_d, acc_d[`SURF_DT_DDRC],
                 ib.n_b2b, db.n_b2b);
        $finish;
    end
endmodule
