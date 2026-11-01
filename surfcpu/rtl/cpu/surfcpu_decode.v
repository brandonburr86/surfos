/*
SurfCPU instruction decoder
--------------------
File: surfcpu_decode.v Date: 10/15/26 (SurfCPU)
--------------------
Combinational decode of one 32-bit instruction for the ID stage of surfcpu_core: which
registers it reads and writes, the ALU operation and operands, the immediate, its class,
and whether it is an illegal instruction, ECALL or EBREAK (docs/ISA.md section 1).

Everything not listed in docs/ISA.md is illegal: 16-bit (compressed) encodings, 0x00000000
and 0xFFFFFFFF, undefined funct3/funct7 combinations, SLLI/SRLI/SRAI with imm[5] = 1, and
every SYSTEM encoding with funct3 = 0 other than exactly ECALL, EBREAK, MRET and WFI (so SRET,
URET, SFENCE.VMA and the like). FENCE ignores its fm, pred/succ, rs1 and rd fields and
FENCE.I its imm, rs1 and rd fields, as the specification asks. Whether a CSR address exists
is decided later, at the commit point, by surfcpu_csr.

An illegal instruction, ECALL or EBREAK has every control output at 0 (it reads and writes
nothing); the core raises the exception at the commit point.
*/
module surfcpu_decode (
    input  wire [31:0] insn,
    output reg         illegal,     // not implemented: illegal-instruction exception (cause 2)
    output reg         ecall,       // environment call (cause 11)
    output reg         ebreak,      // breakpoint (cause 3)
    output reg         use_rs1,     // reads rs1 (insn[19:15])
    output reg         use_rs2,     // reads rs2 (insn[24:20])
    output reg         rd_wen,      // writes rd (insn[11:7]); 0 when rd is x0
    output reg  [3:0]  alu_op,      // {alternate, funct3} (surfcpu_defs.vh)
    output reg         a_pc,        // ALU operand a is the pc (AUIPC)
    output reg         a_zero,      // ALU operand a is 0 (LUI)
    output reg         b_imm,       // ALU operand b is the immediate (otherwise rs2)
    output reg  [31:0] imm,         // the sign-extended immediate of the instruction's format
    output reg         is_branch,   // BEQ BNE BLT BGE BLTU BGEU (condition = funct3)
    output reg         is_jal,
    output reg         is_jalr,
    output reg         is_load,     // LB LH LW LBU LHU (funct3)
    output reg         is_store,    // SB SH SW (funct3)
    output reg         is_mul,      // MUL MULH MULHSU MULHU (funct3)
    output reg         is_div,      // DIV DIVU REM REMU (funct3)
    output reg         is_csr,      // CSRRW CSRRS CSRRC and the immediate forms (funct3)
    output reg         csr_write,   // the CSR instruction writes: CSRRW[I], or rs1/uimm field != 0
    output reg         is_mret,
    output reg         is_wfi,
    output reg         is_fencei
);
`include "surfcpu_defs.vh"

    wire [6:0] opcode = insn[6:0];
    wire [2:0] funct3 = insn[14:12];
    wire [6:0] funct7 = insn[31:25];
    wire       rd_nz  = (insn[11:7] != 5'd0);
    wire       rs1_nz = (insn[19:15] != 5'd0);

    wire [31:0] imm_i = {{20{insn[31]}}, insn[31:20]};
    wire [31:0] imm_s = {{20{insn[31]}}, insn[31:25], insn[11:7]};
    wire [31:0] imm_b = {{19{insn[31]}}, insn[31], insn[7], insn[30:25], insn[11:8], 1'b0};
    wire [31:0] imm_u = {insn[31:12], 12'd0};
    wire [31:0] imm_j = {{11{insn[31]}}, insn[31], insn[19:12], insn[20], insn[30:21], 1'b0};

    reg legal;      // a known instruction other than ECALL/EBREAK

    always @(*) begin
        legal     = 1'b0;
        ecall     = 1'b0;
        ebreak    = 1'b0;
        use_rs1   = 1'b0;
        use_rs2   = 1'b0;
        rd_wen    = 1'b0;
        alu_op    = ALU_ADD;
        a_pc      = 1'b0;
        a_zero    = 1'b0;
        b_imm     = 1'b0;
        imm       = imm_i;
        is_branch = 1'b0;
        is_jal    = 1'b0;
        is_jalr   = 1'b0;
        is_load   = 1'b0;
        is_store  = 1'b0;
        is_mul    = 1'b0;
        is_div    = 1'b0;
        is_csr    = 1'b0;
        csr_write = 1'b0;
        is_mret   = 1'b0;
        is_wfi    = 1'b0;
        is_fencei = 1'b0;

        case (opcode)
            7'b0110111: begin                               // LUI: rd = 0 + imm
                legal = 1'b1; rd_wen = rd_nz;
                a_zero = 1'b1; b_imm = 1'b1; imm = imm_u;
            end
            7'b0010111: begin                               // AUIPC: rd = pc + imm
                legal = 1'b1; rd_wen = rd_nz;
                a_pc = 1'b1; b_imm = 1'b1; imm = imm_u;
            end
            7'b1101111: begin                               // JAL
                legal = 1'b1; rd_wen = rd_nz; is_jal = 1'b1; imm = imm_j;
            end
            7'b1100111: begin                               // JALR: target = (rs1 + imm) & ~1
                if (funct3 == 3'b000) begin
                    legal = 1'b1; rd_wen = rd_nz; is_jalr = 1'b1;
                    use_rs1 = 1'b1; b_imm = 1'b1; imm = imm_i;
                end
            end
            7'b1100011: begin                               // branches
                if (funct3 != 3'b010 && funct3 != 3'b011) begin
                    legal = 1'b1; is_branch = 1'b1;
                    use_rs1 = 1'b1; use_rs2 = 1'b1; imm = imm_b;
                end
            end
            7'b0000011: begin                               // loads: address = rs1 + imm
                if (funct3 == 3'b000 || funct3 == 3'b001 || funct3 == 3'b010 ||
                    funct3 == 3'b100 || funct3 == 3'b101) begin
                    legal = 1'b1; is_load = 1'b1; rd_wen = rd_nz;
                    use_rs1 = 1'b1; b_imm = 1'b1; imm = imm_i;
                end
            end
            7'b0100011: begin                               // stores: address = rs1 + imm
                if (funct3 == 3'b000 || funct3 == 3'b001 || funct3 == 3'b010) begin
                    legal = 1'b1; is_store = 1'b1;
                    use_rs1 = 1'b1; use_rs2 = 1'b1; b_imm = 1'b1; imm = imm_s;
                end
            end
            7'b0010011: begin                               // OP-IMM
                use_rs1 = 1'b1; rd_wen = rd_nz; b_imm = 1'b1; imm = imm_i;
                alu_op = {1'b0, funct3};
                case (funct3)
                    3'b001: legal = (funct7 == 7'b0000000);                       // SLLI
                    3'b101: begin                                                 // SRLI, SRAI
                        legal  = (funct7 == 7'b0000000) || (funct7 == 7'b0100000);
                        alu_op = {insn[30], funct3};
                    end
                    default: legal = 1'b1;
                endcase
            end
            7'b0110011: begin                               // OP and the M extension
                use_rs1 = 1'b1; use_rs2 = 1'b1; rd_wen = rd_nz;
                alu_op = {1'b0, funct3};
                if (funct7 == 7'b0000000) begin
                    legal = 1'b1;
                end else if (funct7 == 7'b0100000 && (funct3 == 3'b000 || funct3 == 3'b101)) begin
                    legal = 1'b1;                                                 // SUB, SRA
                    alu_op = {1'b1, funct3};
                end else if (funct7 == 7'b0000001) begin
                    legal  = 1'b1;
                    is_mul = !funct3[2];
                    is_div = funct3[2];
                end
            end
            7'b0001111: begin                               // MISC-MEM
                if (funct3 == 3'b000) begin
                    legal = 1'b1;                           // FENCE: accesses already complete in order
                end else if (funct3 == 3'b001) begin
                    legal = 1'b1; is_fencei = 1'b1;
                end
            end
            7'b1110011: begin                               // SYSTEM
                if (funct3 == 3'b000) begin
                    if (insn == 32'h0000_0073)      ecall   = 1'b1;
                    else if (insn == 32'h0010_0073) ebreak  = 1'b1;
                    else if (insn == 32'h3020_0073) begin legal = 1'b1; is_mret = 1'b1; end
                    else if (insn == 32'h1050_0073) begin legal = 1'b1; is_wfi  = 1'b1; end
                end else if (funct3 != 3'b100) begin
                    legal = 1'b1; is_csr = 1'b1; rd_wen = rd_nz;
                    use_rs1   = !funct3[2];                 // CSRRW, CSRRS, CSRRC read rs1
                    csr_write = (funct3[1:0] == 2'b01) || rs1_nz;
                end
            end
            default: ;                                      // every other opcode is illegal
        endcase

        illegal = !legal && !ecall && !ebreak;
        if (!legal) begin                                   // reads and writes nothing
            use_rs1   = 1'b0;
            use_rs2   = 1'b0;
            rd_wen    = 1'b0;
            is_branch = 1'b0;
            is_jal    = 1'b0;
            is_jalr   = 1'b0;
            is_load   = 1'b0;
            is_store  = 1'b0;
            is_mul    = 1'b0;
            is_div    = 1'b0;
            is_csr    = 1'b0;
            csr_write = 1'b0;
            is_mret   = 1'b0;
            is_wfi    = 1'b0;
            is_fencei = 1'b0;
        end
    end
endmodule
