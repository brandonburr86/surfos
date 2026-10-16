/*
SurfCPU GPIO
--------------------
File: surf_gpio.v Date: 10/15/26 (SurfCPU)
--------------------
gpio at 0x1000_3000 (docs/MEMORY-MAP.md): the Arty's four green LEDs, four RGB LEDs, four
slide switches and four push buttons; interrupt source 2.
The switch and button pins are synchronized (two flip-flops each). Each button is then
debounced: its state follows the synchronized pin only after the pin has differed from it
for 10 consecutive ticks of a shared 1 ms tick (SYS_HZ / 1000 cycles), so 9 to 10 ms of
stable level, and a bounce restarts the count. A debounced press (0 to 1) sets its BTN_EDGE
bit, which stays set until software writes 1 to it (a press in the same cycle wins).
irq = OR of (BTN_EDGE & IE). LED drives the green LEDs directly. RGB drives the RGB LEDs at a
fixed 1/8 duty cycle (on for 256 of every 2048 cycles, about 12 kHz at 25 MHz), because they
are too bright at full drive; the outputs are registered. A SurfBus slave (docs/SURFBUS.md)
that accepts every request and answers in the next cycle; an unknown offset, a non-word
access or a fetch gets an error and has no effect; writes to SW and BTN are ignored.
*/
module surf_gpio #(
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
    output wire [3:0]  led,
    output wire [11:0] rgb,
    input  wire [3:0]  sw,
    input  wire [3:0]  btn
);
    `include "surfcpu_soc.vh"

    localparam integer MS_CYCLES = (SYS_HZ / 1000 < 2) ? 2 : SYS_HZ / 1000;
    localparam integer MS_W      = $clog2(MS_CYCLES);
    localparam integer MS_LAST_I = MS_CYCLES - 1;
    localparam [MS_W-1:0] MS_LAST = MS_LAST_I[MS_W-1:0];
    localparam [3:0]   DB_LAST   = 4'd9;     // 10 ticks of 1 ms

    // ---------------------------------------------------------------- inputs
    wire [3:0] sw_s, btn_s;
    genvar g;
    generate
        for (g = 0; g < 4; g = g + 1) begin : g_sync
            surf_sync #(.STAGES(2), .INIT(1'b0)) u_sw  (.clk(clk), .rst(rst), .d(sw[g]),  .q(sw_s[g]));
            surf_sync #(.STAGES(2), .INIT(1'b0)) u_btn (.clk(clk), .rst(rst), .d(btn[g]), .q(btn_s[g]));
        end
    endgenerate

    // a 1 ms tick shared by the four debouncers
    reg [MS_W-1:0] ms_cnt;
    reg            ms_tick;
    always @(posedge clk) begin
        if (rst) begin
            ms_cnt  <= {MS_W{1'b0}};
            ms_tick <= 1'b0;
        end else begin
            ms_tick <= (ms_cnt == MS_LAST);
            ms_cnt  <= (ms_cnt == MS_LAST) ? {MS_W{1'b0}} : ms_cnt + 1'b1;
        end
    end

    wire [3:0] btn_db;
    generate
        for (g = 0; g < 4; g = g + 1) begin : g_db
            reg       state;
            reg [3:0] cnt;
            always @(posedge clk) begin
                if (rst) begin
                    state <= 1'b0;
                    cnt   <= 4'd0;
                end else if (btn_s[g] == state) begin
                    cnt <= 4'd0;
                end else if (ms_tick) begin
                    if (cnt == DB_LAST) begin
                        state <= btn_s[g];
                        cnt   <= 4'd0;
                    end else begin
                        cnt <= cnt + 4'd1;
                    end
                end
            end
            assign btn_db[g] = state;
        end
    endgenerate

    // ---------------------------------------------------------------- registers
    reg [3:0]  led_q;
    reg [11:0] rgb_q;
    reg [3:0]  ie_q;
    reg [3:0]  edge_q;
    reg [3:0]  btn_db_d;     // the debounced state one cycle ago, for edge detection

    wire [3:0] btn_rise = btn_db & ~btn_db_d;

    wire [9:0] woff     = s_req_addr[11:2];
    wire       word_acc = (s_req_be == 4'b1111) && (s_req_addr[1:0] == 2'b00) && !s_req_fetch;
    reg        known;
    reg [31:0] rd_mux;
    always @* begin
        known  = 1'b1;
        rd_mux = 32'h0;
        case (woff)
            SURF_GPIO_LED[11:2]:      rd_mux[SURF_GPIO_LED_LED_LSB +: SURF_GPIO_LED_LED_WIDTH] = led_q;
            SURF_GPIO_RGB[11:2]:      rd_mux[SURF_GPIO_RGB_RGB_LSB +: SURF_GPIO_RGB_RGB_WIDTH] = rgb_q;
            SURF_GPIO_SW[11:2]:       rd_mux[SURF_GPIO_SW_SW_LSB +: SURF_GPIO_SW_SW_WIDTH] = sw_s;
            SURF_GPIO_BTN[11:2]:      rd_mux[SURF_GPIO_BTN_BTN_LSB +: SURF_GPIO_BTN_BTN_WIDTH] = btn_db;
            SURF_GPIO_BTN_EDGE[11:2]: rd_mux[SURF_GPIO_BTN_EDGE_BTN_LSB +: SURF_GPIO_BTN_EDGE_BTN_WIDTH] = edge_q;
            SURF_GPIO_IE[11:2]:       rd_mux[SURF_GPIO_IE_BTN_LSB +: SURF_GPIO_IE_BTN_WIDTH] = ie_q;
            default:                  known = 1'b0;
        endcase
    end

    wire ok    = s_req_valid && word_acc && known;
    wire do_wr = ok && s_req_write;
    wire [3:0] edge_clr = (do_wr && woff == SURF_GPIO_BTN_EDGE[11:2]) ?
                          s_req_wdata[SURF_GPIO_BTN_EDGE_BTN_LSB +: SURF_GPIO_BTN_EDGE_BTN_WIDTH] : 4'h0;

    // RGB at 1/8 duty: on while the top three bits of an 11-bit counter are 0
    reg [10:0] pwm_cnt;
    reg [11:0] rgb_out;
    reg [3:0]  led_out;

    reg        rsp_valid_q, rsp_err_q;
    reg [31:0] rsp_rdata_q;
    always @(posedge clk) begin
        if (rst) begin
            led_q       <= 4'h0;
            rgb_q       <= 12'h0;
            ie_q        <= 4'h0;
            edge_q      <= 4'h0;
            btn_db_d    <= 4'h0;
            pwm_cnt     <= 11'd0;
            rgb_out     <= 12'h0;
            led_out     <= 4'h0;
            rsp_valid_q <= 1'b0;
            rsp_err_q   <= 1'b0;
            rsp_rdata_q <= 32'h0;
        end else begin
            if (do_wr && woff == SURF_GPIO_LED[11:2])
                led_q <= s_req_wdata[SURF_GPIO_LED_LED_LSB +: SURF_GPIO_LED_LED_WIDTH];
            if (do_wr && woff == SURF_GPIO_RGB[11:2])
                rgb_q <= s_req_wdata[SURF_GPIO_RGB_RGB_LSB +: SURF_GPIO_RGB_RGB_WIDTH];
            if (do_wr && woff == SURF_GPIO_IE[11:2])
                ie_q <= s_req_wdata[SURF_GPIO_IE_BTN_LSB +: SURF_GPIO_IE_BTN_WIDTH];
            btn_db_d <= btn_db;
            edge_q   <= (edge_q & ~edge_clr) | btn_rise;

            pwm_cnt <= pwm_cnt + 11'd1;
            rgb_out <= (pwm_cnt[10:8] == 3'b000) ? rgb_q : 12'h0;
            led_out <= led_q;

            rsp_valid_q <= s_req_valid;
            rsp_err_q   <= s_req_valid && !(word_acc && known);
            rsp_rdata_q <= (ok && !s_req_write) ? rd_mux : 32'h0;
        end
    end

    assign s_req_ready = 1'b1;
    assign s_rsp_valid = rsp_valid_q;
    assign s_rsp_err   = rsp_err_q;
    assign s_rsp_rdata = rsp_rdata_q;
    assign led         = led_out;
    assign rgb         = rgb_out;
    assign irq         = |(edge_q & ie_q);

    // the decoder has already matched the window (address bits 31:12); no register is
    // wider than 12 bits
    wire unused_ok = &{1'b0, s_req_addr[31:12], s_req_wdata[31:12]};
endmodule
