/*
SurfCPU reset synchronizer
--------------------
File: surf_reset_sync.v Date: 10/15/26 (SurfCPU)
--------------------
One per clock domain: the output asserts as soon as arst does (asynchronously) and is
released synchronously, STAGES clk edges after arst falls, so every flip-flop in the domain
leaves reset on the same edge. Everything downstream uses the output as a synchronous reset.
*/
module surf_reset_sync #(
    parameter integer STAGES = 3
) (
    input  wire clk,
    input  wire arst,   // asynchronous, active high (reset button, a clock not yet locked, ...)
    output wire rst     // active high, released synchronously to clk
);
    (* ASYNC_REG = "TRUE" *) reg [STAGES-1:0] rst_ff = {STAGES{1'b1}};

    always @(posedge clk or posedge arst) begin
        if (arst) rst_ff <= {STAGES{1'b1}};
        else      rst_ff <= {rst_ff[STAGES-2:0], 1'b0};
    end

    assign rst = rst_ff[STAGES-1];
endmodule
