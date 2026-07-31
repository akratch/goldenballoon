#include "memory.h"
#include "math_util.h"
#include "thread0_epc.h"
#include "platform_os.h"
#include "fast3d/gfx_pc_dkr.h"

#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_POOL_BYTES 32768
#define TEST_POOL_SLOTS 300

#define REQUIRE(condition)                                                         \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "test_memory_allocator:%d: requirement failed: %s\n", \
                    __LINE__, #condition);                                         \
            return 1;                                                              \
        }                                                                          \
    } while (0)

#define EXPECT_FATAL(expression)                         \
    do {                                                 \
        sFatalExpected = 1;                              \
        if (setjmp(sFatalJump) == 0) {                   \
            expression;                                  \
            sFatalExpected = 0;                          \
            REQUIRE(!"expected allocator fatal");        \
        }                                                \
        sFatalExpected = 0;                              \
    } while (0)

extern MemoryPool gMemoryPools[POOL_COUNT];
extern s32 gNumberOfMemoryPools;
extern FreeQueueSlot gFreeQueue[FREE_QUEUE_SIZE];
extern s32 gFreeQueueCount;
extern s32 gFreeQueueTimer;

static _Alignas(16) u8 sPoolStorage[TEST_POOL_BYTES];
static jmp_buf sFatalJump;
static int sFatalExpected;
static int sDumpCount;
static StackInfo sStack;

void mdkr_memory_test_abort(void) {
    if (!sFatalExpected) {
        fprintf(stderr, "unexpected allocator fatal\n");
        exit(2);
    }
    longjmp(sFatalJump, 1);
}

u32 interrupts_disable(void) {
    return 0;
}

void interrupts_enable(u32 flags) {
    (void) flags;
}

void *dkr_arena_init(uint32_t size) {
    (void) size;
    return NULL;
}

void gfx_dkr_texcache_invalidate_range(const void *base, uint32_t size) {
    (void) base;
    (void) size;
}

StackInfo *stack_pointer(void) {
    return &sStack;
}

void dump_memory_to_cpak(s32 epc, s32 size, u32 colourTag) {
    (void) epc;
    (void) size;
    (void) colourTag;
    sDumpCount++;
}

void set_render_printf_background_colour(u8 red, u8 green, u8 blue, u8 alpha) {
    (void) red;
    (void) green;
    (void) blue;
    (void) alpha;
}

s32 render_printf(const char *format, ...) {
    (void) format;
    return 0;
}

static MemoryPool *reset_pool(s32 slotCount) {
    memset(sPoolStorage, 0, sizeof(sPoolStorage));
    memset(gMemoryPools, 0, sizeof(gMemoryPools));
    memset(gFreeQueue, 0, sizeof(gFreeQueue));
    gNumberOfMemoryPools = -1;
    gFreeQueueCount = 0;
    gFreeQueueTimer = 0;
    sDumpCount = 0;
    mempool_init((MemoryPoolSlot *) sPoolStorage, sizeof(sPoolStorage), slotCount);
    return &gMemoryPools[POOL_MAIN];
}

static int validate_pool(MemoryPool *pool) {
    s32 count = 0;
    s32 index = 0;
    s32 previous = MEMSLOT_NONE;
    u8 *nextData = NULL;

    while (index != MEMSLOT_NONE) {
        MemoryPoolSlot *slot;
        REQUIRE(index >= 0 && index < pool->maxNumSlots);
        REQUIRE(count < pool->curNumSlots);
        slot = &pool->slots[index];
        REQUIRE(slot->prevIndex == previous);
        REQUIRE(slot->size > 0);
        if (nextData != NULL) {
            REQUIRE(slot->data == nextData);
        }
        nextData = slot->data + slot->size;
        previous = index;
        index = slot->nextIndex;
        count++;
    }
    REQUIRE(count == pool->curNumSlots);
    return 0;
}

static int test_slot_boundaries_and_coalescing(void) {
    MemoryPool *pool = reset_pool(16);
    u8 *a = (u8 *) mempool_alloc(32, COLOUR_TAG_RED);
    u8 *b = (u8 *) mempool_alloc(48, COLOUR_TAG_GREEN);
    u8 *c = (u8 *) mempool_alloc(64, COLOUR_TAG_BLUE);

    REQUIRE(a != NULL && b != NULL && c != NULL);
    REQUIRE(validate_pool(pool) == 0);

    mempool_free_addr(b);
    REQUIRE(pool->curNumSlots == 4);
    REQUIRE(validate_pool(pool) == 0);

    mempool_free_addr(c);
    REQUIRE(pool->curNumSlots == 2);
    REQUIRE(validate_pool(pool) == 0);

    mempool_free_addr(a);
    REQUIRE(pool->curNumSlots == 1);
    REQUIRE(pool->slots[0].flags == SLOT_FREE);
    REQUIRE(pool->slots[0].prevIndex == MEMSLOT_NONE);
    REQUIRE(pool->slots[0].nextIndex == MEMSLOT_NONE);
    REQUIRE(validate_pool(pool) == 0);

    pool = reset_pool(4);
    a = (u8 *) mempool_alloc(pool->slots[0].size, COLOUR_TAG_RED);
    REQUIRE(a != NULL);
    REQUIRE(pool->curNumSlots == 1);
    mempool_free_addr(a);
    REQUIRE(pool->curNumSlots == 1);
    REQUIRE(pool->slots[0].nextIndex == MEMSLOT_NONE);
    return validate_pool(pool);
}

static int test_delayed_free_queue(void) {
    MemoryPool *pool = reset_pool(TEST_POOL_SLOTS);
    void *addresses[FREE_QUEUE_SIZE + 1];
    s32 i;

    for (i = 0; i < FREE_QUEUE_SIZE + 1; i++) {
        addresses[i] = mempool_alloc(16, COLOUR_TAG_WHITE);
        REQUIRE(addresses[i] != NULL);
    }
    mempool_free_timer(2);
    for (i = 0; i < FREE_QUEUE_SIZE; i++) {
        mempool_free(addresses[i]);
    }
    REQUIRE(gFreeQueueCount == FREE_QUEUE_SIZE);
    mempool_free(addresses[FREE_QUEUE_SIZE]);
    REQUIRE(gFreeQueueCount == FREE_QUEUE_SIZE + 1);

    mempool_free_queue_clear();
    REQUIRE(gFreeQueueCount == FREE_QUEUE_SIZE + 1);
    mempool_free_queue_clear();
    REQUIRE(gFreeQueueCount == 0);
    mempool_free_timer(0);
    REQUIRE(pool->curNumSlots == 1);
    return validate_pool(pool);
}

static int test_invalid_addresses_and_sizes(void) {
    MemoryPool *pool = reset_pool(8);
    u8 foreign = 0;
    u8 *valid = (u8 *) mempool_alloc(32, COLOUR_TAG_RED);
    u8 *onePast = sPoolStorage + sizeof(sPoolStorage);
    s32 slotsBefore = pool->curNumSlots;

    REQUIRE(valid != NULL);
    REQUIRE(mempool_get_pool(valid) == POOL_MAIN);
    REQUIRE(mempool_get_pool(NULL) == MEMPOOL_INVALID_POOL);
    REQUIRE(mempool_get_pool(onePast) == MEMPOOL_INVALID_POOL);
    REQUIRE(mempool_get_pool(&foreign) == MEMPOOL_INVALID_POOL);
    mempool_free_addr(NULL);
    mempool_free_addr(onePast);
    mempool_free_addr(&foreign);
    REQUIRE(mempool_locked_set(&foreign) == 0);
    REQUIRE(mempool_locked_unset(&foreign) == 0);
    REQUIRE(pool->curNumSlots == slotsBefore);

    REQUIRE(mempool_alloc(0, COLOUR_TAG_RED) == NULL);
    REQUIRE(mempool_alloc(-1, COLOUR_TAG_RED) == NULL);
    REQUIRE(mempool_alloc(INT32_MAX, COLOUR_TAG_RED) == NULL);
    REQUIRE(mempool_alloc(INT32_MAX - 7, COLOUR_TAG_RED) == NULL);
    REQUIRE(mempool_alloc_fixed(0, valid, COLOUR_TAG_RED) == NULL);
    REQUIRE(mempool_alloc_fixed(-1, valid, COLOUR_TAG_RED) == NULL);
    REQUIRE(pool->curNumSlots == slotsBefore);

    EXPECT_FATAL(mempool_free_timer(-1));
    EXPECT_FATAL(mempool_free_timer(UINT8_MAX + 1));
    EXPECT_FATAL(mempool_alloc_safe(0, COLOUR_TAG_RED));
    REQUIRE(sDumpCount == 1);
    return validate_pool(pool);
}

static int test_slot_and_byte_exhaustion(void) {
    MemoryPool *pool = reset_pool(2);
    u8 *first = (u8 *) mempool_alloc(16, COLOUR_TAG_RED);
    s32 remainder;
    u8 *exact;

    REQUIRE(first != NULL);
    REQUIRE(pool->curNumSlots == pool->maxNumSlots);
    remainder = pool->slots[pool->slots[0].nextIndex].size;
    REQUIRE(mempool_alloc(16, COLOUR_TAG_BLUE) == NULL);
    exact = (u8 *) mempool_alloc(remainder, COLOUR_TAG_GREEN);
    REQUIRE(exact != NULL);
    REQUIRE(pool->curNumSlots == pool->maxNumSlots);
    REQUIRE(mempool_alloc(16, COLOUR_TAG_BLUE) == NULL);
    EXPECT_FATAL(mempool_alloc_safe(16, COLOUR_TAG_BLUE));
    REQUIRE(sDumpCount == 1);
    REQUIRE(validate_pool(pool) == 0);

    pool = reset_pool(8);
    REQUIRE(mempool_alloc(pool->slots[0].size + 16, COLOUR_TAG_RED) == NULL);
    REQUIRE(pool->curNumSlots == 1);
    return validate_pool(pool);
}

static int test_fixed_and_pointer_alignment(void) {
    MemoryPool *pool = reset_pool(8);
    u8 *base = pool->slots[0].data;
    u8 *fixed = base + 64;

    REQUIRE(mempool_alloc_fixed(25, fixed, COLOUR_TAG_RED) == fixed);
    REQUIRE(pool->curNumSlots == 3);
    REQUIRE(validate_pool(pool) == 0);
    mempool_free_addr(fixed);
    REQUIRE(pool->curNumSlots == 1);

    pool = reset_pool(2);
    base = pool->slots[0].data;
    REQUIRE(mempool_alloc_fixed(16, base + 16, COLOUR_TAG_RED) == NULL);
    REQUIRE(pool->curNumSlots == 1);
    REQUIRE(pool->slots[0].flags == SLOT_FREE);
    REQUIRE(validate_pool(pool) == 0);

#if UINTPTR_MAX > UINT32_MAX
    REQUIRE((uintptr_t) align4((u8 *) (uintptr_t) 0x100000003ULL) == 0x100000004ULL);
    REQUIRE((uintptr_t) align8((u8 *) (uintptr_t) 0x100000003ULL) == 0x100000008ULL);
    REQUIRE((uintptr_t) align16((u8 *) (uintptr_t) 0x100000003ULL) == 0x100000010ULL);
#endif
    REQUIRE(align4((u8 *) UINTPTR_MAX) == NULL);
    REQUIRE(align8((u8 *) UINTPTR_MAX) == NULL);
    REQUIRE(align16((u8 *) UINTPTR_MAX) == NULL);
    return 0;
}

static int test_resource_stats(void) {
    MemoryPool *pool = reset_pool(8);
    MdkrMemoryPoolStats stats;
    s32 initialBytes = pool->slots[0].size;
    u8 *allocation;

    REQUIRE(mdkr_mempool_stats(POOL_MAIN, &stats) == TRUE);
    REQUIRE(stats.liveSlots == 0);
    REQUIRE(stats.freeSlots == 1);
    REQUIRE(stats.usedBytes == 0);
    REQUIRE(stats.freeBytes == initialBytes);
    REQUIRE(stats.largestFreeBytes == initialBytes);

    allocation = (u8 *)mempool_alloc(17, COLOUR_TAG_CYAN);
    REQUIRE(allocation != NULL);
    REQUIRE(mdkr_mempool_stats(POOL_MAIN, &stats) == TRUE);
    REQUIRE(stats.liveSlots == 1);
    REQUIRE(stats.freeSlots == 1);
    REQUIRE(stats.usedBytes == 32);
    REQUIRE(stats.freeBytes == initialBytes - 32);
    REQUIRE(stats.largestFreeBytes == initialBytes - 32);

    mempool_free_addr(allocation);
    REQUIRE(mdkr_mempool_stats(POOL_MAIN, &stats) == TRUE);
    REQUIRE(stats.liveSlots == 0);
    REQUIRE(stats.freeSlots == 1);
    REQUIRE(stats.freeBytes == initialBytes);
    REQUIRE(mdkr_mempool_stats(POOL_OBJECT, &stats) == FALSE);
    REQUIRE(mdkr_mempool_stats(POOL_MAIN, NULL) == FALSE);
    return 0;
}

int main(void) {
    if (test_slot_boundaries_and_coalescing() != 0 ||
        test_delayed_free_queue() != 0 ||
        test_invalid_addresses_and_sizes() != 0 ||
        test_slot_and_byte_exhaustion() != 0 ||
        test_fixed_and_pointer_alignment() != 0 ||
        test_resource_stats() != 0) {
        return 1;
    }
    puts("memory allocator invariant tests passed");
    return 0;
}
