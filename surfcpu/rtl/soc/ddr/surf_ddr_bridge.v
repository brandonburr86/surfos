/*
SurfCPU DDR bridge
--------------------
File: surf_ddr_bridge.v Date: 10/15/26 (SurfCPU)
--------------------
The SurfBus slave in front of the DDR3L controller (docs/DDR.md). The SurfBus side runs on
the SoC clock (clk, sys_clk); the controller side is the AMD MIG 7-series native user
interface (app_*) on the controller's clock (ui_clk, about 81.25 MHz for the Arty's MIG at
4:1). One access is in flight at a time, as SurfBus version 1 allows:

  sys: accept -> request mailbox -> ui: write data then write command, or read command and
  wait for the read data -> response mailbox -> sys: response on SurfBus.

A 32-bit access becomes one 128-bit MIG beat (one BL8 burst of the x16 memory): writes put
the word in all four lanes and mask every byte except the enabled ones (app_wdf_mask bit = 1
means "do not write"); reads return the lane addr[3:2] of the beat. The DDR window offset
addr[27:0] maps one to one onto the memory (app_addr counts 16-bit words; its low three bits
are 0 because every access is a whole aligned burst).

Errors (SURFBUS.md section 3): an access is answered with an error and never reaches the
controller while calibration has not succeeded (calib_done = 0) or after a timeout; the ui
side also refuses a request with an error, without touching the controller, if
init_calib_complete is low when it arrives. The watchdog answers an access that has not
completed TIMEOUT_CYCLES sys cycles after acceptance with an error, sets the sticky timeout
output and from then on refuses every access at once (fail-stop) until rst: a controller that
stopped answering is reported, never waited on for ever. An access the watchdog abandons while
it still waits to be sent is never carried out; one already sent may still be carried out later,
in order (docs/DDR.md).

At most one request is ever between the two clock domains, across resets too: the sys side
keeps a never-reset flag for "a request was sent and its answer has not come back" and sends
nothing new until that answer has come (and been dropped, if nobody waits for it any more), so
the next answer is always the current request's; the ui side keeps a never-reset flag for "a
request was taken and not yet answered" and, if its reset interrupted that request, answers it
with an error as soon as the reset ends, so every request sent gets exactly one answer and the
sys side's flag always clears.

calib_done follows init_calib_complete (synchronized), unless calibration did not succeed
within CALIB_TIMEOUT_CYCLES sys cycles of reset: then calib_fail is set and calib_done stays
0 until rst, even if the controller finishes later (0 disables the limit).

Resets: rst and ui_rst (the MIG's ui_clk_sync_rst) may come and go independently. The two
mailboxes and the two flags above have no reset (surf_ddr_mailbox.v), and each side only sends
and takes messages outside its own reset, so a reset of one side never invents, loses or mixes
up a message. A request already sent to the ui side when rst comes is still carried out by it;
its answer is dropped, and every later access waits for it.
*/
module surf_ddr_bridge #(
    parameter integer TIMEOUT_CYCLES       = 4096,      // watchdog, sys_clk cycles after acceptance
    parameter integer CALIB_TIMEOUT_CYCLES = 25000000,  // calibration limit, sys_clk cycles (1 s at 25 MHz); 0 = none
    parameter integer APP_ADDR_WIDTH       = 28,        // MIG app_addr: rank + bank + row + column bits
    parameter integer APP_DATA_WIDTH       = 128,       // MIG app_wdf_data / app_rd_data
    parameter integer DQ_WIDTH             = 16         // memory data width; app_addr counts DQ_WIDTH-bit words
) (
    // ---------------------------------------------------------------- sys domain
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
    output wire        calib_done,      // sysinfo.DDR_STATUS.CALIB_DONE: DDR is usable
    output wire        calib_fail,      // sysinfo.DDR_STATUS.CALIB_FAIL
    output wire        timeout,         // sysinfo.DDR_STATUS.TIMEOUT (sticky until rst)
    // ---------------------------------------------------------------- ui_clk domain: MIG native UI
    input  wire                          ui_clk,
    input  wire                          ui_rst,               // MIG ui_clk_sync_rst
    input  wire                          init_calib_complete,
    output wire [APP_ADDR_WIDTH-1:0]     app_addr,
    output wire [2:0]                    app_cmd,              // 3'b000 write, 3'b001 read
    output wire                          app_en,
    input  wire                          app_rdy,
    output wire [APP_DATA_WIDTH-1:0]     app_wdf_data,
    output wire [APP_DATA_WIDTH/8-1:0]   app_wdf_mask,         // 1 = leave that byte unchanged
    output wire                          app_wdf_wren,
    output wire                          app_wdf_end,
    input  wire                          app_wdf_rdy,
    input  wire [APP_DATA_WIDTH-1:0]     app_rd_data,
    input  wire                          app_rd_data_valid,
    input  wire                          app_rd_data_end,
    output wire                          app_sr_req,           // tied 0: no self-refresh,
    output wire                          app_ref_req,          // user refresh or ZQ requests
    output wire                          app_zq_req
);
    `include "surfcpu_soc.vh"

    // ---------------------------------------------------------------- geometry
    localparam integer OFFSET_BITS   = $clog2(SURF_DDR_SIZE);         // byte address bits in the window (28)
    localparam integer WADDR_BITS    = OFFSET_BITS - 2;               // word address bits (26)
    localparam integer LANES         = APP_DATA_WIDTH / 32;           // 32-bit words per beat (4)
    localparam integer LANE_BITS     = $clog2(LANES);                 // (2)
    localparam integer MASK_W        = APP_DATA_WIDTH / 8;            // bytes per beat (16)
    localparam integer BEAT_LSB      = $clog2(MASK_W);                // byte address bits inside a beat (4)
    localparam integer UNIT_LSB      = $clog2(DQ_WIDTH / 8);          // byte address bits per app_addr unit (1)
    localparam integer BEAT_IDX_BITS = OFFSET_BITS - BEAT_LSB;        // beats in the window (24 bits)
    localparam integer TO_W          = $clog2(TIMEOUT_CYCLES);
    localparam integer CAL_W         = (CALIB_TIMEOUT_CYCLES > 1) ? $clog2(CALIB_TIMEOUT_CYCLES) : 1;
    localparam [31:0]      TO_LAST32  = TIMEOUT_CYCLES - 1;
    localparam [31:0]      CAL_LAST32 = (CALIB_TIMEOUT_CYCLES > 0) ? CALIB_TIMEOUT_CYCLES - 1 : 0;
    localparam [TO_W-1:0]  TO_LAST    = TO_LAST32[TO_W-1:0];      // last count before the watchdog
    localparam [CAL_W-1:0] CAL_LAST   = CAL_LAST32[CAL_W-1:0];

    // request: write, be, word address, write data; response: error, read data
    localparam integer RQ_W = 1 + 4 + WADDR_BITS + 32;
    localparam integer RS_W = 1 + 32;

    // Parameters this bridge cannot serve stop the elaboration (an undefined module on purpose).
    generate
        if (APP_DATA_WIDTH != 8 * DQ_WIDTH || APP_DATA_WIDTH % 32 != 0) begin : g_bad_width
            surf_ddr_bridge_needs_one_bl8_burst_per_ui_beat bad_parameter ();
        end
        if (OFFSET_BITS - UNIT_LSB > APP_ADDR_WIDTH) begin : g_bad_addr
            surf_ddr_bridge_app_addr_too_narrow bad_parameter ();
        end
        if (TIMEOUT_CYCLES < 2) begin : g_bad_timeout
            surf_ddr_bridge_timeout_too_short bad_parameter ();
        end
    endgenerate

    // ================================================================ sys domain

    // ---------------------------------------------------------------- calibration status
    wire calib_seen;
    surf_sync #(.STAGES(2)) calib_sync (
        .clk(clk), .rst(rst), .d(init_calib_complete), .q(calib_seen));

    reg [CAL_W-1:0] cal_count  = {CAL_W{1'b0}};
    reg             cal_ever   = 1'b0;      // calibration has been seen since reset
    reg             cal_fail_q = 1'b0;
    reg             cal_done_q = 1'b0;

    always @(posedge clk) begin
        if (rst) begin
            cal_count  <= {CAL_W{1'b0}};
            cal_ever   <= 1'b0;
            cal_fail_q <= 1'b0;
            cal_done_q <= 1'b0;
        end else begin
            if (calib_seen && !cal_fail_q)
                cal_ever <= 1'b1;
            if (CALIB_TIMEOUT_CYCLES > 0 && !cal_ever && !calib_seen && !cal_fail_q) begin
                if (cal_count == CAL_LAST) cal_fail_q <= 1'b1;
                else                       cal_count  <= cal_count + 1'b1;
            end
            cal_done_q <= calib_seen && !cal_fail_q;
        end
    end

    assign calib_done = cal_done_q;
    assign calib_fail = cal_fail_q;

    // ---------------------------------------------------------------- SurfBus side
    reg              busy        = 1'b0;    // an accepted access waits for its response
    reg              sent        = 1'b0;    // ... and has gone into the request mailbox
    reg              inflight    = 1'b0;    // never reset: a sent request's answer is still due
    reg  [RQ_W-1:0]  pend        = {RQ_W{1'b0}};
    reg  [TO_W-1:0]  wd_count    = {TO_W{1'b0}};
    reg              timeout_q   = 1'b0;
    reg              rsp_valid_q = 1'b0;
    reg  [31:0]      rsp_rdata_q = 32'h0;
    reg              rsp_err_q   = 1'b0;
    reg              rs_got      = 1'b0;    // a response was taken last cycle ...
    reg  [RS_W-1:0]  rs_q        = {RS_W{1'b0}};   // ... into here (the crossing ends in registers)

    wire             rq_ready;              // request mailbox, source side
    wire             rs_valid;              // response mailbox, destination side
    wire [RS_W-1:0]  rs_dout;

    wire             accept  = s_req_valid && s_req_ready;
    wire             usable  = cal_done_q && !timeout_q;
    wire [RQ_W-1:0]  new_req = {s_req_write, s_req_be, s_req_addr[OFFSET_BITS-1:2], s_req_wdata};
    wire             retry   = busy && !sent;
    wire             expire  = busy && wd_count == TO_LAST;   // the watchdog's last cycle
    wire             can_send = !inflight && rq_ready;        // every earlier request answered
    wire             rq_send = !rst && can_send && ((accept && usable) || (retry && !expire));
    wire [RQ_W-1:0]  rq_din  = retry ? pend : new_req;
    wire             rs_take = !rst && rs_valid;          // every answer is taken; stale ones dropped
    wire             rs_mine = rs_got && busy && sent;    // only the current request's answer can come

    always @(posedge clk) begin
        if (rq_send)      inflight <= 1'b1;
        else if (rs_take) inflight <= 1'b0;
    end

    always @(posedge clk) begin
        if (rs_take) rs_q <= rs_dout;
        rs_got <= rs_take;
    end

    always @(posedge clk) begin
        if (rst) begin
            busy        <= 1'b0;
            sent        <= 1'b0;
            wd_count    <= {TO_W{1'b0}};
            timeout_q   <= 1'b0;
            rsp_valid_q <= 1'b0;
            rsp_rdata_q <= 32'h0;
            rsp_err_q   <= 1'b0;
        end else begin
            rsp_valid_q <= 1'b0;
            if (accept) begin
                if (usable) begin
                    busy     <= 1'b1;
                    sent     <= can_send;               // in the mailbox now, or retried
                    pend     <= new_req;
                    wd_count <= {TO_W{1'b0}};
                end else begin
                    rsp_valid_q <= 1'b1;                // refused: never reaches the controller
                    rsp_err_q   <= 1'b1;
                    rsp_rdata_q <= 32'h0;
                end
            end else if (busy) begin
                if (retry && can_send && !expire)
                    sent <= 1'b1;
                if (rs_mine) begin
                    busy        <= 1'b0;
                    rsp_valid_q <= 1'b1;
                    rsp_err_q   <= rs_q[RS_W-1];
                    rsp_rdata_q <= rs_q[31:0];
                end else if (expire) begin
                    busy        <= 1'b0;                // watchdog: answer, report, stop
                    timeout_q   <= 1'b1;
                    rsp_valid_q <= 1'b1;
                    rsp_err_q   <= 1'b1;
                    rsp_rdata_q <= 32'h0;
                end else begin
                    wd_count <= wd_count + 1'b1;
                end
            end
        end
    end

    assign s_req_ready = !rst && !busy;
    assign s_rsp_valid = rsp_valid_q && !rst;
    assign s_rsp_rdata = rsp_rdata_q;
    assign s_rsp_err   = rsp_err_q;
    assign timeout     = timeout_q;

    // ================================================================ the two crossings
    wire             rq_valid;              // request mailbox, destination side
    wire [RQ_W-1:0]  rq_dout;
    wire             rq_take;
    wire             rs_ready;              // response mailbox, source side
    wire             rs_send;
    wire [RS_W-1:0]  rs_din;

    surf_ddr_mailbox #(.WIDTH(RQ_W)) req_box (
        .src_clk(clk),    .src_send(rq_send), .src_data(rq_din),  .src_ready(rq_ready),
        .dst_clk(ui_clk), .dst_valid(rq_valid), .dst_data(rq_dout), .dst_take(rq_take));

    surf_ddr_mailbox #(.WIDTH(RS_W)) rsp_box (
        .src_clk(ui_clk), .src_send(rs_send), .src_data(rs_din),  .src_ready(rs_ready),
        .dst_clk(clk),    .dst_valid(rs_valid), .dst_data(rs_dout), .dst_take(rs_take));

    // ================================================================ ui_clk domain
    localparam [2:0] U_IDLE  = 3'd0,    // wait for a request and capture it
                     U_START = 3'd1,    // decide from the captured request
                     U_WDATA = 3'd2,    // write data into the MIG's write FIFO
                     U_WCMD  = 3'd3,    // then the write command
                     U_RCMD  = 3'd4,    // read command
                     U_RDATA = 3'd5,    // wait for the read data
                     U_RESP  = 3'd6;    // put the response into the mailbox

    reg  [2:0]            u_state    = U_IDLE;
    reg                   u_owe      = 1'b0;    // never reset: a taken request is not yet answered
    reg                   u_write    = 1'b0;
    reg  [3:0]            u_be       = 4'h0;
    reg  [WADDR_BITS-1:0] u_waddr    = {WADDR_BITS{1'b0}};
    reg  [31:0]           u_wdata    = 32'h0;
    reg                   u_err      = 1'b0;
    reg  [31:0]           u_rdata    = 32'h0;
    reg                   u_app_en   = 1'b0;
    reg                   u_wdf_wren = 1'b0;

    wire [LANE_BITS-1:0]     u_lane = u_waddr[LANE_BITS-1:0];
    wire [BEAT_IDX_BITS-1:0] u_beat = u_waddr[WADDR_BITS-1:LANE_BITS];

    assign rq_take = !ui_rst && u_state == U_IDLE && !u_owe && rq_valid;
    assign rs_send = !ui_rst && u_state == U_RESP;
    assign rs_din  = {u_err, u_rdata};

    always @(posedge ui_clk) begin
        if (rq_take)                 u_owe <= 1'b1;
        else if (rs_send && rs_ready) u_owe <= 1'b0;
    end

    always @(posedge ui_clk) begin
        if (ui_rst) begin
            u_state    <= U_IDLE;
            u_app_en   <= 1'b0;
            u_wdf_wren <= 1'b0;
        end else begin
            case (u_state)
                U_IDLE:
                    if (u_owe) begin                         // a reset cut the last request
                        u_err   <= 1'b1;                     // short: answer it, with an error
                        u_rdata <= 32'h0;
                        u_state <= U_RESP;
                    end else if (rq_valid) begin             // the crossing ends in registers
                        {u_write, u_be, u_waddr, u_wdata} <= rq_dout;
                        u_rdata <= 32'h0;
                        u_state <= U_START;
                    end
                U_START:
                    if (!init_calib_complete) begin
                        u_err   <= 1'b1;                     // not calibrated: refuse untouched
                        u_state <= U_RESP;
                    end else if (u_write) begin
                        u_err      <= 1'b0;
                        u_wdf_wren <= 1'b1;
                        u_state    <= U_WDATA;
                    end else begin
                        u_err    <= 1'b0;
                        u_app_en <= 1'b1;
                        u_state  <= U_RCMD;
                    end
                U_WDATA:
                    if (app_wdf_rdy) begin                   // data first, so the command
                        u_wdf_wren <= 1'b0;                  // never waits for its data
                        u_app_en   <= 1'b1;
                        u_state    <= U_WCMD;
                    end
                U_WCMD:
                    if (app_rdy) begin                       // accepted: the write is ordered
                        u_app_en <= 1'b0;                    // before anything sent later
                        u_state  <= U_RESP;
                    end
                U_RCMD:
                    if (app_rdy) begin
                        u_app_en <= 1'b0;
                        u_state  <= U_RDATA;
                    end
                U_RDATA:
                    if (app_rd_data_valid) begin
                        u_rdata <= app_rd_data[{u_lane, 5'b00000} +: 32];
                        u_state <= U_RESP;
                    end
                U_RESP:
                    if (rs_ready)
                        u_state <= U_IDLE;
                default:
                    u_state <= U_IDLE;
            endcase
        end
    end

    assign app_addr     = {{(APP_ADDR_WIDTH - BEAT_IDX_BITS){1'b0}}, u_beat} << (BEAT_LSB - UNIT_LSB);
    assign app_cmd      = u_write ? 3'b000 : 3'b001;
    assign app_en       = u_app_en;
    assign app_wdf_data = {LANES{u_wdata}};
    assign app_wdf_mask = ~({{(MASK_W - 4){1'b0}}, u_be} << {u_lane, 2'b00});
    assign app_wdf_wren = u_wdf_wren;
    assign app_wdf_end  = u_wdf_wren;               // one beat per burst at 4:1
    assign app_sr_req   = 1'b0;
    assign app_ref_req  = 1'b0;
    assign app_zq_req   = 1'b0;

    // Not needed: the fetch flag (DDR serves fetches like loads), the window base bits and
    // the byte offset (the decoder and SURFBUS.md section 4 take care of them), and
    // app_rd_data_end (always 1 with app_rd_data_valid when a beat is a whole burst).
    wire unused = &{1'b0, s_req_fetch, s_req_addr[31:OFFSET_BITS], s_req_addr[1:0], app_rd_data_end};
endmodule
