/*
SurfCPU PS/2 port
--------------------
File: surf_ps2.v Date: 10/15/26 (SurfCPU)
--------------------
The keyboard port of MEMORY-MAP.md (ps2). Both lines are open drain: this block never
drives them high; ps2_clk_oe / ps2_data_oe = 1 pulls a line low, 0 releases it to the
pull-up (the board top turns them into IOBUFs).

Receiving (device to host): the lines are synchronized, then filtered (a level must hold
for about 2 us before it counts, so glitches on slow edges vanish); on each falling edge
of the clock the data line is sampled: start bit 0, eight data bits LSB first, odd parity,
stop bit 1. A good byte goes into a 16-byte FIFO, or is lost with OVF when the FIFO is
full; a bad start or stop bit is FRAME, a bad parity PARITY (the byte is dropped). A frame
with no falling edge for 2 ms is abandoned (TIMEOUT), which also resynchronizes after
noise. Each error also counts in ERRCOUNT (saturating). A bad start bit is rejected at
once, so a stop bit taken for a start bit after a lost clock edge resynchronizes at the
next frame.

Sending (host to device, TXDATA): first wait for a gap between frames (no frame being
received, both lines high after the synchronizer and after the filter, so no edge is still
in the filter; at most 4 ms), so a frame the keyboard already counts as delivered is never
cut off: an IBM keyboard commits a frame once the parity bit's clock has risen and does not
send it again. (An edge still inside the two synchronizer stages can only be a keyboard
starting a frame; it sees the inhibit at its first clock and sends the frame again.) Then
pull the clock low for 125 us (inhibit), pull data low (start bit, request to send),
release the clock 5 us later and let the device clock the byte in: after each falling edge
of the device's clock the next bit goes out (d0..d7, odd parity, then data released for the
stop bit); at the 11th falling edge the device must hold data low (its acknowledge); then
both lines must come back high. The device must start clocking within 15 ms and finish
within 2 ms, else TX_NAK; so does a missing acknowledge. The clock is pulled low only during
the inhibit, never toggled. TX_BUSY covers the wait too. The receiver runs during the wait
and rests from the inhibit to the end of the send.
*/
module surf_ps2 #(
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
    input  wire        ps2_clk_i,
    input  wire        ps2_data_i,
    output wire        ps2_clk_oe,     // 1 = pull the clock line low
    output wire        ps2_data_oe     // 1 = pull the data line low
);
`include "surfcpu_soc.vh"

    // ---------------------------------------------------------------- timing (clk cycles)
    localparam integer FILT_RAW   = SYS_HZ / 500000;         // 2 us glitch filter
    localparam integer FILT       = (FILT_RAW < 2) ? 2 : FILT_RAW;
    localparam integer T_RX_TO    = SYS_HZ / 500;            // 2 ms: partial frame abandoned
    localparam integer T_INHIBIT  = SYS_HZ / 8000;           // 125 us clock low before a send
    localparam integer T_RTS      = SYS_HZ / 200000;         // 5 us data low before the release
    localparam integer T_TX_START = (SYS_HZ / 1000) * 15;    // 15 ms for the device to start
    localparam integer T_TX_FRAME = SYS_HZ / 500;            // 2 ms for the whole frame
    localparam integer T_TX_PEND  = SYS_HZ / 250;            // 4 ms at most waiting for a gap
    localparam integer TW         = 24;                      // timer width (enough below 1 GHz)
    localparam integer FW         = $clog2(FILT + 1);

    localparam [31:0]   RX_TO_32    = T_RX_TO - 1;            // terminal counts, last cycle of each
    localparam [31:0]   INHIBIT_32  = T_INHIBIT - 1;
    localparam [31:0]   RTS_32      = T_RTS - 1;
    localparam [31:0]   TX_START_32 = T_TX_START - 1;
    localparam [31:0]   TX_FRAME_32 = T_TX_FRAME - 1;
    localparam [31:0]   TX_PEND_32  = T_TX_PEND - 1;
    localparam [31:0]   FILT_32     = FILT - 1;
    localparam [TW-1:0] RX_TO_M1    = RX_TO_32[TW-1:0];
    localparam [TW-1:0] INHIBIT_M1  = INHIBIT_32[TW-1:0];
    localparam [TW-1:0] RTS_M1      = RTS_32[TW-1:0];
    localparam [TW-1:0] TX_START_M1 = TX_START_32[TW-1:0];
    localparam [TW-1:0] TX_FRAME_M1 = TX_FRAME_32[TW-1:0];
    localparam [TW-1:0] TX_PEND_M1  = TX_PEND_32[TW-1:0];
    localparam [FW-1:0] FILT_M1     = FILT_32[FW-1:0];

    // ---------------------------------------------------------------- bus decode
    wire [11:0] off       = s_req_addr[11:0];
    wire        hit_data  = (off == SURF_PS2_DATA[11:0]);
    wire        hit_stat  = (off == SURF_PS2_STATUS[11:0]);
    wire        hit_ctrl  = (off == SURF_PS2_CTRL[11:0]);
    wire        hit_tx    = (off == SURF_PS2_TXDATA[11:0]);
    wire        hit_errc  = (off == SURF_PS2_ERRCOUNT[11:0]);
    wire        known     = hit_data | hit_stat | hit_ctrl | hit_tx | hit_errc;
    wire        legal     = known && (s_req_be == 4'b1111) && !s_req_fetch;
    wire        accept    = s_req_valid;                     // always ready: one-cycle answers
    wire        wr        = accept && legal && s_req_write;
    wire        rd        = accept && legal && !s_req_write;
    assign s_req_ready = 1'b1;

    // ---------------------------------------------------------------- line conditioning
    wire clk_s, dat_s;
    surf_sync #(.STAGES(2), .INIT(1'b1)) u_clk_sync (.clk(clk), .rst(rst), .d(ps2_clk_i),  .q(clk_s));
    surf_sync #(.STAGES(2), .INIT(1'b1)) u_dat_sync (.clk(clk), .rst(rst), .d(ps2_data_i), .q(dat_s));

    reg          clk_f = 1'b1, dat_f = 1'b1, clk_f_d = 1'b1;
    reg [FW-1:0] clk_cnt = {FW{1'b0}}, dat_cnt = {FW{1'b0}};
    always @(posedge clk) begin
        if (rst) begin
            clk_f   <= 1'b1;
            dat_f   <= 1'b1;
            clk_f_d <= 1'b1;
            clk_cnt <= {FW{1'b0}};
            dat_cnt <= {FW{1'b0}};
        end else begin
            clk_f_d <= clk_f;
            if (clk_s == clk_f)          clk_cnt <= {FW{1'b0}};
            else if (clk_cnt == FILT_M1) begin clk_f <= clk_s; clk_cnt <= {FW{1'b0}}; end
            else                         clk_cnt <= clk_cnt + 1'b1;
            if (dat_s == dat_f)          dat_cnt <= {FW{1'b0}};
            else if (dat_cnt == FILT_M1) begin dat_f <= dat_s; dat_cnt <= {FW{1'b0}}; end
            else                         dat_cnt <= dat_cnt + 1'b1;
        end
    end
    wire fall = clk_f_d && !clk_f;        // a falling edge of the (filtered) clock line

    // ---------------------------------------------------------------- control register
    reg ctrl_rx_en  = 1'b1;
    reg ctrl_rx_ie  = 1'b0;
    reg ctrl_err_ie = 1'b0;

    // ---------------------------------------------------------------- receiver state
    reg [3:0]    rx_n     = 4'd0;         // bits of the frame so far; 0 = waiting for a start bit
    reg [8:0]    rx_sh    = 9'd0;         // parity and data, shifted in from the top
    reg [TW-1:0] rx_timer = {TW{1'b0}};
    reg          push     = 1'b0;         // one-cycle events
    reg          ev_par   = 1'b0, ev_frm = 1'b0, ev_to = 1'b0;

    // Between frames: no frame being received, and both lines high after the synchronizer and
    // after the filter (no edge still being filtered). A send begins only then.
    wire bus_quiet = (rx_n == 4'd0) && clk_f && dat_f && clk_s && dat_s;

    // ---------------------------------------------------------------- transmitter
    localparam [2:0] TX_IDLE = 3'd0, TX_INHIBIT = 3'd1, TX_RTS = 3'd2, TX_WAIT = 3'd3,
                     TX_BITS = 3'd4, TX_ACK = 3'd5, TX_RELEASE = 3'd6, TX_PEND = 3'd7;
    reg [2:0]    tx_state = TX_IDLE;
    reg [TW-1:0] tx_timer = {TW{1'b0}};
    reg [3:0]    tx_n     = 4'd0;         // bits presented so far
    reg [9:0]    tx_sh    = 10'd0;        // d0..d7, parity, stop: LSB goes out first
    reg          tx_nakd  = 1'b0;         // no acknowledge seen at the 11th edge
    reg          clk_oe_q = 1'b0, dat_oe_q = 1'b0;
    reg          ev_nak   = 1'b0;         // one-cycle event: the send failed

    wire tx_start = wr && hit_tx && (tx_state == TX_IDLE);
    wire [7:0] tx_byte = s_req_wdata[SURF_PS2_TXDATA_DATA_LSB +: 8];

    always @(posedge clk) begin
        if (rst) begin
            tx_state <= TX_IDLE;
            tx_timer <= {TW{1'b0}};
            tx_n     <= 4'd0;
            tx_sh    <= 10'd0;
            tx_nakd  <= 1'b0;
            clk_oe_q <= 1'b0;
            dat_oe_q <= 1'b0;
            ev_nak   <= 1'b0;
        end else begin
            ev_nak <= 1'b0;
            case (tx_state)
                TX_IDLE: begin
                    clk_oe_q <= 1'b0;
                    dat_oe_q <= 1'b0;
                    if (tx_start) begin
                        tx_sh    <= {1'b1, ~^tx_byte, tx_byte};
                        tx_timer <= {TW{1'b0}};
                        tx_n     <= 4'd0;
                        tx_nakd  <= 1'b0;
                        tx_state <= TX_PEND;
                    end
                end
                TX_PEND: begin                                // wait for a gap between frames
                    if (bus_quiet || tx_timer == TX_PEND_M1) begin
                        tx_timer <= {TW{1'b0}};
                        clk_oe_q <= 1'b1;                     // inhibit
                        tx_state <= TX_INHIBIT;
                    end else tx_timer <= tx_timer + 1'b1;
                end
                TX_INHIBIT: begin
                    if (tx_timer == INHIBIT_M1) begin
                        tx_timer <= {TW{1'b0}};
                        dat_oe_q <= 1'b1;                     // start bit: request to send
                        tx_state <= TX_RTS;
                    end else tx_timer <= tx_timer + 1'b1;
                end
                TX_RTS: begin
                    if (tx_timer == RTS_M1) begin
                        tx_timer <= {TW{1'b0}};
                        clk_oe_q <= 1'b0;                     // the device clocks from now on
                        tx_state <= TX_WAIT;
                    end else tx_timer <= tx_timer + 1'b1;
                end
                TX_WAIT: begin                                // the device's first falling edge
                    if (fall) begin
                        tx_timer <= {TW{1'b0}};
                        dat_oe_q <= !tx_sh[0];
                        tx_sh    <= {1'b0, tx_sh[9:1]};
                        tx_n     <= 4'd1;
                        tx_state <= TX_BITS;
                    end else if (tx_timer == TX_START_M1) begin
                        dat_oe_q <= 1'b0;                     // nobody clocks: give up
                        ev_nak   <= 1'b1;
                        tx_state <= TX_IDLE;
                    end else tx_timer <= tx_timer + 1'b1;
                end
                TX_BITS, TX_ACK, TX_RELEASE: begin
                    if (tx_timer == TX_FRAME_M1) begin        // the frame took too long
                        clk_oe_q <= 1'b0;
                        dat_oe_q <= 1'b0;
                        ev_nak   <= 1'b1;
                        tx_state <= TX_IDLE;
                    end else begin
                        tx_timer <= tx_timer + 1'b1;
                        if (tx_state == TX_BITS && fall) begin
                            dat_oe_q <= !tx_sh[0];            // the stop bit (1) releases data
                            tx_sh    <= {1'b0, tx_sh[9:1]};
                            tx_n     <= tx_n + 4'd1;
                            if (tx_n == 4'd9) tx_state <= TX_ACK;
                        end else if (tx_state == TX_ACK && fall) begin
                            tx_nakd  <= dat_f;                // the device holds data low to acknowledge
                            tx_state <= TX_RELEASE;
                        end else if (tx_state == TX_RELEASE && clk_f && dat_f) begin
                            ev_nak   <= tx_nakd;
                            tx_state <= TX_IDLE;
                        end
                    end
                end
                default: tx_state <= TX_IDLE;
            endcase
        end
    end

    wire tx_busy = (tx_state != TX_IDLE);   // STATUS.TX_BUSY: from the write to the end

    // ---------------------------------------------------------------- receiver
    // it runs while a send waits for its gap, and rests from the inhibit on
    wire rx_on = ctrl_rx_en && (tx_state == TX_IDLE || tx_state == TX_PEND);

    always @(posedge clk) begin
        if (rst) begin
            rx_n     <= 4'd0;
            rx_sh    <= 9'd0;
            rx_timer <= {TW{1'b0}};
            push     <= 1'b0;
            ev_par   <= 1'b0;
            ev_frm   <= 1'b0;
            ev_to    <= 1'b0;
        end else begin
            push   <= 1'b0;
            ev_par <= 1'b0;
            ev_frm <= 1'b0;
            ev_to  <= 1'b0;
            if (!rx_on) begin
                rx_n     <= 4'd0;
                rx_timer <= {TW{1'b0}};
            end else if (fall) begin
                rx_timer <= {TW{1'b0}};
                if (rx_n == 4'd0) begin
                    if (!dat_f) rx_n <= 4'd1;                 // start bit
                    else        ev_frm <= 1'b1;               // not a start bit: rejected
                end else if (rx_n == 4'd10) begin             // stop bit
                    rx_n <= 4'd0;
                    if (!dat_f)        ev_frm <= 1'b1;
                    else if (!(^rx_sh)) ev_par <= 1'b1;        // odd parity over data and parity bit
                    else               push   <= 1'b1;
                end else begin                                // d0..d7, parity
                    rx_sh <= {dat_f, rx_sh[8:1]};
                    rx_n  <= rx_n + 4'd1;
                end
            end else if (rx_n != 4'd0) begin
                if (rx_timer == RX_TO_M1) begin
                    rx_n     <= 4'd0;
                    rx_timer <= {TW{1'b0}};
                    ev_to    <= 1'b1;
                end else rx_timer <= rx_timer + 1'b1;
            end
        end
    end

    // ---------------------------------------------------------------- FIFO
    wire       f_empty, f_full;
    wire [7:0] f_data;
    wire [4:0] f_count;
    wire       pop    = rd && hit_data && !f_empty;
    wire       ev_ovf = push && f_full;
    surf_fifo #(.WIDTH(8), .DEPTH_LOG2(4)) u_fifo (
        .clk(clk), .rst(rst), .wr_en(push), .wr_data(rx_sh[7:0]), .rd_en(pop),
        .rd_data(f_data), .empty(f_empty), .full(f_full), .count(f_count));

    // ---------------------------------------------------------------- status, counters
    reg       st_ovf = 1'b0, st_par = 1'b0, st_frm = 1'b0, st_to = 1'b0, st_nak = 1'b0;
    reg [7:0] cnt_par = 8'd0, cnt_frm = 8'd0, cnt_to = 8'd0, cnt_ovf = 8'd0;
    wire      clr = wr && hit_stat;

    always @(posedge clk) begin
        if (rst) begin
            st_ovf <= 1'b0; st_par <= 1'b0; st_frm <= 1'b0; st_to <= 1'b0; st_nak <= 1'b0;
            cnt_par <= 8'd0; cnt_frm <= 8'd0; cnt_to <= 8'd0; cnt_ovf <= 8'd0;
            ctrl_rx_en  <= 1'b1;
            ctrl_rx_ie  <= 1'b0;
            ctrl_err_ie <= 1'b0;
        end else begin
            if (wr && hit_ctrl) begin
                ctrl_rx_en  <= s_req_wdata[SURF_PS2_CTRL_RX_EN_LSB];
                ctrl_rx_ie  <= s_req_wdata[SURF_PS2_CTRL_RX_IE_LSB];
                ctrl_err_ie <= s_req_wdata[SURF_PS2_CTRL_ERR_IE_LSB];
            end
            // write 1 to clear; an event in the same cycle wins
            if (clr && s_req_wdata[SURF_PS2_STATUS_OVF_LSB])     st_ovf <= 1'b0;
            if (clr && s_req_wdata[SURF_PS2_STATUS_PARITY_LSB])  st_par <= 1'b0;
            if (clr && s_req_wdata[SURF_PS2_STATUS_FRAME_LSB])   st_frm <= 1'b0;
            if (clr && s_req_wdata[SURF_PS2_STATUS_TIMEOUT_LSB]) st_to  <= 1'b0;
            if (clr && s_req_wdata[SURF_PS2_STATUS_TX_NAK_LSB])  st_nak <= 1'b0;
            if (ev_ovf) st_ovf <= 1'b1;
            if (ev_par) st_par <= 1'b1;
            if (ev_frm) st_frm <= 1'b1;
            if (ev_to)  st_to  <= 1'b1;
            if (ev_nak) st_nak <= 1'b1;
            if (ev_ovf && cnt_ovf != 8'hFF) cnt_ovf <= cnt_ovf + 8'd1;
            if (ev_par && cnt_par != 8'hFF) cnt_par <= cnt_par + 8'd1;
            if (ev_frm && cnt_frm != 8'hFF) cnt_frm <= cnt_frm + 8'd1;
            if (ev_to  && cnt_to  != 8'hFF) cnt_to  <= cnt_to  + 8'd1;
        end
    end

    // ---------------------------------------------------------------- reads and responses
    reg [31:0] rdata;
    always @(*) begin
        rdata = 32'd0;
        if (hit_data) begin
            rdata[SURF_PS2_DATA_DATA_LSB +: 8] = f_empty ? 8'd0 : f_data;
            rdata[SURF_PS2_DATA_EMPTY_LSB]     = f_empty;
        end
        if (hit_stat) begin
            rdata[SURF_PS2_STATUS_RX_AVAIL_LSB] = !f_empty;
            rdata[SURF_PS2_STATUS_RX_FULL_LSB]  = f_full;
            rdata[SURF_PS2_STATUS_OVF_LSB]      = st_ovf;
            rdata[SURF_PS2_STATUS_PARITY_LSB]   = st_par;
            rdata[SURF_PS2_STATUS_FRAME_LSB]    = st_frm;
            rdata[SURF_PS2_STATUS_TIMEOUT_LSB]  = st_to;
            rdata[SURF_PS2_STATUS_TX_BUSY_LSB]  = tx_busy;
            rdata[SURF_PS2_STATUS_TX_NAK_LSB]   = st_nak;
        end
        if (hit_ctrl) begin
            rdata[SURF_PS2_CTRL_RX_EN_LSB]  = ctrl_rx_en;
            rdata[SURF_PS2_CTRL_RX_IE_LSB]  = ctrl_rx_ie;
            rdata[SURF_PS2_CTRL_ERR_IE_LSB] = ctrl_err_ie;
        end
        if (hit_errc) begin
            rdata[SURF_PS2_ERRCOUNT_PARITY_LSB  +: 8] = cnt_par;
            rdata[SURF_PS2_ERRCOUNT_FRAME_LSB   +: 8] = cnt_frm;
            rdata[SURF_PS2_ERRCOUNT_TIMEOUT_LSB +: 8] = cnt_to;
            rdata[SURF_PS2_ERRCOUNT_OVF_LSB     +: 8] = cnt_ovf;
        end
        // TXDATA is write-only: reads give 0
    end

    reg        rsp_valid_q = 1'b0;
    reg        rsp_err_q   = 1'b0;
    reg [31:0] rsp_rdata_q = 32'd0;
    always @(posedge clk) begin
        if (rst) begin
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'd0;
        end else begin
            rsp_valid_q <= accept;
            rsp_err_q   <= accept && !legal;
            rsp_rdata_q <= (accept && legal && !s_req_write) ? rdata : 32'd0;
        end
    end

    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_rdata = rsp_rdata_q;
    assign s_rsp_err   = rsp_err_q;

    assign irq = (ctrl_rx_ie && !f_empty) ||
                 (ctrl_err_ie && (st_ovf || st_par || st_frm || st_to || st_nak));
    assign ps2_clk_oe  = clk_oe_q;
    assign ps2_data_oe = dat_oe_q;

    // address bits above the window, unused write data and the FIFO level are not needed
    wire unused_ok = &{1'b0, s_req_addr[31:12], s_req_wdata[31:8], f_count, tx_sh[9]};
endmodule
