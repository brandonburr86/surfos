/*
SurfCPU scratch RAM
--------------------
File: surf_scratch.v Date: 10/15/26 (SurfCPU)
--------------------
The 64 KiB scratch RAM (scratch at 0x0001_0000, docs/MEMORY-MAP.md) as a dual-port block RAM:
port A serves the instruction fetch decoder (reads only), port B the data decoder (reads and
writes with byte enables), so fetches and data accesses never wait for each other
(docs/SURFBUS.md section 6). Both ports answer in the next cycle. Port B is read-first: a
read returns the word before any write in the same cycle, and port A reading the word port B
writes in the same cycle also sees the old word (the block RAM's READ_FIRST mode). Starts all
zero. A write offered on port A gets an error and changes nothing.
*/
module surf_scratch (
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
    // port B: data reads and writes
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

    localparam integer WORDS = SURF_SCRATCH_SIZE / 4;
    localparam integer AW    = $clog2(WORDS);

    reg [31:0] ram [0:WORDS-1];

`ifndef SYNTHESIS
    integer k;
    initial for (k = 0; k < WORDS; k = k + 1) ram[k] = 32'h0;
`endif

    wire [AW-1:0] ai = a_req_addr[AW+1:2];
    wire [AW-1:0] bi = b_req_addr[AW+1:2];

    // port B: read-first, byte-enabled writes
    reg [31:0] b_q;
    always @(posedge clk) begin
        if (b_req_valid) begin
            if (b_req_write) begin
                if (b_req_be[0]) ram[bi][7:0]   <= b_req_wdata[7:0];
                if (b_req_be[1]) ram[bi][15:8]  <= b_req_wdata[15:8];
                if (b_req_be[2]) ram[bi][23:16] <= b_req_wdata[23:16];
                if (b_req_be[3]) ram[bi][31:24] <= b_req_wdata[31:24];
            end
            b_q <= ram[bi];
        end
    end

    // port A: reads only
    reg [31:0] a_q;
    always @(posedge clk) if (a_req_valid) a_q <= ram[ai];

    reg a_v, a_e, b_v;
    always @(posedge clk) begin
        if (rst) begin
            a_v <= 1'b0;
            a_e <= 1'b0;
            b_v <= 1'b0;
        end else begin
            a_v <= a_req_valid;
            a_e <= a_req_valid && a_req_write;
            b_v <= b_req_valid;
        end
    end

    assign a_req_ready = 1'b1;
    assign b_req_ready = 1'b1;
    assign a_rsp_valid = a_v;
    assign a_rsp_err   = a_e;
    assign a_rsp_rdata = a_q;
    assign b_rsp_valid = b_v;
    assign b_rsp_err   = 1'b0;
    assign b_rsp_rdata = b_q;

    // the window decode is the decoder's; scratch RAM allows fetches on either port
    wire unused_ok = &{1'b0, a_req_addr[31:AW+2], a_req_addr[1:0], a_req_be, a_req_wdata,
                       a_req_fetch, b_req_addr[31:AW+2], b_req_addr[1:0], b_req_fetch};
endmodule
