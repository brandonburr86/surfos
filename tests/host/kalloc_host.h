/* Force-included when mm/kalloc.c is built for tests/host/test_kalloc.c (10/25/26, robustness B1): the kernel heap in
   a static array of the test instead of at 0xB0000000, where nothing faults its pages in (the test's pmm_commit()
   stands in for the frame allocator, and refuses when told to). The DMA heap's arena is the test's too. */
#include <mm/memory.h>

#define KALLOC_TEST_KHEAP (8u << 20)            /* 8 MB of kernel heap */
extern unsigned char kalloc_test_kheap[];
#undef KHEAP_START
#undef KHEAP_END
#define KHEAP_START ((u_long)kalloc_test_kheap)
#define KHEAP_END   (KHEAP_START + KALLOC_TEST_KHEAP)
