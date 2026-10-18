/*
SurfCPU video memory bank
--------------------
File: surf_vram_bank.v Date: 10/18/26 (SurfGPU)
--------------------
One bank of surf_vram (docs/GPU.md section 2): DEPTH 32-bit words in a dual-clock block RAM,
written exactly like surf_textram. Port A (clk) writes with byte enables and reads only when it
does not write (the block RAM's NO_CHANGE mode: READ_FIRST is not safe with the other port on
another clock, UG473, and Yosys would refuse a block RAM for it); a_q keeps the last word read.
Port B (pix_clk) reads every cycle: b_q is the word at b_addr one pix_clk edge later. A word
read on port B in the instant port A writes it may come out as either value for that one read;
the memory itself is never corrupted. Starts all zero.
*/
module surf_vram_bank #(
    parameter integer DEPTH = 4096
) (
    input  wire        clk,
    input  wire        a_en,       // a read or a write this cycle
    input  wire        a_we,       // with a_en: write the enabled bytes; else read into a_q
    input  wire [3:0]  a_be,
    input  wire [11:0] a_addr,
    input  wire [31:0] a_wdata,
    output wire [31:0] a_q,
    input  wire        pix_clk,
    input  wire [11:0] b_addr,
    output wire [31:0] b_q
);
    reg [31:0] mem [0:DEPTH-1];

`ifndef SYNTHESIS
    integer k;
    initial for (k = 0; k < DEPTH; k = k + 1) mem[k] = 32'h0;
`endif

    reg [31:0] a_r;
    always @(posedge clk) begin
        if (a_en) begin
            if (a_we) begin
                if (a_be[0]) mem[a_addr][7:0]   <= a_wdata[7:0];
                if (a_be[1]) mem[a_addr][15:8]  <= a_wdata[15:8];
                if (a_be[2]) mem[a_addr][23:16] <= a_wdata[23:16];
                if (a_be[3]) mem[a_addr][31:24] <= a_wdata[31:24];
            end else begin
                a_r <= mem[a_addr];
            end
        end
    end
    assign a_q = a_r;

    reg [31:0] b_r;
    always @(posedge pix_clk) b_r <= mem[b_addr];
    assign b_q = b_r;
endmodule
