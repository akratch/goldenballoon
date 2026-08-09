/* Pointer-free, ordered gameplay-event fidelity trace. */
#ifndef MDKR_GAMEPLAY_EVENT_TRACE_H
#define MDKR_GAMEPLAY_EVENT_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GameplayEventKind {
    GAMEPLAY_EVENT_SOUND = 1,
    GAMEPLAY_EVENT_MUSIC,
    GAMEPLAY_EVENT_TRANSITION,
    GAMEPLAY_EVENT_SAVE,
    GAMEPLAY_EVENT_SPAWN,
    GAMEPLAY_EVENT_DESPAWN,
    GAMEPLAY_EVENT_RUMBLE,
    GAMEPLAY_EVENT_LEVEL,
    GAMEPLAY_EVENT_CHECKPOINT,
    GAMEPLAY_EVENT_RACE_RESULT,
    GAMEPLAY_EVENT_CONTEXT,
    GAMEPLAY_EVENT_KIND_COUNT
} GameplayEventKind;

bool gameplay_event_trace_enabled(void);

/*
 * A second reader of the same ordered stream.
 *
 * The hash stream answers "did these two runs do the same things"; an observer
 * answers "what just happened", which is what a presentation layer such as the
 * race announcer needs. Both read the events the port already publishes, so a
 * feature that reacts to race events costs no new observation point inside the
 * race code -- which is the whole reason this hook exists rather than a second
 * set of calls scattered through game/src.
 *
 * Installing an observer arms the emission sites (so the events actually reach
 * it) but changes nothing about the hash: `[EVENTHASH]` rows are still printed
 * only when MDKR_EVENT_HASH is set, and their bytes are unaffected by whether
 * anybody is observing. At most one observer; NULL removes it. The pointer must
 * outlive the process -- pass a file-scope constant, not a stack value.
 */
typedef struct GameplayEventObserver {
    void (*event)(GameplayEventKind kind, int32_t a, int32_t b, int32_t c,
                  int32_t d);
    void (*tick)(uint64_t tick);
} GameplayEventObserver;

void gameplay_event_trace_set_observer(const GameplayEventObserver *observer);

/* Arguments are stable scalar identities only; never pass host pointers. */
void gameplay_event_trace_emit(
    GameplayEventKind kind, int32_t a, int32_t b, int32_t c, int32_t d);

/* Avoid evaluating diagnostic arguments in an ordinary shipping run. */
#define GAMEPLAY_EVENT_TRACE(kind, a, b, c, d)                         \
    do {                                                                \
        if (gameplay_event_trace_enabled()) {                           \
            gameplay_event_trace_emit((kind), (a), (b), (c), (d));     \
        }                                                               \
    } while (0)

/* Emit context changes without making every unchanged tick an event. */
void gameplay_event_trace_observe_context(int32_t mode, int32_t level);

/* Publish and reset the ordered row for one completed authoritative tick. */
void gameplay_event_trace_tick(uint64_t tick);
void gameplay_event_trace_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_GAMEPLAY_EVENT_TRACE_H */
