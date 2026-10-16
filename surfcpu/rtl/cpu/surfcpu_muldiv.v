/*
SurfCPU multiplier and divider
--------------------
File: surfcpu_muldiv.v Date: 10/15/26 (SurfCPU)
--------------------
The M extension for the EX stage of surfcpu_core.

Multiply (MUL, MULH, MULHSU, MULHU) is one 33 x 33 signed product, combinational, in the
cycle the instruction leaves EX: the 33rd bit of each operand is its sign for a signed
operand and 0 for an unsigned one, which covers all four variants. Synthesis maps it to DSP
blocks.

Divide (DIV, DIVU, REM, REMU) is a restoring divider on the operands' magnitudes, one
quotient bit per cycle: div_start latches the operands, 32 cycles later div_done rises and
div_y holds the result until div_clear (the instruction left EX or was flushed). The
results the specification defines for the special cases are chosen explicitly:
division by zero gives a quotient of all ones and a remainder equal to the dividend, and
the signed overflow -2^31 / -1 gives -2^31 with remainder 0 (the magnitude arithmetic gives
that one by itself).
*/
module surfcpu_muldiv (
    input  wire        clk,
    input  wire        rst,
    input  wire [1:0]  op,          // funct3[1:0] of the M instruction (funct3[2] picks mul or div)
    input  wire [31:0] a,           // rs1
    input  wire [31:0] b,           // rs2
    output wire [31:0] mul_y,       // MUL/MULH/MULHSU/MULHU result (funct3 000..011)
    input  wire        div_start,   // start a DIV/DIVU/REM/REMU (funct3 100..111) on a and b
    input  wire        div_clear,   // forget the division (finished or abandoned)
    output wire        div_busy,
    output wire        div_done,    // div_y is valid
    output wire [31:0] div_y
);
    // ---------------------------------------------------------------- multiply
    wire               a_signed = (op[1:0] != 2'b11);   // MULH, MULHSU (MUL: low half is the same)
    wire               b_signed = (op[1:0] == 2'b01);   // MULH only
    wire signed [32:0] ma       = {a_signed & a[31], a};
    wire signed [32:0] mb       = {b_signed & b[31], b};
    // The 66-bit product's two top bits are only sign copies: no variant reads them.
    /* verilator lint_off UNUSEDSIGNAL */
    wire signed [65:0] prod     = ma * mb;
    /* verilator lint_on UNUSEDSIGNAL */
    assign mul_y = (op[1:0] == 2'b00) ? prod[31:0] : prod[63:32];

    // ---------------------------------------------------------------- divide
    reg        busy = 1'b0;
    reg        done = 1'b0;
    reg [4:0]  count;
    reg [31:0] quo;          // dividend magnitude, shifted out at the top; quotient bits in at the bottom
    reg [31:0] rem;          // partial remainder
    reg [31:0] divisor;      // divisor magnitude
    reg [31:0] dividend;     // the original rs1 (the remainder of a division by zero)
    reg        by_zero;
    reg        neg_q;        // negate the quotient magnitude
    reg        neg_r;        // negate the remainder magnitude
    reg        want_rem;     // REM/REMU

    wire        is_signed = !op[0];                     // DIV (100), REM (110)
    wire [32:0] shifted   = {rem, quo[31]};
    wire [32:0] diff      = shifted - {1'b0, divisor};
    wire        fits      = !diff[32];                  // shifted >= divisor

    always @(posedge clk) begin
        if (rst || div_clear) begin
            busy <= 1'b0;
            done <= 1'b0;
        end else if (div_start) begin
            busy     <= 1'b1;
            done     <= 1'b0;
            count    <= 5'd0;
            quo      <= (is_signed && a[31]) ? (32'd0 - a) : a;
            divisor  <= (is_signed && b[31]) ? (32'd0 - b) : b;
            rem      <= 32'd0;
            dividend <= a;
            by_zero  <= (b == 32'd0);
            neg_q    <= is_signed && (a[31] ^ b[31]);
            neg_r    <= is_signed && a[31];
            want_rem <= op[1];
        end else if (busy) begin
            rem   <= fits ? diff[31:0] : shifted[31:0];
            quo   <= {quo[30:0], fits};
            count <= count + 5'd1;
            if (count == 5'd31) begin
                busy <= 1'b0;
                done <= 1'b1;
            end
        end
    end

    wire [31:0] q = neg_q ? (32'd0 - quo) : quo;
    wire [31:0] r = neg_r ? (32'd0 - rem) : rem;

    assign div_busy = busy;
    assign div_done = done;
    assign div_y    = by_zero ? (want_rem ? dividend : 32'hFFFF_FFFF)
                              : (want_rem ? r : q);
endmodule
