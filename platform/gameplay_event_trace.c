#include "gameplay_event_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENT_HASH_OFFSET UINT64_C(14695981039346656037)
#define EVENT_HASH_PRIME  UINT64_C(1099511628211)

static int s_hash_stream = -1;
static const GameplayEventObserver *s_observer;
static const GameplayEventObserver *s_rollback_observer;
static uint64_t s_hash = EVENT_HASH_OFFSET;
static uint64_t s_ticks;
static uint64_t s_total_events;
static uint64_t s_total_kind[GAMEPLAY_EVENT_KIND_COUNT];
static uint32_t s_tick_events;
static uint32_t s_tick_kind[GAMEPLAY_EVENT_KIND_COUNT];
static int32_t s_last_mode;
static int32_t s_last_level;
static int s_context_valid;
static int64_t s_perturb_tick = -1;

/* The `[EVENTHASH]` fidelity stream itself: parsed once, from MDKR_EVENT_HASH,
 * and the sole authority over what is hashed and printed. */
static bool hash_stream_enabled(void) {
    if (s_hash_stream < 0) {
        const char *value = getenv("MDKR_EVENT_HASH");
        const char *perturb = getenv("MDKR_TEST_EVENT_PERTURB");
        char *end = NULL;
        s_hash_stream = value != NULL && value[0] != '\0' &&
                        strcmp(value, "0") != 0;
        if (perturb != NULL && perturb[0] != '\0') {
            long long parsed = strtoll(perturb, &end, 10);
            if (end != perturb && *end == '\0' && parsed >= 0) {
                s_perturb_tick = (int64_t)parsed;
            }
        }
    }
    return s_hash_stream != 0;
}

bool gameplay_event_trace_enabled(void) {
    /* Two readers, one predicate: the emission sites are armed when EITHER the
     * hash stream or an observer wants the events. Keeping this one function is
     * what stops a call site from being armed for one reader and not the
     * other, which would make an observer see a stream the hash never did. */
    return hash_stream_enabled() || s_observer != NULL ||
           s_rollback_observer != NULL;
}

void gameplay_event_trace_set_observer(const GameplayEventObserver *observer) {
    s_observer = observer;
}

void gameplay_event_trace_set_rollback_observer(
    const GameplayEventObserver *observer) {
    s_rollback_observer = observer;
}

static void hash_u32(uint32_t value) {
    unsigned byte;
    for (byte = 0; byte < 4u; byte++) {
        s_hash ^= (uint8_t)(value >> (byte * 8u));
        s_hash *= EVENT_HASH_PRIME;
    }
}

void gameplay_event_trace_emit(
    GameplayEventKind kind, int32_t a, int32_t b, int32_t c, int32_t d) {
    if (!gameplay_event_trace_enabled() ||
        kind <= 0 || kind >= GAMEPLAY_EVENT_KIND_COUNT) {
        return;
    }
    if (s_observer != NULL && s_observer->event != NULL) {
        s_observer->event(kind, a, b, c, d);
    }
    if (s_rollback_observer != NULL &&
        s_rollback_observer->event != NULL) {
        s_rollback_observer->event(kind, a, b, c, d);
    }
    if (!hash_stream_enabled()) {
        return; /* observed, not hashed: the hash stream was not asked for */
    }
    hash_u32((uint32_t)kind);
    hash_u32((uint32_t)a);
    hash_u32((uint32_t)b);
    hash_u32((uint32_t)c);
    hash_u32((uint32_t)d);
    s_tick_events++;
    s_tick_kind[kind]++;
}

void gameplay_event_trace_observe_context(int32_t mode, int32_t level) {
    if (!gameplay_event_trace_enabled()) {
        return;
    }
    if (!s_context_valid || mode != s_last_mode || level != s_last_level) {
        gameplay_event_trace_emit(
            GAMEPLAY_EVENT_CONTEXT, mode, level,
            s_context_valid ? s_last_mode : -1,
            s_context_valid ? s_last_level : -1);
        s_last_mode = mode;
        s_last_level = level;
        s_context_valid = 1;
    }
}

void gameplay_event_trace_tick(uint64_t tick) {
    unsigned kind;
    if (s_observer != NULL && s_observer->tick != NULL) {
        /* The authoritative tick boundary, which is the only clock an observer
         * can pace itself against without inventing one of its own. */
        s_observer->tick(tick);
    }
    if (s_rollback_observer != NULL &&
        s_rollback_observer->tick != NULL) {
        s_rollback_observer->tick(tick);
    }
    if (!hash_stream_enabled()) {
        return;
    }
    if (s_perturb_tick >= 0 && tick == (uint64_t)s_perturb_tick) {
        /* Trace-only phantom event: proves the ordered stream notices an event
         * that intentionally changes no gameplay state. */
        gameplay_event_trace_emit(
            GAMEPLAY_EVENT_CONTEXT, 0x5048414e, 0x544f4d, 1, 0);
    }
    /* Include the row identity and event count after the ordered payload. */
    hash_u32((uint32_t)tick);
    hash_u32((uint32_t)(tick >> 32));
    hash_u32(s_tick_events);
    fprintf(stderr,
            "[EVENTHASH] v=1 tick=%" PRIu64 " hash=%016" PRIx64
            " events=%u sound=%u music=%u transition=%u save=%u "
            "spawn=%u despawn=%u rumble=%u level=%u checkpoint=%u "
            "result=%u context=%u\n",
            tick, s_hash, s_tick_events,
            s_tick_kind[GAMEPLAY_EVENT_SOUND],
            s_tick_kind[GAMEPLAY_EVENT_MUSIC],
            s_tick_kind[GAMEPLAY_EVENT_TRANSITION],
            s_tick_kind[GAMEPLAY_EVENT_SAVE],
            s_tick_kind[GAMEPLAY_EVENT_SPAWN],
            s_tick_kind[GAMEPLAY_EVENT_DESPAWN],
            s_tick_kind[GAMEPLAY_EVENT_RUMBLE],
            s_tick_kind[GAMEPLAY_EVENT_LEVEL],
            s_tick_kind[GAMEPLAY_EVENT_CHECKPOINT],
            s_tick_kind[GAMEPLAY_EVENT_RACE_RESULT],
            s_tick_kind[GAMEPLAY_EVENT_CONTEXT]);
    fflush(stderr);
    s_ticks++;
    s_total_events += s_tick_events;
    for (kind = 1; kind < GAMEPLAY_EVENT_KIND_COUNT; kind++) {
        s_total_kind[kind] += s_tick_kind[kind];
    }
    s_hash = EVENT_HASH_OFFSET;
    s_tick_events = 0;
    memset(s_tick_kind, 0, sizeof(s_tick_kind));
}

void gameplay_event_trace_summary(void) {
    if (!hash_stream_enabled()) {
        return;
    }
    fprintf(stderr,
            "[EVENTHASH-SUMMARY] v=1 ticks=%" PRIu64 " events=%" PRIu64
            " sound=%" PRIu64 " music=%" PRIu64
            " transition=%" PRIu64 " save=%" PRIu64
            " spawn=%" PRIu64 " despawn=%" PRIu64
            " rumble=%" PRIu64 " level=%" PRIu64
            " checkpoint=%" PRIu64 " result=%" PRIu64
            " context=%" PRIu64 "\n",
            s_ticks, s_total_events,
            s_total_kind[GAMEPLAY_EVENT_SOUND],
            s_total_kind[GAMEPLAY_EVENT_MUSIC],
            s_total_kind[GAMEPLAY_EVENT_TRANSITION],
            s_total_kind[GAMEPLAY_EVENT_SAVE],
            s_total_kind[GAMEPLAY_EVENT_SPAWN],
            s_total_kind[GAMEPLAY_EVENT_DESPAWN],
            s_total_kind[GAMEPLAY_EVENT_RUMBLE],
            s_total_kind[GAMEPLAY_EVENT_LEVEL],
            s_total_kind[GAMEPLAY_EVENT_CHECKPOINT],
            s_total_kind[GAMEPLAY_EVENT_RACE_RESULT],
            s_total_kind[GAMEPLAY_EVENT_CONTEXT]);
    fflush(stderr);
}
