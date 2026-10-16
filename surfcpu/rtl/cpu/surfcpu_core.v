/*
SurfCPU core
--------------------
File: surfcpu_core.v Date: 10/15/26 (SurfCPU)
--------------------
SurfCPU: RV32IM + Zicsr + Zifencei in machine mode (docs/ISA.md), an in-order five-stage
pipeline with two SurfBus master ports (docs/SURFBUS.md). docs/CPU.md describes it with
diagrams; in short:

  IF   surfcpu_fetch: sequential requests on the instruction port, four buffered words
  ID   decode of the oldest buffered word; register file read with the WB bypass
  EX   operand forwarding (from MEM and WB), ALU, branch decision and target, load/store
       address, alignment checks, multiply, the multicycle divider
  MEM  the commit point: interrupts and exceptions are taken here; a load or store goes
       to the data port only from here, so it can no longer be cancelled; CSR
       instructions, MRET, WFI and FENCE.I take effect here
  WB   register file write, and the RVFI retirement record

Each stage has a valid bit; a stage advances when the next one can take its instruction.
Hazards:
  * an operand produced by an older instruction still in MEM or WB is forwarded into EX;
    while EX waits it keeps refreshing its operand registers, so a value is never lost
    when the producer retires before the consumer moves on;
  * a load or CSR read in MEM produces its value only as it leaves MEM, so a dependent
    instruction waits in EX for one cycle after it (the load-use interlock);
  * a taken branch or jump redirects fetch as it leaves EX (the two younger stages are
    flushed); a trap, MRET or FENCE.I redirects from MEM and flushes everything younger,
    and wins over a branch in EX in the same cycle (it is older).
Precise traps: an instruction carries its exception from IF/ID/EX to MEM and traps only
there, when everything older has completed; an interrupt is taken in place of the
instruction in MEM before it does anything (never once its bus access has been offered;
not on a WFI, which completes and lets the next instruction take it).
*/
`include "surfcpu_rvfi.vh"

module surfcpu_core #(
    parameter [31:0] RESET_PC = 32'h0000_0000,
    parameter [31:0] MIMPID   = 32'h0000_0100
) (
    input  wire        clk,
    input  wire        rst,             // synchronous, active high
    // instruction port (SurfBus master, SURFBUS.md)
    output wire        ibus_req_valid,
    input  wire        ibus_req_ready,
    output wire [31:0] ibus_req_addr,
    output wire        ibus_req_write,  // 0
    output wire [3:0]  ibus_req_be,     // 4'b1111
    output wire [31:0] ibus_req_wdata,  // 0
    output wire        ibus_req_fetch,  // 1
    input  wire        ibus_rsp_valid,
    input  wire [31:0] ibus_rsp_rdata,
    input  wire        ibus_rsp_err,
    // data port (SurfBus master)
    output wire        dbus_req_valid,
    input  wire        dbus_req_ready,
    output wire [31:0] dbus_req_addr,
    output wire        dbus_req_write,
    output wire [3:0]  dbus_req_be,
    output wire [31:0] dbus_req_wdata,
    output wire        dbus_req_fetch,  // 0
    input  wire        dbus_rsp_valid,
    input  wire [31:0] dbus_rsp_rdata,
    input  wire        dbus_rsp_err,
    // interrupts: levels, synchronous to clk
    input  wire        irq_external,    // mip.MEIP
    input  wire        irq_timer,       // mip.MTIP
    input  wire        irq_software,    // mip.MSIP
    input  wire [63:0] mtime            // for the time/timeh CSRs
`ifdef SURFCPU_RVFI
    , `SURFCPU_RVFI_OUTPUTS             // ISA.md section 5
`endif
);
`include "surfcpu_defs.vh"

    // ==================================================================== declarations
    // fetch -> ID
    wire        redirect;            // change of flow this cycle (from EX or MEM)
    wire [31:0] redirect_pc;
    wire        id_valid;
    wire [31:0] id_pc;
    wire [31:0] id_insn;
    wire        id_fault;
    wire        id_advance;          // ID's instruction moves to EX at this edge

    // EX stage registers (loaded from ID)
    reg         ex_valid;
    reg  [31:0] ex_pc;
    reg  [31:0] ex_insn;
    reg         ex_use_rs1;
    reg         ex_use_rs2;
    reg         ex_rd_wen;
    reg  [3:0]  ex_alu_op;
    reg         ex_a_pc;
    reg         ex_a_zero;
    reg         ex_b_imm;
    reg  [31:0] ex_imm;
    reg         ex_branch;
    reg         ex_jal;
    reg         ex_jalr;
    reg         ex_load;
    reg         ex_store;
    reg         ex_mul;
    reg         ex_div;
    reg         ex_csr;
    reg         ex_csr_write;
    reg         ex_mret;
    reg         ex_wfi;
    reg         ex_fencei;
    reg         ex_exc;              // an exception found in IF or ID
    reg  [4:0]  ex_cause;
    reg  [31:0] ex_tval;
    reg  [31:0] ex_rs1_val;          // operand values, kept current while EX waits
    reg  [31:0] ex_rs2_val;

    // MEM stage registers (loaded from EX)
    reg         mem_valid;
    reg  [31:0] mem_pc;
    reg  [31:0] mem_insn;
    reg         mem_rd_wen;
    reg         mem_load;
    reg         mem_store;
    reg         mem_csr;
    reg         mem_csr_write;
    reg         mem_mret;
    reg         mem_wfi;
    reg         mem_fencei;
    reg  [31:0] mem_result;          // EX's result; the address for a load or store
    reg  [31:0] mem_opnd;            // store data, or the CSR source operand
    reg         mem_exc;             // an exception found in IF, ID or EX
    reg  [4:0]  mem_cause;
    reg  [31:0] mem_tval;
    reg         mem_offered;         // the data request was offered and not yet accepted
    reg         mem_waiting;         // the data request was accepted; its response is due

    // WB stage registers (loaded from MEM)
    reg         wb_wen;              // write wb_wdata to register wb_rd at this edge
    reg  [4:0]  wb_rd;
    reg  [31:0] wb_wdata;

    // ==================================================================== IF
    surfcpu_fetch #(.RESET_PC(RESET_PC)) fetch (
        .clk(clk),
        .rst(rst),
        .redirect(redirect),
        .redirect_pc(redirect_pc),
        .id_valid(id_valid),
        .id_pc(id_pc),
        .id_insn(id_insn),
        .id_fault(id_fault),
        .id_pop(id_advance),
        .ibus_req_valid(ibus_req_valid),
        .ibus_req_ready(ibus_req_ready),
        .ibus_req_addr(ibus_req_addr),
        .ibus_rsp_valid(ibus_rsp_valid),
        .ibus_rsp_rdata(ibus_rsp_rdata),
        .ibus_rsp_err(ibus_rsp_err)
    );
    assign ibus_req_write = 1'b0;
    assign ibus_req_be    = 4'b1111;
    assign ibus_req_wdata = 32'd0;
    assign ibus_req_fetch = 1'b1;

    // ==================================================================== ID
    wire        d_illegal, d_ecall, d_ebreak;
    wire        d_use_rs1, d_use_rs2, d_rd_wen;
    wire [3:0]  d_alu_op;
    wire        d_a_pc, d_a_zero, d_b_imm;
    wire [31:0] d_imm;
    wire        d_branch, d_jal, d_jalr, d_load, d_store, d_mul, d_div;
    wire        d_csr, d_csr_write, d_mret, d_wfi, d_fencei;

    surfcpu_decode decode (
        .insn(id_insn),
        .illegal(d_illegal),
        .ecall(d_ecall),
        .ebreak(d_ebreak),
        .use_rs1(d_use_rs1),
        .use_rs2(d_use_rs2),
        .rd_wen(d_rd_wen),
        .alu_op(d_alu_op),
        .a_pc(d_a_pc),
        .a_zero(d_a_zero),
        .b_imm(d_b_imm),
        .imm(d_imm),
        .is_branch(d_branch),
        .is_jal(d_jal),
        .is_jalr(d_jalr),
        .is_load(d_load),
        .is_store(d_store),
        .is_mul(d_mul),
        .is_div(d_div),
        .is_csr(d_csr),
        .csr_write(d_csr_write),
        .is_mret(d_mret),
        .is_wfi(d_wfi),
        .is_fencei(d_fencei)
    );

    // A fetch fault comes first; the decoder already zeroes its controls for the others.
    wire        id_ok    = !id_fault;
    wire        id_exc   = id_fault || d_illegal || d_ecall || d_ebreak;
    wire [4:0]  id_cause = id_fault  ? CAUSE_FETCH_FAULT :
                           d_illegal ? CAUSE_ILLEGAL :
                           d_ecall   ? CAUSE_ECALL_M : CAUSE_BREAKPOINT;
    wire [31:0] id_tval  = id_fault  ? id_pc :          // the fetch address
                           d_illegal ? id_insn :        // the instruction bits
                           d_ebreak  ? id_pc : 32'd0;   // EBREAK's address; ECALL: 0

    wire [4:0]  id_rs1 = id_insn[19:15];
    wire [4:0]  id_rs2 = id_insn[24:20];
    wire [31:0] rf_rd1, rf_rd2;

    surfcpu_regfile regfile (
        .clk(clk),
        .ra1(id_rs1),
        .rd1(rf_rd1),
        .ra2(id_rs2),
        .rd2(rf_rd2),
        .we(wb_wen),
        .wa(wb_rd),
        .wd(wb_wdata)
    );

    // x0 reads 0; a register being written by WB this cycle reads the new value.
    wire [31:0] id_rs1_val = (id_rs1 == 5'd0)              ? 32'd0 :
                             (wb_wen && wb_rd == id_rs1)    ? wb_wdata : rf_rd1;
    wire [31:0] id_rs2_val = (id_rs2 == 5'd0)              ? 32'd0 :
                             (wb_wen && wb_rd == id_rs2)    ? wb_wdata : rf_rd2;

    // ==================================================================== EX
    wire [4:0]  ex_rs1 = ex_insn[19:15];
    wire [4:0]  ex_rs2 = ex_insn[24:20];
    wire [2:0]  ex_f3  = ex_insn[14:12];
    wire [4:0]  mem_rd = mem_insn[11:7];

    // Forwarding. A load or CSR instruction in MEM has its value only when it leaves MEM
    // ("late"); everything else carries its result from EX in mem_result.
    wire mem_late    = mem_load || mem_csr;
    wire mem_fwd     = mem_valid && mem_rd_wen && !mem_late;
    wire mem_pending = mem_valid && mem_rd_wen && mem_late;

    wire [31:0] ex_a = (mem_fwd && mem_rd == ex_rs1) ? mem_result :
                       (wb_wen  && wb_rd  == ex_rs1) ? wb_wdata   : ex_rs1_val;
    wire [31:0] ex_b = (mem_fwd && mem_rd == ex_rs2) ? mem_result :
                       (wb_wen  && wb_rd  == ex_rs2) ? wb_wdata   : ex_rs2_val;

    // load-use interlock: wait until the producer has reached WB
    wire ex_wait_operand = mem_pending &&
                           ((ex_use_rs1 && ex_rs1 == mem_rd) || (ex_use_rs2 && ex_rs2 == mem_rd));

    // ALU
    wire [31:0] alu_a = ex_a_zero ? 32'd0 : ex_a_pc ? ex_pc : ex_a;
    wire [31:0] alu_b = ex_b_imm  ? ex_imm : ex_b;
    wire [31:0] alu_y;
    surfcpu_alu alu (.op(ex_alu_op), .a(alu_a), .b(alu_b), .y(alu_y));

    // branch decision and targets
    wire br_eq  = (ex_a == ex_b);
    wire br_lt  = ($signed(ex_a) < $signed(ex_b));
    wire br_ltu = (ex_a < ex_b);
    reg  br_cond;
    always @(*) begin
        case (ex_f3)
            3'b000:  br_cond = br_eq;     // BEQ
            3'b001:  br_cond = !br_eq;    // BNE
            3'b100:  br_cond = br_lt;     // BLT
            3'b101:  br_cond = !br_lt;    // BGE
            3'b110:  br_cond = br_ltu;    // BLTU
            3'b111:  br_cond = !br_ltu;   // BGEU
            default: br_cond = 1'b0;      // not decoded as a branch
        endcase
    end
    wire        ex_taken  = ex_jal || ex_jalr || (ex_branch && br_cond);
    wire [31:0] ex_pc4    = ex_pc + 32'd4;
    wire [31:0] ex_target = ex_jalr ? {alu_y[31:1], 1'b0} : (ex_pc + ex_imm);

    // exceptions found here: a misaligned target (bit 0 is always 0 here, so bit 1 says it),
    // a misaligned load or store
    wire ex_target_mis = ex_taken && ex_target[1];
    wire ex_ldst       = ex_load || ex_store;
    wire ex_ldst_mis   = ex_ldst && ((ex_f3[1:0] == 2'b01 && alu_y[0]) ||
                                     (ex_f3[1:0] == 2'b10 && alu_y[1:0] != 2'b00));
    wire        ex_exc_all   = ex_exc || ex_target_mis || ex_ldst_mis;
    wire [4:0]  ex_cause_all = ex_exc        ? ex_cause :
                               ex_target_mis ? CAUSE_FETCH_MISALIGNED :
                               ex_load       ? CAUSE_LOAD_MISALIGNED : CAUSE_STORE_MISALIGNED;
    wire [31:0] ex_tval_all  = ex_exc        ? ex_tval :
                               ex_target_mis ? ex_target : alu_y;

    // multiply and divide
    wire        mem_free;            // MEM can take an instruction at this edge
    wire        mem_redirect;        // MEM traps or redirects: everything younger is flushed
    wire        ex_advance;
    wire [31:0] mul_y, div_y;
    wire        div_busy, div_done;
    wire        div_start = ex_valid && ex_div && !ex_wait_operand && !div_busy && !div_done;

    surfcpu_muldiv muldiv (
        .clk(clk),
        .rst(rst),
        .op(ex_f3[1:0]),
        .a(ex_a),
        .b(ex_b),
        .mul_y(mul_y),
        .div_start(div_start),
        .div_clear(ex_advance || mem_redirect),
        .div_busy(div_busy),
        .div_done(div_done),
        .div_y(div_y)
    );

    wire [31:0] ex_result = (ex_jal || ex_jalr) ? ex_pc4 :
                            ex_mul ? mul_y :
                            ex_div ? div_y : alu_y;

    // flow control
    wire ex_ready    = !ex_wait_operand && (!ex_div || div_done);
    assign ex_advance = ex_valid && ex_ready && mem_free;
    wire ex_redirect = ex_advance && ex_taken && !ex_exc_all;
    wire ex_free     = !ex_valid || ex_advance;

    assign redirect    = mem_redirect || ex_redirect;
    assign id_advance  = id_valid && ex_free && !redirect;

    always @(posedge clk) begin
        if (rst || mem_redirect)
            ex_valid <= 1'b0;
        else if (id_advance)
            ex_valid <= 1'b1;
        else if (ex_advance)
            ex_valid <= 1'b0;

        if (id_advance) begin
            ex_pc        <= id_pc;
            ex_insn      <= id_insn;
            ex_use_rs1   <= id_ok && d_use_rs1;
            ex_use_rs2   <= id_ok && d_use_rs2;
            ex_rd_wen    <= id_ok && d_rd_wen;
            ex_alu_op    <= d_alu_op;
            ex_a_pc      <= d_a_pc;
            ex_a_zero    <= d_a_zero;
            ex_b_imm     <= d_b_imm;
            ex_imm       <= d_imm;
            ex_branch    <= id_ok && d_branch;
            ex_jal       <= id_ok && d_jal;
            ex_jalr      <= id_ok && d_jalr;
            ex_load      <= id_ok && d_load;
            ex_store     <= id_ok && d_store;
            ex_mul       <= id_ok && d_mul;
            ex_div       <= id_ok && d_div;
            ex_csr       <= id_ok && d_csr;
            ex_csr_write <= id_ok && d_csr_write;
            ex_mret      <= id_ok && d_mret;
            ex_wfi       <= id_ok && d_wfi;
            ex_fencei    <= id_ok && d_fencei;
            ex_exc       <= id_exc;
            ex_cause     <= id_cause;
            ex_tval      <= id_tval;
            ex_rs1_val   <= id_rs1_val;
            ex_rs2_val   <= id_rs2_val;
        end else begin
            // keep the operands current: a producer in WB now is gone next cycle
            ex_rs1_val   <= ex_a;
            ex_rs2_val   <= ex_b;
        end
    end

    // ==================================================================== MEM
    wire        csr_illegal;
    wire [31:0] csr_rdata;
    wire [31:0] csr_mtvec;
    wire [31:0] csr_mepc;
    wire        csr_irq_pending;
    wire [4:0]  csr_irq_code;
    wire        csr_wfi_wake;

    wire [2:0]  mem_f3   = mem_insn[14:12];
    wire [1:0]  mem_off  = mem_result[1:0];
    wire        mem_ldst = mem_valid && (mem_load || mem_store) && !mem_exc;
    wire        mem_started = mem_offered || mem_waiting;

    // An interrupt replaces the instruction in MEM, but only before that instruction has
    // done anything: never once its bus request has been offered, and never on a WFI.
    wire irq_take = mem_valid && !mem_started && !mem_wfi && csr_irq_pending;

    // The data request: offered from MEM only, where nothing older can trap any more, and
    // not in a cycle where an interrupt is taken instead; then held until accepted.
    assign dbus_req_valid = !rst && mem_ldst &&
                            (mem_offered || (!mem_waiting && !csr_irq_pending));
    assign dbus_req_addr  = mem_result;
    assign dbus_req_write = mem_store;
    assign dbus_req_be    = (mem_f3[1:0] == 2'b00) ? (4'b0001 << mem_off) :
                            (mem_f3[1:0] == 2'b01) ? (4'b0011 << mem_off) : 4'b1111;
    assign dbus_req_wdata = (mem_f3[1:0] == 2'b00) ? {4{mem_opnd[7:0]}} :
                            (mem_f3[1:0] == 2'b01) ? {2{mem_opnd[15:0]}} : mem_opnd;
    assign dbus_req_fetch = 1'b0;

    wire mem_rsp = mem_waiting && dbus_rsp_valid;

    // load data: select and extend the bytes from the whole word
    wire [31:0] ld_word = dbus_rsp_rdata >> {mem_off, 3'b000};
    reg  [31:0] ld_data;
    always @(*) begin
        case (mem_f3)
            3'b000:  ld_data = {{24{ld_word[7]}}, ld_word[7:0]};     // LB
            3'b001:  ld_data = {{16{ld_word[15]}}, ld_word[15:0]};   // LH
            3'b100:  ld_data = {24'd0, ld_word[7:0]};                // LBU
            3'b101:  ld_data = {16'd0, ld_word[15:0]};               // LHU
            default: ld_data = ld_word;                              // LW
        endcase
    end

    // what the instruction in MEM does this cycle
    wire csr_bad  = mem_csr && csr_illegal;
    wire exc_take = mem_valid && !irq_take &&
                    (mem_exc || csr_bad || (mem_rsp && dbus_rsp_err));
    wire commit   = mem_valid && !irq_take && !mem_exc && !csr_bad &&
                    (mem_ldst ? (mem_rsp && !dbus_rsp_err) :
                     mem_wfi  ? csr_wfi_wake : 1'b1);
    wire trap_take = irq_take || exc_take;
    wire mem_done  = trap_take || commit;

    assign mem_redirect = trap_take || (commit && (mem_mret || mem_fencei));
    assign mem_free     = !mem_valid || (mem_done && !mem_redirect);
    assign redirect_pc  = !mem_redirect ? ex_target :
                          trap_take     ? csr_mtvec :
                          mem_mret      ? csr_mepc  : (mem_pc + 32'd4);   // FENCE.I: refetch

    wire [4:0]  exc_code = mem_exc ? mem_cause :
                           csr_bad ? CAUSE_ILLEGAL :
                           mem_load ? CAUSE_LOAD_FAULT : CAUSE_STORE_FAULT;
    wire [31:0] exc_tval = mem_exc ? mem_tval :
                           csr_bad ? mem_insn : mem_result;       // access fault: the address

    surfcpu_csr #(.MIMPID(MIMPID)) csr (
        .clk(clk),
        .rst(rst),
        .addr(mem_insn[31:20]),
        .write_intent(mem_csr_write),
        .illegal(csr_illegal),
        .rdata(csr_rdata),
        .op(mem_f3[1:0]),
        .src(mem_opnd),
        .we(commit && mem_csr && mem_csr_write),
        .trap(trap_take),
        .trap_irq(irq_take),
        .trap_code(irq_take ? csr_irq_code : exc_code),
        .trap_epc(mem_pc[31:2]),
        .trap_tval(irq_take ? 32'd0 : exc_tval),
        .mret(commit && mem_mret),
        .retire(commit),
        .irq_external(irq_external),
        .irq_timer(irq_timer),
        .irq_software(irq_software),
        .mtime(mtime),
        .mtvec(csr_mtvec),
        .mepc(csr_mepc),
        .irq_pending(csr_irq_pending),
        .irq_code(csr_irq_code),
        .wfi_wake(csr_wfi_wake)
    );

    always @(posedge clk) begin
        if (rst) begin
            mem_valid   <= 1'b0;
            mem_offered <= 1'b0;
            mem_waiting <= 1'b0;
        end else begin
            if (ex_advance)
                mem_valid <= 1'b1;
            else if (mem_done)
                mem_valid <= 1'b0;

            if (dbus_req_valid && dbus_req_ready) begin
                mem_offered <= 1'b0;
                mem_waiting <= 1'b1;
            end else if (dbus_req_valid) begin
                mem_offered <= 1'b1;
            end else if (mem_rsp) begin
                mem_waiting <= 1'b0;
            end
        end

        if (ex_advance) begin
            mem_pc        <= ex_pc;
            mem_insn      <= ex_insn;
            mem_rd_wen    <= ex_rd_wen && !ex_exc_all;
            mem_load      <= ex_load;
            mem_store     <= ex_store;
            mem_csr       <= ex_csr;
            mem_csr_write <= ex_csr_write;
            mem_mret      <= ex_mret;
            mem_wfi       <= ex_wfi;
            mem_fencei    <= ex_fencei;
            mem_result    <= ex_result;
            mem_opnd      <= (ex_csr && ex_f3[2]) ? {27'd0, ex_rs1} : ex_b;   // CSR uimm / store data
            mem_exc       <= ex_exc_all;
            mem_cause     <= ex_cause_all;
            mem_tval      <= ex_tval_all;
        end
    end

    // ==================================================================== WB
    always @(posedge clk) begin
        if (rst)
            wb_wen <= 1'b0;
        else
            wb_wen <= commit && mem_rd_wen;
        wb_rd    <= mem_rd;
        wb_wdata <= mem_load ? ld_data : mem_csr ? csr_rdata : mem_result;
    end

    // ==================================================================== RVFI
`ifdef SURFCPU_RVFI
    // EX -> MEM: what the instruction read, and where it goes if it completes
    reg  [4:0]  mem_rv_rs1_addr, mem_rv_rs2_addr;
    reg  [31:0] mem_rv_rs1_rdata, mem_rv_rs2_rdata;
    reg  [31:0] mem_rv_npc;
    always @(posedge clk) begin
        if (ex_advance) begin
            mem_rv_rs1_addr  <= ex_use_rs1 ? ex_rs1 : 5'd0;
            mem_rv_rs2_addr  <= ex_use_rs2 ? ex_rs2 : 5'd0;
            mem_rv_rs1_rdata <= ex_use_rs1 ? ex_a : 32'd0;
            mem_rv_rs2_rdata <= ex_use_rs2 ? ex_b : 32'd0;
            mem_rv_npc       <= (ex_taken && !ex_exc_all) ? ex_target : ex_pc4;
        end
    end

    // MEM -> WB: one record per retired instruction or exception (not for an interrupt,
    // which retires nothing; the handler's first record has rvfi_intr set)
    wire        mem_record = commit || exc_take;
    wire        rv_ld = commit && mem_load;
    wire        rv_st = commit && mem_store;
    wire [31:0] rv_lanes = {{8{dbus_req_be[3]}}, {8{dbus_req_be[2]}},
                            {8{dbus_req_be[1]}}, {8{dbus_req_be[0]}}};
    reg         rv_valid;
    reg  [63:0] rv_order;
    reg         rv_next_intr;        // the next record is the first of a trap handler
    reg         rv_trap, rv_intr;
    reg  [31:0] rv_pc, rv_insn, rv_npc;
    reg  [4:0]  rv_rs1_addr, rv_rs2_addr;
    reg  [31:0] rv_rs1_rdata, rv_rs2_rdata;
    reg  [31:0] rv_mem_addr, rv_mem_rdata, rv_mem_wdata;
    reg  [3:0]  rv_rmask, rv_wmask;
    always @(posedge clk) begin
        if (rst) begin
            rv_valid     <= 1'b0;
            rv_order     <= 64'd0;
            rv_next_intr <= 1'b0;
        end else begin
            rv_valid <= mem_record;
            if (rv_valid)       rv_order <= rv_order + 64'd1;
            if (trap_take)      rv_next_intr <= 1'b1;
            else if (mem_record) rv_next_intr <= 1'b0;
        end
        if (mem_record) begin
            rv_trap      <= exc_take;
            rv_intr      <= rv_next_intr;
            rv_pc        <= mem_pc;
            rv_insn      <= mem_insn;
            rv_npc       <= exc_take ? csr_mtvec : mem_mret ? csr_mepc : mem_rv_npc;
            rv_rs1_addr  <= mem_rv_rs1_addr;
            rv_rs2_addr  <= mem_rv_rs2_addr;
            rv_rs1_rdata <= mem_rv_rs1_rdata;
            rv_rs2_rdata <= mem_rv_rs2_rdata;
            rv_mem_addr  <= (rv_ld || rv_st) ? {mem_result[31:2], 2'b00} : 32'd0;
            rv_rmask     <= rv_ld ? dbus_req_be : 4'd0;
            rv_wmask     <= rv_st ? dbus_req_be : 4'd0;
            rv_mem_rdata <= rv_ld ? (dbus_rsp_rdata & rv_lanes) : 32'd0;
            rv_mem_wdata <= rv_st ? (dbus_req_wdata & rv_lanes) : 32'd0;
        end
    end

    assign rvfi_valid     = rv_valid;
    assign rvfi_order     = rv_order;
    assign rvfi_insn      = rv_insn;
    assign rvfi_trap      = rv_trap;
    assign rvfi_halt      = 1'b0;
    assign rvfi_intr      = rv_intr;
    assign rvfi_mode      = 2'd3;
    assign rvfi_ixl       = 2'd1;
    assign rvfi_rs1_addr  = rv_rs1_addr;
    assign rvfi_rs2_addr  = rv_rs2_addr;
    assign rvfi_rs1_rdata = rv_rs1_rdata;
    assign rvfi_rs2_rdata = rv_rs2_rdata;
    assign rvfi_rd_addr   = wb_wen ? wb_rd : 5'd0;
    assign rvfi_rd_wdata  = wb_wen ? wb_wdata : 32'd0;
    assign rvfi_pc_rdata  = rv_pc;
    assign rvfi_pc_wdata  = rv_npc;
    assign rvfi_mem_addr  = rv_mem_addr;
    assign rvfi_mem_rmask = rv_rmask;
    assign rvfi_mem_wmask = rv_wmask;
    assign rvfi_mem_rdata = rv_mem_rdata;
    assign rvfi_mem_wdata = rv_mem_wdata;
`endif
endmodule
