/*
SurfCPU Yosys DSP-packing reproducer
--------------------
File: dsp_counter.v Date: 10/15/26 (SurfCPU)
--------------------
The smallest form found of a synthesis defect the io track hit in surf_video: a counter
whose value is also multiplied. Yosys's xilinx_dsp pass (synth_xilinx in 0.52 and in the
0.69 of the openXC7 image) takes the counter's increment as the DSP48E1 pre-adder and the
counter register as the DSP's AD pipeline register, which leaves the fabric without the
counter: cnt and hs below lose their source. tools/openxc7-build.sh's synthesis check must
flag it (devenv/openxc7/synthcheck.py); with -nodsp it synthesizes correctly. A test
fixture, not part of any design.
*/
module dsp_counter (
    input  wire       clk,
    input  wire       rst,
    output wire [9:0] cnt,       // the counter itself
    output wire       hs,        // a decode of it
    output wire [3:0] level      // bits of cnt * 205 (cnt / 40 below 640)
);
    reg [9:0] hcnt = 10'd0;
    always @(posedge clk) begin
        if (rst || hcnt == 10'd799) hcnt <= 10'd0;
        else                        hcnt <= hcnt + 10'd1;
    end
    wire [17:0] h205 = {8'd0, hcnt} * 18'd205;
    assign cnt   = hcnt;
    assign hs    = !((hcnt >= 10'd656) && (hcnt < 10'd752));
    assign level = h205[16:13];
endmodule
