/*
SurfCPU core definitions
--------------------
File: surfcpu_defs.vh Date: 10/15/26 (SurfCPU)
--------------------
Encodings shared by the modules of surfcpu_core: the ALU operation code and the exception
cause numbers of docs/ISA.md section 3. `include it inside a module body (it holds
localparams, so it has no include guard), like rtl/generated/surfcpu_soc.vh.

The ALU operation is {alternate, funct3}, the same bits the OP and OP-IMM encodings carry
(alternate = instruction bit 30 for SUB and SRA/SRAI).
*/
/* verilator lint_off UNUSEDPARAM */
localparam [3:0] ALU_ADD  = 4'b0000;
localparam [3:0] ALU_SUB  = 4'b1000;
localparam [3:0] ALU_SLL  = 4'b0001;
localparam [3:0] ALU_SLT  = 4'b0010;
localparam [3:0] ALU_SLTU = 4'b0011;
localparam [3:0] ALU_XOR  = 4'b0100;
localparam [3:0] ALU_SRL  = 4'b0101;
localparam [3:0] ALU_SRA  = 4'b1101;
localparam [3:0] ALU_OR   = 4'b0110;
localparam [3:0] ALU_AND  = 4'b0111;

// mcause exception codes (interrupts set bit 31 as well)
localparam [4:0] CAUSE_FETCH_MISALIGNED = 5'd0;
localparam [4:0] CAUSE_FETCH_FAULT      = 5'd1;
localparam [4:0] CAUSE_ILLEGAL          = 5'd2;
localparam [4:0] CAUSE_BREAKPOINT       = 5'd3;
localparam [4:0] CAUSE_LOAD_MISALIGNED  = 5'd4;
localparam [4:0] CAUSE_LOAD_FAULT       = 5'd5;
localparam [4:0] CAUSE_STORE_MISALIGNED = 5'd6;
localparam [4:0] CAUSE_STORE_FAULT      = 5'd7;
localparam [4:0] CAUSE_ECALL_M          = 5'd11;
localparam [4:0] CAUSE_IRQ_SOFTWARE     = 5'd3;
localparam [4:0] CAUSE_IRQ_TIMER        = 5'd7;
localparam [4:0] CAUSE_IRQ_EXTERNAL     = 5'd11;
/* verilator lint_on UNUSEDPARAM */
