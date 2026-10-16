/*
SurfCPU level synchronizer
--------------------
File: surf_sync.v Date: 10/15/26 (SurfCPU)
--------------------
Brings a one-bit level (a pin, or a level from another clock domain) into clk's domain
through STAGES flip-flops (STAGES >= 2). Only for single bits and slow levels: a multi-bit
value must cross with surf_cdc_word, and a pulse shorter than two clk periods can be missed
(surf_pulse_sync).
*/
module surf_sync #(
    parameter integer STAGES = 2,
    parameter [0:0]   INIT   = 1'b0
) (
    input  wire clk,
    input  wire rst,    // synchronous, active high: the output returns to INIT
    input  wire d,      // asynchronous to clk
    output wire q
);
    (* ASYNC_REG = "TRUE" *) reg [STAGES-1:0] sync_ff = {STAGES{INIT}};

    always @(posedge clk) begin
        if (rst) sync_ff <= {STAGES{INIT}};
        else     sync_ff <= {sync_ff[STAGES-2:0], d};
    end

    assign q = sync_ff[STAGES-1];
endmodule
