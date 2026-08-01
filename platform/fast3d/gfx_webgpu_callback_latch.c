#include "gfx_webgpu_callback_latch.h"

#include <stdatomic.h>

/* Two low bits hold GfxWebgpuCallbackEvent; generations are monotonically
 * increasing process-local tokens and therefore have ample remaining space.
 *
 * Each packed slot is one indivisible {generation,event} record. This matters
 * when a spontaneous callback races candidate retirement: no thread can clear
 * a different generation's newly published event through a split update. Two
 * slots are sufficient for the only supported transaction: one live device and
 * one replacement candidate. */
#define CALLBACK_EVENT_BITS 2u
#define CALLBACK_EVENT_MASK ((uintptr_t)((1u << CALLBACK_EVENT_BITS) - 1u))
#define CALLBACK_SLOT_COUNT 2u

static _Atomic(uintptr_t) s_active_generation;
static _Atomic(uintptr_t) s_slots[CALLBACK_SLOT_COUNT];

static uintptr_t make_record(
    uintptr_t generation, enum GfxWebgpuCallbackEvent event) {
    if (generation == 0u || generation > (UINTPTR_MAX >> CALLBACK_EVENT_BITS) ||
        event <= GFX_WEBGPU_CALLBACK_NONE ||
        (uintptr_t)event > CALLBACK_EVENT_MASK) {
        return 0u;
    }
    return (generation << CALLBACK_EVENT_BITS) | (uintptr_t)event;
}

static uintptr_t record_generation(uintptr_t record) {
    return record >> CALLBACK_EVENT_BITS;
}

bool gfx_webgpu_callback_latch_begin(uintptr_t generation) {
    const uintptr_t reservation = generation << CALLBACK_EVENT_BITS;
    if (generation == 0u ||
        generation > (UINTPTR_MAX >> CALLBACK_EVENT_BITS)) {
        return false;
    }
    for (unsigned i = 0; i < CALLBACK_SLOT_COUNT; ++i) {
        const uintptr_t existing = atomic_load_explicit(
            &s_slots[i], memory_order_acquire);
        if (record_generation(existing) == generation) {
            return false;
        }
    }
    for (unsigned i = 0; i < CALLBACK_SLOT_COUNT; ++i) {
        uintptr_t empty = 0u;
        if (atomic_compare_exchange_strong_explicit(
                &s_slots[i], &empty, reservation,
                memory_order_acq_rel, memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void gfx_webgpu_callback_latch_retire(uintptr_t generation) {
    if (generation == 0u) return;

    uintptr_t active = generation;
    (void)atomic_compare_exchange_strong_explicit(
        &s_active_generation, &active, 0u,
        memory_order_acq_rel, memory_order_acquire);

    for (unsigned i = 0; i < CALLBACK_SLOT_COUNT; ++i) {
        uintptr_t record = atomic_load_explicit(
            &s_slots[i], memory_order_acquire);
        while (record_generation(record) == generation &&
               !atomic_compare_exchange_weak_explicit(
                   &s_slots[i], &record, 0u,
                   memory_order_acq_rel, memory_order_acquire)) {
        }
    }
}

bool gfx_webgpu_callback_latch_commit(uintptr_t generation) {
    bool reserved = false;
    if (generation == 0u) return false;
    for (unsigned i = 0; i < CALLBACK_SLOT_COUNT; ++i) {
        if (record_generation(atomic_load_explicit(
                &s_slots[i], memory_order_acquire)) == generation) {
            reserved = true;
            break;
        }
    }
    if (!reserved) return false;

    const uintptr_t prior = atomic_exchange_explicit(
        &s_active_generation, generation, memory_order_acq_rel);
    if (prior != 0u && prior != generation) {
        gfx_webgpu_callback_latch_retire(prior);
    }
    return true;
}

uintptr_t gfx_webgpu_callback_latch_active_generation(void) {
    return atomic_load_explicit(&s_active_generation, memory_order_acquire);
}

bool gfx_webgpu_callback_latch_publish(
    uintptr_t generation, enum GfxWebgpuCallbackEvent event) {
    const uintptr_t reservation = generation << CALLBACK_EVENT_BITS;
    const uintptr_t event_record = make_record(generation, event);
    if (event_record == 0u) return false;

    for (unsigned i = 0; i < CALLBACK_SLOT_COUNT; ++i) {
        uintptr_t expected = reservation;
        if (atomic_compare_exchange_strong_explicit(
                &s_slots[i], &expected, event_record,
                memory_order_acq_rel, memory_order_acquire)) {
            return true;
        }
        if (record_generation(expected) == generation) {
            return false;
        }
    }
    return false;
}

bool gfx_webgpu_callback_latch_consume(
    uintptr_t generation, enum GfxWebgpuCallbackEvent *event) {
    if (generation == 0u ||
        atomic_load_explicit(&s_active_generation, memory_order_acquire) !=
            generation) {
        return false;
    }
    for (unsigned i = 0; i < CALLBACK_SLOT_COUNT; ++i) {
        uintptr_t record = atomic_load_explicit(
            &s_slots[i], memory_order_acquire);
        if (record_generation(record) != generation ||
            (record & CALLBACK_EVENT_MASK) == GFX_WEBGPU_CALLBACK_NONE) {
            continue;
        }
        const enum GfxWebgpuCallbackEvent consumed =
            (enum GfxWebgpuCallbackEvent)(record & CALLBACK_EVENT_MASK);
        const uintptr_t reservation = generation << CALLBACK_EVENT_BITS;
        if (!atomic_compare_exchange_strong_explicit(
                &s_slots[i], &record, reservation,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        if (event != NULL) *event = consumed;
        return true;
    }
    return false;
}
