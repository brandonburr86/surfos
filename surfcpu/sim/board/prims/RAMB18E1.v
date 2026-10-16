/*
SurfCPU simulation model of AMD's RAMB18E1
--------------------
File: RAMB18E1.v Date: 10/16/26 (SurfCPU)
--------------------
Yosys's cells_sim.v (the models the open flow copies next to each netlist) declares the block
RAMs without any behaviour, so a netlist with block RAM cannot be simulated with it alone.
This model has AMD's name, parameters and ports (UG953) and the behaviour of UG473 for what
Yosys's block RAM mapping uses: true dual port mode (RAM_MODE "TDP"), data widths 1, 2, 4, 9,
18 on each port (read and write widths may differ), byte-wide write enables (WEA,
WEBWE[3:0]), WRITE_MODE READ_FIRST, WRITE_FIRST and NO_CHANGE, the optional output register
(DOx_REG with REGCE and RSTREG), RSTRAM, INIT_A/B and SRVAL_A/B, and the initial contents of
INIT_xx/INITP_xx. Memory layout (UG473): a word of D data bits at address a holds data bits
a*D .. a*D+D-1 of the 16384-bit array, whose bits 256*i .. 256*i+255 are INIT_i, and P = D/8
parity bits a*P .. in the 2048-bit parity array (INITP_i). The address is ADDR[13:0] with
the word's low bits ignored (ADDR[13:3] for width 9).
Simple dual port (RAM_MODE "SDP", which Yosys uses for LiteDRAM's calibration CPU's register
file and instruction cache) only as 512 x 36 (READ_WIDTH_A = WRITE_WIDTH_B = 36) without the
output register: port A reads word ADDRARDADDR[13:5] into {DOBDO, DOADO} and {DOPBDOP,
DOPADOP} on CLKARDCLK when ENARDEN (RSTRAMARSTRAM loads SRVAL_A into the low half and SRVAL_B
into the high half), port B writes {DIBDI, DIADI} and {DIPBDIP, DIPADIP} to word
ADDRBWRADDR[13:5] on CLKBWRCLK when ENBWREN, byte i where WEBWE[i] (data bits 8i..8i+7 and
parity bit i). Anything else (other SDP widths, cascading, ECC, inverted pins, an INIT_FILE)
stops the simulation at time 0 rather than be modelled wrong. Simulation only; a collision of
two ports on one word is not modelled (in SDP mode a read of the word being written on the same
clock edge returns the old or the new word, in the simulator's order).
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module RAMB18E1 #(
    parameter integer DOA_REG = 0,
    parameter integer DOB_REG = 0,
    parameter [255:0] INIT_00 = 256'h0,
    parameter [255:0] INIT_01 = 256'h0,
    parameter [255:0] INIT_02 = 256'h0,
    parameter [255:0] INIT_03 = 256'h0,
    parameter [255:0] INIT_04 = 256'h0,
    parameter [255:0] INIT_05 = 256'h0,
    parameter [255:0] INIT_06 = 256'h0,
    parameter [255:0] INIT_07 = 256'h0,
    parameter [255:0] INIT_08 = 256'h0,
    parameter [255:0] INIT_09 = 256'h0,
    parameter [255:0] INIT_0A = 256'h0,
    parameter [255:0] INIT_0B = 256'h0,
    parameter [255:0] INIT_0C = 256'h0,
    parameter [255:0] INIT_0D = 256'h0,
    parameter [255:0] INIT_0E = 256'h0,
    parameter [255:0] INIT_0F = 256'h0,
    parameter [255:0] INIT_10 = 256'h0,
    parameter [255:0] INIT_11 = 256'h0,
    parameter [255:0] INIT_12 = 256'h0,
    parameter [255:0] INIT_13 = 256'h0,
    parameter [255:0] INIT_14 = 256'h0,
    parameter [255:0] INIT_15 = 256'h0,
    parameter [255:0] INIT_16 = 256'h0,
    parameter [255:0] INIT_17 = 256'h0,
    parameter [255:0] INIT_18 = 256'h0,
    parameter [255:0] INIT_19 = 256'h0,
    parameter [255:0] INIT_1A = 256'h0,
    parameter [255:0] INIT_1B = 256'h0,
    parameter [255:0] INIT_1C = 256'h0,
    parameter [255:0] INIT_1D = 256'h0,
    parameter [255:0] INIT_1E = 256'h0,
    parameter [255:0] INIT_1F = 256'h0,
    parameter [255:0] INIT_20 = 256'h0,
    parameter [255:0] INIT_21 = 256'h0,
    parameter [255:0] INIT_22 = 256'h0,
    parameter [255:0] INIT_23 = 256'h0,
    parameter [255:0] INIT_24 = 256'h0,
    parameter [255:0] INIT_25 = 256'h0,
    parameter [255:0] INIT_26 = 256'h0,
    parameter [255:0] INIT_27 = 256'h0,
    parameter [255:0] INIT_28 = 256'h0,
    parameter [255:0] INIT_29 = 256'h0,
    parameter [255:0] INIT_2A = 256'h0,
    parameter [255:0] INIT_2B = 256'h0,
    parameter [255:0] INIT_2C = 256'h0,
    parameter [255:0] INIT_2D = 256'h0,
    parameter [255:0] INIT_2E = 256'h0,
    parameter [255:0] INIT_2F = 256'h0,
    parameter [255:0] INIT_30 = 256'h0,
    parameter [255:0] INIT_31 = 256'h0,
    parameter [255:0] INIT_32 = 256'h0,
    parameter [255:0] INIT_33 = 256'h0,
    parameter [255:0] INIT_34 = 256'h0,
    parameter [255:0] INIT_35 = 256'h0,
    parameter [255:0] INIT_36 = 256'h0,
    parameter [255:0] INIT_37 = 256'h0,
    parameter [255:0] INIT_38 = 256'h0,
    parameter [255:0] INIT_39 = 256'h0,
    parameter [255:0] INIT_3A = 256'h0,
    parameter [255:0] INIT_3B = 256'h0,
    parameter [255:0] INIT_3C = 256'h0,
    parameter [255:0] INIT_3D = 256'h0,
    parameter [255:0] INIT_3E = 256'h0,
    parameter [255:0] INIT_3F = 256'h0,
    parameter [255:0] INITP_00 = 256'h0,
    parameter [255:0] INITP_01 = 256'h0,
    parameter [255:0] INITP_02 = 256'h0,
    parameter [255:0] INITP_03 = 256'h0,
    parameter [255:0] INITP_04 = 256'h0,
    parameter [255:0] INITP_05 = 256'h0,
    parameter [255:0] INITP_06 = 256'h0,
    parameter [255:0] INITP_07 = 256'h0,
    parameter INIT_A = 0,
    parameter INIT_B = 0,
    parameter INIT_FILE = "NONE",
    parameter RAM_MODE = "TDP",
    parameter RDADDR_COLLISION_HWCONFIG = "DELAYED_WRITE",
    parameter integer READ_WIDTH_A = 0,
    parameter integer READ_WIDTH_B = 0,
    parameter RSTREG_PRIORITY_A = "RSTREG",
    parameter RSTREG_PRIORITY_B = "RSTREG",
    parameter SIM_COLLISION_CHECK = "ALL",
    parameter SIM_DEVICE = "7SERIES",
    parameter SRVAL_A = 0,
    parameter SRVAL_B = 0,
    parameter WRITE_MODE_A = "WRITE_FIRST",
    parameter WRITE_MODE_B = "WRITE_FIRST",
    parameter integer WRITE_WIDTH_A = 0,
    parameter integer WRITE_WIDTH_B = 0,
    parameter [0:0] IS_CLKARDCLK_INVERTED = 1'b0,
    parameter [0:0] IS_CLKBWRCLK_INVERTED = 1'b0,
    parameter [0:0] IS_ENARDEN_INVERTED = 1'b0,
    parameter [0:0] IS_ENBWREN_INVERTED = 1'b0,
    parameter [0:0] IS_RSTRAMARSTRAM_INVERTED = 1'b0,
    parameter [0:0] IS_RSTRAMB_INVERTED = 1'b0,
    parameter [0:0] IS_RSTREGARSTREG_INVERTED = 1'b0,
    parameter [0:0] IS_RSTREGB_INVERTED = 1'b0
) (
    output wire [15:0] DOADO,
    output wire [15:0] DOBDO,
    output wire [1:0] DOPADOP,
    output wire [1:0] DOPBDOP,
    input  wire ENARDEN,
    input  wire CLKARDCLK,
    input  wire RSTRAMARSTRAM,
    input  wire RSTREGARSTREG,
    input  wire REGCEAREGCE,
    input  wire ENBWREN,
    input  wire CLKBWRCLK,
    input  wire RSTRAMB,
    input  wire RSTREGB,
    input  wire REGCEB,
    input  wire [13:0] ADDRARDADDR,
    input  wire [13:0] ADDRBWRADDR,
    input  wire [15:0] DIADI,
    input  wire [15:0] DIBDI,
    input  wire [1:0] DIPADIP,
    input  wire [1:0] DIPBDIP,
    input  wire [1:0] WEA,
    input  wire [3:0] WEBWE
);
    localparam integer NBITS = 16384;
    localparam integer PBITS = 2048;
    localparam integer SDP   = (RAM_MODE == "SDP") ? 1 : 0;

    function integer dbits(input integer w);     // data bits of a port width
        dbits = (w >= 36) ? 32 : (w >= 18) ? 16 : (w >= 9) ? 8 : w;
    endfunction
    function integer pbits_of(input integer w);  // parity bits of a port width
        pbits_of = (w >= 36) ? 4 : (w >= 18) ? 2 : (w >= 9) ? 1 : 0;
    endfunction
    function legal(input integer w);
        legal = (w == 0) || (w == 1) || (w == 2) || (w == 4) || (w == 9) || (w == 18) || (w <= 18 && w == 36);
    endfunction

    localparam integer RDA = dbits(READ_WIDTH_A),  RPA = pbits_of(READ_WIDTH_A);
    localparam integer RDB = dbits(READ_WIDTH_B),  RPB = pbits_of(READ_WIDTH_B);
    localparam integer WDA = dbits(WRITE_WIDTH_A), WPA = pbits_of(WRITE_WIDTH_A);
    localparam integer WDB = dbits(WRITE_WIDTH_B), WPB = pbits_of(WRITE_WIDTH_B);

    reg [NBITS-1:0] mem;
    reg [PBITS-1:0] memp;
    initial begin
        mem  = {INIT_3F, INIT_3E, INIT_3D, INIT_3C, INIT_3B, INIT_3A, INIT_39, INIT_38, INIT_37, INIT_36, INIT_35, INIT_34, INIT_33, INIT_32, INIT_31, INIT_30, INIT_2F, INIT_2E, INIT_2D, INIT_2C, INIT_2B, INIT_2A, INIT_29, INIT_28, INIT_27, INIT_26, INIT_25, INIT_24, INIT_23, INIT_22, INIT_21, INIT_20, INIT_1F, INIT_1E, INIT_1D, INIT_1C, INIT_1B, INIT_1A, INIT_19, INIT_18, INIT_17, INIT_16, INIT_15, INIT_14, INIT_13, INIT_12, INIT_11, INIT_10, INIT_0F, INIT_0E, INIT_0D, INIT_0C, INIT_0B, INIT_0A, INIT_09, INIT_08, INIT_07, INIT_06, INIT_05, INIT_04, INIT_03, INIT_02, INIT_01, INIT_00};
        memp = {INITP_07, INITP_06, INITP_05, INITP_04, INITP_03, INITP_02, INITP_01, INITP_00};
        if (RAM_MODE != "TDP" && RAM_MODE != "SDP") $fatal(1, "%m: RAM_MODE %0s is not modelled (TDP and SDP only)", RAM_MODE);
        if (SDP != 0 && !(READ_WIDTH_A == 36 && WRITE_WIDTH_B == 36 && READ_WIDTH_B == 0 && WRITE_WIDTH_A == 0))
            $fatal(1, "%m: simple dual port is modelled only as 512 x 36 (READ_WIDTH_A = WRITE_WIDTH_B = 36)");
        if (SDP != 0 && (DOA_REG != 0 || DOB_REG != 0))
            $fatal(1, "%m: the output register is not modelled in simple dual port mode");
        if (INIT_FILE != "NONE" && INIT_FILE != "") $fatal(1, "%m: INIT_FILE is not modelled");
        if (IS_CLKARDCLK_INVERTED | IS_CLKBWRCLK_INVERTED | IS_ENARDEN_INVERTED | IS_ENBWREN_INVERTED | IS_RSTRAMARSTRAM_INVERTED | IS_RSTRAMB_INVERTED | IS_RSTREGARSTREG_INVERTED | IS_RSTREGB_INVERTED)
            $fatal(1, "%m: inverted pins are not modelled");
        if (SDP == 0 && (!legal(READ_WIDTH_A) || !legal(READ_WIDTH_B) || !legal(WRITE_WIDTH_A) || !legal(WRITE_WIDTH_B)))
            $fatal(1, "%m: a port width is not 0, 1, 2, 4, 9, 18");
    end

    // the word offsets of an address for D data bits (the word's own low address bits ignored)
    function integer doff(input integer a, input integer d);
        doff = (d == 0) ? 0 : (a / d) * d;
    endfunction

    // ---------------------------------------------------------------- the two ports
    reg [31:0] lat_a = 0, lat_b = 0, reg_a = 0, reg_b = 0;
    reg [3:0]  latp_a = 0, latp_b = 0, regp_a = 0, regp_b = 0;
    initial begin
        lat_a = INIT_A;  latp_a = INIT_A >> RDA;  reg_a = INIT_A;  regp_a = INIT_A >> RDA;
        lat_b = INIT_B;  latp_b = INIT_B >> RDB;  reg_b = INIT_B;  regp_b = INIT_B >> RDB;
    end

    task automatic do_port(input integer port);
        integer rd, rp, wd, wp, k, off, poff, woff, wpoff;
        reg [13:0] addr;
        reg [15:0] din;
        reg [1:0] dinp;
        reg [7:0] we;
        reg [31:0] old, nw;
        reg [3:0] oldp, nwp;
        reg writing;
        begin
            if (port == 0) begin
                rd = RDA; rp = RPA; wd = WDA; wp = WPA; addr = ADDRARDADDR; din = DIADI; dinp = DIPADIP;
                we = {6'h0, WEA};
            end else begin
                rd = RDB; rp = RPB; wd = WDB; wp = WPB; addr = ADDRBWRADDR; din = DIBDI; dinp = DIPBDIP;
                we = {4'h0, WEBWE[3:0]};
            end
            off = doff({18'h0, addr[13:0]}, rd);
            poff = (rp == 0) ? 0 : off / 8;
            old = 0; oldp = 0;
            for (k = 0; k < rd; k = k + 1) old[k] = mem[off + k];
            for (k = 0; k < rp; k = k + 1) oldp[k] = memp[poff + k];
            writing = (wd != 0) && ((wd < 8) ? (we != 0) : ((we & ((8'h1 << (wd / 8)) - 8'h1)) != 0));
            woff = doff({18'h0, addr[13:0]}, wd);
            wpoff = (wp == 0) ? 0 : woff / 8;
            // The array is written at once (blocking) so that this port's WRITE_FIRST read sees the
            // new word; only this model reads the array, and the outputs change with <= below.
            /* verilator lint_off BLKSEQ */
            if (wd != 0) begin
                for (k = 0; k < wd; k = k + 1)
                    if ((wd < 8) ? (we != 0) : we[k / 8]) mem[woff + k] = din[k];
                for (k = 0; k < wp; k = k + 1)
                    if (we[k]) memp[wpoff + k] = dinp[k];
            end
            /* verilator lint_on BLKSEQ */
            nw = 0; nwp = 0;
            for (k = 0; k < rd; k = k + 1) nw[k] = mem[off + k];
            for (k = 0; k < rp; k = k + 1) nwp[k] = memp[poff + k];
            if (port == 0) begin
                if (!(writing && WRITE_MODE_A == "NO_CHANGE")) begin
                    lat_a <= (WRITE_MODE_A == "WRITE_FIRST") ? nw : old;
                    latp_a <= (WRITE_MODE_A == "WRITE_FIRST") ? nwp : oldp;
                end
            end else begin
                if (!(writing && WRITE_MODE_B == "NO_CHANGE")) begin
                    lat_b <= (WRITE_MODE_B == "WRITE_FIRST") ? nw : old;
                    latp_b <= (WRITE_MODE_B == "WRITE_FIRST") ? nwp : oldp;
                end
            end
        end
    endtask

    // ---------------------------------------------------------------- simple dual port, 512 x 36
    reg [31:0] sdp_d = 0;
    reg [3:0]  sdp_p = 0;
    initial begin
        sdp_d = {INIT_B[15:0], INIT_A[15:0]};
        sdp_p = {INIT_B[17:16], INIT_A[17:16]};
    end

    task automatic sdp_read;
        integer k, off;
        reg [31:0] d;
        reg [3:0] p;
        begin
            off = {18'h0, ADDRARDADDR[13:5], 5'h0};
            for (k = 0; k < 32; k = k + 1) d[k] = mem[off + k];
            for (k = 0; k < 4; k = k + 1) p[k] = memp[off / 8 + k];
            sdp_d <= d;
            sdp_p <= p;
        end
    endtask

    task automatic sdp_write;
        integer k, off;
        reg [31:0] d;
        reg [3:0] p;
        begin
            off = {18'h0, ADDRBWRADDR[13:5], 5'h0};
            d = {DIBDI, DIADI};
            p = {DIPBDIP, DIPADIP};
            /* verilator lint_off BLKSEQ */
            for (k = 0; k < 32; k = k + 1) if (WEBWE[k / 8]) mem[off + k] = d[k];
            for (k = 0; k < 4; k = k + 1) if (WEBWE[k]) memp[off / 8 + k] = p[k];
            /* verilator lint_on BLKSEQ */
        end
    endtask

    always @(posedge CLKARDCLK) begin
        if (SDP != 0) begin
            if (ENARDEN) begin
                if (RSTRAMARSTRAM) begin
                    sdp_d <= {SRVAL_B[15:0], SRVAL_A[15:0]};
                    sdp_p <= {SRVAL_B[17:16], SRVAL_A[17:16]};
                end else
                    sdp_read;
            end
        end else begin
            if (DOA_REG != 0) begin             // the output register samples the latch first
                if (RSTREG_PRIORITY_A == "RSTREG" ? RSTREGARSTREG : (RSTREGARSTREG && REGCEAREGCE)) begin
                    reg_a <= SRVAL_A; regp_a <= SRVAL_A >> RDA;
                end else if (REGCEAREGCE) begin
                    reg_a <= lat_a; regp_a <= latp_a;
                end
            end
            if (ENARDEN) begin
                if (RSTRAMARSTRAM && DOA_REG == 0) begin
                    lat_a <= SRVAL_A; latp_a <= SRVAL_A >> RDA;
                end else
                    do_port(0);
            end
        end
    end
    always @(posedge CLKBWRCLK) begin
        if (SDP != 0) begin
            if (ENBWREN) sdp_write;
        end else begin
            if (DOB_REG != 0) begin
                if (RSTREG_PRIORITY_B == "RSTREG" ? RSTREGB : (RSTREGB && REGCEB)) begin
                    reg_b <= SRVAL_B; regp_b <= SRVAL_B >> RDB;
                end else if (REGCEB) begin
                    reg_b <= lat_b; regp_b <= latp_b;
                end
            end
            if (ENBWREN) begin
                if (RSTRAMB && DOB_REG == 0) begin
                    lat_b <= SRVAL_B; latp_b <= SRVAL_B >> RDB;
                end else
                    do_port(1);
            end
        end
    end

    function [31:0] mask(input integer d);
        mask = (d >= 32) ? 32'hFFFF_FFFF : ((32'h1 << d) - 32'h1);
    endfunction
    function [3:0] pmask(input integer d);
        pmask = (d >= 4) ? 4'hF : ((4'h1 << d) - 4'h1);
    endfunction
    wire [31:0] out_a  = ((DOA_REG != 0) ? reg_a : lat_a) & mask(RDA);
    wire [31:0] out_b  = ((DOB_REG != 0) ? reg_b : lat_b) & mask(RDB);
    wire [3:0]  outp_a = ((DOA_REG != 0) ? regp_a : latp_a) & pmask(RPA);
    wire [3:0]  outp_b = ((DOB_REG != 0) ? regp_b : latp_b) & pmask(RPB);
    assign DOADO   = (SDP != 0) ? sdp_d[15:0]  : out_a[15:0];
    assign DOBDO   = (SDP != 0) ? sdp_d[31:16] : out_b[15:0];
    assign DOPADOP = (SDP != 0) ? sdp_p[1:0]   : outp_a[1:0];
    assign DOPBDOP = (SDP != 0) ? sdp_p[3:2]   : outp_b[1:0];
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
