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

#include <stdint.h>

typedef uint32_t MdkrArenaToken;
typedef uint32_t MdkrRomOffset;

static inline MdkrArenaToken mdkr_arena_token_from_host(const void *pointer) {
    return (MdkrArenaToken)(uintptr_t)pointer;
}

/* libultra passes a signed 32-bit scalar through an opaque pointer parameter or
 * OSMesg slot. This decodes that ABI payload; it does not accept a dereferenceable
 * host pointer. */
static inline int32_t mdkr_s32_from_opaque(const void *payload) {
    return (int32_t)(intptr_t)payload;
}

#endif /* MDKR_ADDRESS_DOMAINS_H */
