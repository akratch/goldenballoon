#include "match_input_runtime.h"

#include <string.h>

static MdkrMatchInputSource sSource;
static bool sActive;
static uint8_t sAiTakeoverMask;

bool mdkr_match_input_runtime_install(const MdkrMatchInputSource *source) {
    if (sActive || source == NULL ||
        source->version != MDKR_MATCH_INPUT_SOURCE_VERSION ||
        source->match_epoch == 0u || source->context == NULL ||
        source->drain == NULL || source->inputs_for_tick == NULL ||
        source->take_dirty == NULL || source->ai_mask_for_tick == NULL) {
        return false;
    }
    sSource = *source;
    sActive = true;
    return true;
}

void mdkr_match_input_runtime_clear(void) {
    memset(&sSource, 0, sizeof(sSource));
    sAiTakeoverMask = 0u;
    sActive = false;
}

bool mdkr_match_input_runtime_active(void) {
    return sActive;
}

uint32_t mdkr_match_input_runtime_epoch(void) {
    return sActive ? sSource.match_epoch : 0u;
}

bool mdkr_match_input_runtime_drain(
    uint32_t tick,
    const MdkrPadSample physical_inputs[MDKR_SESSION_MAX_PLAYERS],
    unsigned physical_input_count, MdkrInputSet *out) {
    if (!sActive || out == NULL || physical_inputs == NULL ||
        physical_input_count != MDKR_SESSION_MAX_PLAYERS) {
        return false;
    }
    return sSource.drain(
        sSource.context, sSource.match_epoch, tick, physical_inputs,
        physical_input_count, out);
}

bool mdkr_match_input_runtime_inputs_for_tick(
    uint32_t tick, MdkrInputSet *out) {
    return sActive && out != NULL && sSource.inputs_for_tick(
        sSource.context, sSource.match_epoch, tick, out);
}

bool mdkr_match_input_runtime_take_dirty(uint32_t *tick) {
    return sActive && tick != NULL && sSource.take_dirty(
        sSource.context, sSource.match_epoch, tick);
}

bool mdkr_match_input_runtime_begin_tick(uint32_t tick) {
    uint8_t mask = 0u;
    if (!sActive || !sSource.ai_mask_for_tick(
            sSource.context, sSource.match_epoch, tick, &mask) ||
        (mask & UINT8_C(0xf0)) != 0u) {
        sAiTakeoverMask = 0u;
        return false;
    }
    sAiTakeoverMask = mask;
    return true;
}

bool mdkr_match_input_runtime_slot_ai_controlled(unsigned slot) {
    return sActive && slot < MDKR_SESSION_MAX_PLAYERS &&
        (sAiTakeoverMask & (uint8_t)(1u << slot)) != 0u;
}
