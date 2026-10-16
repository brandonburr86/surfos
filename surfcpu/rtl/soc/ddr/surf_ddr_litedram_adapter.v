/*
SurfCPU DDR bridge to LiteDRAM adapter
--------------------
File: surf_ddr_litedram_adapter.v Date: 10/15/26 (SurfCPU)
--------------------
Lets surf_ddr_bridge, whose controller side is the MIG 7-series native user interface, drive a
LiteDRAM core's native user port instead (the open-source flow, ip/litedram/, docs/DDR.md).
Everything runs on LiteDRAM's user clock (user_clk, the bridge's ui_clk) and its reset
(user_rst, the bridge's ui_rst).

  MIG side (from the bridge)          LiteDRAM native port
  app_en/app_cmd/app_addr/app_rdy  -> cmd_valid/cmd_we/cmd_addr/cmd_ready
  app_wdf_wren/data/mask/app_wdf_rdy -> a two-beat write-data FIFO -> wdata_valid/data/we/ready
  app_rd_data/_valid/_end          <- rdata_data/rdata_valid (rdata_ready is always 1)
  init_calib_complete              <- init_done && !init_error

The MIG takes write data before its command (the bridge relies on that: data first, then the
command), but LiteDRAM only takes write data once a write command has gone in. The FIFO holds
the beat until LiteDRAM asks for it, so app_wdf_rdy never waits for a command. app_addr counts
16-bit words with the rank on top and three zero burst bits; cmd_addr counts 128-bit beats.
The MIG's mask says which bytes to keep (1), LiteDRAM's we which to write (1). The MIG never
holds back read data, so neither does the adapter.
*/
module surf_ddr_litedram_adapter #(
    parameter integer APP_ADDR_WIDTH = 28,      // MIG app_addr (rank + bank + row + column)
    parameter integer DATA_WIDTH     = 128,     // MIG app data = LiteDRAM native port data
    parameter integer CMD_ADDR_WIDTH = 24,      // LiteDRAM cmd_addr (beats)
    parameter integer BURST_BITS     = 3        // app_addr bits inside a beat (always 0)
) (
    input  wire                         clk,            // LiteDRAM user_clk
    input  wire                         rst,            // LiteDRAM user_rst
    // to surf_ddr_bridge (its MIG-style controller port)
    output wire                         ui_rst,
    output wire                         init_calib_complete,
    input  wire [APP_ADDR_WIDTH-1:0]    app_addr,
    input  wire [2:0]                   app_cmd,
    input  wire                         app_en,
    output wire                         app_rdy,
    input  wire [DATA_WIDTH-1:0]        app_wdf_data,
    input  wire [DATA_WIDTH/8-1:0]      app_wdf_mask,
    input  wire                         app_wdf_wren,
    input  wire                         app_wdf_end,
    output wire                         app_wdf_rdy,
    output wire [DATA_WIDTH-1:0]        app_rd_data,
    output wire                         app_rd_data_valid,
    output wire                         app_rd_data_end,
    input  wire                         app_sr_req,
    input  wire                         app_ref_req,
    input  wire                         app_zq_req,
    // to the LiteDRAM core
    input  wire                         init_done,
    input  wire                         init_error,
    output wire                         cmd_valid,
    input  wire                         cmd_ready,
    output wire                         cmd_we,
    output wire [CMD_ADDR_WIDTH-1:0]    cmd_addr,
    output wire                         wdata_valid,
    input  wire                         wdata_ready,
    output wire [DATA_WIDTH-1:0]        wdata_data,
    output wire [DATA_WIDTH/8-1:0]      wdata_we,
    input  wire                         rdata_valid,
    output wire                         rdata_ready,
    input  wire [DATA_WIDTH-1:0]        rdata_data
);
    localparam integer MASK_W = DATA_WIDTH / 8;

    generate
        if (BURST_BITS + CMD_ADDR_WIDTH + 1 != APP_ADDR_WIDTH) begin : g_bad_addr
            surf_ddr_litedram_adapter_address_widths_disagree bad_parameter ();   // on purpose
        end
    endgenerate

    assign ui_rst              = rst;
    assign init_calib_complete = init_done && !init_error;

    // commands go straight through
    assign cmd_valid = app_en;
    assign app_rdy   = cmd_ready;
    assign cmd_we    = (app_cmd == 3'b000);
    assign cmd_addr  = app_addr[BURST_BITS +: CMD_ADDR_WIDTH];

    // write data waits in a small FIFO until LiteDRAM takes it
    wire                    wf_empty, wf_full;
    wire [1:0]              wf_count;
    wire [MASK_W+DATA_WIDTH-1:0] wf_out;
    surf_fifo #(.WIDTH(MASK_W + DATA_WIDTH), .DEPTH_LOG2(1)) wdata_fifo (
        .clk(clk), .rst(rst),
        .wr_en(app_wdf_wren && !wf_full), .wr_data({~app_wdf_mask, app_wdf_data}),
        .rd_en(wdata_ready && !wf_empty), .rd_data(wf_out),
        .empty(wf_empty), .full(wf_full), .count(wf_count));
    assign app_wdf_rdy = !wf_full;
    assign wdata_valid = !wf_empty;
    assign wdata_we    = wf_out[MASK_W+DATA_WIDTH-1:DATA_WIDTH];
    assign wdata_data  = wf_out[DATA_WIDTH-1:0];

    // read data comes back in order, one beat per read, never held back
    assign rdata_ready       = 1'b1;
    assign app_rd_data       = rdata_data;
    assign app_rd_data_valid = rdata_valid;
    assign app_rd_data_end   = rdata_valid;

    // Not needed: app_wdf_end (every beat is a whole burst), the self-refresh/refresh/ZQ
    // requests (the bridge never makes them), the rank bit and the burst bits of app_addr.
    wire unused = &{1'b0, app_wdf_end, app_sr_req, app_ref_req, app_zq_req, wf_count,
                    app_addr[APP_ADDR_WIDTH-1], app_addr[BURST_BITS-1:0]};
endmodule
