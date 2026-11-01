/*
SurfCPU video memory
--------------------
File: surf_vram.v Date: 10/18/26 (SurfGPU)
--------------------
VRAM (docs/GPU.md section 2, the vram region of docs/MEMORY-MAP.md): BYTES (307,200) bytes of
true dual-port block RAM in banks of 2**BANK_LOG2 words (surf_vram_bank; the last bank holds what
is left), the word address's top bits choosing the bank. With 16,384-word banks (BANK_LOG2 14, the
default) Yosys builds each bank from sixteen RAMB36 of 16Kx2, as it builds the scratch RAM, and the
last (11,264 words) from twelve: 76 in all, and each port's bank multiplexer has 5 inputs per bit.
(4,096-word banks took 75 RAMB36, but their 19-input multiplexers gathered 672 block RAM outputs
per port in one place, and nextpnr could not route that in reasonable time: docs/GPU.md 12.2.)

Port A (clk) has two users. The CPU's window is a SurfBus slave, always ready: its request
takes the port in the cycle it is offered, and its answer comes in the next cycle; an offset
(s_req_addr[18:0]) at or above BYTES or a fetch is answered with an error and does nothing;
reads return the whole word, writes honour the byte enables. The GPU engine gets the port in
every cycle the CPU does not offer a request (g_gnt = g_req and no CPU request): a granted
read's word comes with g_rvalid in the next cycle; a word address at or above BYTES / 4 reads 0
and writes nothing. So the CPU never waits for the engine and the engine never sees an error.

Port B (pix_clk) is the scan-out's: pix_data is the word at pix_addr two pix_clk edges later
(the banks' read, then the bank multiplexer's register); 0 at or above BYTES / 4. A word read
there in the instant port A writes it may show either value for that one read.
*/
module surf_vram #(
    parameter integer BYTES     = 307200,
    parameter integer BANK_LOG2 = 14
) (
    input  wire        clk,
    input  wire        rst,
    // port A: the CPU's window (SurfBus slave)
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
    // port A: the GPU engine, after the CPU
    input  wire        g_req,
    input  wire        g_we,
    input  wire [3:0]  g_be,
    input  wire [16:0] g_addr,         // word address
    input  wire [31:0] g_wdata,
    output wire        g_gnt,
    output wire        g_rvalid,
    output wire [31:0] g_rdata,
    // port B: the scan-out
    input  wire        pix_clk,
    input  wire [16:0] pix_addr,       // word address
    output wire [31:0] pix_data
);
    localparam integer WORDS      = BYTES / 4;
    localparam integer BANK_WORDS = 1 << BANK_LOG2;
    localparam integer BANKS      = (WORDS + BANK_WORDS - 1) / BANK_WORDS;
    localparam integer LAST_WORDS = WORDS - (BANKS - 1) * BANK_WORDS;
    localparam [31:0]  BYTES_W    = BYTES;
    localparam [31:0]  WORDS_W    = WORDS;

    // ---------------------------------------------------------------- port A: who has it
    wire [18:0] off    = s_req_addr[18:0];
    wire        cpu_ok = !s_req_fetch && ({13'd0, off} < BYTES_W);
    wire        cpu    = s_req_valid;                       // the CPU's request takes the port
    assign g_gnt = g_req && !cpu;
    wire        g_ok   = {15'd0, g_addr} < WORDS_W;

    wire [16:0] a_addr  = cpu ? off[18:2] : g_addr;
    wire        a_wr    = cpu ? (s_req_write && cpu_ok) : (g_gnt && g_we && g_ok);
    wire        a_rd    = cpu ? (!s_req_write && cpu_ok) : (g_gnt && !g_we && g_ok);
    wire [3:0]  a_be    = cpu ? s_req_be : g_be;
    wire [31:0] a_wdata = cpu ? s_req_wdata : g_wdata;
    wire [16-BANK_LOG2:0] a_bank = a_addr[16:BANK_LOG2];

    // ---------------------------------------------------------------- the banks
    wire [32*BANKS-1:0] a_q, b_q;
    genvar b;
    generate
        for (b = 0; b < BANKS; b = b + 1) begin : g_bank
            surf_vram_bank #(.DEPTH(b == BANKS - 1 ? LAST_WORDS : BANK_WORDS), .AW(BANK_LOG2)) u_bank (
                .clk(clk), .a_en((a_wr || a_rd) && a_bank == b), .a_we(a_wr), .a_be(a_be),
                .a_addr(a_addr[BANK_LOG2-1:0]), .a_wdata(a_wdata), .a_q(a_q[32*b +: 32]),
                .pix_clk(pix_clk), .b_addr(pix_addr[BANK_LOG2-1:0]), .b_q(b_q[32*b +: 32]));
        end
    endgenerate

    // ---------------------------------------------------------------- port A: answers
    reg        rd_q      = 1'b0;      // the last cycle read a word (its bank's a_q holds it)
    reg [16-BANK_LOG2:0] rd_bank_q = 0;
    reg        rsp_v_q   = 1'b0, rsp_e_q = 1'b0, g_rv_q = 1'b0;
    always @(posedge clk) begin
        if (rst) begin
            rd_q    <= 1'b0;
            rsp_v_q <= 1'b0;
            rsp_e_q <= 1'b0;
            g_rv_q  <= 1'b0;
        end else begin
            rd_q    <= a_rd;
            rsp_v_q <= cpu;
            rsp_e_q <= cpu && !cpu_ok;
            g_rv_q  <= g_gnt && !g_we;
        end
        if (a_rd) rd_bank_q <= a_bank;
    end
    wire [31:0] a_word = rd_q ? a_q[32*rd_bank_q +: 32] : 32'h0;

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = rsp_v_q;
    assign s_rsp_err   = rsp_e_q;
    assign s_rsp_rdata = a_word;
    assign g_rvalid    = g_rv_q;
    assign g_rdata     = a_word;

    // ---------------------------------------------------------------- port B
    reg [16-BANK_LOG2:0] pb_bank = 0;
    reg        pb_ok   = 1'b0;
    reg [31:0] pix_q   = 32'h0;
    always @(posedge pix_clk) begin
        pb_bank <= pix_addr[16:BANK_LOG2];
        pb_ok   <= {15'd0, pix_addr} < WORDS_W;
        pix_q   <= pb_ok ? b_q[32*pb_bank +: 32] : 32'h0;
    end
    assign pix_data = pix_q;

    wire unused_ok = &{1'b0, s_req_addr[31:19]};
endmodule
