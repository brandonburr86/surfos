/*
SurfOS crypto: comparing and wiping secrets
--------------------
File: util.c    Date: 10/9/26 (Wi-Fi: WPA2)
--------------------
crypto_equal() compares in a time that depends only on the length, for MICs and keys;
crypto_wipe() clears memory through a volatile pointer, so the stores are made even when
the buffer is never read again (the compiler would drop a memset() it can see is dead).
See include/crypto/crypto.h.
*/

#include <surfos/types.h>
#include <crypto/crypto.h>

bool crypto_equal(const void *a, const void *b, size_t len) {
    const volatile u8 *x = a, *y = b;
    u32 d = 0;
    size_t i;
    for(i = 0; i < len; i++) d |= x[i] ^ y[i];
    return (bool)((d - 1) >> 31);                  /* 1 when d is 0, with no branch on it */
}

void crypto_wipe(void *p, size_t len) {
    volatile u8 *q = p;
    while(len--) *q++ = 0;
    asm volatile("" : : "r"(p) : "memory");
}
