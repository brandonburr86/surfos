/*
SurfCPU SPI master
--------------------
File: surf_spi.v Date: 10/15/26 (SurfCPU)
--------------------
The microSD card's SPI port (MEMORY-MAP.md, spi): mode 0 (SCLK idles low, both sides
sample on the rising edge and change on the falling edge), MSB first, one byte per
transfer. SCLK = SYS_HZ / (2 * (DIV + 1)): each half period lasts DIV + 1 clk cycles, so
a byte takes exactly 16 * (DIV + 1) cycles. The DIV in force is copied when a transfer
starts; writing DIV during a transfer changes the next one.

Writing DATA starts a transfer (MOSI shows bit 7 at once, half a period before the first
rising edge). Reading DATA while a transfer runs is accepted and answered when the
transfer ends, with the byte it received, so "write DATA, read DATA" is one exchange;
s_req_ready is low only while such a read waits. Between transfers MOSI idles high (the
card sees 0xFF filler) and SCLK low. CS is CTRL.CS straight to the pin (cs_n = ~CS).

MISO is sampled on the clk edge that raises SCLK, i.e. half a period after the card
changed it on the previous falling edge (it is synchronous to our own SCLK, so it does
not go through a synchronizer; the board constrains it). cd_n is a mechanical switch and
is synchronized. Interrupt: STATUS.DONE and CTRL.DONE_IE.
*/
module surf_spi #(
    parameter integer SYS_HZ = 25000000
) (
    input  wire        clk,
    input  wire        rst,
    // SurfBus slave (SURFBUS.md)
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
    // interrupt and pins
    output wire        irq,
    output wire        sclk,
    output wire        mosi,
    input  wire        miso,
    output wire        cs_n,
    input  wire        cd_n      // card detect, low = card present (tie 0 without one)
);
`include "surfcpu_soc.vh"

    // DIV at reset: the smallest value with SYS_HZ / (2 * (DIV + 1)) <= 400 kHz
    localparam integer   DIV_400K   = (SYS_HZ + 799999) / 800000 - 1;
    localparam [31:0]    DIV_RST32  = (DIV_400K > 255) ? 32'd255 : (DIV_400K < 0) ? 32'd0 : DIV_400K;
    localparam [7:0]     DIV_RESET  = DIV_RST32[7:0];

    // ---------------------------------------------------------------- bus decode
    wire [11:0] off        = s_req_addr[11:0];
    wire        hit_data   = (off == SURF_SPI_DATA[11:0]);
    wire        hit_status = (off == SURF_SPI_STATUS[11:0]);
    wire        hit_ctrl   = (off == SURF_SPI_CTRL[11:0]);
    wire        hit_div    = (off == SURF_SPI_DIV[11:0]);
    wire        known      = hit_data | hit_status | hit_ctrl | hit_div;
    // only aligned word data accesses reach a register; anything else is an error
    wire        legal      = known && (s_req_be == 4'b1111) && !s_req_fetch;

    reg         rd_wait = 1'b0;          // an accepted DATA read waits for the transfer
    assign s_req_ready = !rd_wait;

    wire        accept = s_req_valid && !rd_wait;
    wire        wr     = accept && legal && s_req_write;
    wire        rd     = accept && legal && !s_req_write;

    // ---------------------------------------------------------------- registers
    reg       ctrl_cs      = 1'b0;
    reg       ctrl_done_ie = 1'b0;
    reg [7:0] div_q        = DIV_RESET;
    reg       st_done      = 1'b0;
    reg       st_ovr       = 1'b0;

    // ---------------------------------------------------------------- shifter
    reg       busy    = 1'b0;
    reg [7:0] cnt     = 8'd0;            // clk cycles left in this half period, minus one
    reg [7:0] cur_div = 8'd0;            // DIV of the running transfer
    reg [2:0] nbit    = 3'd0;            // bits exchanged so far (falling edges seen)
    reg       sclk_q  = 1'b0;
    reg       mosi_q  = 1'b1;
    reg [6:0] tx_sh   = 7'd0;            // bits still to send after the one on MOSI
    reg [7:0] rx_sh   = 8'd0;
    reg [7:0] rx_byte = 8'd0;            // the byte the last finished transfer received

    wire [7:0] wbyte = s_req_wdata[SURF_SPI_DATA_DATA_LSB +: 8];   // the byte a DATA write sends
    wire half_end = busy && (cnt == 8'd0);
    wire xfer_end = half_end && sclk_q && (nbit == 3'd7);   // the last falling edge
    wire start    = wr && hit_data && !busy;

    always @(posedge clk) begin
        if (rst) begin
            busy    <= 1'b0;
            cnt     <= 8'd0;
            cur_div <= 8'd0;
            nbit    <= 3'd0;
            sclk_q  <= 1'b0;
            mosi_q  <= 1'b1;
            tx_sh   <= 7'd0;
            rx_sh   <= 8'd0;
            rx_byte <= 8'd0;
        end else if (start) begin
            busy    <= 1'b1;
            cnt     <= div_q;
            cur_div <= div_q;
            nbit    <= 3'd0;
            sclk_q  <= 1'b0;
            tx_sh   <= wbyte[6:0];
            mosi_q  <= wbyte[7];
        end else if (busy) begin
            if (!half_end) begin
                cnt <= cnt - 8'd1;
            end else begin
                cnt <= cur_div;
                if (!sclk_q) begin                  // rising edge: sample MISO
                    sclk_q <= 1'b1;
                    rx_sh  <= {rx_sh[6:0], miso};
                end else begin                      // falling edge: next bit, or done
                    sclk_q <= 1'b0;
                    if (nbit == 3'd7) begin
                        busy    <= 1'b0;
                        mosi_q  <= 1'b1;
                        rx_byte <= rx_sh;
                    end else begin
                        nbit   <= nbit + 3'd1;
                        tx_sh  <= {tx_sh[5:0], 1'b0};
                        mosi_q <= tx_sh[6];
                    end
                end
            end
        end
    end

    // ---------------------------------------------------------------- register writes
    always @(posedge clk) begin
        if (rst) begin
            ctrl_cs      <= 1'b0;
            ctrl_done_ie <= 1'b0;
            div_q        <= DIV_RESET;
            st_done      <= 1'b0;
            st_ovr       <= 1'b0;
        end else begin
            if (wr && hit_ctrl) begin
                ctrl_cs      <= s_req_wdata[SURF_SPI_CTRL_CS_LSB];
                ctrl_done_ie <= s_req_wdata[SURF_SPI_CTRL_DONE_IE_LSB];
            end
            if (wr && hit_div)
                div_q <= s_req_wdata[SURF_SPI_DIV_DIV_LSB +: 8];
            // sticky bits: write 1 to clear; a new event in the same cycle wins
            if (wr && hit_status && s_req_wdata[SURF_SPI_STATUS_DONE_LSB]) st_done <= 1'b0;
            if (wr && hit_status && s_req_wdata[SURF_SPI_STATUS_OVR_LSB])  st_ovr  <= 1'b0;
            if (xfer_end)                st_done <= 1'b1;
            if (wr && hit_data && busy)  st_ovr  <= 1'b1;
        end
    end

    // ---------------------------------------------------------------- card detect
    wire cd_n_s;
    surf_sync #(.STAGES(2), .INIT(1'b1)) u_cd_sync (
        .clk(clk), .rst(rst), .d(cd_n), .q(cd_n_s));

    // ---------------------------------------------------------------- reads and responses
    reg [31:0] rdata;
    always @(*) begin
        rdata = 32'd0;
        if (hit_data) begin
            rdata[SURF_SPI_DATA_DATA_LSB +: 8] = xfer_end ? rx_sh : rx_byte;
        end
        if (hit_status) begin
            rdata[SURF_SPI_STATUS_BUSY_LSB] = busy;
            rdata[SURF_SPI_STATUS_CARD_LSB] = !cd_n_s;
            rdata[SURF_SPI_STATUS_DONE_LSB] = st_done;
            rdata[SURF_SPI_STATUS_OVR_LSB]  = st_ovr;
        end
        if (hit_ctrl) begin
            rdata[SURF_SPI_CTRL_CS_LSB]      = ctrl_cs;
            rdata[SURF_SPI_CTRL_DONE_IE_LSB] = ctrl_done_ie;
        end
        if (hit_div) begin
            rdata[SURF_SPI_DIV_DIV_LSB +: 8] = div_q;
        end
    end

    reg        rsp_valid_q = 1'b0;
    reg        rsp_err_q   = 1'b0;
    reg [31:0] rsp_rdata_q = 32'd0;

    always @(posedge clk) begin
        if (rst) begin
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'd0;
            rd_wait     <= 1'b0;
        end else begin
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'd0;
            if (rd_wait) begin
                if (xfer_end) begin                 // the waiting DATA read gets its byte
                    rd_wait     <= 1'b0;
                    rsp_valid_q <= 1'b1;
                    rsp_rdata_q <= {24'd0, rx_sh};
                end
            end else if (accept) begin
                if (!legal) begin
                    rsp_valid_q <= 1'b1;
                    rsp_err_q   <= 1'b1;
                end else if (rd && hit_data && busy && !xfer_end) begin
                    rd_wait     <= 1'b1;
                end else begin
                    rsp_valid_q <= 1'b1;
                    rsp_rdata_q <= s_req_write ? 32'd0 : rdata;
                end
            end
        end
    end

    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_rdata = rsp_rdata_q;
    assign s_rsp_err   = rsp_err_q;

    assign irq  = st_done && ctrl_done_ie;
    assign sclk = sclk_q;
    assign mosi = mosi_q;
    assign cs_n = !ctrl_cs;

    // address bits above the 4 KiB window, and write data above the fields, are not used
    wire unused_ok = &{1'b0, s_req_addr[31:12], s_req_wdata[31:8]};
endmodule
