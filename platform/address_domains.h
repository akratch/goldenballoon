/*
 * Explicit host-pointer/token boundaries.
 *
 * DKR's original ABI carries RDRAM addresses and some scalar filter/message
 * payloads in 32-bit words or opaque pointers. Native LP64 code must never
 * reproduce those crossings with an anonymous cast: doing so makes a real host
 * pointer, an arena token, a ROM offset, and a scalar payload indistinguishable.
 * Keep the unavoidable low-32 conversion here and name every call site by
 * domain. Full pointers must otherwise stay uintptr_t/void * end to end.
 */
#ifndef MDKR_ADDRESS_DOMAINS_H
#define MDKR_ADDRESS_DOMAINS_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t MdkrArenaToken;
typedef uint32_t MdkrRomOffset;

static inline MdkrArenaToken mdkr_arena_token_from_host(const void *pointer) {
    return (MdkrArenaToken)(uintptr_t)pointer;
}

/* Legacy DMA callers may still pass zero-extended arena tokens. Widen one
 * only when the result lands in the live arena. A low host pointer outside
 * that window keeps its own address; NULL is never reconstructed. */
static inline void *mdkr_dma_destination(void *pointer, uintptr_t arena_high,
                                        const void *arena_base, size_t arena_size) {
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)arena_base;
    uintptr_t reconstructed;
    if (address == 0 || arena_base == NULL || arena_size == 0 ||
        arena_size > UINTPTR_MAX - base) return pointer;
    if (address >= base && address < base + arena_size) return pointer;
    if (address > UINT32_MAX) return pointer;
    reconstructed = arena_high | address;
    if (reconstructed >= base && reconstructed < base + arena_size)
        return (void *)reconstructed;
    return pointer;
}

/* libultra passes a signed 32-bit scalar through an opaque pointer parameter or
 * OSMesg slot. This decodes that ABI payload; it does not accept a dereferenceable
 * host pointer. */
static inline int32_t mdkr_s32_from_opaque(const void *payload) {
    return (int32_t)(intptr_t)payload;
}

#endif /* MDKR_ADDRESS_DOMAINS_H */
