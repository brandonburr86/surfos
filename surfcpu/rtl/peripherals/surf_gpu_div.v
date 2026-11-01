/*
SurfGPU gradient divider
--------------------
File: surf_gpu_div.v Date: 10/25/26 (SurfGPU GPU2)
--------------------
One gradient of a TRIANGLE (docs/GPU.md section 16.4): q = floor(n * 2^20 / d) for a signed n
(|n| < 2^33) and a positive d (< 2^33), kept modulo 2^37, which is all the triangle walker uses
(its values wrap at 37 bits: section 16.4). A restoring divider, one quotient bit a cycle: start
loads n and d, and DONE_AT cycles later (53 steps, one per bit of |n| * 2^20) the quotient of
|n| is in q_mag and the remainder in rem; for a negative n, floor(-m / d) = -(m div d) - 1 when
the remainder is not 0, which is ~(m div d) + (rem == 0) in two's complement. q is valid from the
cycle busy falls until the next start.
*/
module surf_gpu_div (
    input  wire               clk,
    input  wire               rst,
    input  wire               start,         // one cycle: take n and d
    input  wire signed [34:0] n,
    input  wire        [32:0] d,             // > 0
    output wire               busy,
    output wire        [36:0] q              // floor(n * 2^20 / d) mod 2^37
);
    localparam [5:0] STEPS = 6'd53;          // the bits of |n| * 2^20

    reg        neg   = 1'b0;
    reg [52:0] dend  = 53'd0;                // |n| * 2^20, taken from its top bit down
    reg [32:0] dsor  = 33'd0;
    reg [33:0] rem   = 34'd0;
    reg [36:0] q_mag = 37'd0;                // the low 37 bits of |n| * 2^20 div d
    reg [5:0]  left  = 6'd0;

    wire [33:0] trial = {rem[32:0], dend[52]};               // the remainder with the next bit
    wire        fits  = trial >= {1'b0, dsor};
    wire [33:0] n_mag = n[34] ? -{n[33:0]} : n[33:0];         // |n| < 2^33

    always @(posedge clk) begin
        if (rst) begin
            left <= 6'd0;
        end else if (start) begin
            neg   <= n[34];
            dend  <= {n_mag[32:0], 20'd0};
            dsor  <= d;
            rem   <= 34'd0;
            q_mag <= 37'd0;
            left  <= STEPS;
        end else if (left != 6'd0) begin
            rem   <= fits ? trial - {1'b0, dsor} : trial;
            q_mag <= {q_mag[35:0], fits};
            dend  <= {dend[51:0], 1'b0};
            left  <= left - 6'd1;
        end
    end

    assign busy = (left != 6'd0);
    assign q    = neg ? ~q_mag + {36'd0, rem == 34'd0} : q_mag;

    wire unused_ok = &{1'b0, n_mag[33]};
endmodule
