/*
SurfGPU triangle walker
--------------------
File: surf_gpu_tri.v Date: 10/25/26 (SurfGPU GPU2)
--------------------
TRIANGLE for surf_gpu (docs/GPU.md section 16): surf_gpu collects the command's six parameter
words, refuses depth without a depth buffer, and starts this unit, which sets the triangle up and
walks it on VRAM through surf_gpu's master port (m_*, the engine's side of surf_vram's port A,
waiting whenever the CPU has the port), then falls idle. ABORT stops it between two accesses.

Setup (16.8), from registers to registers, every product made by one 37 x 18 signed multiplier
(two DSP48E1, multipliers only as the flow keeps them) one a cycle, and every quotient by one
sequential divider (surf_gpu_div), so that the setup's wiring stays small next to VRAM's:
  T_AREA    the doubled area A of the vertices as given (two products) and the box of pixel
            centres that can be inside, cut by the effective clip (16.3);
  T_DECIDE  A = 0, a culled winding or an empty box: done; otherwise v1 and v2 swapped when A < 0
            (the walk goes clockwise), and |A| kept;
  T_SETUP   (16 products) each edge's function at the first group's lane 0 (pixel 4 gs, row
            by0) with the top-left rule folded in (E - 1 for an edge that is not top or left, so
            a pixel is covered when all three are >= 0), its steps per pixel and per row; the
            gradients' numerators; the row addresses of the target and the depth buffer;
  T_DIV     when depth or GOURAUD needs them, the four gradients one after the other, 55 cycles
            each;
  T_START   (4 products) the depth and colour values V at the first lane 0 relative to vertex 0,
            modulo 2^37 and 2^29 (16.4), and their steps;
  T_LANES   lane 0's edge functions and values, kept twice (the group's, and the row's first
            group's), and three pixel steps of each.

The walk (16.5): rows from the box's top, groups of four pixels (the target's words) from the
box's left; every quantity lane 0 needs is a register stepped by exact additions (a group:
4 pixel steps; a row: the row copy plus one row step), and lane L adds L pixel steps to lane 0's,
so a lane's coverage is the sign bits of three sums and its value a rounding and a clamp of one.
A group without a covered lane takes one cycle and no access (T_GRP); with ZTEST its depth words
are read (T_GRP, T_RD1), the last one arrives (T_RDW) and the lanes are compared with registered
values (T_CMP); with ZWRITE the depth words of the passing lanes are written, then the colour
word (T_CMP or T_GRP, T_ZW1, T_CW), each with byte enables. The order of the accesses is section
16.5's; every value is the C model's (sim/models/gpu_model.c), which sim/gpu/test_gpu.cpp compares
with this at every FENCE.
*/
module surf_gpu_tri (
    input  wire               clk,
    input  wire               rst,
    input  wire               stop,           // CTRL.ABORT: stop now
    input  wire               start,          // one cycle: draw the triangle below
    input  wire        [13:0] flags,          // TRIANGLE's header bits 13:0
    input  wire       [191:0] vtx,            // its six parameter words, the first in bits 31:0
    input  wire        [18:0] t_base,         // the target's base and stride
    input  wire        [15:0] t_stride,
    input  wire        [18:0] z_base,         // the depth buffer's
    input  wire        [15:0] z_stride,
    input  wire signed [17:0] ecx0,           // the effective clip [ecx0, ecx1) x [ecy0, ecy1)
    input  wire signed [17:0] ecx1,
    input  wire signed [17:0] ecy0,
    input  wire signed [17:0] ecy1,
    output wire               busy,
    // VRAM's engine side of port A, through surf_gpu
    output reg                m_req,
    output reg                m_we,
    output reg         [3:0]  m_be,
    output reg         [16:0] m_addr,
    output reg         [31:0] m_wdata,
    input  wire               m_gnt,
    input  wire               m_rvalid,
    input  wire        [31:0] m_rdata
);
    localparam [3:0] T_IDLE = 4'd0, T_AREA = 4'd1, T_DECIDE = 4'd2, T_SETUP = 4'd3, T_DIV = 4'd4,
                     T_START = 4'd5, T_LANES = 4'd6, T_GRP = 4'd7, T_RD1 = 4'd8, T_RDW = 4'd9,
                     T_CMP = 4'd10, T_ZW1 = 4'd11, T_CW = 4'd12;
    // header bits (gpu.h GPU_TRI_*)
    localparam integer HDR_TRI_GOURAUD = 8, HDR_TRI_ZTEST = 9, HDR_TRI_ZFUNC = 10, HDR_TRI_ZWRITE = 11,
                       HDR_TRI_CULL_CW = 12, HDR_TRI_CULL_CCW = 13;

    reg [3:0] st = T_IDLE;

    // ---------------------------------------------------------------- arithmetic helpers
    function signed [16:0] sub16(input signed [15:0] a, input signed [15:0] b);   // a - b, exact
        sub16 = {a[15], a} - {b[15], b};
    endfunction
    function signed [36:0] ext17(input signed [16:0] v); ext17 = {{20{v[16]}}, v}; endfunction
    function signed [15:0] smin(input signed [15:0] a, input signed [15:0] b); smin = (a < b) ? a : b; endfunction
    function signed [15:0] smax(input signed [15:0] a, input signed [15:0] b); smax = (a > b) ? a : b; endfunction
    function [15:0] umin16(input [15:0] a, input [15:0] b); umin16 = (a < b) ? a : b; endfunction
    function [15:0] umax16(input [15:0] a, input [15:0] b); umax16 = (a > b) ? a : b; endfunction
    function [7:0] umin8(input [7:0] a, input [7:0] b); umin8 = (a < b) ? a : b; endfunction
    function [7:0] umax8(input [7:0] a, input [7:0] b); umax8 = (a > b) ? a : b; endfunction
    // a value rounded to the nearest (halves up) and clamped: (V + 2^19) >> 20 = (V >>> 20) + V[19],
    // so only V's bits 36:19 (depth) or 28:19 (colour) count
    function [15:0] zround(input [17:0] v, input [15:0] lo, input [15:0] hi);    // v = V[36:19]
        reg signed [17:0] t;
        begin
            t = $signed({v[17], v[17:1]}) + $signed({17'd0, v[0]});
            zround = (t < $signed({2'b00, lo})) ? lo : (t > $signed({2'b00, hi})) ? hi : t[15:0];
        end
    endfunction
    function [7:0] cround(input [9:0] v, input [7:0] lo, input [7:0] hi);        // v = V[28:19]
        reg signed [9:0] t;
        begin
            t = $signed({v[9], v[9:1]}) + $signed({9'd0, v[0]});
            cround = (t < $signed({2'b00, lo})) ? lo : (t > $signed({2'b00, hi})) ? hi : t[7:0];
        end
    endfunction

    // ---------------------------------------------------------------- the triangle
    reg        [13:0] fl = 14'd0;
    reg signed [15:0] x0 = 16'sd0, y0 = 16'sd0, x1 = 16'sd0, y1 = 16'sd0, x2 = 16'sd0, y2 = 16'sd0;
    reg        [15:0] z0 = 16'd0, z1 = 16'd0, z2 = 16'd0;
    reg        [7:0]  c0 = 8'd0, c1 = 8'd0, c2 = 8'd0;
    wire gouraud = fl[HDR_TRI_GOURAUD], ztest = fl[HDR_TRI_ZTEST], zle = fl[HDR_TRI_ZFUNC];
    wire zwrite  = fl[HDR_TRI_ZWRITE];
    wire attrs   = gouraud || ztest || zwrite;           // values to interpolate: T_DIV and T_START run

    // T_AREA: the box (from the vertices as given); A = dx1 dy2 - dy1 dx2 from the multiplier
    wire signed [16:0] dx1 = sub16(x1, x0), dy1 = sub16(y1, y0), dx2 = sub16(x2, x0), dy2 = sub16(y2, y0);
    wire signed [15:0] minx = smin(smin(x0, x1), x2), maxx = smax(smax(x0, x1), x2);
    wire signed [15:0] miny = smin(smin(y0, y1), y2), maxy = smax(smax(y0, y1), y2);
    // columns floor((minX + 7) / 16) .. floor((maxX - 8) / 16), rows likewise, then the clip
    wire signed [16:0] mx7 = {minx[15], minx} + 17'sd7, xm8 = {maxx[15], maxx} - 17'sd8;
    wire signed [16:0] my7 = {miny[15], miny} + 17'sd7, ym8 = {maxy[15], maxy} - 17'sd8;
    wire signed [17:0] bxa = {{5{mx7[16]}}, mx7[16:4]}, bxb = {{5{xm8[16]}}, xm8[16:4]};
    wire signed [17:0] bya = {{5{my7[16]}}, my7[16:4]}, byb = {{5{ym8[16]}}, ym8[16:4]};
    wire signed [17:0] ecx1m = ecx1 - 18'sd1, ecy1m = ecy1 - 18'sd1;
    wire signed [17:0] bx0_c = (bxa > ecx0) ? bxa : ecx0, bx1_c = (bxb < ecx1m) ? bxb : ecx1m;
    wire signed [17:0] by0_c = (bya > ecy0) ? bya : ecy0, by1_c = (byb < ecy1m) ? byb : ecy1m;

    reg signed [34:0] area = 35'sd0;
    reg        [32:0] aabs = 33'd0;                      // |A| < 2^33
    reg signed [17:0] bx0 = 18'sd0, bx1 = 18'sd0, by0 = 18'sd0, by1 = 18'sd0;
    wire              culled = (area > 35'sd0 && fl[HDR_TRI_CULL_CW]) || (area < 35'sd0 && fl[HDR_TRI_CULL_CCW]);
    wire              empty  = (bx0 > bx1) || (by0 > by1);

    // T_SETUP (the vertices now clockwise): edge e from vertex e to vertex e + 1 mod 3
    wire signed [16:0] ex0 = sub16(x1, x0), ey0 = sub16(y1, y0);
    wire signed [16:0] ex1 = sub16(x2, x1), ey1 = sub16(y2, y1);
    wire signed [16:0] ex2 = sub16(x0, x2), ey2 = sub16(y0, y2);
    wire tl0 = ey0[16] || (ey0 == 17'sd0 && ex0 > 17'sd0);
    wire tl1 = ey1[16] || (ey1 == 17'sd0 && ex1 > 17'sd0);
    wire tl2 = ey2[16] || (ey2 == 17'sd0 && ex2 > 17'sd0);
    reg  [12:0] gs = 13'd0, ge = 13'd0;                  // the box's first and last group
    // the first group's lane 0 centre (64 gs + 8, 16 by0 + 8) less each vertex (they fit 18 bits:
    // the centre is within 64 of the vertices' box, section 16.3)
    wire signed [19:0] c0x = {1'b0, gs, 6'd8}, c0y = {1'b0, by0[14:0], 4'd8};
    wire signed [19:0] cx_0 = c0x - {{4{x0[15]}}, x0}, cy_0 = c0y - {{4{y0[15]}}, y0};
    wire signed [19:0] cx_1 = c0x - {{4{x1[15]}}, x1}, cy_1 = c0y - {{4{y1[15]}}, y1};
    wire signed [19:0] cx_2 = c0x - {{4{x2[15]}}, x2}, cy_2 = c0y - {{4{y2[15]}}, y2};
    wire signed [17:0] dcx0 = cx_0[17:0], dcy0 = cy_0[17:0], dcx1 = cx_1[17:0], dcy1 = cy_1[17:0];
    wire signed [17:0] dcx2 = cx_2[17:0], dcy2 = cy_2[17:0];
    // the gradients' numerators (16.4) with the vertices as walked (both signs flip with A's)
    wire signed [16:0] dz1 = {1'b0, z1} - {1'b0, z0}, dz2 = {1'b0, z2} - {1'b0, z0};
    wire signed [16:0] dc1 = {9'd0, c1} - {9'd0, c0}, dc2 = {9'd0, c2} - {9'd0, c0};

    // ---------------------------------------------------------------- the multiplier
    // Step ms's operands go in while step ms - 1's product is taken from mprod (modulo 2^37: a
    // product of the setup's fits 36 bits signed, a start value's is wanted modulo 2^37 or 2^29)
    reg         [4:0]  ms = 5'd0;                        // the step of T_AREA, T_SETUP or T_START
    reg  signed [36:0] ma;
    reg  signed [17:0] mb;
    wire signed [54:0] mfull = $signed({{18{ma[36]}}, ma}) * $signed({{37{mb[17]}}, mb});
    reg         [36:0] mprod = 37'd0;
    reg         [36:0] mtmp = 37'd0;                     // a pair's first product
    wire signed [35:0] mp36 = mprod[35:0], mt36 = mtmp[35:0];
    wire signed [35:0] mdiff = mt36 - mp36;              // a pair: its first product less its second
    reg  signed [34:0] nzx = 35'sd0, nzy = 35'sd0, ncx = 35'sd0, ncy = 35'sd0;   // |N| < 2^33
    reg         [36:0] gzx = 37'd0, gzy = 37'd0, gcx = 37'd0, gcy = 37'd0;   // floor(N 2^20 / |A|) mod 2^37
    always @(*) begin
        ma = 37'sd0;
        mb = 18'sd0;
        if (st == T_AREA) begin
            if (ms == 5'd0) begin ma = ext17(dx1); mb = {dy2[16], dy2}; end
            else            begin ma = ext17(dy1); mb = {dx2[16], dx2}; end
        end else if (st == T_SETUP) begin
            case (ms)
                5'd0:  begin ma = ext17(ex0); mb = dcy0; end           // edge 0: dX (cy - Y0) ...
                5'd1:  begin ma = ext17(ey0); mb = dcx0; end           // ... - dY (cx - X0)
                5'd2:  begin ma = ext17(ex1); mb = dcy1; end
                5'd3:  begin ma = ext17(ey1); mb = dcx1; end
                5'd4:  begin ma = ext17(ex2); mb = dcy2; end
                5'd5:  begin ma = ext17(ey2); mb = dcx2; end
                5'd6:  begin ma = ext17(dz1); mb = {dy2[16], dy2}; end  // Nx of depth ...
                5'd7:  begin ma = ext17(dz2); mb = {dy1[16], dy1}; end
                5'd8:  begin ma = ext17(dz2); mb = {dx1[16], dx1}; end  // Ny
                5'd9:  begin ma = ext17(dz1); mb = {dx2[16], dx2}; end
                5'd10: begin ma = ext17(dc1); mb = {dy2[16], dy2}; end  // and of colour
                5'd11: begin ma = ext17(dc2); mb = {dy1[16], dy1}; end
                5'd12: begin ma = ext17(dc2); mb = {dx1[16], dx1}; end
                5'd13: begin ma = ext17(dc1); mb = {dx2[16], dx2}; end
                5'd14: begin ma = {22'd0, by0[14:0]}; mb = {2'b00, t_stride}; end   // the first rows
                5'd15: begin ma = {22'd0, by0[14:0]}; mb = {2'b00, z_stride}; end
                default: ;
            endcase
        end else if (st == T_START) begin
            case (ms)
                5'd0: begin ma = gzx; mb = dcx0; end
                5'd1: begin ma = gzy; mb = dcy0; end
                5'd2: begin ma = gcx; mb = dcx0; end
                5'd3: begin ma = gcy; mb = dcy0; end
                default: ;
            endcase
        end
    end

    // ---------------------------------------------------------------- the divider
    // T_DIV: the four gradients in turn (dv), each started (dwait 0), then taken when done
    reg  [1:0]  dv = 2'd0;
    reg         dwait = 1'b0;
    wire signed [34:0] dn = (dv == 2'd0) ? nzx : (dv == 2'd1) ? nzy : (dv == 2'd2) ? ncx : ncy;
    wire        div_start = (st == T_DIV) && !dwait;
    wire        div_busy;
    wire [36:0] dq;
    surf_gpu_div u_div (.clk(clk), .rst(rst), .start(div_start), .n(dn), .d(aabs), .busy(div_busy), .q(dq));

    // set by T_SETUP
    reg signed [35:0] e0s = 36'sd0, e1s = 36'sd0, e2s = 36'sd0;        // lane 0 of the first group
    reg signed [35:0] a0 = 36'sd0, a1 = 36'sd0, a2 = 36'sd0;           // per pixel to the right: -16 dY
    reg signed [35:0] b0 = 36'sd0, b1 = 36'sd0, b2 = 36'sd0;           // per row down: 16 dX
    reg        [31:0] row = 32'd0, zrow = 32'd0;                       // byte addresses of (0, y)
    reg        [15:0] zmin = 16'd0, zmax = 16'd0;
    reg        [7:0]  cmin = 8'd0, cmax = 8'd0;
    // set by T_START (modulo 2^37 and 2^29)
    reg        [36:0] vzs = 37'd0, szx = 37'd0, szy = 37'd0;           // start, per pixel, per row
    reg        [28:0] vcs = 29'd0, scx = 29'd0, scy = 29'd0;
    // the lanes' offsets from lane 0: 1, 2 and 3 pixel steps (3 is registered in T_LANES)
    wire signed [35:0] a0x2 = {a0[34:0], 1'b0}, a1x2 = {a1[34:0], 1'b0}, a2x2 = {a2[34:0], 1'b0};
    wire       [36:0] szx2 = {szx[35:0], 1'b0};
    wire       [28:0] scx2 = {scx[27:0], 1'b0};
    reg  signed [35:0] a0x3 = 36'sd0, a1x3 = 36'sd0, a2x3 = 36'sd0;
    reg        [36:0] szx3 = 37'd0;
    reg        [28:0] scx3 = 29'd0;

    // ---------------------------------------------------------------- the lanes
    reg signed [35:0] ew0 = 36'sd0, ew1 = 36'sd0, ew2 = 36'sd0;   // edge e at the group's lane 0
    reg signed [35:0] er0 = 36'sd0, er1 = 36'sd0, er2 = 36'sd0;   // the same at the row's first group
    reg        [36:0] vzw = 37'd0, vzr = 37'd0;                    // depth V at lane 0: the group's, the row's
    reg        [28:0] vcw = 29'd0, vcr = 29'd0;                    // colour V
    reg [12:0]  g = 13'd0;                                 // the group (pixels 4g .. 4g + 3)
    reg [14:0]  y = 15'd0;                                 // the row
    reg [63:0]  zq = 64'd0;                                // the lanes' depths, registered
    reg [31:0]  zb0 = 32'd0, zb1 = 32'd0;                  // the depth words read: lanes 0-1, 2-3
    reg         rd_slot = 1'b0;                            // the read outstanding fills zb1
    wire [63:0] zbw = {zb1, zb0};

    wire [3:0]  cov, pass;
    wire [63:0] zval;
    wire [31:0] cval;
    genvar gl;
    generate
        for (gl = 0; gl < 4; gl = gl + 1) begin : g_lane
            localparam [1:0] LN = gl;
            // lane LN: lane 0's edge functions and values plus LN pixel steps
            wire signed [35:0] le0 = ew0 + ((LN == 2'd0) ? 36'sd0 : (LN == 2'd1) ? a0 : (LN == 2'd2) ? a0x2 : a0x3);
            wire signed [35:0] le1 = ew1 + ((LN == 2'd0) ? 36'sd0 : (LN == 2'd1) ? a1 : (LN == 2'd2) ? a1x2 : a1x3);
            wire signed [35:0] le2 = ew2 + ((LN == 2'd0) ? 36'sd0 : (LN == 2'd1) ? a2 : (LN == 2'd2) ? a2x2 : a2x3);
            wire        [36:0] lz  = vzw + ((LN == 2'd0) ? 37'd0 : (LN == 2'd1) ? szx : (LN == 2'd2) ? szx2 : szx3);
            wire        [28:0] lc  = vcw + ((LN == 2'd0) ? 29'd0 : (LN == 2'd1) ? scx : (LN == 2'd2) ? scx2 : scx3);
            wire        [17:0] xl  = {3'd0, g, LN};
            wire               in_box = ($signed(xl) >= bx0) && ($signed(xl) <= bx1);
            assign cov[gl]  = in_box && !le0[35] && !le1[35] && !le2[35];
            assign zval[16 * gl +: 16] = zround(lz[36:19], zmin, zmax);
            assign cval[8 * gl +: 8]   = gouraud ? cround(lc[28:19], cmin, cmax) : fl[7:0];
            wire unused_lane = &{1'b0, le0[34:0], le1[34:0], le2[34:0], lz[18:0], lc[18:0]};
            wire [15:0] zbuf = zbw[16 * gl +: 16];
            wire        zok  = zle ? (zq[16 * gl +: 16] <= zbuf) : (zq[16 * gl +: 16] < zbuf);
            assign pass[gl] = cov[gl] && (!ztest || zok);
        end
    endgenerate

    // the group's words
    wire [16:0] cword  = row[18:2] + {4'd0, g};
    wire [16:0] zword0 = zrow[18:2] + {3'd0, g, 1'b0};
    wire [16:0] zword1 = zword0 + 17'd1;
    wire [3:0]  zbe0   = {pass[1], pass[1], pass[0], pass[0]};
    wire [3:0]  zbe1   = {pass[3], pass[3], pass[2], pass[2]};
    wire        lo2    = cov[1:0] != 2'd0, hi2 = cov[3:2] != 2'd0;
    wire        plo2   = pass[1:0] != 2'd0, phi2 = pass[3:2] != 2'd0;

    // ---------------------------------------------------------------- the requests
    reg       adv;                                         // the group is done
    reg [3:0] nxt;
    always @(*) begin
        adv = 1'b0;
        nxt = st;
        m_req = 1'b0; m_we = 1'b1; m_be = 4'h0; m_addr = 17'd0; m_wdata = 32'd0;
        case (st)
            T_GRP: begin
                if (cov == 4'd0)
                    adv = 1'b1;                            // nothing covered: one cycle, no access
                else if (ztest) begin                      // read the depth words needed
                    m_req = 1'b1; m_we = 1'b0; m_be = 4'hF; m_addr = lo2 ? zword0 : zword1;
                    if (m_gnt) nxt = (lo2 && hi2) ? T_RD1 : T_RDW;
                end else if (zwrite) begin                 // every covered lane passes
                    m_req = 1'b1; m_be = plo2 ? zbe0 : zbe1; m_addr = plo2 ? zword0 : zword1;
                    m_wdata = plo2 ? zval[31:0] : zval[63:32];
                    if (m_gnt) nxt = (plo2 && phi2) ? T_ZW1 : T_CW;
                end else begin                             // the colour, in the same cycle
                    m_req = 1'b1; m_be = pass; m_addr = cword; m_wdata = cval;
                    if (m_gnt) adv = 1'b1;
                end
            end
            T_RD1: begin
                m_req = 1'b1; m_we = 1'b0; m_be = 4'hF; m_addr = zword1;
                if (m_gnt) nxt = T_RDW;
            end
            T_RDW: nxt = T_CMP;                            // the last depth word arrives
            T_CMP: begin
                if (zwrite && plo2) begin
                    m_req = 1'b1; m_be = zbe0; m_addr = zword0; m_wdata = zval[31:0];
                    if (m_gnt) nxt = phi2 ? T_ZW1 : T_CW;
                end else if (zwrite && phi2) begin
                    m_req = 1'b1; m_be = zbe1; m_addr = zword1; m_wdata = zval[63:32];
                    if (m_gnt) nxt = T_CW;
                end else if (pass != 4'd0) begin
                    m_req = 1'b1; m_be = pass; m_addr = cword; m_wdata = cval;
                    if (m_gnt) adv = 1'b1;
                end else
                    adv = 1'b1;                            // every lane failed the depth test
            end
            T_ZW1: begin
                m_req = 1'b1; m_be = zbe1; m_addr = zword1; m_wdata = zval[63:32];
                if (m_gnt) nxt = T_CW;
            end
            T_CW: begin
                m_req = 1'b1; m_be = pass; m_addr = cword; m_wdata = cval;
                if (m_gnt) adv = 1'b1;
            end
            default: ;
        endcase
    end

    // ---------------------------------------------------------------- the sequence
    always @(posedge clk) begin
        zq <= zval;
        mprod <= mfull[36:0];
        if (m_rvalid) begin
            if (rd_slot) zb1 <= m_rdata;
            else         zb0 <= m_rdata;
        end
        if (rst || stop) begin
            st <= T_IDLE;
        end else begin
            case (st)
                T_IDLE: if (start) begin
                    fl <= flags;
                    x0 <= vtx[15:0];    y0 <= vtx[31:16];   z0 <= vtx[47:32];   c0 <= vtx[55:48];
                    x1 <= vtx[79:64];   y1 <= vtx[95:80];   z1 <= vtx[111:96];  c1 <= vtx[119:112];
                    x2 <= vtx[143:128]; y2 <= vtx[159:144]; z2 <= vtx[175:160]; c2 <= vtx[183:176];
                    ms <= 5'd0;
                    st <= T_AREA;
                end
                T_AREA: begin                              // A = dx1 dy2 - dy1 dx2
                    ms <= ms + 5'd1;
                    if (ms == 5'd1) mtmp <= mprod;
                    if (ms == 5'd2) begin
                        area <= mdiff[34:0];
                        bx0 <= bx0_c; bx1 <= bx1_c; by0 <= by0_c; by1 <= by1_c;
                        st <= T_DECIDE;
                    end
                end
                T_DECIDE: begin
                    if (area == 35'sd0 || culled || empty)
                        st <= T_IDLE;                      // nothing to draw
                    else begin
                        if (area[34]) begin                // counter-clockwise: walk it the other way
                            x1 <= x2; y1 <= y2; z1 <= z2; c1 <= c2;
                            x2 <= x1; y2 <= y1; z2 <= z1; c2 <= c1;
                        end
                        aabs <= area[34] ? -area[32:0] : area[32:0];
                        gs <= bx0[14:2];
                        ge <= bx1[14:2];
                        ms <= 5'd0;
                        st <= T_SETUP;
                    end
                end
                T_SETUP: begin                             // step ms - 1's product is in mprod
                    ms <= ms + 5'd1;
                    case (ms)
                        5'd1, 5'd3, 5'd5, 5'd7, 5'd9, 5'd11, 5'd13: mtmp <= mprod;
                        5'd2:  e0s <= mdiff - {35'd0, !tl0};
                        5'd4:  e1s <= mdiff - {35'd0, !tl1};
                        5'd6:  e2s <= mdiff - {35'd0, !tl2};
                        5'd8:  nzx <= mdiff[34:0];
                        5'd10: nzy <= mdiff[34:0];
                        5'd12: ncx <= mdiff[34:0];
                        5'd14: ncy <= mdiff[34:0];
                        5'd15: row  <= {13'd0, t_base} + {1'b0, mprod[30:0]};
                        5'd16: zrow <= {13'd0, z_base} + {1'b0, mprod[30:0]};
                        default: ;
                    endcase
                    a0 <= -$signed({{15{ey0[16]}}, ey0, 4'd0});
                    a1 <= -$signed({{15{ey1[16]}}, ey1, 4'd0});
                    a2 <= -$signed({{15{ey2[16]}}, ey2, 4'd0});
                    b0 <= $signed({{15{ex0[16]}}, ex0, 4'd0});
                    b1 <= $signed({{15{ex1[16]}}, ex1, 4'd0});
                    b2 <= $signed({{15{ex2[16]}}, ex2, 4'd0});
                    zmin <= umin16(umin16(z0, z1), z2); zmax <= umax16(umax16(z0, z1), z2);
                    cmin <= umin8(umin8(c0, c1), c2); cmax <= umax8(umax8(c0, c1), c2);
                    if (ms == 5'd16) begin
                        dv <= 2'd0;
                        dwait <= 1'b0;
                        ms <= 5'd0;
                        st <= attrs ? T_DIV : T_LANES;
                    end
                end
                T_DIV: begin                               // 55 cycles a gradient
                    if (!dwait)
                        dwait <= 1'b1;                     // the division starts this cycle
                    else if (!div_busy) begin
                        if (dv == 2'd0) gzx <= dq;
                        if (dv == 2'd1) gzy <= dq;
                        if (dv == 2'd2) gcx <= dq;
                        if (dv == 2'd3) gcy <= dq;
                        dwait <= 1'b0;
                        dv <= dv + 2'd1;
                        if (dv == 2'd3) begin
                            ms <= 5'd0;
                            st <= T_START;
                        end
                    end
                end
                T_START: begin                             // V0 = v0 2^20 + GX (cx - X0) + GY (cy - Y0)
                    ms <= ms + 5'd1;
                    if (ms == 5'd1 || ms == 5'd3) mtmp <= mprod;
                    if (ms == 5'd2) vzs <= {1'b0, z0, 20'd0} + mtmp + mprod;
                    szx <= {gzx[32:0], 4'd0}; szy <= {gzy[32:0], 4'd0};
                    scx <= {gcx[24:0], 4'd0}; scy <= {gcy[24:0], 4'd0};
                    if (ms == 5'd4) begin
                        vcs <= {1'b0, c0, 20'd0} + mtmp[28:0] + mprod[28:0];
                        st <= T_LANES;
                    end
                end
                T_LANES: begin
                    ew0 <= e0s; ew1 <= e1s; ew2 <= e2s;
                    er0 <= e0s; er1 <= e1s; er2 <= e2s;
                    vzw <= vzs; vzr <= vzs;
                    vcw <= vcs; vcr <= vcs;
                    a0x3 <= a0 + a0x2;
                    a1x3 <= a1 + a1x2;
                    a2x3 <= a2 + a2x2;
                    szx3 <= szx + szx2;
                    scx3 <= scx + scx2;
                    g <= gs;
                    y <= by0[14:0];
                    st <= T_GRP;
                end
                default: begin                             // the walk
                    if (m_gnt && m_req && !m_we) rd_slot <= (st == T_RD1) || !lo2;
                    if (!adv)
                        st <= nxt;
                    else if (g != ge) begin                // the next group: four pixel steps
                        g <= g + 13'd1;
                        ew0 <= ew0 + {a0[33:0], 2'b00};
                        ew1 <= ew1 + {a1[33:0], 2'b00};
                        ew2 <= ew2 + {a2[33:0], 2'b00};
                        vzw <= vzw + {szx[34:0], 2'b00};
                        vcw <= vcw + {scx[26:0], 2'b00};
                        st <= T_GRP;
                    end else if (y != by1[14:0]) begin       // the next row: its copy plus a row step
                        y <= y + 15'd1;
                        g <= gs;
                        row  <= row + {16'd0, t_stride};
                        zrow <= zrow + {16'd0, z_stride};
                        ew0 <= er0 + b0;
                        ew1 <= er1 + b1;
                        ew2 <= er2 + b2;
                        er0 <= er0 + b0;
                        er1 <= er1 + b1;
                        er2 <= er2 + b2;
                        vzw <= vzr + szy;
                        vzr <= vzr + szy;
                        vcw <= vcr + scy;
                        vcr <= vcr + scy;
                        st <= T_GRP;
                    end else
                        st <= T_IDLE;                      // the last group of the last row
                end
            endcase
        end
    end

    assign busy = (st != T_IDLE);

    // unused: the second words' top bytes, the box's upper bits (the clip keeps it in 0..32766),
    // bits of the gradients above what a lane keeps, the address bits beyond VRAM
    wire unused_ok = &{1'b0, vtx[63:56], vtx[127:120], vtx[191:184], bx0[17:15], bx1[17:15], by0[17:15],
                       gzx[36:33], gzy[36:33], gcx[36:25], gcy[36:25], row[31:19], row[1:0], zrow[31:19],
                       zrow[1:0], cx_0[19:18], cy_0[19:18], cx_1[19:18], cy_1[19:18], cx_2[19:18], cy_2[19:18],
                       mx7[3:0], xm8[3:0], my7[3:0], ym8[3:0], mfull[54:37], mtmp[36], mprod[36]};
endmodule
