/*
SurfCPU boot ROM
--------------------
File: surf_bootrom.v Date: 10/15/26 (SurfCPU)
--------------------
The 64 KiB boot ROM (bootrom at 0x0000_0000, docs/MEMORY-MAP.md) as a dual-port block RAM
with two read ports: port A serves the instruction fetch decoder, port B the data decoder,
so fetches and loads never wait for each other (docs/SURFBUS.md section 6). Contents come
from INIT_FILE ($readmemh, docs/MODULES.md section 5: 16384 lines of 8 hex digits) and are
all zero without one; in simulation +bootrom=<file> loads another image at time 0. Both
ports answer in the next cycle. Nothing can write the ROM: a write request on either port
gets an error and changes nothing (the D decoder already sends ROM writes to its error
slave; this is the second line of defence). Reads honour the whole word (SURFBUS.md
section 4), whatever the byte enables.
*/
module surf_bootrom #(
    parameter INIT_FILE = ""
) (
    input  wire        clk,
    input  wire        rst,
    // port A: instruction fetches
    input  wire        a_req_valid,
    output wire        a_req_ready,
    input  wire [31:0] a_req_addr,
    input  wire        a_req_write,
    input  wire [3:0]  a_req_be,
    input  wire [31:0] a_req_wdata,
    input  wire        a_req_fetch,
    output wire        a_rsp_valid,
    output wire [31:0] a_rsp_rdata,
    output wire        a_rsp_err,
    // port B: data reads
    input  wire        b_req_valid,
    output wire        b_req_ready,
    input  wire [31:0] b_req_addr,
    input  wire        b_req_write,
    input  wire [3:0]  b_req_be,
    input  wire [31:0] b_req_wdata,
    input  wire        b_req_fetch,
    output wire        b_rsp_valid,
    output wire [31:0] b_rsp_rdata,
    output wire        b_rsp_err
);
    `include "surfcpu_soc.vh"

    localparam integer WORDS = SURF_BOOTROM_SIZE / 4;
    localparam integer AW    = $clog2(WORDS);

    reg [31:0] rom [0:WORDS-1];

`ifndef SYNTHESIS
    reg [8*1024-1:0] plusarg_file;
    integer k;
`endif
    initial begin
`ifndef SYNTHESIS
        for (k = 0; k < WORDS; k = k + 1) rom[k] = 32'h0;
`endif
        if (INIT_FILE != "") $readmemh(INIT_FILE, rom);
`ifndef SYNTHESIS
        if ($value$plusargs("bootrom=%s", plusarg_file)) begin
            $display("surf_bootrom: loading %0s", plusarg_file);
            $readmemh(plusarg_file, rom);
        end
`endif
    end

    // the block RAM's two synchronous read ports
    reg [31:0] a_q, b_q;
    always @(posedge clk) if (a_req_valid) a_q <= rom[a_req_addr[AW+1:2]];
    always @(posedge clk) if (b_req_valid) b_q <= rom[b_req_addr[AW+1:2]];

    reg a_v, a_e, b_v, b_e;
    always @(posedge clk) begin
        if (rst) begin
            a_v <= 1'b0;
            a_e <= 1'b0;
            b_v <= 1'b0;
            b_e <= 1'b0;
        end else begin
            a_v <= a_req_valid;
            a_e <= a_req_valid && a_req_write;
            b_v <= b_req_valid;
            b_e <= b_req_valid && b_req_write;
        end
    end

    assign a_req_ready = 1'b1;
    assign b_req_ready = 1'b1;
    assign a_rsp_valid = a_v;
    assign a_rsp_err   = a_e;
    assign a_rsp_rdata = a_q;
    assign b_rsp_valid = b_v;
    assign b_rsp_err   = b_e;
    assign b_rsp_rdata = b_q;

    // the window decode is the decoder's; reads return the whole word
    wire unused_ok = &{1'b0, a_req_addr[31:AW+2], a_req_addr[1:0], a_req_be, a_req_wdata,
                       a_req_fetch, b_req_addr[31:AW+2], b_req_addr[1:0], b_req_be, b_req_wdata,
                       b_req_fetch};
endmodule
