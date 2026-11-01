/*
SurfCPU open flow: a shift chain that Yosys makes an SRL
--------------------
File: srl_chain.v Date: 10/18/26 (SurfGPU)
--------------------
srl_chain delays a 2-bit value by three clocks in registers without a reset: synth_xilinx makes
them two SRL16E, which the open flow's LVS cannot check (devenv/openxc7/srlcheck.py refuses the
netlist). srl_chain_reset is the same delay with a synchronous reset, which keeps it in flip-flops
(the fix surf_video's byte-lane delay got, docs/GPU.md section 12.2). Test fixtures for make
test-flow, not part of any design.
*/
module srl_chain (
    input  wire       clk,
    input  wire [1:0] d,
    output wire [1:0] q
);
    reg [1:0] s1 = 2'd0, s2 = 2'd0, s3 = 2'd0;
    always @(posedge clk) begin
        s1 <= d;
        s2 <= s1;
        s3 <= s2;
    end
    assign q = s3;
endmodule

module srl_chain_reset (
    input  wire       clk,
    input  wire       rst,
    input  wire [1:0] d,
    output wire [1:0] q
);
    reg [1:0] s1 = 2'd0, s2 = 2'd0, s3 = 2'd0;
    always @(posedge clk) begin
        if (rst) begin
            s1 <= 2'd0;
            s2 <= 2'd0;
            s3 <= 2'd0;
        end else begin
            s1 <= d;
            s2 <= s1;
            s3 <= s2;
        end
    end
    assign q = s3;
endmodule
