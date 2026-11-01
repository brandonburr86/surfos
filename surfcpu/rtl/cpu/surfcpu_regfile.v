/*
SurfCPU register file
--------------------
File: surfcpu_regfile.v Date: 10/15/26 (SurfCPU)
--------------------
The 32 x 32-bit integer registers: two asynchronous read ports (the ID stage) and one write
port (the WB stage), so it maps to distributed RAM. Reads return the stored value: the
caller supplies x0 = 0 and the bypass of a write in the same cycle. The contents are not
reset (docs/ISA.md: registers other than x0 are undefined after reset); x0 is never written.
*/
module surfcpu_regfile (
    input  wire        clk,
    input  wire [4:0]  ra1,
    output wire [31:0] rd1,
    input  wire [4:0]  ra2,
    output wire [31:0] rd2,
    input  wire        we,
    input  wire [4:0]  wa,
    input  wire [31:0] wd
);
    reg [31:0] regs [0:31];

    always @(posedge clk) begin
        if (we) regs[wa] <= wd;
    end

    assign rd1 = regs[ra1];
    assign rd2 = regs[ra2];
endmodule
