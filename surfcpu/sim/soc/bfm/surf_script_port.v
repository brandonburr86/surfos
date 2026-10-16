/*
SurfCPU bus-script engine
--------------------
File: surf_script_port.v Date: 10/15/26 (SurfCPU)
--------------------
One SurfBus master port of the scripted core stand-in (sim/soc/stubs/surfcpu_core.v): it loads
the compiled bus script named by +script=<file> (sim/soc/tools/surfscript.py writes it) and
executes this port's list of operations, checking every response. Simulation only, clocked
like RTL so Icarus and Verilator (without --timing) both run it.

Script file ($readmemh words): a header (word 0 = 0x53425331 "SBS1", 1 = data-port record
count, 2 = its first word, 3 = instruction-port record count, 4 = its first word), then 8-word
records: w0 = op << 24 | flags << 16 | byte enables, w1 = address, w2 = data, w3 = mask,
w4 = limit, w5 = source line, w6 = poll interval. Ops: READ (check (rdata & mask) == (data &
mask), or an error when flag 0 is set), WRITE (OK or, with flag 0, an error), POLL (read until
the masked data match, at most `limit` reads, `interval` idle cycles apart), IDLE (data
cycles), SYNC (wait until the other port reaches the same barrier), WAITIRQ (until the masked
interrupt inputs {software, timer, external} equal data, within limit cycles), CHECKIRQ (the
same, now), MTIME (the core's mtime input minus the last read data is at most data), END.

Bus behaviour: one request at a time, held until accepted; before each request 0..+gap=N
(default 3) idle cycles from an LFSR seeded by +seed=N, zero half of the time, and with a zero
gap the next request is offered in the same cycle as the previous response, so slaves see
back-to-back traffic. The instruction port (PORT = 1) always drives write 0, byte enables
1111, write data 0 and fetch 1 (docs/SURFBUS.md section 1). Each failed check prints the
script line and counts in `fails`; `done` rises at END.
*/
module surf_script_port #(
    parameter integer PORT = 0           // 0 = data port, 1 = instruction port
) (
    input  wire        clk,
    input  wire        rst,
    output wire        m_req_valid,
    input  wire        m_req_ready,
    output wire [31:0] m_req_addr,
    output wire        m_req_write,
    output wire [3:0]  m_req_be,
    output wire [31:0] m_req_wdata,
    output wire        m_req_fetch,
    input  wire        m_rsp_valid,
    input  wire [31:0] m_rsp_rdata,
    input  wire        m_rsp_err,
    input  wire [2:0]  irqs,             // {irq_software, irq_timer, irq_external}
    input  wire [63:0] mtime,
    input  wire [31:0] other_syncs,      // barriers the other port has reached
    output reg  [31:0] my_syncs,
    output reg         done,
    output reg  [31:0] fails,
    output reg  [31:0] n_bus,            // bus transfers completed
    output reg  [31:0] n_ops             // operations completed
);
    localparam integer   AW         = 18;           // program memory: 2^18 words
    localparam [31:0]    MAGIC      = 32'h5342_5331;
    localparam [7:0]     OP_READ    = 8'd1;
    localparam [7:0]     OP_WRITE   = 8'd2;
    localparam [7:0]     OP_POLL    = 8'd3;
    localparam [7:0]     OP_IDLE    = 8'd4;
    localparam [7:0]     OP_SYNC    = 8'd5;
    localparam [7:0]     OP_WAITIRQ = 8'd6;
    localparam [7:0]     OP_CHKIRQ  = 8'd7;
    localparam [7:0]     OP_MTIME   = 8'd8;
    localparam [7:0]     OP_END     = 8'd9;
    localparam [7:0]     PNAME      = (PORT == 1) ? "I" : "D";

    reg [31:0] prog [0:(1 << AW) - 1];
    reg [8*1024-1:0] path;
    reg        loaded;
    integer    seed, maxgap;
    initial begin
        loaded = 1'b0;
        seed   = 1;
        maxgap = 3;
        prog[0] = 32'h0;
        if ($value$plusargs("script=%s", path)) begin
            $readmemh(path, prog);
            loaded = (prog[0] == MAGIC);
            if (!loaded) $display("script %0s: %0s is not a compiled bus script", PNAME, path);
        end
        if ($value$plusargs("seed=%d", seed)) begin end
        if ($value$plusargs("gap=%d", maxgap)) begin end
    end

    // ---------------------------------------------------------------- state
    localparam [2:0] S_INIT = 3'd0;
    localparam [2:0] S_GO   = 3'd1;   // working on op pc (offering it, if a bus op)
    localparam [2:0] S_RSP  = 3'd2;   // the request of op pc was accepted; waiting for the response
    localparam [2:0] S_DONE = 3'd3;
    localparam [2:0] S_NONE = 3'd4;   // no script: never issue anything

    reg [2:0]  state;
    reg [31:0] base, count, pc;
    reg [31:0] gap_cnt;               // idle cycles before the current bus op's request
    reg [31:0] gap_pre;               // the gap already drawn for the next bus op
    reg [31:0] cnt;                   // IDLE / WAITIRQ cycles, POLL reads done
    reg        at_sync;
    reg [31:0] lfsr;
    reg [31:0] last_rdata;

    function [31:0] lfsr_next;        // Galois LFSR, taps 32 22 2 1
        input [31:0] v;
        begin
            lfsr_next = v[0] ? ((v >> 1) ^ 32'h8020_0003) : (v >> 1);
        end
    endfunction

    function [31:0] gap_of;           // 0 half of the time, else 1..maxgap
        input [31:0] v;
        begin
            if (maxgap <= 0 || v[0]) gap_of = 32'h0;
            else                     gap_of = 32'h1 + ({1'b0, v[31:1]} % maxgap);
        end
    endfunction

    // ---------------------------------------------------------------- the current and next records
    wire [AW-1:0] ra      = base[AW-1:0] + {pc[AW-4:0], 3'b000};
    wire [31:0]   c_w0    = prog[ra];
    wire [31:0]   c_addr  = prog[ra + 1];
    wire [31:0]   c_data  = prog[ra + 2];
    wire [31:0]   c_mask  = prog[ra + 3];
    wire [31:0]   c_limit = prog[ra + 4];
    wire [31:0]   c_line  = prog[ra + 5];
    wire [31:0]   c_ival  = prog[ra + 6];
    wire [7:0]    c_op    = c_w0[31:24];
    wire          c_experr = c_w0[16];
    wire          c_bus   = (c_op == OP_READ) || (c_op == OP_WRITE) || (c_op == OP_POLL);

    wire          rsp_match  = !m_rsp_err && ((m_rsp_rdata & c_mask) == (c_data & c_mask));
    wire          poll_retry = (c_op == OP_POLL) && !rsp_match && !m_rsp_err && (cnt + 32'd1 < c_limit);
    wire [31:0]   npc     = poll_retry ? pc : pc + 32'd1;
    wire [AW-1:0] na      = base[AW-1:0] + {npc[AW-4:0], 3'b000};
    wire [31:0]   n_w0    = prog[na];
    wire [31:0]   n_addr  = prog[na + 1];
    wire [31:0]   n_data  = prog[na + 2];
    wire [7:0]    n_op    = n_w0[31:24];
    wire          n_isbus = (npc < count) &&
                            ((n_op == OP_READ) || (n_op == OP_WRITE) || (n_op == OP_POLL));

    // offer the current op's request, or (fast) the next op's in the cycle of this response
    wire offer = (state == S_GO) && (pc < count) && c_bus && (gap_cnt == 32'h0);
    wire fast  = (state == S_RSP) && m_rsp_valid && n_isbus && (gap_pre == 32'h0) &&
                 !(poll_retry && c_ival != 32'h0);

    // the request fields are 0 while nothing is offered
    assign m_req_valid = offer || fast;
    assign m_req_addr  = fast ? n_addr : (offer ? c_addr : 32'h0);
    assign m_req_write = (PORT == 1) ? 1'b0 :
                         (fast ? (n_op == OP_WRITE) : (offer && c_op == OP_WRITE));
    assign m_req_be    = (PORT == 1) ? 4'hF : (fast ? n_w0[3:0] : (offer ? c_w0[3:0] : 4'h0));
    assign m_req_wdata = (PORT == 1) ? 32'h0 : (fast ? n_data : (offer ? c_data : 32'h0));
    assign m_req_fetch = (PORT == 1);

    task fail;
        input [8*48-1:0] what;
        begin
            $display("script %0s line %0d: FAIL %0s (addr %h, rdata %h, err %b, expected %h mask %h)",
                     PNAME, c_line, what, c_addr, m_rsp_rdata, m_rsp_err, c_data, c_mask);
            fails <= fails + 32'd1;
        end
    endtask

    always @(posedge clk) begin
        if (rst) begin
            state    <= S_INIT;
            pc       <= 32'h0;
            cnt      <= 32'h0;
            gap_cnt  <= 32'h0;
            gap_pre  <= 32'h0;
            at_sync  <= 1'b0;
            my_syncs <= 32'h0;
            done     <= 1'b0;
            fails    <= 32'h0;
            n_bus    <= 32'h0;
            n_ops    <= 32'h0;
            lfsr     <= seed ^ ((PORT == 1) ? 32'h5A5A_1234 : 32'hC0DE_0001);
        end else begin
            case (state)
                S_INIT: begin
                    if (!loaded) begin
                        state <= S_NONE;
                    end else begin
                        base    <= (PORT == 1) ? prog[4] : prog[2];
                        count   <= (PORT == 1) ? prog[3] : prog[1];
                        pc      <= 32'h0;
                        gap_cnt <= gap_of(lfsr);
                        lfsr    <= lfsr_next(lfsr);
                        state   <= S_GO;
                    end
                end
                S_GO: begin
                    if (pc >= count) begin
                        done  <= 1'b1;
                        state <= S_DONE;
                    end else begin
                        case (c_op)
                            OP_READ, OP_WRITE, OP_POLL: begin
                                if (gap_cnt != 32'h0) begin
                                    gap_cnt <= gap_cnt - 32'd1;
                                end else if (m_req_ready) begin
                                    state   <= S_RSP;
                                    gap_pre <= gap_of(lfsr);
                                    lfsr    <= lfsr_next(lfsr);
                                end
                            end
                            OP_IDLE: begin
                                if (cnt + 32'd1 >= c_data) begin
                                    cnt <= 32'h0; pc <= pc + 32'd1; n_ops <= n_ops + 32'd1;
                                end else begin
                                    cnt <= cnt + 32'd1;
                                end
                            end
                            OP_SYNC: begin
                                if (!at_sync) begin
                                    at_sync  <= 1'b1;
                                    my_syncs <= my_syncs + 32'd1;
                                end else if (other_syncs >= my_syncs) begin
                                    at_sync <= 1'b0; pc <= pc + 32'd1; n_ops <= n_ops + 32'd1;
                                end
                            end
                            OP_WAITIRQ: begin
                                if (({29'h0, irqs} & c_mask) == (c_data & c_mask)) begin
                                    cnt <= 32'h0; pc <= pc + 32'd1; n_ops <= n_ops + 32'd1;
                                end else if (cnt >= c_limit) begin
                                    fail("interrupt level not reached in time");
                                    cnt <= 32'h0; pc <= pc + 32'd1; n_ops <= n_ops + 32'd1;
                                end else begin
                                    cnt <= cnt + 32'd1;
                                end
                            end
                            OP_CHKIRQ: begin
                                if (({29'h0, irqs} & c_mask) != (c_data & c_mask))
                                    fail("interrupt levels differ");
                                pc <= pc + 32'd1; n_ops <= n_ops + 32'd1;
                            end
                            OP_MTIME: begin
                                if (mtime[31:0] - last_rdata > c_data)
                                    fail("mtime input does not follow the timer");
                                pc <= pc + 32'd1; n_ops <= n_ops + 32'd1;
                            end
                            OP_END: begin
                                done  <= 1'b1;
                                state <= S_DONE;
                            end
                            default: begin
                                fail("unknown operation");
                                done  <= 1'b1;
                                state <= S_DONE;
                            end
                        endcase
                    end
                end
                S_RSP: begin
                    if (m_rsp_valid) begin
                        n_bus <= n_bus + 32'd1;
                        last_rdata <= m_rsp_rdata;
                        case (c_op)
                            OP_READ:
                                if (c_experr) begin
                                    if (!m_rsp_err) fail("read should have failed");
                                end else if (m_rsp_err) begin
                                    fail("read failed");
                                end else if (!rsp_match) begin
                                    fail("read data");
                                end
                            OP_WRITE:
                                if (m_rsp_err != c_experr)
                                    fail(c_experr ? "write should have failed" : "write failed");
                            default:     // OP_POLL
                                if (m_rsp_err)                    fail("poll read failed");
                                else if (!rsp_match && !poll_retry) fail("poll never matched");
                        endcase
                        if (!poll_retry) n_ops <= n_ops + 32'd1;
                        cnt <= poll_retry ? cnt + 32'd1 : 32'h0;
                        pc  <= npc;
                        if (fast) begin
                            if (m_req_ready) begin
                                state   <= S_RSP;
                                gap_pre <= gap_of(lfsr);
                                lfsr    <= lfsr_next(lfsr);
                            end else begin
                                state   <= S_GO;
                                gap_cnt <= 32'h0;
                            end
                        end else begin
                            state   <= S_GO;
                            gap_cnt <= (poll_retry && c_ival != 32'h0) ? c_ival : gap_pre;
                        end
                    end
                end
                default: ;   // S_DONE, S_NONE
            endcase
        end
    end

    wire unused_ok = &{1'b0, mtime[63:32], base[31:AW], pc[31:AW-3], npc[31:AW-3], c_w0[23:17],
                       c_w0[15:4], n_w0[23:4]};
endmodule
