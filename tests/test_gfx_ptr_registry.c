#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx_ptr.h"

#define REQUIRE(condition)                                                   \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "requirement failed at %s:%d: %s\n",           \
                    __FILE__, __LINE__, #condition);                          \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static void reset_registry(void) {
    gfx_ptr_clear();
    gfx_ptr_ambiguous = 0;
    gfx_ptr_full_fails = 0;
    gfx_ptr_max_probe = 0;
    gfx_ptr_high_water = 0;
    gfx_ptr_store_suppress = 0;
}

int main(void) {
#if UINTPTR_MAX > UINT32_MAX
    const uintptr_t first = UINT64_C(0x0000000112345678);
    const uintptr_t ambiguous = UINT64_C(0x0000000212345678);
    const uintptr_t second = UINT64_C(0x0000000112345680);
    const uintptr_t sign_extended = UINT64_C(0xffffffff12345678);
#else
    const uintptr_t first = UINT32_C(0x12345678);
    const uintptr_t second = UINT32_C(0x12345680);
#endif

    reset_registry();
    REQUIRE(gfx_ptr_live == 0);

#if UINTPTR_MAX > UINT32_MAX
    /* A sign-extended 32-bit token is never a host pointer. The LP64-only
     * high-half guard must reject it without creating an ambiguous mapping;
     * on ILP32 the guard is absent rather than shifting uintptr_t by 32. */
    gfx_ptr_store((const void *)sign_extended);
    REQUIRE(gfx_ptr_live == 0);
    REQUIRE(gfx_ptr_ambiguous == 0);
#endif

    gfx_ptr_store((const void *)first);
    REQUIRE(gfx_ptr_live == 1);
    REQUIRE(gfx_ptr_high_water == 1);
    REQUIRE(gfx_ptr_resolve((uint32_t)first) == (void *)first);

    gfx_ptr_store((const void *)first);
    REQUIRE(gfx_ptr_live == 1);
    REQUIRE(gfx_ptr_high_water == 1);

    gfx_ptr_store((const void *)second);
    REQUIRE(gfx_ptr_live == 2);
    REQUIRE(gfx_ptr_high_water == 2);
    REQUIRE(gfx_ptr_resolve((uint32_t)second) == (void *)second);

    /* A stage clear retains an already-queued global/raw DL pointer but drops
     * transient ownership and rebuilds without tombstones. */
    gfx_ptr_store_persistent((const void *)first);
    gfx_ptr_clear_transient();
    REQUIRE(gfx_ptr_live == 1);
    REQUIRE(gfx_ptr_resolve((uint32_t)first) == (void *)first);
    REQUIRE(gfx_ptr_resolve((uint32_t)second) == NULL);
    /* Grace is not ownership: an entry not observed again expires at the next
     * stage boundary. */
    gfx_ptr_clear_transient();
    REQUIRE(gfx_ptr_live == 0);
    REQUIRE(gfx_ptr_resolve((uint32_t)first) == NULL);
    gfx_ptr_store((const void *)first);
    gfx_ptr_store((const void *)second);

#if UINTPTR_MAX > UINT32_MAX
    gfx_ptr_store((const void *)ambiguous);
    REQUIRE(gfx_ptr_ambiguous == 1);
    REQUIRE(gfx_ptr_live == 2);
    REQUIRE(gfx_ptr_resolve((uint32_t)first) == (void *)first);
#endif

    gfx_ptr_invalidate_range(first, first + 1);
    REQUIRE(gfx_ptr_live == 1);
    REQUIRE(gfx_ptr_resolve((uint32_t)first) == NULL);
    REQUIRE(gfx_ptr_resolve((uint32_t)second) == (void *)second);

    gfx_ptr_clear();
    REQUIRE(gfx_ptr_live == 0);
    REQUIRE(gfx_ptr_high_water == 2);
    REQUIRE(gfx_ptr_resolve((uint32_t)second) == NULL);
    REQUIRE(gfx_ptr_full_fails == 0);

    puts("gfx pointer registry lifetime tests passed");
    return 0;
}
