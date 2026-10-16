/*
SurfCPU ALU
--------------------
File: surfcpu_alu.v Date: 10/15/26 (SurfCPU)
--------------------
The integer operations of RV32I for the EX stage of surfcpu_core. op is {alternate, funct3}
as in surfcpu_defs.vh: ADD/SUB, SLL, SLT, SLTU, XOR, SRL/SRA, OR, AND. Shifts use b[4:0].
Purely combinational.
*/
module surfcpu_alu (
    input  wire [3:0]  op,
    input  wire [31:0] a,
    input  wire [31:0] b,
    output reg  [31:0] y
);
    // Signed operations get their own wires: inside a ?: with unsigned operands Verilog would
    // evaluate them unsigned (a logical shift and an unsigned compare).
    wire [31:0] sum  = op[3] ? a - b : a + b;
    wire [31:0] sra  = $signed(a) >>> b[4:0];
    wire        lt   = $signed(a) < $signed(b);
    wire        ltu  = a < b;

    always @(*) begin
        case (op[2:0])
            3'b000:  y = sum;
            3'b001:  y = a << b[4:0];
            3'b010:  y = {31'd0, lt};
            3'b011:  y = {31'd0, ltu};
            3'b100:  y = a ^ b;
            3'b101:  y = op[3] ? sra : (a >> b[4:0]);
            3'b110:  y = a | b;
            default: y = a & b;
        endcase
    end
endmodule
