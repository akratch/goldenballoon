#include "address_domains.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "DMA destination line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    uintptr_t high = 0;
    const size_t arena_size = 0x1000000u;
#if UINTPTR_MAX > UINT32_MAX
    const uintptr_t base_value = UINT64_C(0xB01600000);
    high = (uintptr_t)UINT64_C(0xB00000000);
    void *arena = (void *)(uintptr_t)base_value;
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)UINT64_C(0x160001234), high, arena, arena_size) == UINT64_C(0x160001234));
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)UINT64_C(0x100000000), high, arena, arena_size) == UINT64_C(0x100000000));
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)(base_value + 0x20u), high, arena, arena_size) == base_value + 0x20u);
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)(base_value & 0xFFFFFFFFu), high, arena, arena_size) == base_value);
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)0x1234, high, arena, arena_size) == 0x1234);
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)UINT32_MAX, high, arena, arena_size) == UINT32_MAX);
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)0x1234, high, NULL, 0) == 0x1234);
#else
    (void)arena_size;
#endif
    CHECK(mdkr_dma_destination(0, high, NULL, 0) == 0);
    puts("DMA destination: PASS");
    return 0;
}
