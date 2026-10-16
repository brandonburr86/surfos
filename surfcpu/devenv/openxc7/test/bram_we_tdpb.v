/*
SurfCPU open-flow block RAM write-enable probe: port B of a true dual port RAMB18E1
--------------------
File: bram_we_tdpb.v Date: 10/16/26 (SurfCPU)
--------------------
A flow test, not for a board: one RAMB18E1 instantiated directly in true dual port mode, port A
reading (18 bits), port B writing 18 bits with a write enable per byte, WEBWE = {0, 0, we[1],
we[0]}, between the Arty's switches, buttons and LEDs. Vivado gives byte i of an 18-bit port the
site pins 2i and 2i+1 (WEBWE0/1 for we[0], WEBWE2/3 for we[1]). nextpnr 1.0.0 routes port B's
enables in this mode as we[0], we[1], we[0], we[1] on WEBWE0..3 (round ddr-3: a defect of
nextpnr's, not of the netlist), and the flow's LVS, through devenv/openxc7/bramwe.py, reports
both pin pairs and fails the build (tools/openxc7-checks-test.sh, docs/evidence/flow/checkers.txt).
No SurfCPU design has such a port.
*/
module bram_we_tdpb (
    input  wire       clk100,
    input  wire [3:0] sw,
    input  wire [3:0] btn,
    output wire [3:0] led
);
    reg [15:0] wd = 16'h0;
    reg [9:0]  wa = 10'h0, ra = 10'h0;
    reg [1:0]  we = 2'b0;
    wire [15:0] q;
    always @(posedge clk100) begin
        wa <= wa + {6'h0, sw};
        ra <= ra + {6'h0, btn};
        wd <= {wd[14:0], wd[15] ^ sw[0] ^ btn[0]};
        we <= {sw[2] ^ btn[2], sw[1] ^ btn[1]};
    end
    RAMB18E1 #(
        .RAM_MODE("TDP"), .READ_WIDTH_A(18), .WRITE_WIDTH_A(18), .READ_WIDTH_B(18), .WRITE_WIDTH_B(18),
        .DOA_REG(0), .DOB_REG(0), .WRITE_MODE_A("WRITE_FIRST"), .WRITE_MODE_B("WRITE_FIRST")
    ) ram (
        .CLKARDCLK(clk100), .ENARDEN(1'b1), .REGCEAREGCE(1'b0), .RSTRAMARSTRAM(1'b0), .RSTREGARSTREG(1'b0),
        .ADDRARDADDR({ra, 4'b0000}), .WEA(2'b00), .DIADI(16'h0), .DIPADIP(2'b00), .DOADO(q), .DOPADOP(),
        .CLKBWRCLK(clk100), .ENBWREN(1'b1), .REGCEB(1'b0), .RSTRAMB(1'b0), .RSTREGB(1'b0),
        .ADDRBWRADDR({wa, 4'b0000}), .WEBWE({2'b00, we}), .DIBDI(wd), .DIPBDIP(2'b00), .DOBDO(), .DOPBDOP()
    );
    assign led = {^q[15:12], ^q[11:8], ^q[7:4], ^q[3:0]};
endmodule
