#include "address_domains.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "DMA destination line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    /* Includes low addresses overlapping a possible arena's low-32 window.
     * Values are compared only, never dereferenced. */
    const uintptr_t addresses[] = {0, 0x1234, 0x01600000, 0x01600020,
                                   0x025FFFFF, 0x02600000, UINT32_MAX};
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i)
        CHECK((uintptr_t)mdkr_dma_destination((void *)addresses[i]) == addresses[i]);
#if UINTPTR_MAX > UINT32_MAX
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)UINT64_C(0x160001234)) == UINT64_C(0x160001234));
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)UINT64_C(0x100000000)) == UINT64_C(0x100000000));
    CHECK((uintptr_t)mdkr_dma_destination((void *)(uintptr_t)UINT64_C(0xB01600020)) == UINT64_C(0xB01600020));
#endif
    unsigned char destination[8];
    CHECK(mdkr_dma_destination(destination) == destination);
    puts("DMA destination: PASS");
    return 0;
}
