/*
SurfCPU simulation model of AMD's RAMB36E1
--------------------
File: RAMB36E1.v Date: 10/16/26 (SurfCPU)
--------------------
Yosys's cells_sim.v (the models the open flow copies next to each netlist) declares the block
RAMs without any behaviour, so a netlist with block RAM cannot be simulated with it alone.
This model has AMD's name, parameters and ports (UG953) and the behaviour of UG473 for what
Yosys's block RAM mapping uses: true dual port mode (RAM_MODE "TDP"), data widths 1, 2, 4, 9,
18, 36 on each port (read and write widths may differ), byte-wide write enables (WEA,
WEBWE[3:0]), WRITE_MODE READ_FIRST, WRITE_FIRST and NO_CHANGE, the optional output register
(DOx_REG with REGCE and RSTREG), RSTRAM, INIT_A/B and SRVAL_A/B, and the initial contents of
INIT_xx/INITP_xx. Memory layout (UG473): a word of D data bits at address a holds data bits
a*D .. a*D+D-1 of the 32768-bit array, whose bits 256*i .. 256*i+255 are INIT_i, and P = D/8
parity bits a*P .. in the 4096-bit parity array (INITP_i). The address is ADDR[14:0] with
the word's low bits ignored (ADDR[14:3] for width 9). Anything else (simple dual port,
cascading, ECC, inverted pins, an INIT_FILE) stops the simulation at time 0 rather than be
modelled wrong. Simulation only; a collision of two ports on one word is not modelled.
*/
`timescale 1ps/1ps
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
module RAMB36E1 #(
    parameter integer DOA_REG = 0,
    parameter integer DOB_REG = 0,
    parameter EN_ECC_READ = "FALSE",
    parameter EN_ECC_WRITE = "FALSE",
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
    parameter [255:0] INIT_40 = 256'h0,
    parameter [255:0] INIT_41 = 256'h0,
    parameter [255:0] INIT_42 = 256'h0,
    parameter [255:0] INIT_43 = 256'h0,
    parameter [255:0] INIT_44 = 256'h0,
    parameter [255:0] INIT_45 = 256'h0,
    parameter [255:0] INIT_46 = 256'h0,
    parameter [255:0] INIT_47 = 256'h0,
    parameter [255:0] INIT_48 = 256'h0,
    parameter [255:0] INIT_49 = 256'h0,
    parameter [255:0] INIT_4A = 256'h0,
    parameter [255:0] INIT_4B = 256'h0,
    parameter [255:0] INIT_4C = 256'h0,
    parameter [255:0] INIT_4D = 256'h0,
    parameter [255:0] INIT_4E = 256'h0,
    parameter [255:0] INIT_4F = 256'h0,
    parameter [255:0] INIT_50 = 256'h0,
    parameter [255:0] INIT_51 = 256'h0,
    parameter [255:0] INIT_52 = 256'h0,
    parameter [255:0] INIT_53 = 256'h0,
    parameter [255:0] INIT_54 = 256'h0,
    parameter [255:0] INIT_55 = 256'h0,
    parameter [255:0] INIT_56 = 256'h0,
    parameter [255:0] INIT_57 = 256'h0,
    parameter [255:0] INIT_58 = 256'h0,
    parameter [255:0] INIT_59 = 256'h0,
    parameter [255:0] INIT_5A = 256'h0,
    parameter [255:0] INIT_5B = 256'h0,
    parameter [255:0] INIT_5C = 256'h0,
    parameter [255:0] INIT_5D = 256'h0,
    parameter [255:0] INIT_5E = 256'h0,
    parameter [255:0] INIT_5F = 256'h0,
    parameter [255:0] INIT_60 = 256'h0,
    parameter [255:0] INIT_61 = 256'h0,
    parameter [255:0] INIT_62 = 256'h0,
    parameter [255:0] INIT_63 = 256'h0,
    parameter [255:0] INIT_64 = 256'h0,
    parameter [255:0] INIT_65 = 256'h0,
    parameter [255:0] INIT_66 = 256'h0,
    parameter [255:0] INIT_67 = 256'h0,
    parameter [255:0] INIT_68 = 256'h0,
    parameter [255:0] INIT_69 = 256'h0,
    parameter [255:0] INIT_6A = 256'h0,
    parameter [255:0] INIT_6B = 256'h0,
    parameter [255:0] INIT_6C = 256'h0,
    parameter [255:0] INIT_6D = 256'h0,
    parameter [255:0] INIT_6E = 256'h0,
    parameter [255:0] INIT_6F = 256'h0,
    parameter [255:0] INIT_70 = 256'h0,
    parameter [255:0] INIT_71 = 256'h0,
    parameter [255:0] INIT_72 = 256'h0,
    parameter [255:0] INIT_73 = 256'h0,
    parameter [255:0] INIT_74 = 256'h0,
    parameter [255:0] INIT_75 = 256'h0,
    parameter [255:0] INIT_76 = 256'h0,
    parameter [255:0] INIT_77 = 256'h0,
    parameter [255:0] INIT_78 = 256'h0,
    parameter [255:0] INIT_79 = 256'h0,
    parameter [255:0] INIT_7A = 256'h0,
    parameter [255:0] INIT_7B = 256'h0,
    parameter [255:0] INIT_7C = 256'h0,
    parameter [255:0] INIT_7D = 256'h0,
    parameter [255:0] INIT_7E = 256'h0,
    parameter [255:0] INIT_7F = 256'h0,
    parameter [255:0] INITP_00 = 256'h0,
    parameter [255:0] INITP_01 = 256'h0,
    parameter [255:0] INITP_02 = 256'h0,
    parameter [255:0] INITP_03 = 256'h0,
    parameter [255:0] INITP_04 = 256'h0,
    parameter [255:0] INITP_05 = 256'h0,
    parameter [255:0] INITP_06 = 256'h0,
    parameter [255:0] INITP_07 = 256'h0,
    parameter [255:0] INITP_08 = 256'h0,
    parameter [255:0] INITP_09 = 256'h0,
    parameter [255:0] INITP_0A = 256'h0,
    parameter [255:0] INITP_0B = 256'h0,
    parameter [255:0] INITP_0C = 256'h0,
    parameter [255:0] INITP_0D = 256'h0,
    parameter [255:0] INITP_0E = 256'h0,
    parameter [255:0] INITP_0F = 256'h0,
    parameter INIT_A = 0,
    parameter INIT_B = 0,
    parameter INIT_FILE = "NONE",
    parameter RAM_EXTENSION_A = "NONE",
    parameter RAM_EXTENSION_B = "NONE",
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
    output wire CASCADEOUTA,
    output wire CASCADEOUTB,
    output wire [31:0] DOADO,
    output wire [31:0] DOBDO,
    output wire [3:0] DOPADOP,
    output wire [3:0] DOPBDOP,
    output wire [7:0] ECCPARITY,
    output wire [8:0] RDADDRECC,
    output wire SBITERR,
    output wire DBITERR,
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
    input  wire CASCADEINA,
    input  wire CASCADEINB,
    input  wire INJECTDBITERR,
    input  wire INJECTSBITERR,
    input  wire [15:0] ADDRARDADDR,
    input  wire [15:0] ADDRBWRADDR,
    input  wire [31:0] DIADI,
    input  wire [31:0] DIBDI,
    input  wire [3:0] DIPADIP,
    input  wire [3:0] DIPBDIP,
    input  wire [3:0] WEA,
    input  wire [7:0] WEBWE
);
    localparam integer NBITS = 32768;
    localparam integer PBITS = 4096;

    function integer dbits(input integer w);     // data bits of a port width
        dbits = (w >= 36) ? 32 : (w >= 18) ? 16 : (w >= 9) ? 8 : w;
    endfunction
    function integer pbits_of(input integer w);  // parity bits of a port width
        pbits_of = (w >= 36) ? 4 : (w >= 18) ? 2 : (w >= 9) ? 1 : 0;
    endfunction
    function legal(input integer w);
        legal = (w == 0) || (w == 1) || (w == 2) || (w == 4) || (w == 9) || (w == 18) || (w <= 36 && w == 36);
    endfunction

    localparam integer RDA = dbits(READ_WIDTH_A),  RPA = pbits_of(READ_WIDTH_A);
    localparam integer RDB = dbits(READ_WIDTH_B),  RPB = pbits_of(READ_WIDTH_B);
    localparam integer WDA = dbits(WRITE_WIDTH_A), WPA = pbits_of(WRITE_WIDTH_A);
    localparam integer WDB = dbits(WRITE_WIDTH_B), WPB = pbits_of(WRITE_WIDTH_B);

    reg [NBITS-1:0] mem;
    reg [PBITS-1:0] memp;
    initial begin
        mem  = {INIT_7F, INIT_7E, INIT_7D, INIT_7C, INIT_7B, INIT_7A, INIT_79, INIT_78, INIT_77, INIT_76, INIT_75, INIT_74, INIT_73, INIT_72, INIT_71, INIT_70, INIT_6F, INIT_6E, INIT_6D, INIT_6C, INIT_6B, INIT_6A, INIT_69, INIT_68, INIT_67, INIT_66, INIT_65, INIT_64, INIT_63, INIT_62, INIT_61, INIT_60, INIT_5F, INIT_5E, INIT_5D, INIT_5C, INIT_5B, INIT_5A, INIT_59, INIT_58, INIT_57, INIT_56, INIT_55, INIT_54, INIT_53, INIT_52, INIT_51, INIT_50, INIT_4F, INIT_4E, INIT_4D, INIT_4C, INIT_4B, INIT_4A, INIT_49, INIT_48, INIT_47, INIT_46, INIT_45, INIT_44, INIT_43, INIT_42, INIT_41, INIT_40, INIT_3F, INIT_3E, INIT_3D, INIT_3C, INIT_3B, INIT_3A, INIT_39, INIT_38, INIT_37, INIT_36, INIT_35, INIT_34, INIT_33, INIT_32, INIT_31, INIT_30, INIT_2F, INIT_2E, INIT_2D, INIT_2C, INIT_2B, INIT_2A, INIT_29, INIT_28, INIT_27, INIT_26, INIT_25, INIT_24, INIT_23, INIT_22, INIT_21, INIT_20, INIT_1F, INIT_1E, INIT_1D, INIT_1C, INIT_1B, INIT_1A, INIT_19, INIT_18, INIT_17, INIT_16, INIT_15, INIT_14, INIT_13, INIT_12, INIT_11, INIT_10, INIT_0F, INIT_0E, INIT_0D, INIT_0C, INIT_0B, INIT_0A, INIT_09, INIT_08, INIT_07, INIT_06, INIT_05, INIT_04, INIT_03, INIT_02, INIT_01, INIT_00};
        memp = {INITP_0F, INITP_0E, INITP_0D, INITP_0C, INITP_0B, INITP_0A, INITP_09, INITP_08, INITP_07, INITP_06, INITP_05, INITP_04, INITP_03, INITP_02, INITP_01, INITP_00};
        if (RAM_MODE != "TDP") $fatal(1, "%m: RAM_MODE %0s is not modelled (TDP only)", RAM_MODE);
        if (RAM_EXTENSION_A != "NONE" || RAM_EXTENSION_B != "NONE")
            $fatal(1, "%m: RAM_EXTENSION (cascading) is not modelled");
        if (EN_ECC_READ != "FALSE" || EN_ECC_WRITE != "FALSE") $fatal(1, "%m: ECC is not modelled");
        if (INIT_FILE != "NONE" && INIT_FILE != "") $fatal(1, "%m: INIT_FILE is not modelled");
        if (IS_CLKARDCLK_INVERTED | IS_CLKBWRCLK_INVERTED | IS_ENARDEN_INVERTED | IS_ENBWREN_INVERTED | IS_RSTRAMARSTRAM_INVERTED | IS_RSTRAMB_INVERTED | IS_RSTREGARSTREG_INVERTED | IS_RSTREGB_INVERTED)
            $fatal(1, "%m: inverted pins are not modelled");
        if (!legal(READ_WIDTH_A) || !legal(READ_WIDTH_B) || !legal(WRITE_WIDTH_A) || !legal(WRITE_WIDTH_B))
            $fatal(1, "%m: a port width is not 0, 1, 2, 4, 9, 18, 36");
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
        reg [15:0] addr;
        reg [31:0] din;
        reg [3:0] dinp;
        reg [7:0] we;
        reg [31:0] old, nw;
        reg [3:0] oldp, nwp;
        reg writing;
        begin
            if (port == 0) begin
                rd = RDA; rp = RPA; wd = WDA; wp = WPA; addr = ADDRARDADDR; din = DIADI; dinp = DIPADIP;
                we = {4'h0, WEA};
            end else begin
                rd = RDB; rp = RPB; wd = WDB; wp = WPB; addr = ADDRBWRADDR; din = DIBDI; dinp = DIPBDIP;
                we = {4'h0, WEBWE[3:0]};
            end
            off = doff({17'h0, addr[14:0]}, rd);
            poff = (rp == 0) ? 0 : off / 8;
            old = 0; oldp = 0;
            for (k = 0; k < rd; k = k + 1) old[k] = mem[off + k];
            for (k = 0; k < rp; k = k + 1) oldp[k] = memp[poff + k];
            writing = (wd != 0) && ((wd < 8) ? (we != 0) : ((we & ((8'h1 << (wd / 8)) - 8'h1)) != 0));
            woff = doff({17'h0, addr[14:0]}, wd);
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

    always @(posedge CLKARDCLK) begin
        if (DOA_REG != 0) begin                 // the output register samples the latch first
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
    always @(posedge CLKBWRCLK) begin
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
    assign DOADO   = out_a[31:0];
    assign DOBDO   = out_b[31:0];
    assign DOPADOP = outp_a[3:0];
    assign DOPBDOP = outp_b[3:0];
    assign CASCADEOUTA = 1'b0;
    assign CASCADEOUTB = 1'b0;
    assign ECCPARITY = 8'h0;
    assign RDADDRECC = 9'h0;
    assign SBITERR = 1'b0;
    assign DBITERR = 1'b0;
endmodule
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */
