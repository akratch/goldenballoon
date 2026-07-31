#include "memory.h"
#include "config.h"
#include "math_util.h"
#include "printf.h"
#include "thread0_epc.h"
#ifdef NATIVE_PORT
#include "platform_os.h"
#include "fast3d/gfx_pc_dkr.h" /* gfx_dkr_texcache_invalidate_range */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifndef _ALIGN16
#define _ALIGN16(a) (((u32) (a) & ~0xF) + 0x10)
#endif

#ifdef NATIVE_PORT
/* A failed pool allocation is silent in the original (stubbed_printf is a no-op),
 * and DKR's callers frequently do not check — e.g. track_setup_racers() stores
 * spawn_object()'s result and immediately dereferences it, so an exhausted object
 * sub-pool surfaces as a SIGSEGV several frames of call stack away from the cause.
 * On a 64-bit host this is not hypothetical: every structure in the object
 * sub-pool carries real host pointers and so costs about twice its N64 size, and
 * the N64-tuned budgets had to be rescaled (see OBJECT_POOL_SIZE in objects.c).
 * Report the first few failures with the numbers needed to tell byte exhaustion
 * from slot exhaustion; bounded so a genuinely-handled failure cannot spam. */
#define MDKR_ALLOC_FAIL_REPORT_LIMIT 8
extern MemoryPool gMemoryPools[POOL_COUNT];
extern s32 gNumberOfMemoryPools;

static s32 sMdkrAllocFailReports = 0;
static s32 sMdkrInvalidAddressReports = 0;

#ifdef MDKR_MEMORY_UNIT_TEST
extern void mdkr_memory_test_abort(void);
#endif

static void mdkr_allocator_fatal(const char *why) {
    fprintf(stderr, "[MEM] FATAL allocator invariant: %s\n", why);
#ifdef MDKR_MEMORY_UNIT_TEST
    mdkr_memory_test_abort();
#else
    abort();
#endif
}

static s32 mdkr_align_alloc_size(s32 requested, s32 *aligned) {
    if (requested <= 0 || requested > INT32_MAX - 15) {
        return FALSE;
    }
    *aligned = (requested + 15) & ~15;
    return TRUE;
}

static s32 mdkr_pool_is_valid(s32 poolIndex) {
    return poolIndex >= 0 && poolIndex < POOL_COUNT && poolIndex <= gNumberOfMemoryPools &&
           gMemoryPools[poolIndex].slots != NULL && gMemoryPools[poolIndex].size > 0;
}

static void mdkr_report_invalid_address(const char *operation, const void *address) {
    if (sMdkrInvalidAddressReports >= MDKR_ALLOC_FAIL_REPORT_LIMIT) {
        return;
    }
    sMdkrInvalidAddressReports++;
    fprintf(stderr, "[MEM] %s rejected address %p outside every memory pool\n", operation, address);
    if (sMdkrInvalidAddressReports == MDKR_ALLOC_FAIL_REPORT_LIMIT) {
        fprintf(stderr, "[MEM] (further invalid-address reports suppressed)\n");
    }
}

static void mdkr_report_alloc_fail(MemoryPool *pool, s32 poolIndex, s32 size, const char *why) {
    s32 freeTot = 0, freeMax = 0, used = 0, slotIndex = 0;
    MemoryPoolSlot *slot;
    if (sMdkrAllocFailReports >= MDKR_ALLOC_FAIL_REPORT_LIMIT) {
        return;
    }
    sMdkrAllocFailReports++;
    do {
        slot = &pool->slots[slotIndex];
        if (slot->flags == SLOT_FREE) {
            freeTot += slot->size;
            if (slot->size > freeMax) {
                freeMax = slot->size;
            }
        } else {
            used += slot->size;
        }
        slotIndex = slot->nextIndex;
    } while (slotIndex != MEMSLOT_NONE);
    fprintf(stderr,
            "[MEM] pool %d allocation FAILED (%s): want %d bytes; slots %d/%d, used %d, free %d, largest free %d\n",
            (int) poolIndex, why, (int) size, (int) pool->curNumSlots, (int) pool->maxNumSlots, (int) used,
            (int) freeTot, (int) freeMax);
    if (sMdkrAllocFailReports == MDKR_ALLOC_FAIL_REPORT_LIMIT) {
        fprintf(stderr, "[MEM] (further allocation failures suppressed)\n");
    }
}
#endif

/************ .bss ************/

MemoryPool gMemoryPools[POOL_COUNT]; // Only two are used.
s32 gNumberOfMemoryPools;
UNUSED s32 D_801235C4;
FreeQueueSlot gFreeQueue[FREE_QUEUE_SIZE];
s32 gFreeQueueCount;
s32 gFreeQueueTimer;
#ifdef NATIVE_PORT
static FreeQueueSlot *sMdkrFreeQueue = gFreeQueue;
static s32 sMdkrFreeQueueCapacity = FREE_QUEUE_SIZE;

static void mdkr_free_queue_grow(void) {
    FreeQueueSlot *grown;
    s32 newCapacity;

    if (sMdkrFreeQueueCapacity > INT32_MAX / 2 ||
        (size_t) (sMdkrFreeQueueCapacity * 2) > SIZE_MAX / sizeof(*grown)) {
        mdkr_allocator_fatal("delayed-free queue capacity overflow");
    }
    newCapacity = sMdkrFreeQueueCapacity * 2;
    grown = malloc((size_t) newCapacity * sizeof(*grown));
    if (grown == NULL) {
        mdkr_allocator_fatal("could not grow delayed-free queue");
    }
    memcpy(grown, sMdkrFreeQueue, (size_t) gFreeQueueCount * sizeof(*grown));
    if (sMdkrFreeQueue != gFreeQueue) {
        free(sMdkrFreeQueue);
    }
    sMdkrFreeQueue = grown;
    sMdkrFreeQueueCapacity = newCapacity;
}
#endif

/******************************/

#ifdef NATIVE_PORT
s32 mdkr_mempool_stats(
    MemoryPools poolIndex, MdkrMemoryPoolStats *stats) {
    MemoryPool *pool;
    s32 slotIndex = 0;
    s32 visited = 0;

    if (stats == NULL || !mdkr_pool_is_valid((s32)poolIndex)) {
        return FALSE;
    }
    memset(stats, 0, sizeof(*stats));
    pool = &gMemoryPools[poolIndex];
    do {
        MemoryPoolSlot *slot;
        if (slotIndex < 0 || slotIndex >= pool->maxNumSlots ||
            visited++ >= pool->maxNumSlots) {
            memset(stats, 0, sizeof(*stats));
            return FALSE;
        }
        slot = &pool->slots[slotIndex];
        if (slot->size < 0) {
            memset(stats, 0, sizeof(*stats));
            return FALSE;
        }
        if (slot->flags == SLOT_FREE) {
            stats->freeSlots++;
            stats->freeBytes += slot->size;
            if (slot->size > stats->largestFreeBytes) {
                stats->largestFreeBytes = slot->size;
            }
        } else {
            stats->liveSlots++;
            stats->usedBytes += slot->size;
        }
        slotIndex = slot->nextIndex;
    } while (slotIndex != MEMSLOT_NONE);
    return TRUE;
}

void mdkr_memory_shutdown(void) {
    if (sMdkrFreeQueue != gFreeQueue) {
        free(sMdkrFreeQueue);
    }
    sMdkrFreeQueue = gFreeQueue;
    sMdkrFreeQueueCapacity = FREE_QUEUE_SIZE;
    gFreeQueueCount = 0;
    gFreeQueueTimer = 0;
    memset(gMemoryPools, 0, sizeof(gMemoryPools));
    gNumberOfMemoryPools = -1;
}
#endif

/**
 * Creates the main memory pool.
 * Starts at 0x8012D3F0. Ends at 0x80400000. Contains 1600 allocation slots.
 * Official Name: mmInit
 */
void mempool_init_main(void) {
#ifdef NATIVE_PORT
    /* PLAN decision 2: RDRAM is a single contiguous, 16 MB-aligned malloc arena
     * rather than a fixed N64 address range. The alignment keeps all arena
     * pointers within one 4 GB window so the game's `(u32)ptr` truncations
     * round-trip (see stubs_dkr.c osPiStartDma / dkr_lo32_to_ptr). */
    void *arena = dkr_arena_init(DKR_ARENA_SIZE);
    gNumberOfMemoryPools = -1;
    mempool_init((MemoryPoolSlot *)arena, (s32)DKR_ARENA_SIZE, MAIN_POOL_SLOT_COUNT);
    mempool_free_timer(2);
    gFreeQueueCount = 0;
#else
    s32 ramEnd;

    gNumberOfMemoryPools = -1;
    if (EXPANSION_PAK_SUPPORT) {
        ramEnd = EXPANSION_RAM_END;
    } else {
        ramEnd = RAM_END;
    }
    mempool_init(&gMainMemoryPool, ramEnd - (s32) (&gMainMemoryPool), MAIN_POOL_SLOT_COUNT);
    mempool_free_timer(2);
    gFreeQueueCount = 0;
#endif
}

/**
 * Creates a new memory pool that's contained inside another one.
 * Official name: mmAllocRegion
 */
MemoryPoolSlot *mempool_new_sub(s32 poolDataSize, s32 numSlots) {
    s32 size;
    MemoryPoolSlot *slots;
    UNUSED s32 unused_2;
    u32 intFlags = interrupts_disable();
    MemoryPoolSlot *newPool;

#ifdef NATIVE_PORT
    if (poolDataSize <= 0 || numSlots <= 0 ||
        numSlots > (INT32_MAX - poolDataSize) / (s32) sizeof(MemoryPoolSlot)) {
        mdkr_allocator_fatal("invalid sub-pool size");
    }
#endif
    size = poolDataSize + (numSlots * sizeof(MemoryPoolSlot));
    slots = (MemoryPoolSlot *) mempool_alloc_safe(size, COLOUR_TAG_WHITE);
    newPool = mempool_init(slots, size, numSlots);
    interrupts_enable(intFlags);
    return newPool;
}

/**
 * Create and initialise a memory pool in RAM that will act as the place where arbitrary allocations can go.
 * Will return the location of the first free slot in that pool.
 */
MemoryPoolSlot *mempool_init(MemoryPoolSlot *slots, s32 poolSize, s32 numSlots) {
    MemoryPoolSlot *firstSlot;
    s32 poolCount;
    s32 i;
    s32 firstSlotSize;
#ifdef NATIVE_PORT
    uintptr_t dataStart;
    uintptr_t metadataEnd;
#endif

#ifdef NATIVE_PORT
    if (slots == NULL || poolSize <= 0 || numSlots <= 0 ||
        numSlots > (poolSize - 1) / (s32) sizeof(MemoryPoolSlot)) {
        mdkr_allocator_fatal("invalid pool geometry");
    }
    if (gNumberOfMemoryPools + 1 >= POOL_COUNT) {
        mdkr_allocator_fatal("too many memory pools");
    }
#endif
    poolCount = ++gNumberOfMemoryPools;
    firstSlotSize = poolSize - (numSlots * sizeof(MemoryPoolSlot));
    gMemoryPools[poolCount].maxNumSlots = numSlots;
    gMemoryPools[poolCount].curNumSlots = 0;
    gMemoryPools[poolCount].slots = slots;
    gMemoryPools[poolCount].size = poolSize;
    firstSlot = slots;
    for (i = 0; i < gMemoryPools[poolCount].maxNumSlots; i++) {
        firstSlot->index = i;
        firstSlot++;
    }
    firstSlot = &gMemoryPools[poolCount].slots[0];
    slots += numSlots;
#ifdef NATIVE_PORT
    metadataEnd = (uintptr_t) slots;
    if (metadataEnd > UINTPTR_MAX - 15) {
        mdkr_allocator_fatal("pool data alignment overflow");
    }
    dataStart = (metadataEnd + 15) & ~(uintptr_t) 15;
    firstSlotSize -= (s32) (dataStart - metadataEnd);
    if (firstSlotSize <= 0) {
        mdkr_allocator_fatal("pool has no allocatable data");
    }
    firstSlot->data = (u8 *) dataStart;
#else
    if ((s32) slots & 0xF) {
        firstSlot->data = (u8 *) _ALIGN16(slots);
    } else {
        firstSlot->data = (u8 *) slots;
    }
#endif
    firstSlot->size = firstSlotSize;
    firstSlot->flags = SLOT_FREE;
    firstSlot->prevIndex = MEMSLOT_NONE;
    firstSlot->nextIndex = MEMSLOT_NONE;
    gMemoryPools[poolCount].curNumSlots++;
    return gMemoryPools[poolCount].slots;
}

/**
 * Reserves and returns memory from the main memory pool. Has 2 assert checks.
 * Will cause an exception if the size is 0 or if memory cannot be reserved,
 * dumping the function stack contents onto a controller pak for debugging.
 */
void *mempool_alloc_safe(s32 size, u32 colourTag) {
    void *addr;
    addr = mempool_slot_find(POOL_MAIN, size, colourTag);
    if (addr == NULL) {
        dump_memory_to_cpak(stack_pointer()->sp, size, colourTag);
#ifdef NATIVE_PORT
        mdkr_allocator_fatal("mempool_alloc_safe could not satisfy allocation");
#endif
    }
    return addr;
}

/**
 * Reserves and returns memory from the main memory pool. Has no assert checks.
 */
void *mempool_alloc(s32 size, u32 colourTag) {
    return mempool_slot_find(POOL_MAIN, size, colourTag);
}

/**
 * Search the existing empty slots and try to find one that can meet the size requirement.
 * Afterwards, write the new allocation data to the slot in question and return the address.
 */
void *mempool_slot_find(MemoryPools poolIndex, s32 size, u32 colourTag) {
    s32 slotSize;
    MemoryPoolSlot *curSlot;
    UNUSED s32 pad;
    MemoryPool *pool;
    MemoryPoolSlot *slots;
    u32 intFlags;
    s32 nextIndex;
    s32 currIndex;

    intFlags = interrupts_disable();
#ifdef NATIVE_PORT
    if (!mdkr_pool_is_valid(poolIndex)) {
        interrupts_enable(intFlags);
        fprintf(stderr, "[MEM] allocation rejected invalid pool %d\n", (int) poolIndex);
        return NULL;
    }
    if (!mdkr_align_alloc_size(size, &size)) {
        interrupts_enable(intFlags);
        fprintf(stderr, "[MEM] allocation rejected invalid size %d\n", (int) size);
        return NULL;
    }
#else
    if (size == 0) {
        stubbed_printf("*** mmAlloc: size = 0 ***\n");
    }
#endif
    pool = &gMemoryPools[poolIndex];
    currIndex = MEMSLOT_NONE;
#ifndef NATIVE_PORT
    if (size & 0xF) {
        size = (size & ~0xF);
        size += 0x10;
    }
#endif
    slots = pool->slots;
    slotSize = 0x7FFFFFFF;
    nextIndex = 0;
    do {
        curSlot = &slots[nextIndex];
        if (curSlot->flags == SLOT_FREE) {
            if (curSlot->size >= size && curSlot->size < slotSize) {
                slotSize = curSlot->size;
                currIndex = nextIndex;
            }
        }
        nextIndex = curSlot->nextIndex;
    } while (nextIndex != MEMSLOT_NONE);
    if (currIndex != MEMSLOT_NONE) {
#ifdef NATIVE_PORT
        if (slots[currIndex].size > size && pool->curNumSlots >= pool->maxNumSlots) {
            interrupts_enable(intFlags);
            mdkr_report_alloc_fail(pool, poolIndex, size, "out of slots");
            return NULL;
        }
#endif
        mempool_slot_assign(poolIndex, (s32) currIndex, size, 1, 0, colourTag);
        interrupts_enable(intFlags);
        return (slots + currIndex)->data;
    }
    interrupts_enable(intFlags);
#ifdef NATIVE_PORT
    mdkr_report_alloc_fail(pool, poolIndex, size, "no block large enough");
#endif
    stubbed_printf("\n*** mm Error *** ---> No suitble block found for allocation.\n");
    return NULL;
}

/**
 * Allocate memory from a specific pool.
 * Official name: mmAllocR
 */
void *mempool_alloc_pool(MemoryPoolSlot *slots, s32 size) {
    s32 i;
    for (i = gNumberOfMemoryPools; i != 0; i--) {
        if (slots == gMemoryPools[i].slots) {
            return mempool_slot_find(i, size, 0);
        }
    }
    return (void *) NULL;
}

/**
 * Allocates memory from the main pool at a fixed address.
 * Rearranges the memory slots to place one at that address if possible.
 * Official Name: mmAllocAtAddr
 */
void *mempool_alloc_fixed(s32 size, u8 *address, u32 colorTag) {
    s32 i;
    MemoryPoolSlot *curSlot;
    MemoryPoolSlot *slots;
    u32 intFlags;

    intFlags = interrupts_disable();
#ifdef NATIVE_PORT
    if (address == NULL || !mdkr_pool_is_valid(POOL_MAIN) || !mdkr_align_alloc_size(size, &size)) {
        interrupts_enable(intFlags);
        fprintf(stderr, "[MEM] fixed allocation rejected invalid address/size (%p, %d)\n", address, (int) size);
        return NULL;
    }
#else
    if (size == 0) {
        stubbed_printf("*** mmAllocAtAddr: size = 0 ***\n");
    }
#endif
#ifndef NATIVE_PORT
    if ((gMemoryPools[POOL_MAIN].curNumSlots + 1) == gMemoryPools[POOL_MAIN].maxNumSlots) {
        interrupts_enable(intFlags);
        stubbed_printf("\n*** mm Error *** ---> No more slots available.\n");
    } else {
        if (size & 0xF) {
            size = _ALIGN16(size);
        }
#else
    {
#endif
        slots = gMemoryPools[POOL_MAIN].slots;
        for (i = 0; i != MEMSLOT_NONE; i = curSlot->nextIndex) {
            curSlot = &slots[i];
            if (curSlot->flags == SLOT_FREE) {
#ifdef NATIVE_PORT
                uintptr_t blockAddress = (uintptr_t) curSlot->data;
                uintptr_t requestedAddress = (uintptr_t) address;
                uintptr_t offset;
                s32 slotsNeeded;

                if (requestedAddress < blockAddress) {
                    continue;
                }
                offset = requestedAddress - blockAddress;
                if (offset > (uintptr_t) curSlot->size ||
                    (uintptr_t) size > (uintptr_t) curSlot->size - offset) {
                    continue;
                }
                slotsNeeded = (offset != 0) + ((uintptr_t) size < (uintptr_t) curSlot->size - offset);
                if (slotsNeeded > gMemoryPools[POOL_MAIN].maxNumSlots -
                                      gMemoryPools[POOL_MAIN].curNumSlots) {
                    continue;
                }
                if (offset == 0) {
                    mempool_slot_assign(POOL_MAIN, i, size, 1, 0, colorTag);
                    interrupts_enable(intFlags);
                    return curSlot->data;
                } else {
                    i = mempool_slot_assign(POOL_MAIN, i, (s32) offset, 0, 1, colorTag);
                    mempool_slot_assign(POOL_MAIN, i, size, 1, 0, colorTag);
                    interrupts_enable(intFlags);
                    return (slots + i)->data;
                }
#else
                if ((u32) address >= (u32) curSlot->data &&
                    (u32) address + size <= (u32) curSlot->data + curSlot->size) {
                    if (address == (u8 *) curSlot->data) {
                        mempool_slot_assign(POOL_MAIN, i, size, 1, 0, colorTag);
                        interrupts_enable(intFlags);
                        return curSlot->data;
                    } else {
                        i = mempool_slot_assign(POOL_MAIN, i, (u32) address - (u32) curSlot->data, 0, 1, colorTag);
                        mempool_slot_assign(POOL_MAIN, i, size, 1, 0, colorTag);
                        interrupts_enable(intFlags);
                        return (slots + i)->data;
                    }
                }
#endif
            }
        }
        interrupts_enable(intFlags);
    }
    stubbed_printf("\n*** mm Error *** ---> Can't allocate memory at desired address.\n");
    return NULL;
}

/**
 * Sets the tick timer for the free queue.
 * If it's set to 0, then it clears the existing queue.
 * Nonzero amounts set any future frees to wait that many ticks
 * before clearing from memory.
 * Official Name: mmSetDelay
 */
void mempool_free_timer(s32 state) {
    u32 intFlags = interrupts_disable();
#ifdef NATIVE_PORT
    if (state < 0 || state > UINT8_MAX) {
        interrupts_enable(intFlags);
        mdkr_allocator_fatal("free delay is outside the queue timer range");
    }
#endif
    gFreeQueueTimer = state;
    if (state == 0) { // flush free queue if state is 0.
        while (gFreeQueueCount > 0) {
#ifdef NATIVE_PORT
            mempool_free_addr(sMdkrFreeQueue[--gFreeQueueCount].dataAddress);
#else
            mempool_free_addr(gFreeQueue[--gFreeQueueCount].dataAddress);
#endif
        }
    }
    interrupts_enable(intFlags);
}

/**
 * Unallocates data from the pool that contains the data. Will free immediately if the free queue
 * state is set to 0, otherwise the data will just be marked for deletion.
 * Official Name: mmFree
 */
void mempool_free(void *data) {
    u32 intFlags = interrupts_disable();
    if (gFreeQueueTimer == 0) {
        mempool_free_addr(data);
    } else {
        mempool_free_queue(data);
    }
    interrupts_enable(intFlags);
}

/**
 * Frees all the addresses in the free queue.
 * Official Name: mmFreeTick
 */
void mempool_free_queue_clear(void) {
    s32 i;
    u32 intFlags;

    intFlags = interrupts_disable();

    for (i = 0; i < gFreeQueueCount;) {
#ifdef NATIVE_PORT
        if (sMdkrFreeQueue[i].freeTimer <= 1) {
            mempool_free_addr(sMdkrFreeQueue[i].dataAddress);
            sMdkrFreeQueue[i] = sMdkrFreeQueue[gFreeQueueCount - 1];
#else
        if (gFreeQueue[i].freeTimer <= 1) {
            mempool_free_addr(gFreeQueue[i].dataAddress);
            gFreeQueue[i].dataAddress = gFreeQueue[gFreeQueueCount - 1].dataAddress;
            gFreeQueue[i].freeTimer = gFreeQueue[gFreeQueueCount - 1].freeTimer;
#endif
            gFreeQueueCount--;
        } else {
#ifdef NATIVE_PORT
            sMdkrFreeQueue[i].freeTimer--;
#else
            gFreeQueue[i].freeTimer--;
#endif
            stubbed_printf("\n*** mm Error *** ---> Can't free ram at this location: %x\n", gFreeQueue[i].dataAddress);
            i++;
        }
    }

    interrupts_enable(intFlags);
}

/**
 * Searches the memory pools for a slot matching the given address.
 * If a slot is found, free it.
 */
void mempool_free_addr(u8 *address) {
    s32 slotIndex;
    s32 poolIndex;
    MemoryPool *pool;
    MemoryPoolSlot *slots;
    MemoryPoolSlot *slot;

    poolIndex = mempool_get_pool(address);
#ifdef NATIVE_PORT
    if (poolIndex == MEMPOOL_INVALID_POOL) {
        mdkr_report_invalid_address("free", address);
        return;
    }
#endif
    pool = gMemoryPools;
    slots = pool[poolIndex].slots;
    for (slotIndex = 0; slotIndex != MEMSLOT_NONE; slotIndex = slot->nextIndex) {
        slot = &slots[slotIndex];

        if (address == (u8 *) slot->data) {
            if (slot->flags == SLOT_USED || slot->flags == SLOT_SAFEGUARD) {
                mempool_slot_clear(poolIndex, slotIndex);
            }
            return;
        }
    }
    stubbed_printf("\n*** mm Error *** ---> No match found for mmFree.\n");
#ifdef NATIVE_PORT
    fprintf(stderr, "[MEM] free found no live allocation at %p\n", address);
#endif
}

/**
 * Clears all memory pools.
 * When clearing a pool, it checks if the slot uses the 0x04 flag.
 * It calls an early return if that flag is set and more slots remain after.
 */
UNUSED void mempool_clear(void) {
    MemoryPoolSlot *slotPos;
    MemoryPool *pool;
    UNUSED s32 pad;
    u32 intFlags;
    s32 poolIndex;
    s32 slotIndex;

    intFlags = interrupts_disable();
    poolIndex = gNumberOfMemoryPools;
    while (poolIndex != -1) {
        pool = &gMemoryPools[poolIndex];
        slotPos = pool->slots;
        slotIndex = 0;
        do {
            if ((slotPos + slotIndex)->flags == SLOT_USED) {
                mempool_slot_clear(poolIndex, slotIndex);
            }
            if ((slotPos + slotIndex)->flags == SLOT_SAFEGUARD) {
                if (pool->curNumSlots == 1) {
                    mempool_slot_clear(poolIndex, slotIndex);
                } else {
                    interrupts_enable(intFlags);
                    stubbed_printf("*** Slots still in use in region ***\n");
                    return;
                }
            }
            slotIndex = (slotPos + slotIndex)->nextIndex;
        } while (slotIndex != MEMSLOT_NONE);
        poolIndex--;
    }
    interrupts_enable(intFlags);
}

/**
 * Adds the current memory address to the back of the queue, so it can be freed.
 */
void mempool_free_queue(void *dataAddress) {
#ifdef NATIVE_PORT
    if (gFreeQueueCount < 0) {
        mdkr_allocator_fatal("negative delayed-free queue count");
    }
    if (gFreeQueueCount >= sMdkrFreeQueueCapacity) {
        mdkr_free_queue_grow();
    }
    sMdkrFreeQueue[gFreeQueueCount].dataAddress = dataAddress;
    sMdkrFreeQueue[gFreeQueueCount].freeTimer = gFreeQueueTimer;
#else
    gFreeQueue[gFreeQueueCount].dataAddress = dataAddress;
    gFreeQueue[gFreeQueueCount].freeTimer = gFreeQueueTimer;
#endif
    gFreeQueueCount++;

#ifndef NATIVE_PORT
    if (gFreeQueueCount >= ARRAY_COUNT(gFreeQueue)) {
        stubbed_printf("\n*** mm Error *** ---> stbf stack too deep!\n");
    }
#endif
}

/**
 * Search the memory pool for the current address.
 * Check if the slot is used, then set the 0x02 slot flag.
 * Return 1 if successful, otherwise return 0.
 */
s32 mempool_locked_set(u8 *address) {
    s32 slotIndex;
    MemoryPoolSlot *slot;
    MemoryPool *pool;
    u32 intFlags;

    intFlags = interrupts_disable();
    slotIndex = mempool_get_pool(address);
#ifdef NATIVE_PORT
    if (slotIndex == MEMPOOL_INVALID_POOL) {
        mdkr_report_invalid_address("lock", address);
        interrupts_enable(intFlags);
        return 0;
    }
#endif
    pool = &gMemoryPools[slotIndex];
    slotIndex = 0;
    while (slotIndex != MEMSLOT_NONE) {
        slot = slotIndex + pool->slots; // `slot = &pool->slots[slotIndex];` does not match.
        if (address == (u8 *) slot->data) {
            if (slot->flags == SLOT_USED || slot->flags == SLOT_SAFEGUARD) {
                slot->flags |= SLOT_LOCKED;
                interrupts_enable(intFlags);
                return 1;
            }
        }
        slotIndex = slot->nextIndex;
    }
    stubbed_printf("\n*** mm Error *** ---> Can't fix the specified block.\n");
    interrupts_enable(intFlags);
    return 0;
}

/**
 * Search the memory pool for the current address.
 * Check if the slot is marked with 0x02.
 * Unset that flag and return 1 if so, otherwise 0.
 */
s32 mempool_locked_unset(u8 *address) {
    s32 slotIndex;
    MemoryPoolSlot *slot;
    MemoryPool *pool;
    u32 intFlags;

    intFlags = interrupts_disable();
    slotIndex = mempool_get_pool(address);
#ifdef NATIVE_PORT
    if (slotIndex == MEMPOOL_INVALID_POOL) {
        mdkr_report_invalid_address("unlock", address);
        interrupts_enable(intFlags);
        return 0;
    }
#endif
    pool = &gMemoryPools[slotIndex];
    slotIndex = 0;
    while (slotIndex != MEMSLOT_NONE) {
        slot = slotIndex + pool->slots; // `slot = &pool->slots[slotIndex];` does not match.
        if (address == (u8 *) slot->data) {
            if (slot->flags & SLOT_LOCKED) {
                slot->flags ^= SLOT_LOCKED;
                interrupts_enable(intFlags);
                return 1;
            }
        }
        slotIndex = slot->nextIndex;
    }
    stubbed_printf("\n*** mm Error *** ---> Can't unfix the specified block.\n");
    interrupts_enable(intFlags);
    return 0;
}

/**
 * Returns the index of the memory pool containing the memory address.
 */
s32 mempool_get_pool(u8 *address) {
    s32 i;
    MemoryPool *pool;

#ifdef NATIVE_PORT
    uintptr_t needle = (uintptr_t) address;
    uintptr_t begin;

    if (address == NULL) {
        return MEMPOOL_INVALID_POOL;
    }
    for (i = gNumberOfMemoryPools; i >= 0; i--) {
        if (!mdkr_pool_is_valid(i)) {
            continue;
        }
        pool = &gMemoryPools[i];
        begin = (uintptr_t) pool->slots;
        if (needle >= begin && (uintptr_t) (needle - begin) < (uintptr_t) pool->size) {
            return i;
        }
    }
    return MEMPOOL_INVALID_POOL;
#else
    for (i = gNumberOfMemoryPools; i > 0; i--) {
        pool = &gMemoryPools[i];
        if ((u8 *) pool->slots >= address) {
            continue;
        }
        if (address < pool->size + (u8 *) pool->slots) {
            break;
        }
    }
    return i;
#endif
}

/**
 * Clears the current slot of all information, effectively freeing the allocated memory.
 * Unused slots before and after will be merged with this slot
 */
void mempool_slot_clear(MemoryPools poolIndex, s32 slotIndex) {
    s32 nextIndex;
    s32 prevIndex;
    s32 tempNextIndex;
    MemoryPool *pool;
    MemoryPoolSlot *slots;
    MemoryPoolSlot *slot;
    MemoryPoolSlot *nextSlot;
    MemoryPoolSlot *prevSlot;

    pool = &gMemoryPools[poolIndex];
    slots = pool->slots;
    pool = pool; // Fakematch
    slot = &slots[slotIndex];
    nextIndex = slot->nextIndex;
    prevIndex = slot->prevIndex;
    slot = slot; // Fakematch
#ifdef NATIVE_PORT
    /* This is the single point at which arena bytes become reusable. The HLE's
     * texture cache is keyed on the source ADDRESS, so an entry would otherwise
     * outlive the asset and be handed back for whatever mempool puts there next —
     * silently rendering the previous asset's image whenever the new one happens
     * to share its dimensions and format. Drop the overlapping entries here,
     * BEFORE the free-slot coalescing below rewrites slot->size. */
    gfx_dkr_texcache_invalidate_range(slot->data, (u32) slot->size);
#endif
    slot->flags = SLOT_FREE;
    if (nextIndex != MEMSLOT_NONE) {
        nextSlot = &slots[nextIndex];
        if (nextSlot->flags == SLOT_FREE) {
            slot->size += nextSlot->size;
            tempNextIndex = nextSlot->nextIndex;
            slot->nextIndex = tempNextIndex;
            if (tempNextIndex != MEMSLOT_NONE) {
                slots[tempNextIndex].prevIndex = slotIndex;
            }
            pool->curNumSlots--;
            slots[pool->curNumSlots].index = nextIndex;
        }
    }
    if (prevIndex != MEMSLOT_NONE) {
        prevSlot = &slots[prevIndex];
        if (prevSlot->flags == SLOT_FREE) {
            prevSlot->size += slot->size;
            tempNextIndex = slot->nextIndex;
            prevSlot->nextIndex = tempNextIndex;
            if (tempNextIndex != MEMSLOT_NONE) {
                slots[tempNextIndex].prevIndex = prevIndex;
            }
            pool->curNumSlots--;
            slots[pool->curNumSlots].index = slotIndex;
        }
    }
}

/**
 * Return the address of the first slot of a given memory pool.
 * Official Name: mmGetSlotPtr
 */
UNUSED MemoryPoolSlot *get_memory_pool_address(MemoryPools poolIndex) {
    return gMemoryPools[poolIndex].slots;
}

/**
 * Initialise and attempts to fit the new memory block in the slot given.
 * Updates the linked list with any entries before and after then returns the new slot index.
 * If the region cannot fit, return the old slot instead.
 */
s32 mempool_slot_assign(MemoryPools poolIndex, s32 slotIndex, s32 size, s32 slotIsTaken, s32 newSlotIsTaken,
                        u32 colourTag) {
    MemoryPool *pool;
    MemoryPoolSlot *poolSlots;
    s32 index;
    s32 nextIndex;
    s32 poolSize;

    pool = &gMemoryPools[poolIndex];
    poolSlots = pool->slots;
    pool = pool; // Fakematch
#ifdef NATIVE_PORT
    poolSize = poolSlots[slotIndex].size;
    if (size <= 0 || size > poolSize || (size < poolSize && pool->curNumSlots >= pool->maxNumSlots)) {
        return slotIndex;
    }
#endif
    poolSlots[slotIndex].flags = slotIsTaken;
    poolSize = poolSlots[slotIndex].size;
    poolSlots[slotIndex].size = size;
    poolSlots[slotIndex].colourTag = colourTag;
    if (size < poolSize) {
        index = poolSlots[pool->curNumSlots].index;
        index = (pool->curNumSlots + poolSlots)->index;
        pool->curNumSlots++;
        poolSlots[index].data = &poolSlots[slotIndex].data[size];
        poolSlots[index].size = poolSize;
        poolSlots[index].size -= size;
        poolSlots[index].flags = newSlotIsTaken;
        poolSize = poolSlots[slotIndex].nextIndex;
        nextIndex = poolSize;
        poolSlots[index].prevIndex = slotIndex;
        poolSlots[index].nextIndex = nextIndex;
        poolSlots[slotIndex].nextIndex = index;
        if (nextIndex != MEMSLOT_NONE) {
            poolSlots[nextIndex].prevIndex = index;
        }
        return index;
    }
    return slotIndex;
}

/**
 * Returns the passed in address aligned to the next 16-byte boundary.
 * Official name: mmAlign16
 */
u8 *align16(u8 *address) {
#ifdef NATIVE_PORT
    /* LP64: the original casts the pointer through s32, truncating a 64-bit host
     * pointer to 32 bits when the low nibble is non-zero. That silently corrupts
     * every alignment result (e.g. the texture material-list cursor in
     * load_texture, sprite allocators), producing wild writes. Align through
     * uintptr_t so the high bits survive. */
    uintptr_t value = (uintptr_t) address;
    if (value > UINTPTR_MAX - 15) {
        return NULL;
    }
    return (u8 *) ((value + 15) & ~(uintptr_t) 15);
#else
    s32 remainder = (s32) address & 0xF;
    if (remainder > 0) {
        address = (u8 *) (((s32) address - remainder) + 16);
    }
    return address;
#endif
}

/**
 * Returns the passed in address aligned to the next 8-byte boundary.
 * Official name: mmAlign8
 */
UNUSED u8 *align8(u8 *address) {
#ifdef NATIVE_PORT
    uintptr_t value = (uintptr_t) address;
    if (value > UINTPTR_MAX - 7) {
        return NULL;
    }
    return (u8 *) ((value + 7) & ~(uintptr_t) 7);
#else
    s32 remainder = (s32) address & 0x7;
    if (remainder > 0) {
        address = (u8 *) (((s32) address - remainder) + 8);
    }
    return address;
#endif
}

/**
 * Returns the passed in address aligned to the next 4-byte boundary.
 * Official name: mmAlign4
 */
UNUSED u8 *align4(u8 *address) {
#ifdef NATIVE_PORT
    uintptr_t value = (uintptr_t) address;
    if (value > UINTPTR_MAX - 3) {
        return NULL;
    }
    return (u8 *) ((value + 3) & ~(uintptr_t) 3);
#else
    s32 remainder = (s32) address & 0x3;
    if (remainder > 0) {
        address = (u8 *) (((s32) address - remainder) + 4);
    }
    return address;
#endif
}

/**
 * Iterate through all active memory pool slots and categorise them by colour.
 * Tally how many colours there are, then how many colours are in use.
 * Afterwards, print out the results.
 * Since the function only has space for 64 different colours, make sure
 * any missed tallies are at least reported.
 */
UNUSED s32 find_active_pool_slot_colours(void) {
    u32 colours[64];
    u32 colourCounts[64];
    MemoryPoolSlot *curSlot;
    s32 numOverflows;
    s32 i;
    s32 j;
    s32 colourIndex;
    s32 slotColour;
    u32 curColour;

    numOverflows = 0;
    for (j = 0; j < 64; j++) {
        colours[j] = 0;
        colourCounts[j] = 0;
    }
    for (i = 0; i < gNumberOfMemoryPools; i++) {
        curSlot = gMemoryPools[i].slots;
        do {
            if (curSlot->flags != SLOT_FREE) {
                slotColour = curSlot->colourTag;
                if (slotColour != 0) {
                    curColour = slotColour;
                    colourIndex = 0;
                    while (colourIndex < 64 && curColour != colours[colourIndex] && colours[colourIndex] != 0) {
                        slotColour = colours[colourIndex];
                        colourIndex++;
                    }
                    if (colourIndex < 64) {
                        colours[colourIndex] = curColour;
                        colourCounts[colourIndex]++;
                    } else {
                        numOverflows++;
                    }
                }
            }
            slotColour = curSlot->nextIndex;
            if (slotColour != MEMSLOT_NONE) {
                curSlot = &gMemoryPools[i].slots[curSlot->nextIndex];
            }
        } while (slotColour != MEMSLOT_NONE);
    }
    slotColour = 0;
    for (j = 0; colours[j] != 0 && j < 64; j++) {
        stubbed_printf("Colour %x >> %d\n", colours[j], colourCounts[j]);
    }
    slotColour = 0;
    if (numOverflows) {
        stubbed_printf("Unable to record %d slots, colours overflowed table.\n", numOverflows);
    }
    numOverflows = slotColour;
    return 0;
}

/**
 * Search through each memory pool, counting up the slots that match the colour tag looking to be found.
 * Marked as unused, since the functions calling it is also unused.
 */
UNUSED s32 get_memory_colour_tag_count(u32 colourTag) {
    s32 i, count;
    MemoryPoolSlot *slot;
    count = 0;
    slot = &gMemoryPools[POOL_MAIN].slots[0];
    for (i = 0; i < MAIN_POOL_SLOT_COUNT; i++) {
        if (slot->flags != SLOT_FREE) {
            if (colourTag == slot->colourTag) {
                count++;
            }
        }
        slot++;
    }
    return count;
}

/**
 * Prints out the counts for each color tag in the main pool.
 */
UNUSED void mempool_print_tags_usb(void) {
    stubbed_printf("RED %d\n", get_memory_colour_tag_count(COLOUR_TAG_RED));
    stubbed_printf("GREEN %d\n", get_memory_colour_tag_count(COLOUR_TAG_GREEN));
    stubbed_printf("BLUE %d\n", get_memory_colour_tag_count(COLOUR_TAG_BLUE));
    stubbed_printf("YELLOW %d\n", get_memory_colour_tag_count(COLOUR_TAG_YELLOW));
    stubbed_printf("MAGENTA %d\n", get_memory_colour_tag_count(COLOUR_TAG_MAGENTA));
    stubbed_printf("CYAN %d\n", get_memory_colour_tag_count(COLOUR_TAG_CYAN));
    stubbed_printf("WHITE %d\n", get_memory_colour_tag_count(COLOUR_TAG_WHITE));
    stubbed_printf("GREY %d\n", get_memory_colour_tag_count(COLOUR_TAG_GREY));
    stubbed_printf("ORANGE %d\n\n", get_memory_colour_tag_count(COLOUR_TAG_ORANGE));
}

/**
 * Draws the counts for each color tag in the main pool.
 * See: https://tcrf.net/Diddy_Kong_Racing#Current_Colors
 */
UNUSED void mempool_print_tags_screen(void) {
    set_render_printf_background_colour(0, 0, 0, 128);
    render_printf("RED %d\n", get_memory_colour_tag_count(COLOUR_TAG_RED));
    render_printf("GREEN %d\n", get_memory_colour_tag_count(COLOUR_TAG_GREEN));
    render_printf("BLUE %d\n", get_memory_colour_tag_count(COLOUR_TAG_BLUE));
    render_printf("YELLOW %d\n", get_memory_colour_tag_count(COLOUR_TAG_YELLOW));
    render_printf("MAGENTA %d\n", get_memory_colour_tag_count(COLOUR_TAG_MAGENTA));
    render_printf("CYAN %d\n", get_memory_colour_tag_count(COLOUR_TAG_CYAN));
    render_printf("WHITE %d\n", get_memory_colour_tag_count(COLOUR_TAG_WHITE));
    render_printf("GREY %d\n", get_memory_colour_tag_count(COLOUR_TAG_GREY));
    render_printf("ORANGE %d\n\n", get_memory_colour_tag_count(COLOUR_TAG_ORANGE));
}

/**
 * Prints out the status of each memory pool slot.
 * Will mark based on what flags the slot has.
 * Official name: mmSlotPrint
 */
UNUSED void mempool_print_slots(void) {
    int flags;
    int nextIndex;
    int i;
    MemoryPoolSlot *slot;

    for (i = 0; i <= gNumberOfMemoryPools; i++) {
        stubbed_printf("Region = %d	 loc = %x	 size = %x\t", i, gMemoryPools[i].slots, gMemoryPools[i].size);
        slot = &gMemoryPools[i].slots[0];

        do {
            flags = slot->flags;
            nextIndex = slot->nextIndex;

            switch (flags) {
                case 0:
                    stubbed_printf("FREE");
                    break;
                case 1:
                    stubbed_printf("ALLOCATED");
                    break;
                case 2:
                    stubbed_printf("ALLOCATED,FIXED");
                    break;
                default:
                    stubbed_printf("\n");
                    break;
            }
            stubbed_printf("\n");
            if (nextIndex == -1) {
                continue;
            } else {
                slot = &gMemoryPools[i].slots[slot->nextIndex];
            }
        } while (nextIndex != -1);
    }
}

/**
 * Print out the current status of each existing memory pool.
 */
UNUSED void mempool_print_data(void) {
    s32 i;
    for (i = gNumberOfMemoryPools; i != -1; i--) {
        stubbed_printf("Region number = %d\t", i);
        stubbed_printf("maxSlots = %d\t", gMemoryPools[i].maxNumSlots);
        stubbed_printf("slotsUsed = %d\t", gMemoryPools[i].curNumSlots);
        stubbed_printf("loc = %x\t", gMemoryPools[i].slots);
        stubbed_printf("size = %x\n", gMemoryPools[i].size);
        stubbed_printf("\n");
    }
}
