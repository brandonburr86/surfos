/*
SurfCPU DDR bridge proof harness
--------------------
File: bridge_formal.sv Date: 10/15/26 (SurfCPU)
--------------------
Formal harness for surf_ddr_bridge (bridge.sby), with a short watchdog (TIMEOUT_CYCLES = 4)
and calibration limit (3) to keep the state small. Everything on the controller side is free:
init_calib_complete, app_rdy, app_wdf_rdy, read data valid at any time or never, ui_rst at any
time, and both clocks are free (any interleaving of edges, either clock may stop). rst is free
too. The SurfBus master is a legal one built here (one request outstanding, never withdrawn,
idle in reset) whose choices are free. Proved in every reachable state (SURFBUS.md rules 4, 7
and 8 for the bridge, and two DDR_STATUS rules):
  * a response only while an accepted request waits for one, never during rst (so exactly one
    response per request; rst cancels the request);
  * no request waits more than TIMEOUT_CYCLES + 1 sys cycles;
  * calib_done and calib_fail are never both 1;
  * timeout, once set, stays set until rst;
  * (task stale, defining STALE_CHECK) a successful response only after the controller
    accepted a command (app_en with app_rdy) since its request was accepted: no stale answer
    from an earlier request, for example one cut short by rst or abandoned by the watchdog, is
    ever delivered as the answer to a later one.
Synchronizers are plain flip-flops here: metastability is outside the proof.
*/
module bridge_formal (
    input wire         clk,
    input wire         ui_clk,
    input wire         rst,
    input wire         ui_rst,
    // the master's free choices
    input wire         offer,
    input wire         f_write,
    input wire [31:0]  f_addr,
    input wire [3:0]   f_be,
    input wire [31:0]  f_wdata,
    // the controller, entirely free
    input wire         init_calib_complete,
    input wire         app_rdy,
    input wire         app_wdf_rdy,
    input wire [127:0] app_rd_data,
    input wire         app_rd_data_valid,
    input wire         app_rd_data_end
);
    localparam integer TIMEOUT = 4;

    reg         m_valid = 1'b0;
    reg         m_write = 1'b0;
    reg  [31:0] m_addr  = 32'h0;
    reg  [3:0]  m_be    = 4'hF;
    reg  [31:0] m_wdata = 32'h0;
    reg         waiting = 1'b0;         // accepted, no response yet
    reg  [3:0]  waited  = 4'd0;         // sys cycles it has waited
    reg         was_timeout = 1'b0;
    reg  [3:0]  cmds        = 4'd0;     // commands the controller accepted (ui_clk edges)
    reg  [3:0]  cmds_at_acc = 4'd0;     // ... when the waiting request was accepted

    wire        s_req_ready, s_rsp_valid, s_rsp_err, calib_done, calib_fail, timeout;
    wire [31:0] s_rsp_rdata;
    wire [27:0] app_addr;
    wire [2:0]  app_cmd;
    wire        app_en, app_wdf_wren, app_wdf_end, app_sr_req, app_ref_req, app_zq_req;
    wire [127:0] app_wdf_data;
    wire [15:0] app_wdf_mask;

    surf_ddr_bridge #(.TIMEOUT_CYCLES(TIMEOUT), .CALIB_TIMEOUT_CYCLES(3)) dut (
        .clk(clk), .rst(rst), .s_req_valid(m_valid), .s_req_ready(s_req_ready),
        .s_req_addr(m_addr), .s_req_write(m_write), .s_req_be(m_be), .s_req_wdata(m_wdata),
        .s_req_fetch(1'b0), .s_rsp_valid(s_rsp_valid), .s_rsp_rdata(s_rsp_rdata),
        .s_rsp_err(s_rsp_err), .calib_done(calib_done), .calib_fail(calib_fail), .timeout(timeout),
        .ui_clk(ui_clk), .ui_rst(ui_rst), .init_calib_complete(init_calib_complete),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_end(app_wdf_end), .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data),
        .app_rd_data_valid(app_rd_data_valid), .app_rd_data_end(app_rd_data_end),
        .app_sr_req(app_sr_req), .app_ref_req(app_ref_req), .app_zq_req(app_zq_req));

    always @(posedge ui_clk)
        if (app_en && app_rdy) cmds <= cmds + 4'd1;

    // a legal SurfBus master: offers only with nothing outstanding, keeps the offer until taken
    always @(posedge clk) begin
        was_timeout <= timeout && !rst;
        if (rst) begin
            m_valid <= 1'b0;
            waiting <= 1'b0;
            waited  <= 4'd0;
        end else begin
            if (m_valid && s_req_ready) begin
                m_valid     <= 1'b0;
                waiting     <= 1'b1;
                waited      <= 4'd0;
                cmds_at_acc <= cmds;
            end else if (s_rsp_valid) begin
                waiting <= 1'b0;
            end else if (waiting && waited != 4'hF) begin
                waited <= waited + 4'd1;
            end
            if (!m_valid && !waiting && offer) begin
                m_valid <= 1'b1;
                m_write <= f_write;
                m_addr  <= f_addr;
                m_be    <= f_be;
                m_wdata <= f_wdata;
            end
        end
    end

    always @(*) begin
        if (s_rsp_valid) assert (waiting && !rst);
        assert (waited <= TIMEOUT + 1);
        assert (!(calib_done && calib_fail));
        if (was_timeout && !rst) assert (timeout);
`ifdef STALE_CHECK
        if (s_rsp_valid && !s_rsp_err) assert (cmds != cmds_at_acc);
`endif
        cover (s_rsp_valid && !s_rsp_err);   // a real answer from the controller side
    end
endmodule
