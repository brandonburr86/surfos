/*
SurfOS riscv32 Processor
--------------------
File: cpu.c     Date: 10/15/26 (SurfCPU)
--------------------
What the machine-mode ID registers say about the hart, for cpuinfo. SurfCPU reports
misa = RV32IM, mvendorid and marchid 0, mimpid its SoC version (surfcpu/docs/ISA.md);
QEMU's rv32 reports its own. The kernel is built for rv32im_zicsr_zifencei and needs
nothing more: no A (critical sections disable interrupts instead), no C, F or D.
*/

#include <surfos/types.h>
#include <surfos/cpu.h>
#include <surfos/console.h>
#include <riscv/csr.h>
#include <blibc_common.h>

struct cpu_info cpu;

void init_cpu(void) {
    char *p = cpu.isa;
    int i;
    cpu.misa = csr_read(misa);
    cpu.mvendorid = csr_read(mvendorid);
    cpu.marchid = csr_read(marchid);
    cpu.mimpid = csr_read(mimpid);
    cpu.mhartid = csr_read(mhartid);
    cpu.mstatus_reset = csr_read(mstatus);
    cpu.tsc = true;
    p += snprintf(p, sizeof(cpu.isa), "rv%d", (cpu.misa >> 30) == 1 ? 32 : (cpu.misa >> 30) == 2 ? 64 : 0);
    for(i = 0; i < 26 && p < cpu.isa + sizeof(cpu.isa) - 1; i++) if(cpu.misa & (1u << i)) *p++ = (char)('a' + i);
    *p = 0;
}

void cpu_print(void) {
    u64 c = rv_read_cycle64(), r = rv_read_instret64();
    printf("    hart %lu: %s (misa 0x%08lx), mvendorid 0x%lx, marchid 0x%lx, mimpid 0x%08lx\n", (u_long)cpu.mhartid,
           cpu.isa, (u_long)cpu.misa, (u_long)cpu.mvendorid, (u_long)cpu.marchid, (u_long)cpu.mimpid);
    printf("    kernel built for rv32im_zicsr_zifencei, ilp32; machine mode only, no MMU, no PMP used\n");
    printf("    mstatus 0x%08lx (at boot 0x%08lx), mie 0x%08lx, mip 0x%08lx, mtvec 0x%08lx\n", csr_read(mstatus),
           (u_long)cpu.mstatus_reset, csr_read(mie), csr_read(mip), csr_read(mtvec));
    printf("    cycles %llu, instructions retired %llu\n", c, r);
}
