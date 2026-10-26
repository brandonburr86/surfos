/*
SurfGPU drawing engine
--------------------
File: surf_gpu.v Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
The gpu block of docs/MEMORY-MAP.md (registers at 0x1000_9000) and docs/GPU.md sections 5 and 6:
software writes command words to CMD, a FIFO of 2**FIFO_LOG2 words (surf_fifo, distributed RAM)
holds them, and the engine runs them in order on VRAM through surf_vram's engine side of port A
(m_*): four 8-bit pixels per 32-bit access, with byte enables, waiting whenever the CPU has the
port (m_gnt = 0). A CMD write into a full FIFO is dropped and sets STATUS.OVERFLOW; the bus is
never held. Registers follow the peripheral rules of docs/SOC.md section 4.

The sequencer takes a header word (bits 31:24 the opcode), then the command's parameter words,
then works out the rectangle or line it draws; a 16-cycle shift-add multiplier (no DSP) makes
the row addresses (y * stride) and SET_TARGET's bound. The walkers:
  * rectangles (FILL_RECT, EXPAND, WRITE_RECT): row by row, one destination word (four pixel
    lanes) per cycle, byte enables for the lanes inside the rectangle and the clip. FILL_RECT
    walks the clipped rectangle; EXPAND and WRITE_RECT the whole one, so that their data words
    are taken exactly (a word with no lane to write is not written). Their data go through a
    64-bit buffer (bits MSB first, or bytes LSB first) that takes a FIFO word only when the next
    destination word needs more than it holds: never past the command's own data.
  * COPY_RECT: the clipped destination rectangle (also limited to pixels whose source is in the
    target), rows bottom-up when dy > sy and right to left when dy = sy and dx > sx, so every
    source word is read before anything overwrites it. A window of two source words (lo, hi)
    and a byte funnel give destination lane L the source byte L + s, s = (sx - dx) mod 4: each
    step reads one new source word and writes one destination word (the write goes out in the
    cycle the word arrives); a lane whose source byte equals the key is masked with KEY.
  * LINE: one pixel per cycle, the algorithm of docs/GPU.md section 5, the row address kept by
    adding or subtracting the stride; pixels outside the clip are stepped over unwritten.
FENCE sets FENCE (and STATUS.FENCE_EV with its IRQ flag) when it is reached: every earlier
command's writes have been granted, so they are in VRAM. WAIT_VBLANK waits for the next vblank
pulse (surf_video's, in this clock domain). CTRL.ABORT empties the FIFO and ends the running
command between two accesses. Opcodes and header bits are firmware/include/surfcpu/gpu.h's
(sim/gpu/check_gpu_h.py compares them); every behaviour is the C model's (sim/models/gpu_model.c).

GPU2 (docs/GPU.md section 16): SET_ZBUF checks the depth buffer against the target with the same
multiplier and keeps it (an accepted SET_TARGET drops it); TRIANGLE's six words are collected
like any parameters, a TRIANGLE that tests or writes depth without a depth buffer is refused,
and any other is handed to surf_gpu_tri, which has VRAM's port until it is done; FLIP waits for
the vblank pulse as WAIT_VBLANK does and then writes its base into surf_video's GFX_BASE through
gfx_base_we / gfx_base_out (bits 18:2, one cycle). gpu.ID reads "GPU2".
*/
module surf_gpu #(
    parameter integer FIFO_LOG2  = 6,
    parameter integer VRAM_BYTES = 307200
) (
    input  wire        clk,
    input  wire        rst,
    // SurfBus slave (SURFBUS.md)
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
    output wire        irq,
    input  wire        vblank,             // one clk pulse as vertical blanking begins
    output wire        gfx_base_we,        // FLIP: write gfx_base_out into video GFX_BASE (one cycle)
    output wire [16:0] gfx_base_out,       // its value, GFX_BASE bits 18:2
    // VRAM's engine side of port A (surf_vram g_*)
    output wire        m_req,
    output wire        m_we,
    output wire [3:0]  m_be,
    output wire [16:0] m_addr,
    output wire [31:0] m_wdata,
    input  wire        m_gnt,
    input  wire        m_rvalid,
    input  wire [31:0] m_rdata
);
`include "surfcpu_soc.vh"

    // ---------------------------------------------------------------- opcodes (gpu.h)
    localparam [7:0] OP_NOP = 8'h00, OP_SET_TARGET = 8'h01, OP_SET_CLIP = 8'h02, OP_FILL_RECT = 8'h03,
                     OP_COPY_RECT = 8'h04, OP_LINE = 8'h05, OP_EXPAND = 8'h06, OP_WRITE_RECT = 8'h07,
                     OP_FENCE = 8'h08, OP_WAIT_VBLANK = 8'h09, OP_SET_ZBUF = 8'h0A, OP_TRIANGLE = 8'h0B,
                     OP_FLIP = 8'h0C;
    localparam integer HDR_COPY_KEY = 8, HDR_EXPAND_TRANSP = 16, HDR_FENCE_IRQ = 0;
    localparam integer HDR_TRI_ZTEST = 9, HDR_TRI_ZWRITE = 11;       // the rest: surf_gpu_tri.v
    localparam [31:0]  ID_VALUE = 32'h3255_5047;                  // "GPU2"
    localparam integer DEPTH    = 1 << FIFO_LOG2;
    localparam [31:0]  VRAM_32  = VRAM_BYTES;
    wire       [31:0]  vram_32  = VRAM_32;
    wire       [35:0]  VRAM_36  = {4'd0, vram_32};              // VRAM's size, for the 36-bit sums
    localparam [31:0]  INFO_VALUE = ((VRAM_BYTES / 1024) << SURF_GPU_INFO_VRAM_KIB_LSB) | DEPTH;

    // ---------------------------------------------------------------- bus decode
    wire [11:0] off      = s_req_addr[11:0];
    wire        hit_id   = (off == SURF_GPU_ID[11:0]);
    wire        hit_stat = (off == SURF_GPU_STATUS[11:0]);
    wire        hit_ie   = (off == SURF_GPU_IE[11:0]);
    wire        hit_ctrl = (off == SURF_GPU_CTRL[11:0]);
    wire        hit_cmd  = (off == SURF_GPU_CMD[11:0]);
    wire        hit_free = (off == SURF_GPU_FIFO_FREE[11:0]);
    wire        hit_fen  = (off == SURF_GPU_FENCE[11:0]);
    wire        hit_info = (off == SURF_GPU_INFO[11:0]);
    wire        known    = hit_id | hit_stat | hit_ie | hit_ctrl | hit_cmd | hit_free | hit_fen | hit_info;
    wire        legal    = known && (s_req_be == 4'b1111) && !s_req_fetch;
    wire        accept   = s_req_valid;
    wire        wr       = accept && legal && s_req_write;
    wire        abort    = wr && hit_ctrl && s_req_wdata[SURF_GPU_CTRL_ABORT_LSB];
    assign s_req_ready = 1'b1;

    // ---------------------------------------------------------------- engine state
    localparam [3:0] S_IDLE = 4'd0, S_PARAM = 4'd1, S_SETUP = 4'd2, S_MUL = 4'd3, S_TARGET = 4'd4,
                     S_RECT = 4'd5, S_COPY_GO = 4'd6, S_COPY_RD = 4'd7, S_COPY_WT = 4'd8, S_COPY_WR = 4'd9,
                     S_LINE = 4'd10, S_VBL = 4'd11, S_ZBUF = 4'd12, S_TRI = 4'd13, S_FLIP = 4'd14;
    reg [3:0]  st = S_IDLE;
    reg [31:0] hdr = 32'd0;
    reg [31:0] p0 = 32'd0, p1 = 32'd0, p2 = 32'd0, p3 = 32'd0, p4 = 32'd0, p5 = 32'd0;
    reg [2:0]  pn = 3'd0, need = 3'd0;                            // parameter words held, needed
    wire [7:0] op = hdr[31:24];

    // ---------------------------------------------------------------- registers, FIFO
    reg        ovf_q = 1'b0, badop_q = 1'b0, badpar_q = 1'b0, fev_q = 1'b0;
    reg [1:0]  ie_q = 2'd0;
    reg [31:0] fence_q = 32'd0;

    wire [31:0]        fifo_q;
    wire               fifo_empty, fifo_full;
    wire [FIFO_LOG2:0] fifo_count;
    wire               fifo_push = wr && hit_cmd && !fifo_full;
    wire               pop;                                       // the engine takes fifo_q this cycle
    surf_fifo #(.WIDTH(32), .DEPTH_LOG2(FIFO_LOG2)) u_fifo (
        .clk(clk), .rst(rst || abort), .wr_en(fifo_push), .wr_data(s_req_wdata),
        .rd_en(pop), .rd_data(fifo_q), .empty(fifo_empty), .full(fifo_full), .count(fifo_count));
    wire [15:0] fifo_free = DEPTH[15:0] - {{(15 - FIFO_LOG2){1'b0}}, fifo_count};

    // ---------------------------------------------------------------- target, clip, depth buffer
    reg [18:0] t_base = 19'd0;
    reg [15:0] t_stride = 16'd640;
    reg [14:0] t_w = 15'd640, t_h = 15'd480;
    reg [18:0] z_base = 19'd0;
    reg [15:0] z_stride = 16'd0;
    reg        z_valid = 1'b0;                                    // SET_ZBUF taken for this target
    reg        flip_we = 1'b0;                                    // FLIP's write to video GFX_BASE
    reg [16:0] flip_base = 17'd0;
    assign gfx_base_we  = flip_we;
    assign gfx_base_out = flip_base;
    reg [15:0] c_x0 = 16'd0, c_y0 = 16'd0, c_x1 = 16'd640, c_y1 = 16'd480;
    // the effective clip as 18-bit signed values: [ecx0, ecx1) x [ecy0, ecy1)
    wire [15:0] cx1m = (c_x1 < {1'b0, t_w}) ? c_x1 : {1'b0, t_w};
    wire [15:0] cy1m = (c_y1 < {1'b0, t_h}) ? c_y1 : {1'b0, t_h};
    wire signed [17:0] ecx0 = {2'b00, c_x0}, ecx1 = {2'b00, cx1m};
    wire signed [17:0] ecy0 = {2'b00, c_y0}, ecy1 = {2'b00, cy1m};

    function signed [17:0] smax(input signed [17:0] a, input signed [17:0] b); smax = (a > b) ? a : b; endfunction
    function signed [17:0] smin(input signed [17:0] a, input signed [17:0] b); smin = (a < b) ? a : b; endfunction

    // the parameters: coordinates signed 16-bit, sizes unsigned 16-bit, both as 18-bit signed
    wire signed [17:0] ax = {{2{p0[15]}}, p0[15:0]}, ay = {{2{p0[31]}}, p0[31:16]};   // x, y (or sx, sy)
    wire signed [17:0] bx = {{2{p1[15]}}, p1[15:0]}, by = {{2{p1[31]}}, p1[31:16]};   // dx, dy; x1, y1
    wire signed [17:0] aw = {2'b00, p1[15:0]}, ah = {2'b00, p1[31:16]};               // FILL/EXPAND/WRITE size
    wire signed [17:0] cw = {2'b00, p2[15:0]}, ch = {2'b00, p2[31:16]};               // COPY size
    wire signed [17:0] tw_s = {3'd0, t_w}, th_s = {3'd0, t_h};

    // FILL_RECT: the clipped rectangle
    wire signed [17:0] f_x0 = smax(ax, ecx0), f_x1 = smin(ax + aw, ecx1);
    wire signed [17:0] f_y0 = smax(ay, ecy0), f_y1 = smin(ay + ah, ecy1);
    // EXPAND, WRITE_RECT: the whole rectangle
    wire signed [17:0] e_x1 = ax + aw, e_y1 = ay + ah;
    // COPY_RECT: the destination, clipped and limited to sources in the target
    wire signed [17:0] k_ddx = bx - ax, k_ddy = by - ay;                             // destination - source
    wire signed [17:0] k_x0 = smax(smax(bx, ecx0), k_ddx), k_x1 = smin(smin(bx + cw, ecx1), k_ddx + tw_s);
    wire signed [17:0] k_y0 = smax(smax(by, ecy0), k_ddy), k_y1 = smin(smin(by + ch, ecy1), k_ddy + th_s);
    wire               k_up = by > ay;                                               // rows bottom-up
    wire               k_rl = (by == ay) && (bx > ax);                               // right to left
    wire signed [17:0] k_ys = k_up ? k_y1 - 18'sd1 : k_y0;                           // the first row
    wire signed [17:0] k_ye = k_up ? k_y0 : k_y1 - 18'sd1;                           // the last row
    wire signed [17:0] k_sdy = ay - by, k_sdx = ax - bx;                             // source - destination
    // LINE
    wire signed [17:0] l_dxa = (bx > ax) ? bx - ax : ax - bx;
    wire signed [17:0] l_dya = (by > ay) ? by - ay : ay - by;
    // group (word) indexes of the x ranges: floor(x / 4)
    wire signed [17:0] f_x1m = f_x1 - 18'sd1, e_x1m = e_x1 - 18'sd1, k_x1m = k_x1 - 18'sd1;

    // ---------------------------------------------------------------- the multiplier: a * b, 16 cycles
    reg signed [35:0] mul_acc = 36'sd0, mul_a = 36'sd0;
    reg        [15:0] mul_b = 16'd0;
    reg        [4:0]  mul_n = 5'd0;
    reg               mul_two = 1'b0;                              // COPY: the second product follows

    // ---------------------------------------------------------------- rectangle walk
    reg signed [17:0] rx0 = 18'sd0, rx1 = 18'sd0;                // the x range walked [rx0, rx1)
    reg signed [17:0] cy = 18'sd0, cy_last = 18'sd0;             // the current row, the last row
    reg               ydn = 1'b0;                                 // rows go up the screen (cy decreasing)
    reg               xdn = 1'b0;                                 // groups go right to left (COPY)
    reg signed [15:0] cg = 16'sd0, g_first = 16'sd0, g_last = 16'sd0;
    reg signed [35:0] row = 36'sd0;                               // byte address of (0, cy)
    reg signed [35:0] src_off = 36'sd0;                           // COPY: source byte - destination byte
    reg [63:0]        sbuf = 64'd0;                               // EXPAND bits (MSB first) / WRITE_RECT bytes
    reg [6:0]         scnt = 7'd0;                                // bits or bytes in sbuf
    reg [31:0]        lo = 32'd0, hi = 32'd0;                     // COPY: the source window
    reg               cw_hi = 1'b0;                               // COPY: the word read goes to hi
    reg               cprime = 1'b0;                              // COPY: the row's first group, first word

    // the four lanes of group cg: inside the rectangle walked, inside the clip
    wire signed [17:0] gx = {cg, 2'b00};
    wire               row_in = (cy >= ecy0) && (cy < ecy1);
    wire [3:0]         in_rect, in_clip;
    wire [2:0]  k      = {2'b00, in_rect[0]} + {2'b00, in_rect[1]} + {2'b00, in_rect[2]} + {2'b00, in_rect[3]};
    wire [1:0]  l_lo   = in_rect[0] ? 2'd0 : in_rect[1] ? 2'd1 : in_rect[2] ? 2'd2 : 2'd3;
    wire [16:0] g_word = row[18:2] + {cg[15], cg};                // the group's word (when in VRAM)

    // EXPAND and WRITE_RECT: lane L takes stream item L - l_lo (the rectangle's lanes in a group
    // are contiguous from l_lo); FILL_RECT the colour
    wire        is_exp = (op == OP_EXPAND);
    wire        is_wrr = (op == OP_WRITE_RECT);
    wire        have   = !(is_exp || is_wrr) || (scnt >= {4'd0, k});
    wire [31:0] lane_data;
    wire [3:0]  lane_mask;
    genvar gl;
    generate
        for (gl = 0; gl < 4; gl = gl + 1) begin : g_lane
            localparam [17:0] OFF = gl;
            localparam [1:0]  LN  = gl;
            wire signed [17:0] xl = gx + $signed(OFF);
            assign in_rect[gl] = (xl >= rx0) && (xl < rx1);
            assign in_clip[gl] = (xl >= ecx0) && (xl < ecx1) && row_in;
            wire [1:0] d   = LN - l_lo;                              // its stream item (when LN >= l_lo)
            wire       px  = sbuf[6'd63 - {4'd0, d}];                // EXPAND: its bit, MSB first
            assign lane_data[8 * gl +: 8] = is_exp ? (px ? hdr[7:0] : hdr[15:8]) :
                                            is_wrr ? sbuf[{1'b0, d, 3'b000} +: 8] : hdr[7:0];
            assign lane_mask[gl] = in_rect[gl] && in_clip[gl] && !(is_exp && !px && hdr[HDR_EXPAND_TRANSP]);
        end
    endgenerate

    // COPY: the window this cycle (with the word arriving), the funnel, the key
    wire        two    = (src_off[1:0] != 2'd0);                  // a group needs two source words
    wire [31:0] lo_n   = (m_rvalid && !cw_hi) ? m_rdata : lo;
    wire [31:0] hi_n   = (m_rvalid &&  cw_hi) ? m_rdata : hi;
    wire [63:0] win    = {hi_n, lo_n} >> {src_off[1:0], 3'b000};
    reg  [3:0]  copy_mask;
    integer     lk;
    always @(*) begin
        for (lk = 0; lk < 4; lk = lk + 1)
            copy_mask[lk] = in_rect[lk] && !(hdr[HDR_COPY_KEY] && win[8 * lk +: 8] == hdr[7:0]);
    end
    wire signed [35:0] src_row = row + src_off;
    wire [16:0] src_w = src_row[18:2] + {cg[15], cg};             // the group's first source word
    wire        copy_more = cprime && two;                        // the arriving word is the first of two

    // ---------------------------------------------------------------- LINE
    reg signed [17:0] lx = 18'sd0, ly = 18'sd0, lx1 = 18'sd0, ly1 = 18'sd0;
    reg signed [19:0] lerr = 20'sd0, ldx = 20'sd0, ldy = 20'sd0;
    reg               lsx = 1'b0, lsy = 1'b0;                     // 1: that step is -1
    wire signed [19:0] le2   = lerr <<< 1;
    wire               l_in  = (lx >= ecx0) && (lx < ecx1) && (ly >= ecy0) && (ly < ecy1);
    wire signed [35:0] l_byte = row + {{18{lx[17]}}, lx};
    wire               l_end = (lx == lx1) && (ly == ly1);
    wire               l_mvx = le2 >= ldy;
    wire               l_mvy = le2 <= ldx;

    // ---------------------------------------------------------------- the request to VRAM
    reg        q_req, q_we;
    reg [3:0]  q_be;
    reg [16:0] q_addr;
    reg [31:0] q_wdata;
    always @(*) begin
        q_req = 1'b0; q_we = 1'b1; q_be = 4'h0; q_addr = 17'd0; q_wdata = 32'd0;
        case (st)
            S_RECT: if (have && lane_mask != 4'd0) begin
                q_req = 1'b1; q_be = lane_mask; q_addr = g_word; q_wdata = lane_data;
            end
            S_COPY_RD: begin
                q_req = 1'b1; q_we = 1'b0; q_be = 4'hF; q_addr = src_w + {16'd0, cw_hi};
            end
            S_COPY_WT, S_COPY_WR: if (!copy_more && copy_mask != 4'd0) begin
                q_req = 1'b1; q_be = copy_mask; q_addr = g_word; q_wdata = win[31:0];
            end
            S_LINE: if (l_in) begin
                q_req = 1'b1; q_be = 4'b0001 << l_byte[1:0]; q_addr = l_byte[18:2]; q_wdata = {4{hdr[7:0]}};
            end
            default: ;
        endcase
    end

    // ---------------------------------------------------------------- the header's command
    // (comparisons, not a case statement: Yosys would make a case of constants a ROM, and the
    // board's netlist check wants every memory of the RTL in RAM cells)
    wire [7:0] h_op = fifo_q[31:24];
    wire       h_p0 = (h_op == OP_NOP) || (h_op == OP_WAIT_VBLANK);
    wire       h_p1 = (h_op == OP_FENCE) || (h_op == OP_FLIP);
    wire       h_p2 = (h_op == OP_SET_CLIP) || (h_op == OP_FILL_RECT) || (h_op == OP_LINE) ||
                      (h_op == OP_EXPAND) || (h_op == OP_WRITE_RECT) || (h_op == OP_SET_ZBUF);
    wire       h_p3 = (h_op == OP_SET_TARGET) || (h_op == OP_COPY_RECT);
    wire       h_p6 = (h_op == OP_TRIANGLE);
    wire       h_known = h_p0 || h_p1 || h_p2 || h_p3 || h_p6;
    wire [2:0] h_params = h_p6 ? 3'd6 : h_p3 ? 3'd3 : h_p2 ? 3'd2 : h_p1 ? 3'd1 : 3'd0;

    // the FIFO word is taken: a header, a parameter, or a data word EXPAND or WRITE_RECT needs
    assign pop = !rst && !abort && !fifo_empty &&
                 (st == S_IDLE || st == S_PARAM || (st == S_RECT && !have));

    // the end of a group: last group of the row, last row
    wire last_g   = (cg == (xdn ? g_first : g_last));
    wire last_row = (cy == cy_last);
    wire signed [35:0] stride36 = {20'd0, t_stride};

    // SET_TARGET's checks (with (h - 1) * stride in mul_acc)
    wire [15:0] tg_w = p2[15:0], tg_h = p2[31:16];
    wire        tg_bad = (p0[1:0] != 2'd0) || (p1[1:0] != 2'd0) || ({4'd0, p0} >= VRAM_36) ||
                         (p1[31:16] != 16'd0) || tg_w[15] || tg_h[15] ||
                         (tg_w != 16'd0 && tg_h != 16'd0 &&
                          (!(tg_w <= p1[15:0] || tg_h == 16'd1) ||
                           ({4'd0, p0} + mul_acc + {20'd0, tg_w} > VRAM_36)));
    // SET_ZBUF's: the target's pixels at 2 bytes each (with (H - 1) * stride in mul_acc)
    wire [16:0] tw2 = {1'b0, t_w, 1'b0};
    wire        zb_bad = (p0[1:0] != 2'd0) || (p1[1:0] != 2'd0) || ({4'd0, p0} >= VRAM_36) ||
                         (p1[31:16] != 16'd0) ||
                         (t_w != 15'd0 && t_h != 15'd0 &&
                          (!(tw2 <= {1'b0, p1[15:0]} || t_h == 15'd1) ||
                           ({4'd0, p0} + mul_acc + {19'd0, tw2} > VRAM_36)));

    // TRIANGLE: surf_gpu_tri walks it on the port (m_* below while st is S_TRI)
    wire        tri_depth = hdr[HDR_TRI_ZTEST] || hdr[HDR_TRI_ZWRITE];
    wire        tri_start = (st == S_SETUP) && (op == OP_TRIANGLE) && !abort && !(tri_depth && !z_valid);
    wire        tri_busy;
    wire        t_req, t_we;
    wire [3:0]  t_be;
    wire [16:0] t_addr;
    wire [31:0] t_wdata;
    surf_gpu_tri u_tri (
        .clk(clk), .rst(rst), .stop(abort), .start(tri_start), .flags(hdr[13:0]),
        .vtx({p5, p4, p3, p2, p1, p0}), .t_base(t_base), .t_stride(t_stride), .z_base(z_base),
        .z_stride(z_stride), .ecx0(ecx0), .ecx1(ecx1), .ecy0(ecy0), .ecy1(ecy1), .busy(tri_busy),
        .m_req(t_req), .m_we(t_we), .m_be(t_be), .m_addr(t_addr), .m_wdata(t_wdata),
        .m_gnt(m_gnt), .m_rvalid(m_rvalid), .m_rdata(m_rdata));

    // VRAM's port: surf_gpu_tri's while a TRIANGLE runs, else the walkers' above
    wire   tri_port = (st == S_TRI);
    assign m_req   = tri_port ? t_req   : q_req;
    assign m_we    = tri_port ? t_we    : q_we;
    assign m_be    = tri_port ? t_be    : q_be;
    assign m_addr  = tri_port ? t_addr  : q_addr;
    assign m_wdata = tri_port ? t_wdata : q_wdata;

    // ---------------------------------------------------------------- the engine
    always @(posedge clk) begin
        if (rst) begin
            st <= S_IDLE;
            pn <= 3'd0;
            scnt <= 7'd0;
            t_base <= 19'd0; t_stride <= 16'd640; t_w <= 15'd640; t_h <= 15'd480;
            z_valid <= 1'b0;
            flip_we <= 1'b0;
            c_x0 <= 16'd0; c_y0 <= 16'd0; c_x1 <= 16'd640; c_y1 <= 16'd480;
            fence_q <= 32'd0;
            ovf_q <= 1'b0; badop_q <= 1'b0; badpar_q <= 1'b0; fev_q <= 1'b0;
            ie_q <= 2'd0;
        end else begin
            // the bus side first, so that an event below wins over its clear in the same cycle
            if (wr && hit_ie) ie_q <= s_req_wdata[1:0];
            if (wr && hit_stat) begin
                if (s_req_wdata[SURF_GPU_STATUS_OVERFLOW_LSB])   ovf_q    <= 1'b0;
                if (s_req_wdata[SURF_GPU_STATUS_BAD_OPCODE_LSB]) badop_q  <= 1'b0;
                if (s_req_wdata[SURF_GPU_STATUS_BAD_PARAM_LSB])  badpar_q <= 1'b0;
                if (s_req_wdata[SURF_GPU_STATUS_FENCE_EV_LSB])   fev_q    <= 1'b0;
            end
            if (wr && hit_cmd && fifo_full) ovf_q <= 1'b1;
            flip_we <= 1'b0;

            if (abort) begin
                st <= S_IDLE;
                scnt <= 7'd0;
            end else begin
                case (st)
                    S_IDLE: if (!fifo_empty) begin
                        hdr  <= fifo_q;
                        pn   <= 3'd0;
                        need <= h_params;
                        if (!h_known)
                            badop_q <= 1'b1;
                        else if (h_params != 3'd0)
                            st <= S_PARAM;
                        else if (h_op == OP_WAIT_VBLANK)
                            st <= S_VBL;
                    end
                    S_PARAM: if (!fifo_empty) begin
                        if (pn == 3'd0) p0 <= fifo_q;
                        if (pn == 3'd1) p1 <= fifo_q;
                        if (pn == 3'd2) p2 <= fifo_q;
                        if (pn == 3'd3) p3 <= fifo_q;
                        if (pn == 3'd4) p4 <= fifo_q;
                        if (pn == 3'd5) p5 <= fifo_q;
                        pn <= pn + 3'd1;
                        if (pn + 3'd1 == need) st <= S_SETUP;
                    end
                    S_SETUP: begin
                        st <= S_IDLE;
                        mul_two <= 1'b0;
                        mul_acc <= 36'sd0;
                        mul_n <= 5'd0;
                        mul_b <= t_stride;
                        case (op)
                            OP_SET_TARGET: begin
                                mul_a <= (tg_h == 16'd0) ? 36'sd0 : {20'd0, tg_h - 16'd1};
                                mul_b <= p1[15:0];
                                st <= S_MUL;
                            end
                            OP_SET_CLIP: begin
                                c_x0 <= p0[15:0]; c_y0 <= p0[31:16]; c_x1 <= p1[15:0]; c_y1 <= p1[31:16];
                            end
                            OP_SET_ZBUF: begin                            // (H - 1) * stride, then S_ZBUF
                                mul_a <= (t_h == 15'd0) ? 36'sd0 : {21'd0, t_h - 15'd1};
                                mul_b <= p1[15:0];
                                st <= S_MUL;
                            end
                            OP_TRIANGLE: begin
                                if (tri_depth && !z_valid)
                                    badpar_q <= 1'b1;                     // depth without a depth buffer
                                else
                                    st <= S_TRI;                          // tri_start this cycle
                            end
                            OP_FLIP: st <= S_FLIP;
                            OP_FILL_RECT: if (f_x0 < f_x1 && f_y0 < f_y1) begin
                                rx0 <= f_x0; rx1 <= f_x1; cy <= f_y0; cy_last <= f_y1 - 18'sd1;
                                ydn <= 1'b0; xdn <= 1'b0;
                                g_first <= f_x0[17:2]; g_last <= f_x1m[17:2]; cg <= f_x0[17:2];
                                mul_a <= {{18{f_y0[17]}}, f_y0};
                                st <= S_MUL;
                            end
                            OP_EXPAND, OP_WRITE_RECT: if (aw != 18'sd0 && ah != 18'sd0) begin
                                rx0 <= ax; rx1 <= e_x1; cy <= ay; cy_last <= e_y1 - 18'sd1;
                                ydn <= 1'b0; xdn <= 1'b0;
                                g_first <= ax[17:2]; g_last <= e_x1m[17:2]; cg <= ax[17:2];
                                sbuf <= 64'd0;
                                scnt <= 7'd0;
                                mul_a <= {{18{ay[17]}}, ay};
                                st <= S_MUL;
                            end
                            OP_COPY_RECT: if (k_x0 < k_x1 && k_y0 < k_y1) begin
                                rx0 <= k_x0; rx1 <= k_x1; cy <= k_ys; cy_last <= k_ye;
                                ydn <= k_up; xdn <= k_rl;
                                g_first <= k_x0[17:2]; g_last <= k_x1m[17:2];
                                cg <= k_rl ? k_x1m[17:2] : k_x0[17:2];
                                mul_a <= {{18{k_ys[17]}}, k_ys};
                                mul_two <= 1'b1;
                                st <= S_MUL;
                            end
                            OP_LINE: begin
                                lx <= ax; ly <= ay; lx1 <= bx; ly1 <= by;
                                ldx <= {2'b00, l_dxa};
                                ldy <= -{2'b00, l_dya};
                                lerr <= {2'b00, l_dxa} - {2'b00, l_dya};
                                lsx <= !(ax < bx); lsy <= !(ay < by);
                                mul_a <= {{18{ay[17]}}, ay};
                                st <= S_MUL;
                            end
                            OP_FENCE: begin
                                fence_q <= p0;
                                if (hdr[HDR_FENCE_IRQ]) fev_q <= 1'b1;
                            end
                            default: ;
                        endcase
                    end
                    S_MUL: begin
                        if (mul_n != 5'd16) begin
                            if (mul_b[0]) mul_acc <= mul_acc + mul_a;
                            mul_a <= mul_a <<< 1;
                            mul_b <= mul_b >> 1;
                            mul_n <= mul_n + 5'd1;
                        end else if (op == OP_SET_TARGET) begin
                            st <= S_TARGET;
                        end else if (op == OP_SET_ZBUF) begin
                            st <= S_ZBUF;
                        end else if (mul_two) begin                       // COPY: the first row's address,
                            row <= mul_acc + {17'd0, t_base};            // then (sy - dy) * stride
                            mul_two <= 1'b0;
                            mul_a <= {{18{k_sdy[17]}}, k_sdy};
                            mul_b <= t_stride;
                            mul_acc <= 36'sd0;
                            mul_n <= 5'd0;
                        end else if (op == OP_COPY_RECT) begin
                            src_off <= mul_acc + {{18{k_sdx[17]}}, k_sdx};
                            st <= S_COPY_GO;
                        end else begin
                            row <= mul_acc + {17'd0, t_base};
                            st <= (op == OP_LINE) ? S_LINE : S_RECT;
                        end
                    end
                    S_TARGET: begin
                        st <= S_IDLE;
                        if (tg_bad)
                            badpar_q <= 1'b1;
                        else begin
                            t_base <= p0[18:0]; t_stride <= p1[15:0]; t_w <= tg_w[14:0]; t_h <= tg_h[14:0];
                            c_x0 <= 16'd0; c_y0 <= 16'd0; c_x1 <= tg_w; c_y1 <= tg_h;
                            z_valid <= 1'b0;                              // the depth buffer was the old target's
                        end
                    end
                    S_ZBUF: begin
                        st <= S_IDLE;
                        if (!zb_bad) begin
                            z_base <= p0[18:0]; z_stride <= p1[15:0]; z_valid <= 1'b1;
                        end else
                            badpar_q <= 1'b1;                             // refused: nothing changes
                    end
                    S_TRI: if (!tri_busy) st <= S_IDLE;
                    S_FLIP: if (vblank) begin                             // as WAIT_VBLANK, then the write
                        flip_we   <= 1'b1;
                        flip_base <= p0[18:2];
                        st <= S_IDLE;
                    end
                    S_RECT: begin
                        if (!have) begin
                            if (!fifo_empty) begin                         // take the next data word
                                if (is_exp) sbuf <= sbuf | ({fifo_q, 32'd0} >> scnt);
                                else        sbuf <= sbuf | ({32'd0, fifo_q} << {scnt[2:0], 3'b000});
                                scnt <= scnt + (is_exp ? 7'd32 : 7'd4);
                            end
                        end else if (lane_mask == 4'd0 || m_gnt) begin
                            if (is_exp) begin sbuf <= sbuf << k; scnt <= scnt - {4'd0, k}; end
                            if (is_wrr) begin sbuf <= sbuf >> {k, 3'b000}; scnt <= scnt - {4'd0, k}; end
                            if (!last_g)
                                cg <= cg + 16'sd1;
                            else if (!last_row) begin
                                cy  <= cy + 18'sd1;
                                row <= row + stride36;
                                cg  <= g_first;
                            end else
                                st <= S_IDLE;
                        end
                    end
                    S_COPY_GO: begin                                       // a row's first group
                        cprime <= 1'b1;
                        cw_hi <= xdn && two;                               // right to left: hi first
                        st <= S_COPY_RD;
                    end
                    S_COPY_RD: if (m_gnt) st <= S_COPY_WT;
                    S_COPY_WT, S_COPY_WR: begin                            // WT: the word arrives now
                        if (st == S_COPY_WT) begin
                            if (cw_hi) hi <= m_rdata; else lo <= m_rdata;
                        end
                        if (copy_more) begin                               // the row's second word
                            cprime <= 1'b0;
                            cw_hi <= !cw_hi;
                            st <= S_COPY_RD;
                        end else if (copy_mask == 4'd0 || m_gnt) begin     // written: the next group
                            cprime <= 1'b0;
                            if (!last_g) begin
                                cg <= xdn ? cg - 16'sd1 : cg + 16'sd1;
                                if (two) begin
                                    if (xdn) begin hi <= lo_n; cw_hi <= 1'b0; end
                                    else     begin lo <= hi_n; cw_hi <= 1'b1; end
                                end else
                                    cw_hi <= 1'b0;
                                st <= S_COPY_RD;
                            end else if (!last_row) begin
                                cy  <= ydn ? cy - 18'sd1 : cy + 18'sd1;
                                row <= ydn ? row - stride36 : row + stride36;
                                cg  <= xdn ? g_last : g_first;
                                st  <= S_COPY_GO;
                            end else
                                st <= S_IDLE;
                        end else begin
                            cprime <= 1'b0;
                            st <= S_COPY_WR;                               // write again next cycle
                        end
                    end
                    S_LINE: if (!l_in || m_gnt) begin
                        if (l_end)
                            st <= S_IDLE;
                        else begin
                            if (l_mvx) lx <= lsx ? lx - 18'sd1 : lx + 18'sd1;
                            if (l_mvy) begin
                                ly  <= lsy ? ly - 18'sd1 : ly + 18'sd1;
                                row <= lsy ? row - stride36 : row + stride36;
                            end
                            lerr <= lerr + (l_mvx ? ldy : 20'sd0) + (l_mvy ? ldx : 20'sd0);
                        end
                    end
                    S_VBL: if (vblank) st <= S_IDLE;
                    default: st <= S_IDLE;
                endcase
            end
        end
    end

    // ---------------------------------------------------------------- reads and responses
    wire busy = !fifo_empty || st != S_IDLE;
    reg [31:0] rdata;
    always @(*) begin
        rdata = 32'd0;
        if (hit_id)   rdata = ID_VALUE;
        if (hit_stat) begin
            rdata[SURF_GPU_STATUS_BUSY_LSB]       = busy;
            rdata[SURF_GPU_STATUS_OVERFLOW_LSB]   = ovf_q;
            rdata[SURF_GPU_STATUS_BAD_OPCODE_LSB] = badop_q;
            rdata[SURF_GPU_STATUS_BAD_PARAM_LSB]  = badpar_q;
            rdata[SURF_GPU_STATUS_FENCE_EV_LSB]   = fev_q;
        end
        if (hit_ie)   rdata[1:0] = ie_q;
        if (hit_free) rdata[15:0] = fifo_free;
        if (hit_fen)  rdata = fence_q;
        if (hit_info) rdata = INFO_VALUE;
    end

    reg        rsp_valid_q = 1'b0, rsp_err_q = 1'b0;
    reg [31:0] rsp_rdata_q = 32'd0;
    always @(posedge clk) begin
        if (rst) begin
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'd0;
        end else begin
            rsp_valid_q <= accept;
            rsp_err_q   <= accept && !legal;
            rsp_rdata_q <= (accept && legal && !s_req_write) ? rdata : 32'd0;
        end
    end
    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_rdata = rsp_rdata_q;
    assign s_rsp_err   = rsp_err_q;
    assign irq = (fev_q && ie_q[SURF_GPU_IE_FENCE_LSB]) ||
                 ((ovf_q || badop_q || badpar_q) && ie_q[SURF_GPU_IE_ERR_LSB]);

    // unused: the address above the window, the top of the 18-bit sizes, the COPY size's top bits,
    // the header bits TRIANGLE does not use, FLIP's base outside GFX_BASE's bits
    wire unused_ok = &{1'b0, s_req_addr[31:12], cw[17:16], ch[17:16], src_row[35:19], l_byte[35:19],
                       row[35:19], row[1:0], src_row[1:0], f_x1m[1:0], e_x1m[1:0], k_x1m[1:0], hdr[23:14],
                       p0[31:19], p3[31:16], p5[31:24]};
endmodule
