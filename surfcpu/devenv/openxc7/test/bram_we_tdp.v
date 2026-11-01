/*
SurfCPU open-flow block RAM write-enable probe: two byte enables
--------------------
File: bram_we_tdp.v Date: 10/16/26 (SurfCPU)
--------------------
A flow test, not for a board: a 1024 x 16 memory with a write enable per byte between the
Arty's switches, buttons and LEDs. Yosys maps it to one RAMB18E1 whose 18-bit port A writes,
WEA = {we[1], we[0]}; nextpnr gives each byte enable two site pins (WEA0/1 and WEA2/3), and
the LVS extractor read WEA[1] from WEA1 and reported it different until
devenv/openxc7/bramwe.py (tools/openxc7-checks-test.sh, docs/evidence/flow/checkers.txt).
*/
module bram_we_tdp (
    input  wire       clk100,
    input  wire [3:0] sw,
    input  wire [3:0] btn,
    output wire [3:0] led
);
    reg [15:0] mem [0:1023];
    reg [15:0] q = 16'h0, wd = 16'h0;
    reg [9:0]  wa = 10'h0, ra = 10'h0;
    reg [1:0]  we = 2'b0;
    always @(posedge clk100) begin
        wa <= wa + {6'h0, sw};
        ra <= ra + {6'h0, btn};
        wd <= {wd[14:0], wd[15] ^ sw[0] ^ btn[0]};
        we <= {sw[2] ^ btn[2], sw[1] ^ btn[1]};
        if (we[0]) mem[wa][7:0]  <= wd[7:0];
        if (we[1]) mem[wa][15:8] <= wd[15:8];
        q <= mem[ra];
    end
    assign led = {^q[15:12], ^q[11:8], ^q[7:4], ^q[3:0]};
endmodule
