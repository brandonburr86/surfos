/*
SurfCPU surf_spi stand-in
--------------------
File: surf_spi.v Date: 10/15/26 (SurfCPU)
--------------------
Simulation stand-in for the io track's SPI master, used by mk/area-soc.mk only while
rtl/peripherals/surf_spi.v does not exist; never part of a synthesized design. It has the
exact port list of docs/MODULES.md section 3 and keeps the SurfBus rules of the real block
(the registers of MEMORY-MAP.md answer, unknown offsets, non-word accesses and fetches get an
error) but does nothing: writes are ignored, DIV reads its reset value (the smallest divider
giving at most 400 kHz), every other register reads 0, and the pins stay idle (chip select
released, no clock, no interrupt).
*/
module surf_spi #(
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
    output wire        sclk,
    output wire        mosi,
    input  wire        miso,
    output wire        cs_n,
    input  wire        cd_n
);
    `include "surfcpu_soc.vh"

    localparam integer  DIV_RESET_I = (SYS_HZ + 799999) / 800000 - 1;
    localparam [31:0]   DIV_RESET   = DIV_RESET_I;

    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    reg        known;
    reg [31:0] rd_mux;
    always @* begin
        known  = 1'b1;
        rd_mux = 32'h0;
        case (woff)
            SURF_SPI_DATA[11:2], SURF_SPI_STATUS[11:2], SURF_SPI_CTRL[11:2]: rd_mux = 32'h0;
            SURF_SPI_DIV[11:2]: rd_mux = DIV_RESET;
            default: known = 1'b0;
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
    assign irq  = 1'b0;
    assign sclk = 1'b0;
    assign mosi = 1'b1;
    assign cs_n = 1'b1;

    wire unused_ok = &{1'b0, s_req_addr[31:12], s_req_wdata, miso, cd_n};
endmodule
