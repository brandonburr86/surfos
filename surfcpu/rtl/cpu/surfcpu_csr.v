/*
SurfCPU control and status registers
--------------------
File: surfcpu_csr.v Date: 10/15/26 (SurfCPU)
--------------------
The machine-mode CSRs of docs/ISA.md section 2, trap entry and MRET (section 3), the
counters and the interrupt decision, for the MEM stage (the commit point) of surfcpu_core.

The CSR instruction in MEM presents its address: `illegal` says whether the access is an
illegal instruction (no such CSR, or a write to a read-only one) and `rdata` is the current
value. When the instruction commits with a write, `we` applies op (write/set/clear) with
`src` under the CSR's writable mask at the clock edge. A trap (`trap`) and MRET (`mret`)
update mstatus/mepc/mcause/mtval at the same edge; they never coincide with a CSR write,
because they belong to different instructions. Writes to mcycle/minstret replace the half
written and lose that cycle's increment, so a CSR instruction that writes minstret is not
counted. mip is the three interrupt inputs; `irq_pending` is an enabled pending interrupt
with mstatus.MIE set and `irq_cause` the one to take (MEI, then MSI, then MTI); `wfi_wake`
is (mip & mie) != 0 whatever MIE is.
*/
module surfcpu_csr #(
    parameter [31:0] MIMPID = 32'h0000_0100
) (
    input  wire        clk,
    input  wire        rst,
    // the CSR instruction in MEM
    input  wire [11:0] addr,
    input  wire        write_intent,   // CSRRW[I], or CSRRS/CSRRC[I] with rs1/uimm != 0
    output reg         illegal,
    output reg  [31:0] rdata,
    input  wire [1:0]  op,             // funct3[1:0]: 01 write, 10 set bits, 11 clear bits
    input  wire [31:0] src,            // rs1 value, or the zero-extended uimm
    input  wire        we,             // commit the write at this edge
    // traps, MRET, retirement
    input  wire        trap,
    input  wire        trap_irq,       // mcause bit 31
    input  wire [4:0]  trap_code,      // mcause exception or interrupt code
    input  wire [31:2] trap_epc,
    input  wire [31:0] trap_tval,
    input  wire        mret,
    input  wire        retire,         // an instruction completes at this edge (minstret)
    // interrupt levels and the time
    input  wire        irq_external,
    input  wire        irq_timer,
    input  wire        irq_software,
    input  wire [63:0] mtime,
    // for the pipeline
    output wire [31:0] mtvec,          // trap target (direct mode)
    output wire [31:0] mepc,           // MRET target
    output wire        irq_pending,
    output wire [4:0]  irq_code,
    output wire        wfi_wake
);
`include "surfcpu_defs.vh"

    reg        mstatus_mie;
    reg        mstatus_mpie;
    reg        mie_msie;
    reg        mie_mtie;
    reg        mie_meie;
    reg [31:2] mtvec_base;
    reg        inhibit_cy;
    reg        inhibit_ir;
    reg [31:0] mscratch;
    reg [31:2] mepc_q;
    reg        mcause_irq;
    reg [4:0]  mcause_code;
    reg [31:0] mtval;
    reg [63:0] mcycle;
    reg [63:0] minstret;

    // mstatus: MPP (12:11) fixed at M, MPIE (7), MIE (3); every other bit 0
    wire [31:0] mstatus_val = {19'd0, 2'b11, 3'd0, mstatus_mpie, 3'd0, mstatus_mie, 3'd0};
    wire [31:0] mie_val     = {20'd0, mie_meie, 3'd0, mie_mtie, 3'd0, mie_msie, 3'd0};
    wire [31:0] mip_val     = {20'd0, irq_external, 3'd0, irq_timer, 3'd0, irq_software, 3'd0};

    // ---------------------------------------------------------------- read and legality
    always @(*) begin
        illegal = 1'b0;
        case (addr)
            12'hF11, 12'hF12, 12'hF14, 12'hF15:
                     rdata = 32'd0;                      // mvendorid marchid mhartid mconfigptr
            12'hF13: rdata = MIMPID;                     // mimpid
            12'h300: rdata = mstatus_val;
            12'h301: rdata = 32'h4000_1100;              // misa: MXL = 1, I, M
            12'h304: rdata = mie_val;
            12'h305: rdata = {mtvec_base, 2'b00};        // MODE 0 (direct)
            12'h310: rdata = 32'd0;                      // mstatush
            12'h320: rdata = {29'd0, inhibit_ir, 1'b0, inhibit_cy};
            12'h340: rdata = mscratch;
            12'h341: rdata = {mepc_q, 2'b00};
            12'h342: rdata = {mcause_irq, 26'd0, mcause_code};
            12'h343: rdata = mtval;
            12'h344: rdata = mip_val;
            12'hB00, 12'hC00: rdata = mcycle[31:0];      // mcycle, cycle
            12'hB80, 12'hC80: rdata = mcycle[63:32];     // mcycleh, cycleh
            12'hB02, 12'hC02: rdata = minstret[31:0];    // minstret, instret
            12'hB82, 12'hC82: rdata = minstret[63:32];   // minstreth, instreth
            12'hC01: rdata = mtime[31:0];                // time
            12'hC81: rdata = mtime[63:32];               // timeh
            default: begin
                rdata = 32'd0;                           // the read-as-zero ranges below
                illegal = !((addr >= 12'h323 && addr <= 12'h33F) ||   // mhpmevent3..31
                            (addr >= 12'h3A0 && addr <= 12'h3EF) ||   // pmpcfg0..15, pmpaddr0..63
                            (addr >= 12'hB03 && addr <= 12'hB1F) ||   // mhpmcounter3..31
                            (addr >= 12'hB83 && addr <= 12'hB9F) ||   // mhpmcounter3h..31h
                            (addr >= 12'hC03 && addr <= 12'hC1F) ||   // hpmcounter3..31
                            (addr >= 12'hC83 && addr <= 12'hC9F));    // hpmcounter3h..31h
            end
        endcase
        if (write_intent && addr[11:10] == 2'b11)        // read-only address
            illegal = 1'b1;
    end

    // the value a committing CSR instruction writes, before the CSR's mask
    wire [31:0] wval = (op == 2'b01) ? src :
                       (op == 2'b10) ? (rdata | src) :
                                       (rdata & ~src);

    // ---------------------------------------------------------------- state
    always @(posedge clk) begin
        if (rst) begin
            mstatus_mie  <= 1'b0;
            mstatus_mpie <= 1'b0;
            mie_msie     <= 1'b0;
            mie_mtie     <= 1'b0;
            mie_meie     <= 1'b0;
            mtvec_base   <= 30'd0;
            inhibit_cy   <= 1'b0;
            inhibit_ir   <= 1'b0;
            mscratch     <= 32'd0;
            mepc_q       <= 30'd0;
            mcause_irq   <= 1'b0;
            mcause_code  <= 5'd0;
            mtval        <= 32'd0;
            mcycle       <= 64'd0;
            minstret     <= 64'd0;
        end else begin
            // counters: a write replaces one half and that cycle's increment is lost
            if (we && addr == 12'hB00)      mcycle <= {mcycle[63:32], wval};
            else if (we && addr == 12'hB80) mcycle <= {wval, mcycle[31:0]};
            else if (!inhibit_cy)           mcycle <= mcycle + 64'd1;

            if (we && addr == 12'hB02)      minstret <= {minstret[63:32], wval};
            else if (we && addr == 12'hB82) minstret <= {wval, minstret[31:0]};
            else if (retire && !inhibit_ir) minstret <= minstret + 64'd1;

            if (trap) begin
                mepc_q       <= trap_epc;
                mcause_irq   <= trap_irq;
                mcause_code  <= trap_code;
                mtval        <= trap_tval;
                mstatus_mpie <= mstatus_mie;
                mstatus_mie  <= 1'b0;
            end else if (mret) begin
                mstatus_mie  <= mstatus_mpie;
                mstatus_mpie <= 1'b1;
            end else if (we) begin
                case (addr)
                    12'h300: begin mstatus_mie <= wval[3]; mstatus_mpie <= wval[7]; end
                    12'h304: begin mie_msie <= wval[3]; mie_mtie <= wval[7]; mie_meie <= wval[11]; end
                    12'h305: mtvec_base <= wval[31:2];
                    12'h320: begin inhibit_cy <= wval[0]; inhibit_ir <= wval[2]; end
                    12'h340: mscratch <= wval;
                    12'h341: mepc_q <= wval[31:2];
                    12'h342: begin mcause_irq <= wval[31]; mcause_code <= wval[4:0]; end
                    12'h343: mtval <= wval;
                    default: ;                           // read-only or fixed: writes ignored
                endcase
            end
        end
    end

    // ---------------------------------------------------------------- outputs
    wire take_ext = mie_meie && irq_external;
    wire take_sw  = mie_msie && irq_software;
    wire take_tmr = mie_mtie && irq_timer;

    assign mtvec       = {mtvec_base, 2'b00};
    assign mepc        = {mepc_q, 2'b00};
    assign wfi_wake    = take_ext || take_sw || take_tmr;
    assign irq_pending = mstatus_mie && wfi_wake;
    assign irq_code    = take_ext ? CAUSE_IRQ_EXTERNAL :
                         take_sw  ? CAUSE_IRQ_SOFTWARE : CAUSE_IRQ_TIMER;
endmodule
