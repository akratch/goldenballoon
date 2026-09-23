/* Synthetic CTL records; no ROM content. Exercise the real parser with an
 * arena allocator so an invalid libc free is observable under sanitizers. */
#include "../platform/audio_bank.c"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>

static union { max_align_t alignment; unsigned char bytes[65536]; } arena;
static size_t used;
static unsigned allocations;
static unsigned fail_allocation;
void *alHeapDBAlloc(u8 *file, s32 line, ALHeap *heap, s32 num, s32 size) {
    size_t count = (size_t)num * (size_t)size;
    size_t start = (used + 15u) & ~(size_t)15u;
    (void)file; (void)line; (void)heap;
    if (++allocations == fail_allocation) return NULL;
    if (start + count > sizeof(arena.bytes)) return NULL;
    used = start + count;
    memset(arena.bytes + start, 0, count);
    return arena.bytes + start;
}
static void be32(u8 *p, u32 v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
int main(void) {
    u8 ctl[512] = {0};
    /* bank@32 -> instrument@64 -> sound@96 -> wave@128 */
    ctl[33] = 1; be32(ctl + 44, 64);
    ctl[79] = 1; be32(ctl + 80, 96);
    be32(ctl + 104, 128);
    ctl[136] = AL_ADPCM_WAVE;
    assert(bank_make_bank(ctl, sizeof ctl, 32, 0) != NULL);
    used = 0;
    assert(bank_make_bank(ctl, 140, 32, 0) == NULL);
    ctl[136] = AL_RAW16_WAVE;
    used = 0;
    assert(bank_make_bank(ctl, 140, 32, 0) == NULL);
    /* Every nonzero child reference must reject its parent when truncated. */
    const unsigned fields[] = {40, 44, 80, 96, 100, 104, 140, 144};
    ctl[136] = AL_ADPCM_WAVE;
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        u8 saved[4]; memcpy(saved, ctl + fields[i], 4);
        be32(ctl + fields[i], sizeof ctl - 1);
        used = 0;
        assert(bank_make_bank(ctl, sizeof ctl, 32, 0) == NULL);
        memcpy(ctl + fields[i], saved, 4);
    }
    ctl[136] = AL_RAW16_WAVE;
    be32(ctl + 140, sizeof ctl - 1); used = 0;
    assert(bank_make_bank(ctl, sizeof ctl, 32, 0) == NULL);
    /* Valid nested records exercise every allocation, then fail each one in
     * turn. No partially constructed bank may escape on heap exhaustion. */
    be32(ctl + 96, 160);  /* envelope */
    be32(ctl + 100, 176); /* keymap */
    be32(ctl + 140, 192); /* loop (large enough for ADPCM or raw) */
    be32(ctl + 144, 240); /* book */
    be32(ctl + 240, 1); be32(ctl + 244, 1);
    for (unsigned type = 0; type < 2; ++type) {
        ctl[136] = type == 0 ? AL_ADPCM_WAVE : AL_RAW16_WAVE;
        used = 0; allocations = 0;
        assert(bank_make_bank(ctl, sizeof ctl, 32, 0) != NULL);
        const unsigned allocation_count = allocations;
        assert(allocation_count >= 7);
        for (unsigned failure = 1; failure <= allocation_count; ++failure) {
            used = 0; allocations = 0; fail_allocation = failure;
            assert(bank_make_bank(ctl, sizeof ctl, 32, 0) == NULL);
        }
        fail_allocation = 0;
        used = 0; allocations = 0;
        assert(bank_make_bank(ctl, sizeof ctl, 32, 0) != NULL);
    }
    puts("audio bank ownership and nested rejection passed");
    return 0;
}
