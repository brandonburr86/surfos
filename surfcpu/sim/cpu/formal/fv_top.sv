/*
SurfCPU core: formal harness (bounded model check)
--------------------
File: fv_top.sv Date: 10/15/26 (SurfCPU)
--------------------
Written by the independent review of surfcpu_core and adopted by the cpu track.
make formal-cpu runs it with SymbiYosys (sim/cpu/formal/fv.sby); the core is built with
SURFCPU_RVFI and SURFCPU_FV (the observation ports fv_*).

Reviewer's formal harness for surfcpu_core (bounded model check with SymbiYosys).
Free inputs: bus readiness and response timing, data read values and errors, interrupt
levels, mtime. Instruction memory: 8 arbitrary constant words aliased every 32 bytes
(with an arbitrary per-word fetch error bit), so the solver picks the program.
Checked: SurfBus master rules on both ports; RVFI order, x0, insn and rs/rd fields;
program-order continuity of pc across records, exceptions and interrupts (mepc of an
interrupt = the next instruction in program order); register values read = last value
written (one arbitrary register); a small RV32I spec of each record (no M); every data
access accepted on the bus is retired exactly once, by the next record, with the same
address, lanes and data; no record has a memory access that did not go to the bus.
*/
`include "surfcpu_rvfi.vh"
module fv_top (
    input wire        clk,
    input wire        ibus_req_ready,
    input wire        ibus_rsp_go,
    input wire        dbus_req_ready,
    input wire        dbus_rsp_go,
    input wire [31:0] dbus_rsp_rdata_in,
    input wire        dbus_rsp_err_in,
    input wire        irq_external,
    input wire        irq_timer,
    input wire        irq_software,
    input wire [63:0] mtime
);
    reg init = 1'b1;
    always @(posedge clk) init <= 1'b0;
    wire rst = init;

    (* anyconst *) reg [255:0] imem;
    (* anyconst *) reg [7:0]   ierr;
    (* anyconst *) reg [4:0]   chk_reg;

    wire        ibus_req_valid, ibus_req_write, ibus_req_fetch;
    wire [31:0] ibus_req_addr, ibus_req_wdata;
    wire [3:0]  ibus_req_be;
    wire        dbus_req_valid, dbus_req_write, dbus_req_fetch;
    wire [31:0] dbus_req_addr, dbus_req_wdata;
    wire [3:0]  dbus_req_be;
    `SURFCPU_RVFI_WIRES
    wire        fv_irq_take, fv_wb_wen, fv_mem_started, fv_exc_take, fv_irq_pending;
    wire [31:0] fv_mtvec, fv_mem_pc, fv_mepc;
    wire [4:0]  fv_wb_rd;

    // ---------------------------------------------------------------- slaves
    reg        i_out = 1'b0;
    reg [31:0] i_addr;
    wire       ibus_rsp_valid = !rst && i_out && ibus_rsp_go;
    wire [2:0] i_idx = i_addr[4:2];
    wire [31:0] ibus_rsp_rdata = imem[{i_idx, 5'd0} +: 32];
    wire       ibus_rsp_err = ierr[i_idx];

    reg        d_out = 1'b0;
    reg [31:0] d_addr, d_wdata;
    reg [3:0]  d_be;
    reg        d_write;
    wire       dbus_rsp_valid = !rst && d_out && dbus_rsp_go;
    wire [31:0] dbus_rsp_rdata = dbus_rsp_rdata_in;
    wire       dbus_rsp_err = dbus_rsp_err_in;

    always @(posedge clk) begin
        if (rst) begin
            i_out <= 1'b0;
            d_out <= 1'b0;
        end else begin
            if (ibus_rsp_valid) i_out <= 1'b0;
            if (ibus_req_valid && ibus_req_ready) begin i_out <= 1'b1; i_addr <= ibus_req_addr; end
            if (dbus_rsp_valid) d_out <= 1'b0;
            if (dbus_req_valid && dbus_req_ready) begin
                d_out <= 1'b1; d_addr <= dbus_req_addr; d_wdata <= dbus_req_wdata;
                d_be <= dbus_req_be; d_write <= dbus_req_write;
            end
        end
    end

    surfcpu_core #(.RESET_PC(32'h0)) core (
        .clk(clk), .rst(rst),
        .ibus_req_valid(ibus_req_valid), .ibus_req_ready(ibus_req_ready), .ibus_req_addr(ibus_req_addr),
        .ibus_req_write(ibus_req_write), .ibus_req_be(ibus_req_be), .ibus_req_wdata(ibus_req_wdata),
        .ibus_req_fetch(ibus_req_fetch), .ibus_rsp_valid(ibus_rsp_valid), .ibus_rsp_rdata(ibus_rsp_rdata),
        .ibus_rsp_err(ibus_rsp_err),
        .dbus_req_valid(dbus_req_valid), .dbus_req_ready(dbus_req_ready), .dbus_req_addr(dbus_req_addr),
        .dbus_req_write(dbus_req_write), .dbus_req_be(dbus_req_be), .dbus_req_wdata(dbus_req_wdata),
        .dbus_req_fetch(dbus_req_fetch), .dbus_rsp_valid(dbus_rsp_valid), .dbus_rsp_rdata(dbus_rsp_rdata),
        .dbus_rsp_err(dbus_rsp_err),
        .irq_external(irq_external), .irq_timer(irq_timer), .irq_software(irq_software), .mtime(mtime),
        `SURFCPU_RVFI_CONNECT,
        .fv_irq_take(fv_irq_take), .fv_mtvec(fv_mtvec), .fv_mem_pc(fv_mem_pc), .fv_wb_wen(fv_wb_wen),
        .fv_wb_rd(fv_wb_rd), .fv_mem_started(fv_mem_started), .fv_exc_take(fv_exc_take), .fv_mepc(fv_mepc),
        .fv_irq_pending(fv_irq_pending)
    );

    // ---------------------------------------------------------------- bus protocol
    reg        ip_wait = 1'b0;
    reg [31:0] ip_addr;
    reg        dp_wait = 1'b0;
    reg [31:0] dp_addr, dp_wdata;
    reg [3:0]  dp_be;
    reg        dp_write;
    always @(posedge clk) begin
        ip_wait  <= !rst && ibus_req_valid && !ibus_req_ready;
        ip_addr  <= ibus_req_addr;
        dp_wait  <= !rst && dbus_req_valid && !dbus_req_ready;
        dp_addr  <= dbus_req_addr;
        dp_wdata <= dbus_req_wdata;
        dp_be    <= dbus_req_be;
        dp_write <= dbus_req_write;
    end

    wire [1:0] doff = dbus_req_addr[1:0];
    wire be_ok = (dbus_req_be == (4'b0001 << doff)) ||
                 (!doff[0] && dbus_req_be == (4'b0011 << doff)) ||
                 (doff == 2'b00 && dbus_req_be == 4'b1111);

    always @(*) begin
        if (rst) begin
            assert (!ibus_req_valid);
            assert (!dbus_req_valid);
        end else begin
            if (ip_wait) assert (ibus_req_valid && ibus_req_addr == ip_addr);
            if (dp_wait) assert (dbus_req_valid && dbus_req_addr == dp_addr && dbus_req_wdata == dp_wdata &&
                                 dbus_req_be == dp_be && dbus_req_write == dp_write);
            if (ibus_req_valid) assert (!i_out || ibus_rsp_valid);
            if (dbus_req_valid) assert (!d_out || dbus_rsp_valid);
            if (ibus_req_valid) assert (ibus_req_addr[1:0] == 2'b00 && !ibus_req_write && ibus_req_be == 4'hF &&
                                        ibus_req_fetch && ibus_req_wdata == 32'd0);
            if (dbus_req_valid) assert (!dbus_req_fetch && be_ok);
        end
    end

    // ---------------------------------------------------------------- RVFI record checks
    reg [63:0] n_rec = 64'd0;
    reg [31:0] exp_pc = 32'd0;      // pc of the next instruction in program order
    reg        exp_intr = 1'b0;     // the next record is the first of a trap handler
    reg        chk_known = 1'b0;
    reg [31:0] chk_val;
    reg        exp_mem = 1'b0;      // a data response arrived last cycle: its record is due now
    reg        e_err, e_write;
    reg [31:0] e_addr, e_wdata, e_rdata;
    reg [3:0]  e_be;

    wire [31:0] cur_exp  = rvfi_valid ? rvfi_pc_wdata : exp_pc;
    wire        cur_intr = rvfi_valid ? rvfi_trap : exp_intr;

    always @(posedge clk) begin
        if (rst) begin
            n_rec <= 64'd0;
            exp_pc <= 32'd0;
            exp_intr <= 1'b0;
            chk_known <= 1'b0;
            exp_mem <= 1'b0;
        end else begin
            if (rvfi_valid) n_rec <= n_rec + 64'd1;
            exp_pc   <= fv_irq_take ? fv_mtvec : cur_exp;
            exp_intr <= fv_irq_take ? 1'b1 : cur_intr;
            if (rvfi_valid && chk_reg != 5'd0 && rvfi_rd_addr == chk_reg) begin
                chk_known <= 1'b1;
                chk_val <= rvfi_rd_wdata;
            end
            exp_mem <= dbus_rsp_valid;
            e_err   <= dbus_rsp_err;
            e_addr  <= d_addr;
            e_be    <= d_be;
            e_write <= d_write;
            e_wdata <= d_wdata;
            e_rdata <= dbus_rsp_rdata;
        end
    end

    function [31:0] lanes(input [3:0] be);
        lanes = {{8{be[3]}}, {8{be[2]}}, {8{be[1]}}, {8{be[0]}}};
    endfunction

    // decode of the record
    wire [6:0]  op  = rvfi_insn[6:0];
    wire [2:0]  f3  = rvfi_insn[14:12];
    wire [6:0]  f7  = rvfi_insn[31:25];
    wire [31:0] i_imm = {{20{rvfi_insn[31]}}, rvfi_insn[31:20]};
    wire [31:0] s_imm = {{20{rvfi_insn[31]}}, rvfi_insn[31:25], rvfi_insn[11:7]};
    wire [31:0] b_imm = {{19{rvfi_insn[31]}}, rvfi_insn[31], rvfi_insn[7], rvfi_insn[30:25], rvfi_insn[11:8], 1'b0};
    wire [31:0] u_imm = {rvfi_insn[31:12], 12'd0};
    wire [31:0] j_imm = {{11{rvfi_insn[31]}}, rvfi_insn[31], rvfi_insn[19:12], rvfi_insn[20], rvfi_insn[30:21], 1'b0};
    wire [31:0] ra = rvfi_rs1_rdata;
    wire [31:0] rb = rvfi_rs2_rdata;
    wire [31:0] pc = rvfi_pc_rdata;
    wire [31:0] pc4 = pc + 32'd4;
    wire [2:0]  pidx = pc[4:2];
    wire [31:0] word_at_pc = imem[{pidx, 5'd0} +: 32];
    wire [31:0] bits_at_pc = (word_at_pc[1:0] == 2'b11) ? word_at_pc : {16'd0, word_at_pc[15:0]};

    wire is_lui = op == 7'b0110111, is_auipc = op == 7'b0010111, is_jal = op == 7'b1101111;
    wire is_jalr = op == 7'b1100111, is_br = op == 7'b1100011, is_ld = op == 7'b0000011;
    wire is_st = op == 7'b0100011, is_opi = op == 7'b0010011, is_op = op == 7'b0110011;
    wire is_sys = op == 7'b1110011;
    wire is_csr = is_sys && f3 != 3'b000;
    wire is_mret = rvfi_insn == 32'h30200073;
    wire uses_rs1 = is_jalr || is_br || is_ld || is_st || is_opi || is_op || (is_csr && !f3[2]);
    wire uses_rs2 = is_br || is_st || is_op;
    wire writes_rd = is_lui || is_auipc || is_jal || is_jalr || is_ld || is_opi || is_op || is_csr;

    reg br_taken;
    always @(*) begin
        case (f3)
            3'b000: br_taken = ra == rb;
            3'b001: br_taken = ra != rb;
            3'b100: br_taken = $signed(ra) < $signed(rb);
            3'b101: br_taken = !($signed(ra) < $signed(rb));
            3'b110: br_taken = ra < rb;
            default: br_taken = !(ra < rb);
        endcase
    end

    wire [31:0] sra_val = $signed(ra) >>> (is_opi ? i_imm[4:0] : rb[4:0]);
    reg        spec_rd_ok;          // spec_rd is defined for this record
    reg [31:0] spec_rd;
    reg [31:0] spec_npc;
    reg [31:0] opb;
    always @(*) begin
        spec_rd_ok = 1'b0;
        spec_rd = 32'd0;
        spec_npc = pc4;
        opb = is_opi ? i_imm : rb;
        if (is_lui) begin spec_rd_ok = 1'b1; spec_rd = u_imm; end
        if (is_auipc) begin spec_rd_ok = 1'b1; spec_rd = pc + u_imm; end
        if (is_jal) begin spec_rd_ok = 1'b1; spec_rd = pc4; spec_npc = pc + j_imm; end
        if (is_jalr) begin spec_rd_ok = 1'b1; spec_rd = pc4; spec_npc = (ra + i_imm) & ~32'd1; end
        if (is_br) spec_npc = br_taken ? pc + b_imm : pc4;
        if (is_opi || (is_op && f7 != 7'b0000001)) begin
            spec_rd_ok = 1'b1;
            case (f3)
                3'b000: spec_rd = (is_op && rvfi_insn[30]) ? ra - opb : ra + opb;
                3'b001: spec_rd = ra << opb[4:0];
                3'b010: spec_rd = {31'd0, $signed(ra) < $signed(opb)};
                3'b011: spec_rd = {31'd0, ra < opb};
                3'b100: spec_rd = ra ^ opb;
                3'b101: spec_rd = rvfi_insn[30] ? sra_val : (ra >> opb[4:0]);
                3'b110: spec_rd = ra | opb;
                default: spec_rd = ra & opb;
            endcase
        end
        if (is_mret) spec_npc = fv_mepc;
    end

    // loads and stores
    wire [31:0] ld_addr = ra + i_imm;
    wire [31:0] st_addr = ra + s_imm;
    wire [31:0] ls_addr = is_st ? st_addr : ld_addr;
    wire [1:0]  ls_off = ls_addr[1:0];
    wire        ls_mis = (f3[1:0] == 2'b01 && ls_off[0]) || (f3[1:0] == 2'b10 && ls_off != 2'b00);
    wire [3:0]  ls_be = (f3[1:0] == 2'b00) ? (4'b0001 << ls_off) : (f3[1:0] == 2'b01) ? (4'b0011 << ls_off) : 4'b1111;
    wire [31:0] ld_word = rvfi_mem_rdata >> {ls_off, 3'b000};
    reg  [31:0] ld_val;
    always @(*) begin
        case (f3)
            3'b000:  ld_val = {{24{ld_word[7]}}, ld_word[7:0]};
            3'b001:  ld_val = {{16{ld_word[15]}}, ld_word[15:0]};
            3'b100:  ld_val = {24'd0, ld_word[7:0]};
            3'b101:  ld_val = {16'd0, ld_word[15:0]};
            default: ld_val = ld_word;
        endcase
    end
    wire [31:0] st_data = (rb << {ls_off, 3'b000}) & lanes(ls_be);

    always @(*) begin
        if (!rst) begin
            // a register write always has its record
            if (fv_wb_wen) assert (rvfi_valid && fv_wb_rd != 5'd0);
            // an interrupt replaces the next instruction in program order, before it started
            if (fv_irq_take) begin
                assert (fv_mem_pc == cur_exp);
                assert (!fv_mem_started);
                assert (fv_irq_pending);
            end
            // the data access answered last cycle retires now, exactly once
            if (exp_mem) begin
                assert (rvfi_valid);
                if (e_err) assert (rvfi_trap && rvfi_mem_rmask == 4'd0 && rvfi_mem_wmask == 4'd0);
                else begin
                    assert (!rvfi_trap && rvfi_mem_addr == {e_addr[31:2], 2'b00});
                    if (e_write) assert (is_st && rvfi_mem_wmask == e_be && rvfi_mem_rmask == 4'd0 &&
                                         rvfi_mem_wdata == (e_wdata & lanes(e_be)));
                    else assert (is_ld && rvfi_mem_rmask == e_be && rvfi_mem_wmask == 4'd0 &&
                                 rvfi_mem_rdata == (e_rdata & lanes(e_be)));
                end
            end else if (rvfi_valid) begin
                assert (rvfi_mem_rmask == 4'd0 && rvfi_mem_wmask == 4'd0);
            end
            if (rvfi_valid) begin
                assert (rvfi_order == n_rec);
                assert (rvfi_pc_rdata == exp_pc);
                assert (rvfi_intr == exp_intr);
                assert (rvfi_mode == 2'd3 && rvfi_ixl == 2'd1 && !rvfi_halt);
                if (rvfi_rs1_addr == 5'd0) assert (rvfi_rs1_rdata == 32'd0);
                if (rvfi_rs2_addr == 5'd0) assert (rvfi_rs2_rdata == 32'd0);
                if (rvfi_rd_addr == 5'd0) assert (rvfi_rd_wdata == 32'd0);
                if (chk_known && chk_reg != 5'd0) begin
                    if (rvfi_rs1_addr == chk_reg) assert (rvfi_rs1_rdata == chk_val);
                    if (rvfi_rs2_addr == chk_reg) assert (rvfi_rs2_rdata == chk_val);
                end
                // the instruction reported is the word in memory at pc (0 for a fetch error)
                if (ierr[pidx]) assert (rvfi_trap && rvfi_insn == 32'd0);
                else assert (rvfi_insn == bits_at_pc);
                if (rvfi_trap) begin
                    assert (rvfi_rd_addr == 5'd0);
                    assert (rvfi_pc_wdata == fv_mtvec);
                    assert (fv_mepc == pc);
                    // a control transfer that traps (and was fetched and decoded) has a misaligned target
                    if (!ierr[pidx] && (is_jal || (is_jalr && f3 == 3'b000))) assert (spec_npc[1]);
                    if (!ierr[pidx] && is_br && f3 != 3'b010 && f3 != 3'b011) assert (br_taken && spec_npc[1]);
                    // a load or store that traps is misaligned or got an error response
                    if (!ierr[pidx] && ((is_ld && f3 != 3'b011 && f3 != 3'b110 && f3 != 3'b111) ||
                                        (is_st && f3[2:1] == 2'b00) || (is_st && f3 == 3'b010)))
                        assert (ls_mis || (exp_mem && e_err));
                end else begin
                    if (uses_rs1) assert (rvfi_rs1_addr == rvfi_insn[19:15]);
                    if (uses_rs2) assert (rvfi_rs2_addr == rvfi_insn[24:20]);
                    assert (rvfi_rd_addr == (writes_rd ? rvfi_insn[11:7] : 5'd0));
                    assert (rvfi_pc_wdata == spec_npc);
                    if (spec_rd_ok && rvfi_rd_addr != 5'd0) assert (rvfi_rd_wdata == spec_rd);
                    if (is_ld) begin
                        assert (exp_mem && !e_err && !e_write && !ls_mis);
                        assert (rvfi_mem_addr == {ld_addr[31:2], 2'b00} && rvfi_mem_rmask == ls_be);
                        if (rvfi_rd_addr != 5'd0) assert (rvfi_rd_wdata == ld_val);
                    end
                    if (is_st) begin
                        assert (exp_mem && !e_err && e_write && !ls_mis);
                        assert (rvfi_mem_addr == {st_addr[31:2], 2'b00} && rvfi_mem_wmask == ls_be &&
                                rvfi_mem_wdata == st_data);
                    end
                end
            end
        end
    end

    // reachability of interesting situations
    always @(*) begin
        if (!rst) begin
            cover (rvfi_valid && rvfi_intr && !exp_mem && n_rec > 2);
            cover (fv_irq_take && n_rec >= 2);
            cover (rvfi_valid && is_st && !rvfi_trap && n_rec >= 3);
            cover (rvfi_valid && rvfi_trap && exp_mem);
        end
    end
endmodule
