/* Minimal libatomic providing 16-byte (__int128) atomics for ARMv8.0 (cortex-a53,
 * no LSE) where GCC needs out-of-line helpers but the SDK ships no libatomic.
 * Lock-based implementation (a small spinlock pool keyed by address) -- correct
 * and sufficient for WebKit/libpas usage. ABI matches GCC's libatomic.
 *
 * TODO: the ABI surface here is incomplete -- __atomic_fetch_sub/and/or/xor/nand_16,
 * __atomic_test_and_set_16, __atomic_clear_16 and __atomic_is_lock_free are missing.
 * Today's WPE links fine, but a WebKit rebuild with different flags may hit undefined
 * symbols at runtime -> add the remaining helpers here, or add a symbol check to the
 * bundle step. */
#include <stdint.h>
#include <stddef.h>

typedef __int128 i128;

/* tiny spinlock pool to make the 16-byte ops mutually atomic */
#define NLOCKS 64
static volatile int locks[NLOCKS];

static inline unsigned idx(const volatile void *p) {
    return (unsigned)(((uintptr_t)p >> 4) & (NLOCKS - 1));
}
static inline void lock(unsigned i) {
    while (__sync_lock_test_and_set(&locks[i], 1)) {
        while (locks[i]) __asm__ __volatile__("yield" ::: "memory");
    }
}
static inline void unlock(unsigned i) {
    __sync_lock_release(&locks[i]);
}

i128 __atomic_load_16(const volatile void *mem, int model) {
    (void)model;
    unsigned i = idx(mem);
    lock(i);
    i128 v = *(const volatile i128 *)mem;
    unlock(i);
    return v;
}

void __atomic_store_16(volatile void *mem, i128 val, int model) {
    (void)model;
    unsigned i = idx(mem);
    lock(i);
    *(volatile i128 *)mem = val;
    unlock(i);
}

i128 __atomic_exchange_16(volatile void *mem, i128 val, int model) {
    (void)model;
    unsigned i = idx(mem);
    lock(i);
    i128 old = *(volatile i128 *)mem;
    *(volatile i128 *)mem = val;
    unlock(i);
    return old;
}

_Bool __atomic_compare_exchange_16(volatile void *mem, void *expected, i128 desired,
                                   _Bool weak, int success, int failure) {
    (void)weak; (void)success; (void)failure;
    unsigned i = idx(mem);
    lock(i);
    i128 cur = *(volatile i128 *)mem;
    i128 exp = *(i128 *)expected;
    _Bool ok = (cur == exp);
    if (ok) *(volatile i128 *)mem = desired;
    else    *(i128 *)expected = cur;
    unlock(i);
    return ok;
}

i128 __atomic_fetch_add_16(volatile void *mem, i128 val, int model) {
    (void)model;
    unsigned i = idx(mem);
    lock(i);
    i128 old = *(volatile i128 *)mem;
    *(volatile i128 *)mem = old + val;
    unlock(i);
    return old;
}
