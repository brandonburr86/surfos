/*
SurfCPU pulse synchronizer
--------------------
File: surf_pulse_sync.v Date: 10/15/26 (SurfCPU)
--------------------
Carries single-cycle event pulses from src_clk to dst_clk with a toggle flip-flop: each
src_pulse gives exactly one dst_pulse two to three dst_clk cycles later. Pulses must be at
least three dst_clk cycles apart, or some are merged. Resetting only one side can produce
one spurious pulse.
*/
module surf_pulse_sync (
    input  wire src_clk,
    input  wire src_rst,
    input  wire src_pulse,
    input  wire dst_clk,
    input  wire dst_rst,
    output wire dst_pulse
);
    reg src_toggle = 1'b0;
    always @(posedge src_clk) begin
        if (src_rst)        src_toggle <= 1'b0;
        else if (src_pulse) src_toggle <= ~src_toggle;
    end

    (* ASYNC_REG = "TRUE" *) reg [1:0] dst_sync = 2'b00;
    reg dst_last = 1'b0;
    always @(posedge dst_clk) begin
        if (dst_rst) begin
            dst_sync <= 2'b00;
            dst_last <= 1'b0;
        end else begin
            dst_sync <= {dst_sync[0], src_toggle};
            dst_last <= dst_sync[1];
        end
    end

    assign dst_pulse = dst_sync[1] ^ dst_last;
endmodule
