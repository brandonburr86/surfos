/*
SurfOS Floating Point Test
--------------------
File: fputest.c Date: 10/25/26 (robustness, track B: L10-L13)
--------------------
fputest: a program's floating point. It reads the control words a program starts with (x87
0x037F, MXCSR 0x1F80), checks the 16-byte stack alignment at main that SSE code relies on, computes results
that are exact in binary with the x87 (this program is built for the i486, as every program is)
and, when the CPU has SSE2, with SSE2 instructions, and compares them with what they must be:
FPUTEST PASS.

fputest race: its registers are its own across a switch. It loads 8 x87 registers with values of
its pid, the x87 control word with rounding toward zero, and with SSE the 8 XMM registers and
MXCSR, sleeps 300 ms in the same asm block (int 0x80), reads them all back and compares them bit
for bit: FPUTEST race PASS. Two at once (the self test's test_fpu) make the kernel save one's
registers and load the other's in turn.

The SSE code is in asm, chosen with CPUID: the program itself stays i486 code (-mno-sse), so it
runs on a CPU without SSE (QEMU's -cpu pentium: fnsave instead of fxsave). It prints its numbers
with a decimal of its own: it tests the FPU, not printf.
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SYS_SLEEP 15            /* the kernel's SYS_sleep (include/surfos/syscall.h, which programs never see) */

/* what the race holds in the registers, and reads back: the offsets are the asm's */
struct regs {
    unsigned char xmm[8][16];   /* 0 */
    double x87[8];              /* 128 */
    unsigned int mxcsr;         /* 192 */
    unsigned short fcw;         /* 196 */
} __attribute__((aligned(16)));

static int has_cpuid(void) {
    unsigned long a, b;
    asm volatile("pushfl\n\tpopl %0\n\tmovl %0, %1\n\txorl $0x200000, %0\n\tpushl %0\n\tpopfl\n\t"
                 "pushfl\n\tpopl %0\n\tpushl %1\n\tpopfl" : "=&r"(a), "=&r"(b) : : "cc");
    return ((a ^ b) & 0x200000) != 0;
}

/* CPUID leaf 1's EDX: bit 25 SSE, 26 SSE2 (the kernel turns SSE on for programs when the CPU has it) */
static unsigned cpu_features(void) {
    unsigned a, b, c, d;
    if(!has_cpuid()) return 0;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    if(a < 1) return 0;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return d;
}

/* v as the decimal it is: this program prints only values with a few places that fit an int */
static const char *dec(double v, char *buf) {
    char *p = buf;
    int whole, n = 0;
    double frac;
    if(v < 0) {
        *p++ = '-';
        v = -v;
    }
    whole = (int)v;
    frac = v - whole;
    p += sprintf(p, "%d", whole);
    if(frac != 0) *p++ = '.';
    while(frac != 0 && n++ < 9) {
        frac *= 10;
        *p++ = (char)('0' + (int)frac);
        frac -= (int)frac;
    }
    *p = 0;
    return buf;
}

static double x87_sqrt(double x) {
    double r;
    asm("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

/* the same five results with SSE2: a + b, then * c, then / d, sqrt(b), and (int)e truncated (cvttsd2si) */
static void sse2_math(const double in[5], double out[4], int *trunc) {
    int t;
    asm volatile("movsd 0(%1), %%xmm0\n\t"
                 "movsd 8(%1), %%xmm1\n\t"
                 "addsd %%xmm1, %%xmm0\n\t"
                 "movsd %%xmm0, 0(%2)\n\t"
                 "mulsd 16(%1), %%xmm0\n\t"
                 "movsd %%xmm0, 8(%2)\n\t"
                 "divsd 24(%1), %%xmm0\n\t"
                 "movsd %%xmm0, 16(%2)\n\t"
                 "sqrtsd %%xmm1, %%xmm2\n\t"
                 "movsd %%xmm2, 24(%2)\n\t"
                 "cvttsd2si 32(%1), %0"
                 : "=r"(t) : "r"(in), "r"(out) : "memory");     /* no xmm clobbers: -mno-sse, the compiler uses none */
    *trunc = t;
}

static int check(const char *what, const double got[4], int trunc) {
    char b[5][24];
    int ok = got[0] == 3.75 && got[1] == 15 && got[2] == 1.875 && got[3] == 1.5 && trunc == -2;
    printf("fputest: %s: 1.5 + 2.25 = %s, * 4 = %s, / 8 = %s, sqrt(2.25) = %s, (int)-2.5 = %d%s\n", what,
           dec(got[0], b[0]), dec(got[1], b[1]), dec(got[2], b[2]), dec(got[3], b[3]), trunc, ok ? "" : "   WRONG");
    return ok;
}

/* aligned: main's first argument is 16-aligned (crt0's call: main) */
static int results(int aligned) {
    static const double in[5] = { 1.5, 2.25, 4, 8, -2.5 };
    volatile double a = in[0], b = in[1], c = in[2], d = in[3], e = in[4];     /* computed here, never folded */
    unsigned feat = cpu_features();
    unsigned short fcw = 0;
    unsigned int mxcsr = 0;
    double got[4];
    int ok = 1, trunc;

    asm volatile("fnstcw %0" : "=m"(fcw));      /* first: what this program got, before any of its own */
    if(feat & (1u << 25)) asm volatile("stmxcsr %0" : "=m"(mxcsr));
    if(feat & (1u << 25)) printf("fputest: control words at the start: x87 0x%04x, MXCSR 0x%04x\n", fcw, mxcsr);
    else printf("fputest: control words at the start: x87 0x%04x (no SSE)\n", fcw);
    ok = fcw == 0x037f && (!(feat & (1u << 25)) || mxcsr == 0x1f80);
    if(aligned) printf("fputest: the stack is 16-aligned at main\n");
    else {
        printf("fputest: the stack is not 16-aligned at main   WRONG\n");
        ok = 0;
    }
    got[0] = a + b;
    got[1] = got[0] * c;
    got[2] = got[1] / d;
    got[3] = x87_sqrt(b);
    trunc = (int)e;
    ok = check("x87", got, trunc) && ok;
    if(feat & (1u << 26)) {
        memset(got, 0, sizeof(got));
        sse2_math(in, got, &trunc);
        ok = check("SSE2", got, trunc) && ok;
    } else {
        printf("fputest: SSE2: not on this CPU\n");
    }
    printf("FPUTEST %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

#define X87_LOAD   "fldl 184(%1)\n\tfldl 176(%1)\n\tfldl 168(%1)\n\tfldl 160(%1)\n\t" \
                   "fldl 152(%1)\n\tfldl 144(%1)\n\tfldl 136(%1)\n\tfldl 128(%1)\n\t"
#define X87_STORE  "fstpl 128(%2)\n\tfstpl 136(%2)\n\tfstpl 144(%2)\n\tfstpl 152(%2)\n\t" \
                   "fstpl 160(%2)\n\tfstpl 168(%2)\n\tfstpl 176(%2)\n\tfstpl 184(%2)\n\tfnstcw 196(%2)\n\t"
#define XMM_LOAD   "ldmxcsr 192(%1)\n\tmovups 0(%1), %%xmm0\n\tmovups 16(%1), %%xmm1\n\tmovups 32(%1), %%xmm2\n\t" \
                   "movups 48(%1), %%xmm3\n\tmovups 64(%1), %%xmm4\n\tmovups 80(%1), %%xmm5\n\t" \
                   "movups 96(%1), %%xmm6\n\tmovups 112(%1), %%xmm7\n\t"
#define XMM_STORE  "stmxcsr 192(%2)\n\tmovups %%xmm0, 0(%2)\n\tmovups %%xmm1, 16(%2)\n\tmovups %%xmm2, 32(%2)\n\t" \
                   "movups %%xmm3, 48(%2)\n\tmovups %%xmm4, 64(%2)\n\tmovups %%xmm5, 80(%2)\n\t" \
                   "movups %%xmm6, 96(%2)\n\tmovups %%xmm7, 112(%2)\n\t"
#define X87_REGS   "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"

static int race(void) {
    static struct regs in, out;
    unsigned feat = cpu_features();
    int sse = (feat & (1u << 25)) != 0, pid = getpid(), i, r, ok;

    for(i = 0; i < 8; i++) in.x87[i] = pid * 1000.0 + i + 0.25;                /* exact, and this race's own */
    for(i = 0; i < 128; i++) in.xmm[i / 16][i % 16] = (unsigned char)(pid * 31 + i * 7);
    in.fcw = 0x0f7f;                            /* every exception masked, 64-bit precision, rounding toward zero */
    in.mxcsr = 0x7f80;                          /* every exception masked, rounding toward zero */
    memset(&out, 0, sizeof(out));
    if(sse)
        asm volatile("fldcw 196(%1)\n\t" X87_LOAD XMM_LOAD "int $0x80\n\t" X87_STORE XMM_STORE
                     : "=a"(r) : "S"(&in), "D"(&out), "a"(SYS_SLEEP), "b"(300) : "memory", X87_REGS);
    else
        asm volatile("fldcw 196(%1)\n\t" X87_LOAD "int $0x80\n\t" X87_STORE
                     : "=a"(r) : "S"(&in), "D"(&out), "a"(SYS_SLEEP), "b"(300) : "memory", X87_REGS);
    asm volatile("fninit");                     /* this program's own rounding again */
    ok = r == 0 && !memcmp(in.x87, out.x87, sizeof(in.x87)) && out.fcw == in.fcw &&
         (!sse || (!memcmp(in.xmm, out.xmm, sizeof(in.xmm)) && out.mxcsr == in.mxcsr));
    printf("fputest race: pid %d held 8 x87 registers%s across 300 ms of sleep: %s\n", pid,
           sse ? " and 8 XMM registers" : " (no SSE)", ok ? "the same bits" : "CHANGED");
    if(!ok) printf("fputest race: sleep %d, x87 control word 0x%04x (0x%04x), MXCSR 0x%04x (0x%04x)\n", r, out.fcw,
                   in.fcw, out.mxcsr, in.mxcsr);
    printf("FPUTEST race %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* GCC realigns main's own frame (andl $-16), but argc stays where crt0 put it: the i386 ABI's 16-byte alignment at a
   call puts the first argument on a 16-byte boundary, which SSE code in a program built for it relies on (L12) */
int main(int argc, char **argv) {
    if(argc == 1) return results(((unsigned long)&argc & 15) == 0);
    if(argc == 2 && !strcmp(argv[1], "race")) return race();
    fprintf(stderr, "usage: fputest [race]\n");
    return 2;
}
