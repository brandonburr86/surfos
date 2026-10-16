/*
SurfCPU Arty A7 milestone 1 baseline logic
--------------------
File: arty_a7_baseline_core.v Date: 10/15/26 (SurfCPU)
--------------------
Everything of the milestone 1 design except the clock tiles (arty_a7_baseline.v adds those),
so it simulates without them. No CPU: it shows the owner that the board, the clocks, the reset
and the serial link work (docs/BOARD.md section 5).

  LD4..LD7 (led[3:0])   switches SW0..SW3, as set
  LD0..LD3 blue         buttons BTN0..BTN3, while pressed
  LD0 green             sys_clk heartbeat: 1 Hz, on for the first half of each second
  LD1 green             pix_clk heartbeat: 1 Hz, counted in the pixel clock domain
  UART TX, 115200 8N1   one line a second (the first one second after reset):

  SurfCPU baseline arty_a7_100t 0x00000100 build 1a2b3c4d dirty 1760560000 | up 000001 s | pix 25173611 Hz | sw 0101 btn 0000 rx --

The line holds the board and its sysinfo BOARD_ID, the build identity (commit, clean or dirty,
"sim" in simulation, the build time in seconds since 1970), the seconds since reset, the pixel
clock as measured against sys_clk over the last second (it should read clocks.pix_hz, +-1; the
first line covers the time since reset, so it can be off by a few counts), the switches and buttons
(SW3..SW0, BTN3..BTN0), and the last byte received on the UART's RX pin in hex.

The RGB LEDs are lit 1/2^RGB_DIM_SHIFT of the time (0..3; Digilent advises against full drive,
the GPIO block of soc.toml uses 1/8). The CDC paths are surf_sync (pins, the pix heartbeat),
surf_pulse_sync (the 1 s gate into the pixel domain) and surf_cdc_word (the count back).
*/
module arty_a7_baseline_core #(
    parameter         BOARD           = "sim",   // spec/soc.toml [boards] name
    parameter [31:0]  BUILD_ID        = 32'h0,   // first 8 hex digits of the commit
    parameter [31:0]  BUILD_FLAGS     = 32'h0,   // sysinfo.BUILD_FLAGS: bit 0 dirty, bit 1 sim
    parameter [31:0]  BUILD_TIME      = 32'h0,   // seconds since 1970-01-01 UTC
    parameter integer TICK_CYCLES     = 0,       // sys_clk cycles per second; 0 = clocks.sys_hz
    parameter integer PIX_TICK_CYCLES = 0,       // pix_clk cycles per heartbeat second; 0 = clocks.pix_hz
    parameter integer RGB_DIM_SHIFT   = 3        // RGB LEDs on 1/2^n of the time (0 = always)
) (
    input  wire        clk,                      // sys_clk
    input  wire        rst,
    input  wire        pix_clk,
    input  wire        pix_rst,
    input  wire [3:0]  sw,                       // pins, asynchronous
    input  wire [3:0]  btn,                      // pins, asynchronous
    output reg  [3:0]  led    = 4'h0,
    output reg  [11:0] rgb    = 12'h000,         // LED n: bit 3n red, 3n+1 green, 3n+2 blue
    input  wire        uart_rx,                  // pin, asynchronous
    output wire        uart_tx
);
    `include "surfcpu_soc.vh"

    localparam integer SECOND     = (TICK_CYCLES > 0) ? TICK_CYCLES : SURF_SYS_HZ;
    localparam integer PIX_SECOND = (PIX_TICK_CYCLES > 0) ? PIX_TICK_CYCLES : SURF_PIX_HZ;
    localparam integer UART_DIV   = (SURF_SYS_HZ + 57600) / 115200;   // round(sys_hz / 115200), as surf_uart

    // ---------------------------------------------------------------- elaboration-time text
    // The waivers cover this elaboration-time text only: BOARD and the pieces of the line are
    // strings of different lengths that Verilog zero-extends on the left, which app() relies
    // on, and dec10 does whole-word arithmetic to keep one byte of it.
    /* verilator lint_off WIDTHEXPAND */
    /* verilator lint_off UNUSEDSIGNAL */
    localparam [31:0] BOARD_ID = (BOARD == "arty_a7_35t")  ? SURF_BOARD_ARTY_A7_35T :
                                 (BOARD == "arty_a7_100t") ? SURF_BOARD_ARTY_A7_100T : SURF_BOARD_SIM;

    // The line's template: plain characters, and codes for the live fields
    // (8x uptime digit x, 9x pixel clock digit x, Ax switch/button bit x, Bx the rx byte).
    localparam integer TMAX = 160;

    function [8*TMAX+7:0] app;                   // {length, text}: append s's non-NUL characters
        input [8*TMAX+7:0] acc;
        input [8*40-1:0]   s;                    // right-aligned string, up to 40 characters
        integer k;
        reg [7:0]          len;
        reg [8*TMAX-1:0]   txt;
        begin
            len = acc[8*TMAX +: 8];
            txt = acc[8*TMAX-1:0];
            for (k = 39; k >= 0; k = k - 1)
                if (s[8*k +: 8] != 8'h00) begin
                    txt = {txt[8*TMAX-9:0], s[8*k +: 8]};
                    len = len + 8'd1;
                end
            app = {len, txt};
        end
    endfunction

    function [8*8-1:0] hex8;                     // eight lowercase hex digits
        input [31:0] v;
        integer k;
        begin
            for (k = 0; k < 8; k = k + 1)
                hex8[8*k +: 8] = (v[4*k +: 4] < 4'd10) ? 8'h30 + {4'h0, v[4*k +: 4]}
                                                       : 8'h57 + {4'h0, v[4*k +: 4]};
        end
    endfunction

    function [8*10-1:0] dec10;                   // ten decimal digits
        input [31:0] v;
        integer k;
        reg [31:0] x, d;
        begin
            x = v;
            for (k = 0; k < 10; k = k + 1) begin
                d = x % 10;
                dec10[8*k +: 8] = 8'h30 + d[7:0];
                x = x / 10;
            end
        end
    endfunction

    function [8*TMAX+7:0] make_template;
        input [1:0] flags;                       // BUILD_FLAGS: dirty, sim
        reg [8*TMAX+7:0] t;
        begin
            t = {(8*TMAX+8){1'b0}};
            t = app(t, "SurfCPU baseline ");
            t = app(t, BOARD);
            t = app(t, " 0x");
            t = app(t, hex8(BOARD_ID));
            t = app(t, " build ");
            t = app(t, hex8(BUILD_ID));
            t = app(t, flags[0] ? " dirty" : " clean");
            if (flags[1]) t = app(t, " sim");
            t = app(t, " ");
            t = app(t, dec10(BUILD_TIME));
            t = app(t, " | up ");
            t = app(t, {8'h85, 8'h84, 8'h83, 8'h82, 8'h81, 8'h80});
            t = app(t, " s | pix ");
            t = app(t, {8'h97, 8'h96, 8'h95, 8'h94, 8'h93, 8'h92, 8'h91, 8'h90});
            t = app(t, " Hz | sw ");
            t = app(t, {8'hA3, 8'hA2, 8'hA1, 8'hA0});
            t = app(t, " btn ");
            t = app(t, {8'hA7, 8'hA6, 8'hA5, 8'hA4});
            t = app(t, " rx ");
            t = app(t, {8'hB1, 8'hB0});
            t = app(t, {8'h0D, 8'h0A});
            make_template = t;
        end
    endfunction
    /* verilator lint_on UNUSEDSIGNAL */
    /* verilator lint_on WIDTHEXPAND */

    localparam [8*TMAX+7:0] TEMPLATE = make_template(BUILD_FLAGS[1:0]);
    localparam integer      TLEN     = {24'd0, TEMPLATE[8*TMAX +: 8]};
    localparam [8*TMAX-1:0] TEXT     = TEMPLATE[8*TMAX-1:0];   // last character in bits 7:0

    // ---------------------------------------------------------------- counter limits
    localparam integer SW_  = $clog2(SECOND);
    localparam integer PW_  = $clog2(PIX_SECOND);
    localparam integer SEC_LAST_I = SECOND - 1, SEC_TICK_I = SECOND - 2, SEC_HALF_I = SECOND / 2;
    localparam integer PIX_LAST_I = PIX_SECOND - 1, PIX_HALF_I = PIX_SECOND / 2;
    localparam [SW_-1:0] SEC_LAST = SEC_LAST_I[SW_-1:0];
    localparam [SW_-1:0] SEC_TICK = SEC_TICK_I[SW_-1:0];
    localparam [SW_-1:0] SEC_HALF = SEC_HALF_I[SW_-1:0];
    localparam [PW_-1:0] PIX_LAST = PIX_LAST_I[PW_-1:0];
    localparam [PW_-1:0] PIX_HALF = PIX_HALF_I[PW_-1:0];
    localparam integer   TLAST_I  = TLEN - 1;
    localparam [7:0]     TLAST    = TLAST_I[7:0];

    // BCD increment of an n-digit number (combinational helpers)
    function [31:0] bcd_inc(input [31:0] v);
        integer k;
        reg carry;
        begin
            carry = 1'b1;
            for (k = 0; k < 8; k = k + 1) begin
                if (carry && v[4*k +: 4] == 4'd9) begin
                    bcd_inc[4*k +: 4] = 4'd0;
                end else begin
                    bcd_inc[4*k +: 4] = v[4*k +: 4] + {3'b000, carry};
                    carry = 1'b0;
                end
            end
        end
    endfunction

    // ---------------------------------------------------------------- inputs
    wire [3:0] sw_s, btn_s;
    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : in
            surf_sync u_sw  (.clk(clk), .rst(rst), .d(sw[i]),  .q(sw_s[i]));
            surf_sync u_btn (.clk(clk), .rst(rst), .d(btn[i]), .q(btn_s[i]));
        end
    endgenerate

    // ---------------------------------------------------------------- the second
    reg  [SW_-1:0] sec_cnt  = {SW_{1'b0}};
    reg            tick     = 1'b0;              // one cycle at the end of each second
    reg  [31:0]    uptime   = 32'h0;             // seconds since reset in BCD (six digits shown)
    reg            go_wait  = 1'b0;              // the line starts 256 cycles after the tick
    reg  [7:0]     go_delay = 8'h0;
    wire [31:0]    uptime_inc = bcd_inc(uptime);

    always @(posedge clk) begin
        if (rst) begin
            sec_cnt  <= {SW_{1'b0}};
            tick     <= 1'b0;
            uptime   <= 32'h0;
            go_wait  <= 1'b0;
            go_delay <= 8'h0;
        end else begin
            tick <= (sec_cnt == SEC_TICK);
            if (sec_cnt == SEC_LAST) sec_cnt <= {SW_{1'b0}};
            else                     sec_cnt <= sec_cnt + 1'b1;
            if (tick) begin
                uptime  <= uptime_inc;
                go_wait <= 1'b1;
            end
            if (go_wait) go_delay <= go_delay + 8'd1;
            if (go_wait && go_delay == 8'hFF) go_wait <= 1'b0;
        end
    end
    wire banner_go = go_wait && go_delay == 8'hFF;
    wire hb_sys    = (sec_cnt < SEC_HALF);

    // ---------------------------------------------------------------- pixel clock: heartbeat and counter
    reg  [PW_-1:0] pix_sec  = {PW_{1'b0}};
    reg            hb_pix   = 1'b0;
    reg  [31:0]    pix_cnt  = 32'h0;             // pix_clk cycles since the last gate, eight BCD digits
    reg  [31:0]    pix_meas = 32'h0;             // the count over the last whole gate interval
    wire [31:0]    pix_cnt_inc = bcd_inc(pix_cnt);
    wire           pix_gate;

    surf_pulse_sync u_gate (.src_clk(clk), .src_rst(rst), .src_pulse(tick),
                            .dst_clk(pix_clk), .dst_rst(pix_rst), .dst_pulse(pix_gate));

    always @(posedge pix_clk) begin
        if (pix_rst) begin
            pix_sec  <= {PW_{1'b0}};
            hb_pix   <= 1'b0;
            pix_cnt  <= 32'h0;
            pix_meas <= 32'h0;
        end else begin
            if (pix_sec == PIX_LAST) pix_sec <= {PW_{1'b0}};
            else                     pix_sec <= pix_sec + 1'b1;
            hb_pix <= (pix_sec < PIX_HALF);
            if (pix_gate) begin
                pix_meas <= pix_cnt;
                pix_cnt  <= 32'h1;
            end else begin
                pix_cnt  <= pix_cnt_inc;
            end
        end
    end

    wire [31:0] pix_meas_s;
    surf_cdc_word #(.WIDTH(32)) u_meas (.src_clk(pix_clk), .src_rst(pix_rst), .src_data(pix_meas),
                                        .dst_clk(clk), .dst_rst(rst), .dst_data(pix_meas_s));
    wire hb_pix_s;
    surf_sync u_hb (.clk(clk), .rst(rst), .d(hb_pix), .q(hb_pix_s));

    // ---------------------------------------------------------------- UART receive
    wire       rx_valid;
    wire [7:0] rx_data;
    reg  [7:0] rx_last = 8'h00;
    reg        rx_seen = 1'b0;
    arty_a7_uart_rx #(.DIV(UART_DIV)) u_rx (.clk(clk), .rst(rst), .rx(uart_rx),
                                            .valid(rx_valid), .data(rx_data));
    always @(posedge clk) begin
        if (rst) begin
            rx_seen <= 1'b0;
            rx_last <= 8'h00;
        end else if (rx_valid) begin
            rx_seen <= 1'b1;
            rx_last <= rx_data;
        end
    end

    // ---------------------------------------------------------------- the line
    reg        sending       = 1'b0;
    reg [7:0]  idx           = 8'h0;
    reg [31:0] up_snap       = 32'h0;            // BCD; the line shows the low six digits
    reg [31:0] pix_snap      = 32'h0;
    reg [7:0]  io_snap       = 8'h0;             // {btn, sw}
    reg [7:0]  rx_snap       = 8'h0;
    reg        rx_valid_snap = 1'b0;

    wire [7:0] pos  = TLAST - idx;               // characters left after this one
    wire [7:0] code = TEXT[8 * pos +: 8];

    function [7:0] hexc(input [3:0] n);
        hexc = (n < 4'd10) ? 8'h30 + {4'h0, n} : 8'h57 + {4'h0, n};
    endfunction

    reg [7:0] ch;
    always @* begin
        case (code[7:4])
            4'h8:    ch = {4'h3, up_snap[4 * code[2:0] +: 4]};
            4'h9:    ch = {4'h3, pix_snap[4 * code[2:0] +: 4]};
            4'hA:    ch = io_snap[code[2:0]] ? "1" : "0";
            4'hB:    ch = !rx_valid_snap ? "-" : hexc(code[0] ? rx_snap[7:4] : rx_snap[3:0]);
            default: ch = code;
        endcase
    end

    wire tx_ready;
    arty_a7_uart_tx #(.DIV(UART_DIV)) u_tx (.clk(clk), .rst(rst), .valid(sending), .data(ch),
                                            .ready(tx_ready), .tx(uart_tx));

    always @(posedge clk) begin
        if (rst) begin
            sending <= 1'b0;
            idx     <= 8'h0;
        end else if (!sending) begin
            if (banner_go) begin                 // a line still being sent skips this second
                sending       <= 1'b1;
                idx           <= 8'h0;
                up_snap       <= uptime;
                pix_snap      <= pix_meas_s;
                io_snap       <= {btn_s, sw_s};
                rx_snap       <= rx_last;
                rx_valid_snap <= rx_seen;
            end
        end else if (tx_ready) begin             // the transmitter takes ch at this edge
            if (idx == TLAST) begin
                sending <= 1'b0;
                idx     <= 8'h0;
            end else begin
                idx <= idx + 8'd1;
            end
        end
    end

    // ---------------------------------------------------------------- LEDs
    reg  [10:0] dim = 11'h0;                     // RGB duty cycle: on while dim's top bits are 0
    wire [2:0]  dim_top = dim[10:8];
    wire        rgb_on  = (RGB_DIM_SHIFT == 0) || (dim_top >> (3 - RGB_DIM_SHIFT)) == 3'd0;
    wire [11:0] rgb_level = {btn_s[3], 2'b00,               // LD3: blue
                             btn_s[2], 2'b00,               // LD2: blue
                             btn_s[1], hb_pix_s, 1'b0,      // LD1: blue, green = pix heartbeat
                             btn_s[0], hb_sys,   1'b0};     // LD0: blue, green = sys heartbeat

    always @(posedge clk) begin
        if (rst) begin
            led <= 4'h0;
            rgb <= 12'h000;
            dim <= 11'h0;
        end else begin
            dim <= dim + 11'd1;
            led <= sw_s;
            rgb <= rgb_on ? rgb_level : 12'h000;
        end
    end
endmodule
