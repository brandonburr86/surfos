/*
SurfCPU DDR controller register bridge
--------------------
File: surf_ddr_csr.v Date: 10/16/26 (SurfCPU)
--------------------
The SurfBus slave behind the ddrc region (docs/MEMORY-MAP.md, spec/soc.toml): it carries
SurfCPU's accesses to a memory controller's control and status registers across into the
controller's clock and onto its Wishbone control bus, for a controller that software
initializes and calibrates itself (LiteDRAM generated without its own CPU: its wb_ctrl port,
docs/DDR.md). The SurfBus side runs on the SoC clock (clk, sys_clk); the Wishbone side on
the controller's (wb_clk, LiteDRAM's user_clk). One access at a time:

  sys: accept -> request mailbox -> wb: one Wishbone classic cycle -> response mailbox ->
  sys: response on SurfBus.

Only whole aligned words are carried (the registers are 32 bits): any other access, and a
fetch, is answered with an error at once and goes nowhere. The window offset addr[ADDR_BITS-1:2]
becomes the Wishbone word address (wb_adr); wb_sel is always 4'b1111, wb_cti and wb_bte 0.
wb_err, or no wb_ack within WB_TIMEOUT_CYCLES wb_clk cycles (LiteX's CSR bus always answers;
this is for a broken or absent one), ends the Wishbone cycle and answers the access with an
error. The SurfBus side's own watchdog answers an access that has no answer TIMEOUT_CYCLES
sys cycles after acceptance with an error (the controller's clock not running, or its domain
held in reset); such an access may still be carried out later, and the next access waits
until it has been (or times out in turn), so an answer always belongs to the access it
answers. There is no fail-stop: every access is answered, by the controller or with an error.

Crossings and resets as in surf_ddr_bridge: two surf_ddr_mailbox boxes (no reset), a
never-reset flag on each side for "a request is between the domains" (sys: sent and not yet
answered; wb: taken and not yet answered, answered with an error if the wb-side reset cut it
short), and each side sends and takes only outside its own reset. Timing: the mailboxes'
crossings are constrained in surf_ddr_csr.xdc (set_max_delay -datapath_only).
*/
module surf_ddr_csr #(
    parameter integer ADDR_BITS          = 13,    // byte address bits of the window (8 KiB)
    parameter integer TIMEOUT_CYCLES     = 1024,  // sys-side watchdog, sys_clk cycles after acceptance
    parameter integer WB_TIMEOUT_CYCLES  = 256    // wb-side limit for wb_ack, wb_clk cycles
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
    // ---------------------------------------------------------------- wb_clk domain: Wishbone master
    input  wire        wb_clk,
    input  wire        wb_rst,
    output wire [29:0] wb_adr,
    output wire [31:0] wb_dat_w,
    input  wire [31:0] wb_dat_r,
    output wire [3:0]  wb_sel,
    output wire        wb_cyc,
    output wire        wb_stb,
    output wire        wb_we,
    output wire [2:0]  wb_cti,
    output wire [1:0]  wb_bte,
    input  wire        wb_ack,
    input  wire        wb_err
);
    localparam integer WA_BITS = ADDR_BITS - 2;            // word address bits
    localparam integer TO_W    = $clog2(TIMEOUT_CYCLES);
    localparam integer WTO_W   = $clog2(WB_TIMEOUT_CYCLES);
    localparam [31:0]  TO_LAST32  = TIMEOUT_CYCLES - 1;
    localparam [31:0]  WTO_LAST32 = WB_TIMEOUT_CYCLES - 1;
    localparam [TO_W-1:0]  TO_LAST  = TO_LAST32[TO_W-1:0];
    localparam [WTO_W-1:0] WTO_LAST = WTO_LAST32[WTO_W-1:0];

    // request: write, word address, write data; response: error, read data
    localparam integer RQ_W = 1 + WA_BITS + 32;
    localparam integer RS_W = 1 + 32;

    generate
        if (TIMEOUT_CYCLES < 2 || WB_TIMEOUT_CYCLES < 2) begin : g_bad_timeout
            surf_ddr_csr_timeout_too_short bad_parameter ();
        end
        if (ADDR_BITS < 3 || ADDR_BITS > 31) begin : g_bad_addr
            surf_ddr_csr_bad_window bad_parameter ();
        end
    endgenerate

    // ================================================================ sys domain
    reg              busy        = 1'b0;    // an accepted access waits for its answer
    reg              sent        = 1'b0;    // ... and has gone into the request mailbox
    reg              inflight    = 1'b0;    // never reset: a sent request's answer is still due
    reg  [RQ_W-1:0]  pend        = {RQ_W{1'b0}};
    reg  [TO_W-1:0]  wd_count    = {TO_W{1'b0}};
    reg              rsp_valid_q = 1'b0;
    reg  [31:0]      rsp_rdata_q = 32'h0;
    reg              rsp_err_q   = 1'b0;
    reg              rs_got      = 1'b0;    // an answer was taken last cycle ...
    reg  [RS_W-1:0]  rs_q        = {RS_W{1'b0}};   // ... into here (the crossing ends in registers)

    wire             rq_ready;              // request mailbox, source side
    wire             rs_valid;              // response mailbox, destination side
    wire [RS_W-1:0]  rs_dout;

    wire             accept   = s_req_valid && s_req_ready;
    wire             word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    wire [RQ_W-1:0]  new_req  = {s_req_write, s_req_addr[ADDR_BITS-1:2], s_req_wdata};
    wire             retry    = busy && !sent;
    wire             expire   = busy && wd_count == TO_LAST;  // the watchdog's last cycle
    wire             can_send = !inflight && rq_ready;        // every earlier request answered
    wire             rq_send  = !rst && can_send && ((accept && word_acc) || (retry && !expire));
    wire [RQ_W-1:0]  rq_din   = retry ? pend : new_req;
    wire             rs_take  = !rst && rs_valid;         // every answer is taken; stale ones dropped
    wire             rs_mine  = rs_got && busy && sent;   // only the current request's answer can come

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
            rsp_valid_q <= 1'b0;
            rsp_rdata_q <= 32'h0;
            rsp_err_q   <= 1'b0;
        end else begin
            rsp_valid_q <= 1'b0;
            if (accept) begin
                if (word_acc) begin
                    busy     <= 1'b1;
                    sent     <= can_send;               // in the mailbox now, or retried
                    pend     <= new_req;
                    wd_count <= {TO_W{1'b0}};
                end else begin
                    rsp_valid_q <= 1'b1;                // not a whole word: refused at once
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
                    busy        <= 1'b0;                // no answer in time: an error
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
    wire unused_addr = &{1'b0, s_req_addr[31:ADDR_BITS]};    // the window's base is the decoder's
    assign s_rsp_valid = rsp_valid_q && !rst;
    assign s_rsp_rdata = rsp_rdata_q;
    assign s_rsp_err   = rsp_err_q;

    // ================================================================ the two crossings
    wire             rq_valid;              // request mailbox, destination side
    wire [RQ_W-1:0]  rq_dout;
    wire             rq_take;
    wire             rs_ready;              // response mailbox, source side
    wire             rs_send;
    wire [RS_W-1:0]  rs_din;

    surf_ddr_mailbox #(.WIDTH(RQ_W)) req_box (
        .src_clk(clk),    .src_send(rq_send), .src_data(rq_din),  .src_ready(rq_ready),
        .dst_clk(wb_clk), .dst_valid(rq_valid), .dst_data(rq_dout), .dst_take(rq_take));

    surf_ddr_mailbox #(.WIDTH(RS_W)) rsp_box (
        .src_clk(wb_clk), .src_send(rs_send), .src_data(rs_din),  .src_ready(rs_ready),
        .dst_clk(clk),    .dst_valid(rs_valid), .dst_data(rs_dout), .dst_take(rs_take));

    // ================================================================ wb_clk domain
    localparam [1:0] W_IDLE = 2'd0,     // wait for a request and capture it
                     W_BUS  = 2'd1,     // the Wishbone cycle
                     W_RESP = 2'd2;     // put the answer into the mailbox

    reg  [1:0]         w_state = W_IDLE;
    reg                w_owe   = 1'b0;  // never reset: a taken request is not yet answered
    reg                w_we    = 1'b0;
    reg  [WA_BITS-1:0] w_adr   = {WA_BITS{1'b0}};
    reg  [31:0]        w_dat   = 32'h0;
    reg                w_err   = 1'b0;
    reg  [31:0]        w_rdata = 32'h0;
    reg  [WTO_W-1:0]   w_count = {WTO_W{1'b0}};

    assign rq_take = !wb_rst && w_state == W_IDLE && !w_owe && rq_valid;
    assign rs_send = !wb_rst && w_state == W_RESP;
    assign rs_din  = {w_err, w_rdata};

    always @(posedge wb_clk) begin
        if (rq_take)                  w_owe <= 1'b1;
        else if (rs_send && rs_ready) w_owe <= 1'b0;
    end

    always @(posedge wb_clk) begin
        if (wb_rst) begin
            w_state <= W_IDLE;
        end else begin
            case (w_state)
                W_IDLE:
                    if (w_owe) begin                    // a reset cut the last request short:
                        w_err   <= 1'b1;                // answer it, with an error
                        w_rdata <= 32'h0;
                        w_state <= W_RESP;
                    end else if (rq_valid) begin        // the crossing ends in registers
                        {w_we, w_adr, w_dat} <= rq_dout;
                        w_count <= {WTO_W{1'b0}};
                        w_state <= W_BUS;
                    end
                W_BUS:
                    if (wb_ack || wb_err) begin
                        w_err   <= wb_err;
                        w_rdata <= (wb_ack && !w_we) ? wb_dat_r : 32'h0;
                        w_state <= W_RESP;
                    end else if (w_count == WTO_LAST) begin
                        w_err   <= 1'b1;                // no answer from the bus
                        w_rdata <= 32'h0;
                        w_state <= W_RESP;
                    end else begin
                        w_count <= w_count + 1'b1;
                    end
                default:                                // W_RESP
                    if (rs_ready)
                        w_state <= W_IDLE;
            endcase
        end
    end

    wire in_bus = (w_state == W_BUS) && !wb_rst;
    assign wb_cyc   = in_bus;
    assign wb_stb   = in_bus;
    assign wb_we    = w_we;
    assign wb_adr   = {{(30 - WA_BITS){1'b0}}, w_adr};
    assign wb_dat_w = w_dat;
    assign wb_sel   = 4'b1111;
    assign wb_cti   = 3'b000;
    assign wb_bte   = 2'b00;
endmodule
