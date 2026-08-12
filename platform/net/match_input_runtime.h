/* Engine-facing, launcher-owned canonical input provider.
 *
 * The game knows only this copy-out C ABI. Authentication, packet parsing,
 * matchmaking, local-seat ownership, and transport history remain launcher
 * responsibilities. The provider is installed before engine workers start and
 * cleared only after they join, so the hot path needs no ownership transfer. */
#ifndef MDKR_MATCH_INPUT_RUNTIME_H
#define MDKR_MATCH_INPUT_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "session/session_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_INPUT_SOURCE_VERSION 2u

typedef bool (*MdkrMatchInputDrainFn)(
    void *context, uint32_t match_epoch, uint32_t tick,
    const MdkrPadSample physical_inputs[MDKR_SESSION_MAX_PLAYERS],
    unsigned physical_input_count, MdkrInputSet *out);
typedef bool (*MdkrMatchInputForTickFn)(
    void *context, uint32_t match_epoch, uint32_t tick, MdkrInputSet *out);
typedef bool (*MdkrMatchInputTakeDirtyFn)(
    void *context, uint32_t match_epoch, uint32_t *tick);
typedef bool (*MdkrMatchInputAiMaskFn)(
    void *context, uint32_t match_epoch, uint32_t tick, uint8_t *slot_mask);

typedef struct MdkrMatchInputSource {
    uint32_t version;
    uint32_t match_epoch;
    void *context;
    MdkrMatchInputDrainFn drain;
    MdkrMatchInputForTickFn inputs_for_tick;
    MdkrMatchInputTakeDirtyFn take_dirty;
    MdkrMatchInputAiMaskFn ai_mask_for_tick;
} MdkrMatchInputSource;

/* Lifecycle calls are launcher-thread only and bracket the complete blocking
 * engine invocation. Data callbacks may run on the engine simulation thread;
 * providers must synchronize any concurrent carrier ingress themselves. */
bool mdkr_match_input_runtime_install(const MdkrMatchInputSource *source);
void mdkr_match_input_runtime_clear(void);
bool mdkr_match_input_runtime_active(void);
uint32_t mdkr_match_input_runtime_epoch(void);

bool mdkr_match_input_runtime_drain(
    uint32_t tick,
    const MdkrPadSample physical_inputs[MDKR_SESSION_MAX_PLAYERS],
    unsigned physical_input_count, MdkrInputSet *out);
bool mdkr_match_input_runtime_inputs_for_tick(
    uint32_t tick, MdkrInputSet *out);
bool mdkr_match_input_runtime_take_dirty(uint32_t *tick);
/* Called at every ordinary and resimulated authored boundary before racer
 * dispatch. The current mask is derived from the immutable launcher control
 * schedule; it is not restored host state. */
bool mdkr_match_input_runtime_begin_tick(uint32_t tick);
bool mdkr_match_input_runtime_slot_ai_controlled(unsigned slot);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_MATCH_INPUT_RUNTIME_H */
