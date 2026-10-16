/*
SurfCPU core: Icarus smoke testbench
--------------------
File: tb_core_icarus.v Date: 10/15/26 (SurfCPU)
--------------------
Runs a test program on surfcpu_core (built without RVFI) under Icarus Verilog, to show the
RTL simulates in the second simulator MODULES.md names. A plain Verilog SurfBus slave on
each port: 4 MiB of RAM at 0x80000000 from a $readmemh image (one little-endian word per
line), bus errors everywhere else (fetches and data alike), random req_ready and
latencies of 1..3 cycles from +seed. The program ends by writing tohost (+tohost=ADDR in
hex): 1 prints PASS, anything else FAIL; running past +max_cycles prints TIMEOUT.

    vvp tb_core_icarus.vvp +image=prog.hex +tohost=80001000 [+seed=N] [+max_cycles=N]

The Verilator testbench (surfcpu_tb.cpp) is the full one: protocol checks, devices,
traces. This one only runs the programs that need nothing but RAM and bus errors.
*/
`timescale 1ns/1ps
module tb_core_icarus;
    reg clk = 1'b0;
    reg rst = 1'b1;
    always #20 clk = ~clk;                        // 25 MHz

    localparam [31:0] RAM_BASE  = 32'h8000_0000;
    localparam integer RAM_WORDS = 1 << 20;       // 4 MiB

    reg [31:0] ram [0:RAM_WORDS-1];
    reg [1023:0] image;
    reg [31:0] tohost;
    integer seed;
    integer max_cycles;
    integer cycle;
    integer i;

    // ------------------------------------------------------------------ core
    wire        i_req_valid, i_req_write, i_req_fetch;
    wire [31:0] i_req_addr, i_req_wdata;
    wire [3:0]  i_req_be;
    reg         i_req_ready = 1'b0;
    reg         i_rsp_valid = 1'b0, i_rsp_err = 1'b0;
    reg  [31:0] i_rsp_rdata = 32'd0;
    wire        d_req_valid, d_req_write, d_req_fetch;
    wire [31:0] d_req_addr, d_req_wdata;
    wire [3:0]  d_req_be;
    reg         d_req_ready = 1'b0;
    reg         d_rsp_valid = 1'b0, d_rsp_err = 1'b0;
    reg  [31:0] d_rsp_rdata = 32'd0;

    surfcpu_core #(.RESET_PC(32'h8000_0000)) core (
        .clk(clk), .rst(rst),
        .ibus_req_valid(i_req_valid), .ibus_req_ready(i_req_ready), .ibus_req_addr(i_req_addr),
        .ibus_req_write(i_req_write), .ibus_req_be(i_req_be), .ibus_req_wdata(i_req_wdata),
        .ibus_req_fetch(i_req_fetch), .ibus_rsp_valid(i_rsp_valid), .ibus_rsp_rdata(i_rsp_rdata),
        .ibus_rsp_err(i_rsp_err),
        .dbus_req_valid(d_req_valid), .dbus_req_ready(d_req_ready), .dbus_req_addr(d_req_addr),
        .dbus_req_write(d_req_write), .dbus_req_be(d_req_be), .dbus_req_wdata(d_req_wdata),
        .dbus_req_fetch(d_req_fetch), .dbus_rsp_valid(d_rsp_valid), .dbus_rsp_rdata(d_rsp_rdata),
        .dbus_rsp_err(d_rsp_err),
        .irq_external(1'b0), .irq_timer(1'b0), .irq_software(1'b0), .mtime(64'd0)
    );

    function in_ram;
        input [31:0] a;
        begin
            in_ram = (a >= RAM_BASE) && (a - RAM_BASE < RAM_WORDS * 4);
        end
    endfunction

    // ------------------------------------------------------------------ the two slaves
    // One request at a time per port (the core never has more). A request accepted at a
    // clock edge is answered L = 1..3 cycles later: with L = 1 the response is valid in
    // the cycle right after the acceptance (the earliest SurfBus allows).
    reg        i_busy = 1'b0, d_busy = 1'b0;
    integer    i_left, d_left, lat;
    reg [31:0] i_addr;
    reg [31:0] d_addr, d_wdata;
    reg [3:0]  d_be;
    reg        d_write;
    reg [31:0] word;

    task fetch_respond(input [31:0] a);
        begin
            i_rsp_valid <= 1'b1;
            i_rsp_err   <= !in_ram(a);
            i_rsp_rdata <= in_ram(a) ? ram[(a - RAM_BASE) >> 2] : 32'd0;
        end
    endtask

    task data_respond(input [31:0] a, input wr, input [3:0] be, input [31:0] wd);
        begin
            d_rsp_valid <= 1'b1;
            d_rsp_err   <= !in_ram(a);
            d_rsp_rdata <= 32'd0;
            if (in_ram(a)) begin
                word = ram[(a - RAM_BASE) >> 2];
                if (wr) begin
                    if (be[0]) word[7:0]   = wd[7:0];
                    if (be[1]) word[15:8]  = wd[15:8];
                    if (be[2]) word[23:16] = wd[23:16];
                    if (be[3]) word[31:24] = wd[31:24];
                    ram[(a - RAM_BASE) >> 2] = word;
                    if ({a[31:2], 2'b00} == tohost && word != 32'd0) begin
                        if (word == 32'd1) $display("tb_core_icarus: PASS after %0d cycles", cycle);
                        else $display("tb_core_icarus: FAIL (test %0d) after %0d cycles", word >> 1, cycle);
                        $finish;
                    end
                end else begin
                    d_rsp_rdata <= word;
                end
            end
        end
    endtask

    always @(posedge clk) begin
        if (rst) begin
            i_busy <= 1'b0; i_rsp_valid <= 1'b0; i_req_ready <= 1'b0;
            d_busy <= 1'b0; d_rsp_valid <= 1'b0; d_req_ready <= 1'b0;
        end else begin
            // instruction port
            i_rsp_valid <= 1'b0;
            if (i_req_valid && i_req_ready) begin
                lat = 1 + ($unsigned($random(seed)) % 3);
                if (lat == 1) fetch_respond(i_req_addr);
                else begin i_busy <= 1'b1; i_addr <= i_req_addr; i_left = lat - 1; end
            end else if (i_busy) begin
                i_left = i_left - 1;
                if (i_left == 0) begin i_busy <= 1'b0; fetch_respond(i_addr); end
            end
            i_req_ready <= ($random(seed) & 3) != 0;
            // data port
            d_rsp_valid <= 1'b0;
            if (d_req_valid && d_req_ready) begin
                lat = 1 + ($unsigned($random(seed)) % 3);
                if (lat == 1) data_respond(d_req_addr, d_req_write, d_req_be, d_req_wdata);
                else begin
                    d_busy <= 1'b1; d_addr <= d_req_addr; d_wdata <= d_req_wdata;
                    d_be <= d_req_be; d_write <= d_req_write; d_left = lat - 1;
                end
            end else if (d_busy) begin
                d_left = d_left - 1;
                if (d_left == 0) begin d_busy <= 1'b0; data_respond(d_addr, d_write, d_be, d_wdata); end
            end
            d_req_ready <= ($random(seed) & 3) != 0;
        end
    end

    // ------------------------------------------------------------------ run
    initial begin
        for (i = 0; i < RAM_WORDS; i = i + 1) ram[i] = 32'd0;
        if (!$value$plusargs("image=%s", image)) begin
            $display("tb_core_icarus: ERROR: +image=FILE is required"); $finish;
        end
        if (!$value$plusargs("tohost=%h", tohost)) begin
            $display("tb_core_icarus: ERROR: +tohost=ADDR is required"); $finish;
        end
        if (!$value$plusargs("seed=%d", seed)) seed = 1;
        if (!$value$plusargs("max_cycles=%d", max_cycles)) max_cycles = 2000000;
        $readmemh(image, ram);
        cycle = 0;
        repeat (5) @(posedge clk);
        rst = 1'b0;
        while (cycle < max_cycles) begin
            @(posedge clk);
            cycle = cycle + 1;
        end
        $display("tb_core_icarus: TIMEOUT after %0d cycles", cycle);
        $finish;
    end
endmodule
