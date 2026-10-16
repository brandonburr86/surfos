/*
SurfCPU directed test: loads and stores
--------------------
File: ldst.S Date: 10/15/26 (SurfCPU)
--------------------
LB/LBU/LH/LHU/LW at every legal offset with sign and zero extension; SB/SH/SW byte enables
(only the addressed bytes change, whatever the upper bits of rs2), positive and negative
offsets, a store followed at once by loads of the same bytes, a load whose base register
is its destination, and pointer chasing (each load's address from the previous load).
*/
#include "surf_test.h"

#define TEST_LD(num, inst, result, off, base)                           \
        li      TESTNUM, num;                                           \
        la      x13, base;                                              \
        inst    x14, off(x13);                                          \
        li      x7, result;                                             \
        bne     x14, x7, fail;

TEST_BEGIN
test_main:
        // word0 = 0x80ff7f01: bytes 01 7f ff 80
        TEST_LD(1,  lb,  0x00000001, 0, word0)
        TEST_LD(2,  lb,  0x0000007f, 1, word0)
        TEST_LD(3,  lb,  0xffffffff, 2, word0)
        TEST_LD(4,  lb,  0xffffff80, 3, word0)
        TEST_LD(5,  lbu, 0x00000001, 0, word0)
        TEST_LD(6,  lbu, 0x0000007f, 1, word0)
        TEST_LD(7,  lbu, 0x000000ff, 2, word0)
        TEST_LD(8,  lbu, 0x00000080, 3, word0)
        TEST_LD(9,  lh,  0x00007f01, 0, word0)
        TEST_LD(10, lh,  0xffff80ff, 2, word0)
        TEST_LD(11, lhu, 0x00007f01, 0, word0)
        TEST_LD(12, lhu, 0x000080ff, 2, word0)
        TEST_LD(13, lw,  0x80ff7f01, 0, word0)
        // word1 = 0x7fff8000
        TEST_LD(14, lh,  0xffff8000, 0, word1)
        TEST_LD(15, lh,  0x00007fff, 2, word1)
        TEST_LD(16, lhu, 0x00008000, 0, word1)
        TEST_LD(17, lhu, 0x00007fff, 2, word1)
        TEST_LD(18, lw,  0x7fff8000, 0, word1)
        // negative and larger offsets
        TEST_LD(19, lw,  0x80ff7f01, -4, word1)
        TEST_LD(20, lb,  0xffffff80, -1, word1)
        TEST_LD(21, lhu, 0x000080ff, -2, word1)
        TEST_LD(22, lw,  0x12345678, 2044, base2k)
        TEST_LD(23, lw,  0x9abcdef0, -2048, top2k)

        // ---------------------------------------------------------- SB
        la      x13, scratch
        sw      x0, 0(x13)
        li      TESTNUM, 30
        li      x11, 0x123456aa
        sb      x11, 1(x13)
        lw      x14, 0(x13)
        li      x7, 0x0000aa00
        bne     x14, x7, fail
        li      TESTNUM, 31
        li      x11, 0xffffff55
        sb      x11, 3(x13)
        lw      x14, 0(x13)
        li      x7, 0x5500aa00
        bne     x14, x7, fail
        li      TESTNUM, 32
        li      x11, 0x876543bb
        sb      x11, 0(x13)
        lw      x14, 0(x13)
        li      x7, 0x5500aabb
        bne     x14, x7, fail
        li      TESTNUM, 33
        li      x11, 0xcc
        sb      x11, 2(x13)
        lw      x14, 0(x13)
        li      x7, 0x55ccaabb
        bne     x14, x7, fail
        li      TESTNUM, 34                     // and the neighbouring words are untouched
        lw      x14, -4(x13)
        li      x7, 0xa5a5a5a5
        bne     x14, x7, fail
        lw      x14, 4(x13)
        li      x7, 0x5a5a5a5a
        bne     x14, x7, fail

        // ---------------------------------------------------------- SH
        li      TESTNUM, 40
        li      x11, -1
        sw      x11, 0(x13)
        li      x11, 0xdead1234
        sh      x11, 2(x13)
        lw      x14, 0(x13)
        li      x7, 0x1234ffff
        bne     x14, x7, fail
        li      TESTNUM, 41
        li      x11, 0x9876abcd
        sh      x11, 0(x13)
        lw      x14, 0(x13)
        li      x7, 0x1234abcd
        bne     x14, x7, fail
        li      TESTNUM, 42
        lh      x14, 0(x13)
        li      x7, 0xffffabcd
        bne     x14, x7, fail
        lhu     x14, 2(x13)
        li      x7, 0x00001234
        bne     x14, x7, fail

        // ---------------------------------------------------------- SW
        li      TESTNUM, 50
        li      x11, 0xcafef00d
        sw      x11, 0(x13)
        lw      x14, 0(x13)
        bne     x14, x11, fail
        li      TESTNUM, 51                     // negative offset
        addi    x15, x13, 8
        li      x11, 0x0badc0de
        sw      x11, -8(x15)
        lw      x14, 0(x13)
        bne     x14, x11, fail

        // ---------------------------------------------------------- store, then load at once
        li      TESTNUM, 60
        li      x11, 0x11223344
        sw      x11, 0(x13)
        lb      x14, 3(x13)
        li      x7, 0x11
        bne     x14, x7, fail
        li      TESTNUM, 61
        li      x11, 0x80
        sb      x11, 1(x13)
        lb      x14, 1(x13)
        li      x7, 0xffffff80
        bne     x14, x7, fail
        lw      x14, 0(x13)
        li      x7, 0x11228044
        bne     x14, x7, fail
        li      TESTNUM, 62
        sh      x0, 2(x13)
        lhu     x14, 2(x13)
        bnez    x14, fail
        lw      x14, 0(x13)
        li      x7, 0x00008044
        bne     x14, x7, fail

        // ---------------------------------------------------------- base register = destination
        li      TESTNUM, 70
        la      x5, pointer0
        lw      x5, 0(x5)
        la      x7, pointer1
        bne     x5, x7, fail
        li      TESTNUM, 71                     // pointer chasing: four dependent loads
        la      x5, pointer0
        lw      x5, 0(x5)
        lw      x5, 0(x5)
        lw      x5, 0(x5)
        lw      x5, 0(x5)
        li      x7, 0x600df00d
        bne     x5, x7, fail
        li      TESTNUM, 72                     // a loaded address used by a store
        la      x5, ptr_scratch2
        lw      x6, 0(x5)
        li      x11, 0x77
        sb      x11, 0(x6)
        la      x8, scratch2
        lw      x14, 0(x8)
        li      x7, 0x77
        bne     x14, x7, fail
        j       pass

        .data
        .balign 16
        .word   0
word0:  .word   0x80ff7f01
word1:  .word   0x7fff8000
        .word   0xa5a5a5a5
scratch:
        .word   0
        .word   0x5a5a5a5a
pointer0:
        .word   pointer1
pointer1:
        .word   pointer2
pointer2:
        .word   pointer3
pointer3:
        .word   0x600df00d
ptr_scratch2:
        .word   scratch2
scratch2:
        .word   0
        .balign 16
base2k: .space  2044
        .word   0x12345678
        .word   0x9abcdef0
        .space  2044
top2k:  .word   0

TEST_DATA
