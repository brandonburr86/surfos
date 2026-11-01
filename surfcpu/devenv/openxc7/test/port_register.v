// synthcheck regression (tools/openxc7-flow-test.sh): a register inside an instance drives
// the top's output port. In the elaborated RTL its bits carry the port's name, the
// shortest; in the netlist an OBUF stands between and the port's name moves to the pad
// side. The check must credit the 32 flip-flops to u_sub in both, not to (top) in one.
module port_register_sub (
    input  wire        clk,
    input  wire [31:0] d,
    output reg  [31:0] q
);
    always @(posedge clk) q <= d;
endmodule

module port_register (
    input  wire        clk,
    input  wire [31:0] d,
    output wire [31:0] q
);
    port_register_sub u_sub (.clk(clk), .d(d), .q(q));
endmodule
