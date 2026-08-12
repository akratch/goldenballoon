/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "platform/gameplay_event_trace.h"

typedef struct ObservationCounts {
    unsigned events;
    unsigned ticks;
} ObservationCounts;

static ObservationCounts sAccessibility;
static ObservationCounts sRollback;

static void accessibility_event(
    GameplayEventKind kind, int32_t a, int32_t b, int32_t c, int32_t d) {
    assert(kind == GAMEPLAY_EVENT_SOUND);
    assert(a == 7 && b == 8 && c == 9 && d == 10);
    sAccessibility.events++;
}

static void accessibility_tick(uint64_t tick) {
    assert(tick == 42u);
    sAccessibility.ticks++;
}

static void rollback_event(
    GameplayEventKind kind, int32_t a, int32_t b, int32_t c, int32_t d) {
    assert(kind == GAMEPLAY_EVENT_SOUND);
    assert(a == 7 && b == 8 && c == 9 && d == 10);
    sRollback.events++;
}

static void rollback_tick(uint64_t tick) {
    assert(tick == 42u);
    sRollback.ticks++;
}

int main(void) {
    static const GameplayEventObserver accessibility = {
        accessibility_event, accessibility_tick};
    static const GameplayEventObserver rollback = {
        rollback_event, rollback_tick};

    gameplay_event_trace_set_observer(&accessibility);
    gameplay_event_trace_set_rollback_observer(&rollback);
    assert(gameplay_event_trace_enabled());
    gameplay_event_trace_emit(GAMEPLAY_EVENT_SOUND, 7, 8, 9, 10);
    gameplay_event_trace_tick(42u);
    assert(sAccessibility.events == 1u && sAccessibility.ticks == 1u);
    assert(sRollback.events == 1u && sRollback.ticks == 1u);

    gameplay_event_trace_set_rollback_observer(NULL);
    gameplay_event_trace_emit(GAMEPLAY_EVENT_SOUND, 7, 8, 9, 10);
    gameplay_event_trace_tick(42u);
    assert(sAccessibility.events == 2u && sAccessibility.ticks == 2u);
    assert(sRollback.events == 1u && sRollback.ticks == 1u);

    gameplay_event_trace_set_observer(NULL);
    puts("test_gameplay_event_trace: PASS");
    return 0;
}
