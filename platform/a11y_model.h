/* a11y_model.h — what the app would say, independent of any voice.
 *
 * ImGui exposes no accessibility tree, so this layer IS the accessibility tree:
 * the app tells it what is focused and what happened, and it produces an ordered
 * text stream. Keeping it free of any speech API is what lets the whole
 * behaviour be gated on text, in CI, with no audio device.
 *
 * The traps it exists to avoid, in the order they bite. A player who tabs
 * through twenty settings must hear the twentieth, so an announcement supersedes
 * the pending one in its own category and the fixed ring drops its oldest entry
 * rather than growing. A race-critical line must not be chopped off by the next
 * focus change, so priority is carried per utterance and CRITICAL is never
 * superseded. And text that will not fit is refused whole: a voice reading a
 * sentence truncated mid-word says something the player cannot act on, and
 * silent truncation is this port's most-repeated defect. Nothing here allocates,
 * blocks, or knows what a speech engine is.
 *
 * Ownership: single-threaded, like the rest of the port (see
 * docs/ARCHITECTURE_DECISIONS.md). Nothing here is synchronised.
 *
 * "One pushes, one pops" is NOT a licence to put the pusher and the popper on
 * different threads. This is not a single-producer/single-consumer ring: the
 * push writes the head index too, in its drop-oldest arm, the same index the
 * pop advances (`s_head`, platform/a11y_model.c). Split them across threads and
 * two writers race the same variable. A speech backend that wants to talk on a
 * worker must therefore drain on the thread that pushed and hand the text over
 * afterwards -- which is what platform/a11y_speech_worker.c does, and why
 * mdkr_a11y_speech_service_pump() is called from the frame loop rather than
 * from the worker. A plan for this file once specified the opposite; it was
 * wrong, and this paragraph exists so it is not specified again.
 */
#ifndef MDKR64_A11Y_MODEL_H
#define MDKR64_A11Y_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_A11Y_TEXT_MAX 512
/* Utterances held before the oldest is dropped. Bounded on purpose: a slow
 * voice engine must fall behind by losing stale lines, never by growing. */
#define MDKR_A11Y_QUEUE_MAX 64

typedef enum MdkrA11yCategory {
    MDKR_A11Y_CAT_FOCUS = 0,   /* the control under the cursor */
    MDKR_A11Y_CAT_SECTION,     /* moving between panels */
    MDKR_A11Y_CAT_STATUS,      /* settings applied, errors */
    MDKR_A11Y_CAT_RACE_POSITION,
    MDKR_A11Y_CAT_RACE_LAP,
    MDKR_A11Y_CAT_RACE_EVENT,
    MDKR_A11Y_CAT_COUNT
} MdkrA11yCategory;

typedef enum MdkrA11yPriority {
    MDKR_A11Y_PRI_LOW = 0,     /* superseded by anything newer in its category */
    MDKR_A11Y_PRI_NORMAL,
    MDKR_A11Y_PRI_CRITICAL     /* never cancelled once in flight */
} MdkrA11yPriority;

void mdkr_a11y_init(void);
void mdkr_a11y_announce(MdkrA11yCategory category, MdkrA11yPriority priority,
                        const char *text);
/* Convenience for the commonest case: name, value, help -> one focus utterance. */
void mdkr_a11y_focus(const char *name, const char *value, const char *help);

/* Pops the next utterance. Returns 1 and fills `out`, or 0 when empty.
 * `out_size` must be at least MDKR_A11Y_TEXT_MAX; a buffer too small for the
 * queued text yields 0 and leaves the utterance queued, because a half-read
 * sentence is worse than a late one. `out_category` may be NULL. */
int  mdkr_a11y_next(char *out, size_t out_size, MdkrA11yCategory *out_category);

/* The popped utterance is "in flight" until the consumer says it finished — the
 * speech backend in Task 3 pops, speaks, and calls this when the engine reports
 * completion. Popping again also ends the previous one: there is one voice. */
void mdkr_a11y_finish(void);
/* True when the in-flight utterance has been superseded and the consumer should
 * stop speaking it: any newer accepted announcement barges in, whatever its
 * category, because the player has moved on. A CRITICAL utterance never reports
 * true — that is what CRITICAL is for. */
bool mdkr_a11y_in_flight_cancelled(void);

void mdkr_a11y_set_category_enabled(MdkrA11yCategory category, bool enabled);
bool mdkr_a11y_category_enabled(MdkrA11yCategory category);
/* Emits `[SPEAK] cat=... pri=... text=...` when MDKR_A11Y_TRACE=1. */
void mdkr_a11y_set_trace(bool on);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_A11Y_MODEL_H */
