/*
SurfCPU C test: compiled code
--------------------
File: cprogram.c Date: 10/15/26 (SurfCPU)
--------------------
A C program built with GCC -O2 for rv32im_zicsr_zifencei (crt0.S, link.ld in this
directory), so the core runs the code a compiler really makes: calls and returns, stack
frames and spills, recursion, a switch compiled to a jump table, byte/halfword structure
fields, table-driven CRC32, an in-place quicksort, and 64-bit arithmetic through libgcc
(__udivdi3, __divdi3, __moddi3, multiplication). Each result is compared with a value
computed independently (by a Python script when the test was written; the program also
passes on Spike). main returns 0 when all agree, else the number of the first check that
did not.
*/
// surf-test: max-cycles=20000000
#include <stdint.h>

static uint32_t crc_table[256];

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static uint32_t lcg_state = 12345;

static uint32_t lcg(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state;
}

static void quicksort(uint32_t *a, int lo, int hi)
{
    while (lo < hi) {
        uint32_t pivot = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                uint32_t t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (j - lo < hi - i) {          /* recurse into the smaller part */
            quicksort(a, lo, j);
            lo = i;
        } else {
            quicksort(a, i, hi);
            hi = j;
        }
    }
}

__attribute__((noipa)) static int classify(int x)
{
    switch (x & 15) {                   /* dense enough for a jump table */
    case 0: return 7;
    case 1: return 3;
    case 2: return 11;
    case 3: return -5;
    case 4: return 42;
    case 5: return 0;
    case 6: return 9;
    case 7: return -1000;
    case 8: return 123456;
    case 9: return -7;
    default: return -1;
    }
}

struct item {
    uint8_t  tag;
    uint16_t count;
    int8_t   delta;
    uint32_t value;
};

static struct item items[16];
static uint8_t buffer[1000];
static uint32_t numbers[200];

/* noipa and volatile operands keep GCC from computing these at compile time */
__attribute__((noipa)) static uint64_t udiv64(uint64_t a, uint64_t b) { return a / b; }
__attribute__((noipa)) static uint64_t umod64(uint64_t a, uint64_t b) { return a % b; }
__attribute__((noipa)) static int64_t sdiv64(int64_t a, int64_t b) { return a / b; }
__attribute__((noipa)) static int64_t smod64(int64_t a, int64_t b) { return a % b; }
__attribute__((noipa)) static uint64_t umul64(uint64_t a, uint64_t b) { return a * b; }
static volatile uint64_t v_a = 0x123456789ABCDEF0ull, v_b = 12345, v_m = 0x9E3779B97F4A7C15ull;
static volatile int64_t v_sa = -0x123456789ABCDEFll, v_sb = -12345;

int main(void)
{
    crc32_init();
    if (crc32((const uint8_t *)"123456789", 9) != 0xCBF43926u)
        return 1;
    for (int i = 0; i < 1000; i++)
        buffer[i] = (uint8_t)(i * 7 + 3);
    if (crc32(buffer, 1000) != 0x17BC2A46u)
        return 2;

    if (fib(16) != 987)
        return 3;

    for (int i = 0; i < 200; i++)
        numbers[i] = lcg();
    if (lcg_state != 0x7C38FEB1u)
        return 4;
    quicksort(numbers, 0, 199);
    uint32_t sum = 0;
    for (int i = 0; i < 200; i++) {
        if (i > 0 && numbers[i - 1] > numbers[i])
            return 5;
        sum += numbers[i] ^ (uint32_t)i;
    }
    if (sum != 0xB6F68214u)
        return 6;

    int total = 0;
    for (int x = -300; x < 700; x++)
        total += classify(x);
    if (total != 7718118)
        return 7;

    for (int i = 0; i < 16; i++) {
        items[i].tag = (uint8_t)(0xF0 + i);
        items[i].count = (uint16_t)(0x8000u + (unsigned)i * 0x111u);
        items[i].delta = (int8_t)(-i * 8);
        items[i].value = 0x01010101u * (uint32_t)i;
    }
    int32_t acc = 0;
    for (int i = 0; i < 16; i++)
        acc += items[i].tag + items[i].count + items[i].delta + (int32_t)(items[i].value >> 24);
    /* tags 0xF0..0xFF: 3960; counts: 16 * 0x8000 + 0x111 * 120 = 557048; deltas: -8 * 120 = -960;
       value >> 24 = i: 120 */
    if (acc != 3960 + 557048 - 960 + 120)
        return 8;

    if (udiv64(v_a, v_b) != 0x000060A45F5207DBull)
        return 9;
    if (umod64(v_a, v_b) != 0xF2D)
        return 10;
    if (umul64(v_a, v_m) != 0xC93A7B79AEDA89B0ull)
        return 11;
    if (sdiv64(v_sa, v_sb) != 6641193132157ll)
        return 12;
    if (smod64(v_sa, v_sb) != -8730)
        return 13;
    return 0;
}
