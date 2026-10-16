/*
SurfCPU UART
--------------------
File: surf_uart.v Date: 10/15/26 (SurfCPU)
--------------------
uart at 0x1000_0000 (docs/MEMORY-MAP.md): 8N1, a 16-byte TX FIFO and a 64-byte RX FIFO, the bit
time DIV in sys_clk cycles (reset round(SYS_HZ / BAUD), 217 at 25 MHz and 115200 baud; a value
below the minimum of 16 is stored as 16), interrupt source 0.
TX: a write to TXDATA queues bits 7:0, or drops them and sets STATUS.TX_OVF when the FIFO is
full. The shifter sends start, 8 data bits LSB first and one stop bit, DIV cycles each, back
to back while bytes are waiting. TX_IDLE = FIFO empty and shifter idle.
RX: rx is synchronized; a falling edge starts a frame, the start bit is checked half a bit
later (a glitch shorter than that is ignored), then every bit is sampled in its middle. A
good frame goes into the RX FIFO, or sets RX_OVF if the FIFO is full (the byte is lost). A
stop bit of 0 drops the byte and sets RX_FRAME, and also RX_BREAK when all eight data bits
were 0 too (a break); the receiver then waits for the line to go back to 1 before it looks
for the next start bit, so a long break counts once. Reading RXDATA pops the FIFO (EMPTY =
bit 31 when there was nothing).
irq = (CTRL.RX_IE and RX_AVAIL) or (CTRL.TX_IE and TX FIFO empty) or (CTRL.ERR_IE and any
sticky error). Sticky bits clear by writing 1 to them in STATUS; an event in the same cycle
wins over the clear. A SurfBus slave (docs/SURFBUS.md) that accepts every request and
answers in the next cycle; an unknown offset, a non-word access or a fetch gets an error and
has no effect (no push, no pop); writes to RXDATA and LEVEL are ignored.
*/
module surf_uart #(
    parameter integer SYS_HZ = 25000000,
    parameter integer BAUD   = 115200
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
    input  wire        rx,
    output wire        tx
);
    `include "surfcpu_soc.vh"

    localparam integer DIV_ROUND   = (SYS_HZ + BAUD / 2) / BAUD;
    localparam integer DIV_RESET_I = (DIV_ROUND < 16) ? 16 : DIV_ROUND;
    localparam [15:0]  DIV_RESET   = DIV_RESET_I[15:0];
    localparam [15:0]  DIV_MIN     = 16'd16;

    // ---------------------------------------------------------------- registers
    reg [15:0] div_q;
    reg        rx_ie_q, tx_ie_q, err_ie_q;
    reg        rx_ovf_q, rx_frame_q, tx_ovf_q, rx_break_q;   // sticky

    // ---------------------------------------------------------------- SurfBus decode
    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    wire       hit_tx   = (woff == SURF_UART_TXDATA[11:2]);
    wire       hit_rx   = (woff == SURF_UART_RXDATA[11:2]);
    wire       hit_st   = (woff == SURF_UART_STATUS[11:2]);
    wire       hit_ctrl = (woff == SURF_UART_CTRL[11:2]);
    wire       hit_div  = (woff == SURF_UART_DIV[11:2]);
    wire       hit_lvl  = (woff == SURF_UART_LEVEL[11:2]);
    wire       known    = hit_tx | hit_rx | hit_st | hit_ctrl | hit_div | hit_lvl;
    wire       ok       = s_req_valid && word_acc && known;
    wire       do_wr    = ok && s_req_write;
    wire       do_rd    = ok && !s_req_write;

    // ---------------------------------------------------------------- FIFOs
    wire       tx_full, tx_empty;
    wire [7:0] tx_head;
    wire [4:0] tx_count;
    wire       tx_pop;
    wire       tx_push = do_wr && hit_tx && !tx_full;
    wire       tx_ovf_set = do_wr && hit_tx && tx_full;

    surf_fifo #(.WIDTH(8), .DEPTH_LOG2(4)) u_txfifo (
        .clk(clk), .rst(rst), .wr_en(tx_push),
        .wr_data(s_req_wdata[SURF_UART_TXDATA_DATA_LSB +: SURF_UART_TXDATA_DATA_WIDTH]),
        .rd_en(tx_pop), .rd_data(tx_head), .empty(tx_empty), .full(tx_full), .count(tx_count));

    wire       rx_full, rx_empty;
    wire [7:0] rx_head;
    wire [6:0] rx_count;
    wire       rx_push;
    wire [7:0] rx_byte;
    wire       rx_pop = do_rd && hit_rx && !rx_empty;

    surf_fifo #(.WIDTH(8), .DEPTH_LOG2(6)) u_rxfifo (
        .clk(clk), .rst(rst), .wr_en(rx_push), .wr_data(rx_byte), .rd_en(rx_pop),
        .rd_data(rx_head), .empty(rx_empty), .full(rx_full), .count(rx_count));

    // ---------------------------------------------------------------- transmitter
    reg        tx_busy;
    reg [15:0] tx_cnt;      // cycles into the current bit
    reg [3:0]  tx_left;     // bits still to send after the one on the line
    reg [8:0]  tx_shift;    // the remaining data bits, LSB first, then the stop bit
    reg        tx_q;        // the line, 1 = idle

    wire tx_bit_end = (tx_cnt >= div_q - 16'd1);
    wire tx_last    = tx_busy && tx_bit_end && (tx_left == 4'd0);   // the stop bit is complete
    assign tx_pop   = (!tx_busy || tx_last) && !tx_empty;

    always @(posedge clk) begin
        if (rst) begin
            tx_busy  <= 1'b0;
            tx_cnt   <= 16'd0;
            tx_left  <= 4'd0;
            tx_shift <= 9'h1FF;
            tx_q     <= 1'b1;
        end else if (tx_pop) begin
            tx_busy  <= 1'b1;               // start bit now, then 8 data bits and the stop bit
            tx_q     <= 1'b0;
            tx_shift <= {1'b1, tx_head};
            tx_left  <= 4'd9;
            tx_cnt   <= 16'd0;
        end else if (tx_busy) begin
            if (tx_bit_end) begin
                tx_cnt <= 16'd0;
                if (tx_left == 4'd0) begin
                    tx_busy <= 1'b0;        // stop bit done and nothing waiting
                end else begin
                    tx_q     <= tx_shift[0];
                    tx_shift <= {1'b1, tx_shift[8:1]};
                    tx_left  <= tx_left - 4'd1;
                end
            end else begin
                tx_cnt <= tx_cnt + 16'd1;
            end
        end
    end

    assign tx = tx_q;
    wire tx_idle = tx_empty && !tx_busy;

    // ---------------------------------------------------------------- receiver
    wire rx_s;
    surf_sync #(.STAGES(2), .INIT(1'b1)) u_rxsync (.clk(clk), .rst(rst), .d(rx), .q(rx_s));

    localparam [2:0] R_IDLE  = 3'd0;
    localparam [2:0] R_START = 3'd1;
    localparam [2:0] R_DATA  = 3'd2;
    localparam [2:0] R_STOP  = 3'd3;
    localparam [2:0] R_WAIT  = 3'd4;   // after a bad stop bit: wait for the line to return to 1

    reg [2:0]  rx_state;
    reg [15:0] rx_cnt;
    reg [2:0]  rx_bitn;
    reg [7:0]  rx_shift;

    // the start bit is checked half a bit after the falling edge was seen; the synchronizer
    // has already delayed that by about two cycles
    wire [15:0] rx_half    = {1'b0, div_q[15:1]} - 16'd2;
    wire        rx_mid     = (rx_cnt >= rx_half - 16'd1);
    wire        rx_bit_end = (rx_cnt >= div_q - 16'd1);
    wire        rx_stop    = (rx_state == R_STOP) && rx_bit_end;
    wire        rx_good    = rx_stop && rx_s;
    wire        rx_bad     = rx_stop && !rx_s;

    assign rx_byte = rx_shift;
    assign rx_push = rx_good && !rx_full;
    wire rx_ovf_set   = rx_good && rx_full;
    wire rx_frame_set = rx_bad;
    wire rx_break_set = rx_bad && (rx_shift == 8'h00);

    always @(posedge clk) begin
        if (rst) begin
            rx_state <= R_IDLE;
            rx_cnt   <= 16'd0;
            rx_bitn  <= 3'd0;
            rx_shift <= 8'h00;
        end else begin
            case (rx_state)
                R_IDLE: begin
                    rx_cnt <= 16'd0;
                    if (!rx_s) rx_state <= R_START;
                end
                R_START: begin
                    if (rx_mid) begin
                        rx_cnt   <= 16'd0;
                        rx_bitn  <= 3'd0;
                        rx_state <= rx_s ? R_IDLE : R_DATA;
                    end else begin
                        rx_cnt <= rx_cnt + 16'd1;
                    end
                end
                R_DATA: begin
                    if (rx_bit_end) begin
                        rx_cnt   <= 16'd0;
                        rx_shift <= {rx_s, rx_shift[7:1]};
                        rx_bitn  <= rx_bitn + 3'd1;
                        if (rx_bitn == 3'd7) rx_state <= R_STOP;
                    end else begin
                        rx_cnt <= rx_cnt + 16'd1;
                    end
                end
                R_STOP: begin
                    if (rx_bit_end) begin
                        rx_cnt   <= 16'd0;
                        rx_state <= rx_s ? R_IDLE : R_WAIT;
                    end else begin
                        rx_cnt <= rx_cnt + 16'd1;
                    end
                end
                R_WAIT: begin
                    rx_cnt <= 16'd0;
                    if (rx_s) rx_state <= R_IDLE;
                end
                default: rx_state <= R_IDLE;
            endcase
        end
    end

    // ---------------------------------------------------------------- register file
    wire any_err = rx_ovf_q | rx_frame_q | tx_ovf_q | rx_break_q;

    reg [31:0] status;
    always @* begin
        status = 32'h0;
        status[SURF_UART_STATUS_TX_FULL_LSB]  = tx_full;
        status[SURF_UART_STATUS_TX_IDLE_LSB]  = tx_idle;
        status[SURF_UART_STATUS_RX_AVAIL_LSB] = !rx_empty;
        status[SURF_UART_STATUS_RX_FULL_LSB]  = rx_full;
        status[SURF_UART_STATUS_RX_OVF_LSB]   = rx_ovf_q;
        status[SURF_UART_STATUS_RX_FRAME_LSB] = rx_frame_q;
        status[SURF_UART_STATUS_TX_OVF_LSB]   = tx_ovf_q;
        status[SURF_UART_STATUS_RX_BREAK_LSB] = rx_break_q;
    end

    reg [31:0] rd_mux;
    always @* begin
        rd_mux = 32'h0;
        if (hit_tx) begin
            rd_mux[SURF_UART_TXDATA_FULL_LSB] = tx_full;
        end
        if (hit_rx) begin
            if (rx_empty) rd_mux[SURF_UART_RXDATA_EMPTY_LSB] = 1'b1;
            else          rd_mux[SURF_UART_RXDATA_DATA_LSB +: SURF_UART_RXDATA_DATA_WIDTH] = rx_head;
        end
        if (hit_st) rd_mux = status;
        if (hit_ctrl) begin
            rd_mux[SURF_UART_CTRL_RX_IE_LSB]  = rx_ie_q;
            rd_mux[SURF_UART_CTRL_TX_IE_LSB]  = tx_ie_q;
            rd_mux[SURF_UART_CTRL_ERR_IE_LSB] = err_ie_q;
        end
        if (hit_div) rd_mux[SURF_UART_DIV_DIV_LSB +: SURF_UART_DIV_DIV_WIDTH] = div_q;
        if (hit_lvl) begin
            rd_mux[SURF_UART_LEVEL_TX_LSB +: SURF_UART_LEVEL_TX_WIDTH] =
                {{(SURF_UART_LEVEL_TX_WIDTH - 5){1'b0}}, tx_count};
            rd_mux[SURF_UART_LEVEL_RX_LSB +: SURF_UART_LEVEL_RX_WIDTH] =
                {{(SURF_UART_LEVEL_RX_WIDTH - 7){1'b0}}, rx_count};
        end
    end

    wire        wr_st   = do_wr && hit_st;
    wire        wr_ctrl = do_wr && hit_ctrl;
    wire        wr_div  = do_wr && hit_div;
    wire [15:0] wdiv    = s_req_wdata[SURF_UART_DIV_DIV_LSB +: SURF_UART_DIV_DIV_WIDTH];

    reg        rsp_valid_q, rsp_err_q;
    reg [31:0] rsp_rdata_q;
    always @(posedge clk) begin
        if (rst) begin
            div_q       <= DIV_RESET;
            rx_ie_q     <= 1'b0;
            tx_ie_q     <= 1'b0;
            err_ie_q    <= 1'b0;
            rx_ovf_q    <= 1'b0;
            rx_frame_q  <= 1'b0;
            tx_ovf_q    <= 1'b0;
            rx_break_q  <= 1'b0;
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'h0;
        end else begin
            if (wr_div) div_q <= (wdiv < DIV_MIN) ? DIV_MIN : wdiv;
            if (wr_ctrl) begin
                rx_ie_q  <= s_req_wdata[SURF_UART_CTRL_RX_IE_LSB];
                tx_ie_q  <= s_req_wdata[SURF_UART_CTRL_TX_IE_LSB];
                err_ie_q <= s_req_wdata[SURF_UART_CTRL_ERR_IE_LSB];
            end
            rx_ovf_q   <= rx_ovf_set   | (rx_ovf_q   & ~(wr_st & s_req_wdata[SURF_UART_STATUS_RX_OVF_LSB]));
            rx_frame_q <= rx_frame_set | (rx_frame_q & ~(wr_st & s_req_wdata[SURF_UART_STATUS_RX_FRAME_LSB]));
            tx_ovf_q   <= tx_ovf_set   | (tx_ovf_q   & ~(wr_st & s_req_wdata[SURF_UART_STATUS_TX_OVF_LSB]));
            rx_break_q <= rx_break_set | (rx_break_q & ~(wr_st & s_req_wdata[SURF_UART_STATUS_RX_BREAK_LSB]));

            rsp_valid_q <= s_req_valid;
            rsp_err_q   <= s_req_valid && !(word_acc && known);
            rsp_rdata_q <= do_rd ? rd_mux : 32'h0;
        end
    end

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_err   = rsp_err_q;
    assign s_rsp_rdata = rsp_rdata_q;

    assign irq = (rx_ie_q & !rx_empty) | (tx_ie_q & tx_empty) | (err_ie_q & any_err);

    // the decoder has already matched the window (address bits 31:12)
    wire unused_ok = &{1'b0, s_req_addr[31:12]};
endmodule
