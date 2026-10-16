/*
SurfCPU RAMB36E1 simulation model
--------------------
File: surf_ramb36e1_model.v Date: 10/15/26 (SurfCPU)
--------------------
A behavioural model of the 7-series RAMB36E1 block RAM for the post-synthesis simulations of
make test-soc-gate (Yosys 0.52's own cells_sim.v has only a timing shell for it). Written from
AMD UG473 (7 Series FPGAs Memory Resources): true dual-port mode, port widths 1, 2, 4, 9, 18
and 36 (0 = port direction unused), the 32768 data bits and 4096 parity bits laid out as the
INIT_xx and INITP_xx parameters give them (word w of a D-data-bit port holds data bits
w*D..w*D+D-1, parity bits w*P..w*P+P-1), byte write enables, the WRITE_FIRST, READ_FIRST and
NO_CHANGE modes, synchronous output reset to SRVAL. INIT bits that synthesis left undefined
load as 0, as in the FPGA. A write lands after the reads of the same edge, so with one clock
the other port reads the old word (READ_FIRST behaviour); collisions between different clocks
are not modelled. Unsupported features (simple dual-port and ECC modes, output registers,
cascading) stop the simulation, so a netlist that uses them cannot pass unnoticed.
Not a synthesizable or vendor file; only for simulation.
*/
`timescale 1ns/1ps
module RAMB36E1 #(
    parameter integer DOA_REG = 0,
    parameter integer DOB_REG = 0,
    parameter [35:0]  INIT_A = 36'h0,
    parameter [35:0]  INIT_B = 36'h0,
    parameter [71:0]  SRVAL_A = 72'h0,
    parameter [71:0]  SRVAL_B = 72'h0,
    parameter         RAM_EXTENSION_A = "NONE",
    parameter         RAM_EXTENSION_B = "NONE",
    parameter         RAM_MODE = "TDP",
    parameter integer READ_WIDTH_A = 0,
    parameter integer READ_WIDTH_B = 0,
    parameter integer WRITE_WIDTH_A = 0,
    parameter integer WRITE_WIDTH_B = 0,
    parameter         WRITE_MODE_A = "WRITE_FIRST",
    parameter         WRITE_MODE_B = "WRITE_FIRST",
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
    parameter         SIM_DEVICE = "7SERIES"
) (
    output reg  [31:0] DOADO,
    output reg  [31:0] DOBDO,
    output reg  [3:0]  DOPADOP,
    output reg  [3:0]  DOPBDOP,
    output wire        CASCADEOUTA,
    output wire        CASCADEOUTB,
    output wire [7:0]  ECCPARITY,
    output wire [8:0]  RDADDRECC,
    output wire        SBITERR,
    output wire        DBITERR,
    input  wire        ENARDEN,
    input  wire        CLKARDCLK,
    input  wire        RSTRAMARSTRAM,
    input  wire        RSTREGARSTREG,
    input  wire        CASCADEINA,
    input  wire        REGCEAREGCE,
    input  wire        ENBWREN,
    input  wire        CLKBWRCLK,
    input  wire        RSTRAMB,
    input  wire        RSTREGB,
    input  wire        CASCADEINB,
    input  wire        REGCEB,
    input  wire        INJECTDBITERR,
    input  wire        INJECTSBITERR,
    input  wire [15:0] ADDRARDADDR,
    input  wire [15:0] ADDRBWRADDR,
    input  wire [31:0] DIADI,
    input  wire [31:0] DIBDI,
    input  wire [3:0]  DIPADIP,
    input  wire [3:0]  DIPBDIP,
    input  wire [3:0]  WEA,
    input  wire [7:0]  WEBWE
);
    reg mem_d [0:32767];
    reg mem_p [0:4095];

    // data and parity bits of a port of width w (0 = unused)
    function automatic integer dbits;
        input integer w;
        begin
            case (w)
                1: dbits = 1;  2: dbits = 2;  4: dbits = 4;  9: dbits = 8;
                18: dbits = 16; 36: dbits = 32; default: dbits = 0;
            endcase
        end
    endfunction
    function automatic integer pbits;
        input integer w;
        begin
            case (w)
                9: pbits = 1; 18: pbits = 2; 36: pbits = 4; default: pbits = 0;
            endcase
        end
    endfunction

    integer i;
    initial begin
        if (RAM_MODE != "TDP")
            $fatal(1, "RAMB36E1 model %m: RAM_MODE %0s is not modelled", RAM_MODE);
        if (DOA_REG != 0 || DOB_REG != 0)
            $fatal(1, "RAMB36E1 model %m: output registers are not modelled");
        if (RAM_EXTENSION_A != "NONE" || RAM_EXTENSION_B != "NONE")
            $fatal(1, "RAMB36E1 model %m: cascading is not modelled");
        if ((READ_WIDTH_A != 0 && dbits(READ_WIDTH_A) == 0) || (READ_WIDTH_B != 0 && dbits(READ_WIDTH_B) == 0) ||
            (WRITE_WIDTH_A != 0 && dbits(WRITE_WIDTH_A) == 0) || (WRITE_WIDTH_B != 0 && dbits(WRITE_WIDTH_B) == 0))
            $fatal(1, "RAMB36E1 model %m: unsupported port width");
        if ((WRITE_WIDTH_A != 0 && READ_WIDTH_A != 0 && WRITE_WIDTH_A != READ_WIDTH_A) ||
            (WRITE_WIDTH_B != 0 && READ_WIDTH_B != 0 && WRITE_WIDTH_B != READ_WIDTH_B))
            $fatal(1, "RAMB36E1 model %m: different read and write widths on one port");
        for (i = 0; i < 256; i = i + 1) mem_d[0 + i] = (INIT_00[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[256 + i] = (INIT_01[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[512 + i] = (INIT_02[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[768 + i] = (INIT_03[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[1024 + i] = (INIT_04[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[1280 + i] = (INIT_05[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[1536 + i] = (INIT_06[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[1792 + i] = (INIT_07[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[2048 + i] = (INIT_08[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[2304 + i] = (INIT_09[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[2560 + i] = (INIT_0A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[2816 + i] = (INIT_0B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[3072 + i] = (INIT_0C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[3328 + i] = (INIT_0D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[3584 + i] = (INIT_0E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[3840 + i] = (INIT_0F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[4096 + i] = (INIT_10[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[4352 + i] = (INIT_11[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[4608 + i] = (INIT_12[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[4864 + i] = (INIT_13[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[5120 + i] = (INIT_14[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[5376 + i] = (INIT_15[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[5632 + i] = (INIT_16[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[5888 + i] = (INIT_17[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[6144 + i] = (INIT_18[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[6400 + i] = (INIT_19[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[6656 + i] = (INIT_1A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[6912 + i] = (INIT_1B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[7168 + i] = (INIT_1C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[7424 + i] = (INIT_1D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[7680 + i] = (INIT_1E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[7936 + i] = (INIT_1F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[8192 + i] = (INIT_20[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[8448 + i] = (INIT_21[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[8704 + i] = (INIT_22[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[8960 + i] = (INIT_23[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[9216 + i] = (INIT_24[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[9472 + i] = (INIT_25[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[9728 + i] = (INIT_26[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[9984 + i] = (INIT_27[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[10240 + i] = (INIT_28[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[10496 + i] = (INIT_29[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[10752 + i] = (INIT_2A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[11008 + i] = (INIT_2B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[11264 + i] = (INIT_2C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[11520 + i] = (INIT_2D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[11776 + i] = (INIT_2E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[12032 + i] = (INIT_2F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[12288 + i] = (INIT_30[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[12544 + i] = (INIT_31[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[12800 + i] = (INIT_32[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[13056 + i] = (INIT_33[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[13312 + i] = (INIT_34[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[13568 + i] = (INIT_35[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[13824 + i] = (INIT_36[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[14080 + i] = (INIT_37[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[14336 + i] = (INIT_38[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[14592 + i] = (INIT_39[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[14848 + i] = (INIT_3A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[15104 + i] = (INIT_3B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[15360 + i] = (INIT_3C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[15616 + i] = (INIT_3D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[15872 + i] = (INIT_3E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[16128 + i] = (INIT_3F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[16384 + i] = (INIT_40[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[16640 + i] = (INIT_41[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[16896 + i] = (INIT_42[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[17152 + i] = (INIT_43[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[17408 + i] = (INIT_44[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[17664 + i] = (INIT_45[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[17920 + i] = (INIT_46[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[18176 + i] = (INIT_47[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[18432 + i] = (INIT_48[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[18688 + i] = (INIT_49[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[18944 + i] = (INIT_4A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[19200 + i] = (INIT_4B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[19456 + i] = (INIT_4C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[19712 + i] = (INIT_4D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[19968 + i] = (INIT_4E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[20224 + i] = (INIT_4F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[20480 + i] = (INIT_50[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[20736 + i] = (INIT_51[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[20992 + i] = (INIT_52[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[21248 + i] = (INIT_53[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[21504 + i] = (INIT_54[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[21760 + i] = (INIT_55[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[22016 + i] = (INIT_56[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[22272 + i] = (INIT_57[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[22528 + i] = (INIT_58[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[22784 + i] = (INIT_59[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[23040 + i] = (INIT_5A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[23296 + i] = (INIT_5B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[23552 + i] = (INIT_5C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[23808 + i] = (INIT_5D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[24064 + i] = (INIT_5E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[24320 + i] = (INIT_5F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[24576 + i] = (INIT_60[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[24832 + i] = (INIT_61[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[25088 + i] = (INIT_62[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[25344 + i] = (INIT_63[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[25600 + i] = (INIT_64[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[25856 + i] = (INIT_65[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[26112 + i] = (INIT_66[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[26368 + i] = (INIT_67[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[26624 + i] = (INIT_68[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[26880 + i] = (INIT_69[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[27136 + i] = (INIT_6A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[27392 + i] = (INIT_6B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[27648 + i] = (INIT_6C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[27904 + i] = (INIT_6D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[28160 + i] = (INIT_6E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[28416 + i] = (INIT_6F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[28672 + i] = (INIT_70[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[28928 + i] = (INIT_71[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[29184 + i] = (INIT_72[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[29440 + i] = (INIT_73[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[29696 + i] = (INIT_74[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[29952 + i] = (INIT_75[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[30208 + i] = (INIT_76[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[30464 + i] = (INIT_77[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[30720 + i] = (INIT_78[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[30976 + i] = (INIT_79[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[31232 + i] = (INIT_7A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[31488 + i] = (INIT_7B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[31744 + i] = (INIT_7C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[32000 + i] = (INIT_7D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[32256 + i] = (INIT_7E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_d[32512 + i] = (INIT_7F[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[0 + i] = (INITP_00[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[256 + i] = (INITP_01[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[512 + i] = (INITP_02[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[768 + i] = (INITP_03[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[1024 + i] = (INITP_04[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[1280 + i] = (INITP_05[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[1536 + i] = (INITP_06[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[1792 + i] = (INITP_07[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[2048 + i] = (INITP_08[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[2304 + i] = (INITP_09[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[2560 + i] = (INITP_0A[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[2816 + i] = (INITP_0B[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[3072 + i] = (INITP_0C[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[3328 + i] = (INITP_0D[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[3584 + i] = (INITP_0E[i] === 1'b1);
        for (i = 0; i < 256; i = i + 1) mem_p[3840 + i] = (INITP_0F[i] === 1'b1);
        DOADO = INIT_A[31:0];
        DOPADOP = INIT_A[35:32];
        DOBDO = INIT_B[31:0];
        DOPBDOP = INIT_B[35:32];
    end

    // one clock edge of a port: returns the new output word (data in 31:0, parity in 35:32)
    task automatic port_edge;
        input  integer    rw;          // the port's width (read or write)
        input  [15:0]     addr;
        input  [31:0]     di;
        input  [3:0]      dip;
        input  [3:0]      we;
        input  integer    wmode;       // 0 WRITE_FIRST, 1 READ_FIRST, 2 NO_CHANGE
        input  integer    wwidth;
        input  [35:0]     cur;
        output [35:0]     out;
        integer d, p, base, pbase, k;
        reg wr;
        begin
            d = dbits(rw);
            p = pbits(rw);
            out = cur;
            if (d == 0) d = 1;            // an unused port: nothing to read or write
            base  = (addr[14:0] / d) * d;
            pbase = (addr[14:0] / d) * p;
            wr = (wwidth != 0) && ((d <= 8) ? we[0] : |we);
            if (rw != 0 && !(wr && wmode == 2)) begin
                for (k = 0; k < 32; k = k + 1) out[k] = (k < d) ? mem_d[base + k] : 1'b0;
                for (k = 0; k < 4; k = k + 1) out[32 + k] = (k < p) ? mem_p[pbase + k] : 1'b0;
            end
            if (wr) begin
                // the write lands after this edge's reads (nonblocking), so the other port
                // reading the same word at the same edge sees the old word, as with READ_FIRST
                for (k = 0; k < d; k = k + 1)
                    if ((d <= 8) || we[k / 8]) begin
                        mem_d[base + k] <= di[k];
                        if (wmode == 0) out[k] = di[k];
                    end
                for (k = 0; k < p; k = k + 1)
                    if ((d <= 8) || we[k]) begin
                        mem_p[pbase + k] <= dip[k];
                        if (wmode == 0) out[32 + k] = dip[k];
                    end
            end
        end
    endtask

    function automatic integer mode_of;
        input [8*11-1:0] m;
        begin
            mode_of = (m == "READ_FIRST") ? 1 : (m == "NO_CHANGE") ? 2 : 0;
        end
    endfunction

    reg [35:0] oa, ob;
    always @(posedge CLKARDCLK) begin
        if (ENARDEN) begin
            port_edge((READ_WIDTH_A != 0) ? READ_WIDTH_A : WRITE_WIDTH_A, ADDRARDADDR, DIADI,
                      DIPADIP, WEA, mode_of(WRITE_MODE_A), WRITE_WIDTH_A, {DOPADOP, DOADO}, oa);
            if (RSTRAMARSTRAM) oa = SRVAL_A[35:0];
            {DOPADOP, DOADO} <= oa;
        end
    end
    always @(posedge CLKBWRCLK) begin
        if (ENBWREN) begin
            port_edge((READ_WIDTH_B != 0) ? READ_WIDTH_B : WRITE_WIDTH_B, ADDRBWRADDR, DIBDI,
                      DIPBDIP, WEBWE[3:0], mode_of(WRITE_MODE_B), WRITE_WIDTH_B, {DOPBDOP, DOBDO},
                      ob);
            if (RSTRAMB) ob = SRVAL_B[35:0];
            {DOPBDOP, DOBDO} <= ob;
        end
    end

    assign CASCADEOUTA = 1'b0;
    assign CASCADEOUTB = 1'b0;
    assign ECCPARITY   = 8'h0;
    assign RDADDRECC   = 9'h0;
    assign SBITERR     = 1'b0;
    assign DBITERR     = 1'b0;
endmodule
