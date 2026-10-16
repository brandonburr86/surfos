/*
SurfCPU open-flow block RAM write-enable probe: simple dual port
--------------------
File: bram_we_sdp.v Date: 10/16/26 (SurfCPU)
--------------------
A flow test, not for a board: a 512 x 23 simple dual port memory between the Arty's switches,
buttons and LEDs, as LiteX's VexRiscv instruction-cache tags were (round ddr-2). Yosys maps it
to one RAMB18E1 with the 36-bit write port, WEBWE = {0, we, we, we}: three byte lanes on one
enable and the fourth tied to 0. nextpnr gives each byte enable two site pins (WEBWE 2i and
2i+1); the LVS extractor read them one to one and reported WEBWE[3] different until
devenv/openxc7/bramwe.py (tools/openxc7-lvs-bram-test.sh, docs/evidence/flow/lvs-bram.txt).
*/
module bram_we_sdp (
    input  wire       clk100,
    input  wire [3:0] sw,
    input  wire [3:0] btn,
    output wire [3:0] led
);
    reg [22:0] mem [0:511];
    reg [22:0] q = 23'h0;
    reg [8:0]  wa = 9'h0, ra = 9'h0;
    reg [22:0] wd = 23'h0;
    reg        we = 1'b0;
    always @(posedge clk100) begin
        wa <= wa + {5'h0, sw};
        ra <= ra + {5'h0, btn};
        wd <= {wd[21:0], wd[22] ^ sw[0] ^ btn[0]};
        we <= sw[1] ^ btn[1];
        if (we) mem[wa] <= wd;
        q <= mem[ra];
    end
    assign led = {^q[22:16], ^q[15:8], ^q[7:0], q[0]};
endmodule
