/*
SurfCPU text RAM
--------------------
File: surf_textram.v Date: 10/15/26 (SurfCPU)
--------------------
The 16 KiB text memory (textram at 0x2000_0000, docs/MEMORY-MAP.md): 4096 32-bit cells in a
dual-clock block RAM. Port A is a SurfBus slave in the clk domain (reads and writes with byte
enables, read-first, answer in the next cycle); port B is the video scan-out in the pix_clk
domain: pix_data is the cell at pix_addr one pix_clk edge later (docs/MODULES.md section 3).
Cells 0..2399 are the 80x30 screen (cell = row * 80 + col), 2400..4095 ordinary RAM. The
memory is the only thing shared by the two clocks, so no other crossing is needed; a cell
read by the video in the same instant the CPU writes it may show either value for that one
read. Text RAM has no fetch: a fetch request gets an error (the I decoder already sends
fetches elsewhere). Starts all zero.
*/
module surf_textram (
    input  wire        clk,
    input  wire        rst,
    input  wire        s_req_valid,
    output wire        s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output wire        s_rsp_valid,
    output wire [31:0] s_rsp_rdata,
    output wire        s_rsp_err,
    input  wire        pix_clk,
    input  wire [11:0] pix_addr,
    output wire [31:0] pix_data
);
    `include "surfcpu_soc.vh"

    localparam integer WORDS = SURF_TEXTRAM_SIZE / 4;
    localparam integer AW    = $clog2(WORDS);

    reg [31:0] mem [0:WORDS-1];

`ifndef SYNTHESIS
    integer k;
    initial for (k = 0; k < WORDS; k = k + 1) mem[k] = 32'h0;
`endif

    wire [AW-1:0] ai = s_req_addr[AW+1:2];
    wire          we = s_req_valid && s_req_write && !s_req_fetch;

    // port A (clk): read-first, byte-enabled writes
    reg [31:0] a_q;
    always @(posedge clk) begin
        if (s_req_valid) begin
            if (we) begin
                if (s_req_be[0]) mem[ai][7:0]   <= s_req_wdata[7:0];
                if (s_req_be[1]) mem[ai][15:8]  <= s_req_wdata[15:8];
                if (s_req_be[2]) mem[ai][23:16] <= s_req_wdata[23:16];
                if (s_req_be[3]) mem[ai][31:24] <= s_req_wdata[31:24];
            end
            a_q <= mem[ai];
        end
    end

    // port B (pix_clk): the video scan-out
    reg [31:0] b_q;
    always @(posedge pix_clk) b_q <= mem[pix_addr];
    assign pix_data = b_q;

    reg v_q, e_q;
    always @(posedge clk) begin
        if (rst) begin
            v_q <= 1'b0;
            e_q <= 1'b0;
        end else begin
            v_q <= s_req_valid;
            e_q <= s_req_valid && s_req_fetch;
        end
    end

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = v_q;
    assign s_rsp_err   = e_q;
    assign s_rsp_rdata = a_q;

    wire unused_ok = &{1'b0, s_req_addr[31:AW+2], s_req_addr[1:0]};
endmodule
