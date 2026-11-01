/*
SurfCPU surf_ps2 stand-in
--------------------
File: surf_ps2.v Date: 10/15/26 (SurfCPU)
--------------------
Simulation stand-in for the io track's PS/2 port, used by mk/area-soc.mk only while
rtl/peripherals/surf_ps2.v does not exist; never part of a synthesized design. It has the
exact port list of docs/MODULES.md section 3 and keeps the SurfBus rules of the real block
(the registers of MEMORY-MAP.md answer, unknown offsets, non-word accesses and fetches get an
error) but receives nothing: DATA reads EMPTY, CTRL reads its reset value (RX_EN), every other
register reads 0, writes are ignored, and both open-drain lines stay released.
*/
module surf_ps2 #(
    parameter integer SYS_HZ = 25000000
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        s_req_valid,
    output wire        s_req_ready,
    input  wire [31:0] s_req_addr,
    input  wire        s_req_write,
    input  wire [3:0]  s_req_be,
    input  wire [31:0] s_req_wdata,
    input  wire        s_req_fetch,
    output wire        s_rsp_valid,
    output wire [31:0] s_rsp_rdata,
    output wire        s_rsp_err,
    output wire        irq,
    input  wire        ps2_clk_i,
    input  wire        ps2_data_i,
    output wire        ps2_clk_oe,
    output wire        ps2_data_oe
);
    `include "surfcpu_soc.vh"

    // the stand-in has no timing; SYS_HZ is part of the contract's parameter list
    /* verilator lint_off UNUSEDPARAM */
    localparam integer SYS_HZ_UNUSED = SYS_HZ;
    /* verilator lint_on UNUSEDPARAM */

    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    reg        known;
    reg [31:0] rd_mux;
    always @* begin
        known  = 1'b1;
        rd_mux = 32'h0;
        case (woff)
            SURF_PS2_DATA[11:2]:     rd_mux = SURF_PS2_DATA_EMPTY_MASK;
            SURF_PS2_CTRL[11:2]:     rd_mux = SURF_PS2_CTRL_RX_EN_MASK;
            SURF_PS2_STATUS[11:2], SURF_PS2_TXDATA[11:2], SURF_PS2_ERRCOUNT[11:2]: rd_mux = 32'h0;
            default:                 known  = 1'b0;
        endcase
    end

    reg        rsp_valid_q, rsp_err_q;
    reg [31:0] rsp_rdata_q;
    always @(posedge clk) begin
        if (rst) begin
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'h0;
        end else begin
            rsp_valid_q <= s_req_valid;
            rsp_err_q   <= s_req_valid && !(word_acc && known);
            rsp_rdata_q <= (s_req_valid && word_acc && known && !s_req_write) ? rd_mux : 32'h0;
        end
    end

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_err   = rsp_err_q;
    assign s_rsp_rdata = rsp_rdata_q;
    assign irq         = 1'b0;
    assign ps2_clk_oe  = 1'b0;
    assign ps2_data_oe = 1'b0;

    wire unused_ok = &{1'b0, s_req_addr[31:12], s_req_wdata, ps2_clk_i, ps2_data_i};
endmodule
