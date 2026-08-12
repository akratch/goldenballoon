#include "rollback_game_runtime.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_env.h"
#include "PR/os_cont.h"
#include "asset_enums.h"
#include "common.h"
#include "fade_transition.h"
#include "game.h"
#include "gameplay_event_trace.h"
#include "joypad.h"
#include "memory.h"
#include "net/net_input.h"
#include "net/match_input_runtime.h"
#include "net/net_roster_runtime.h"
#include "objects.h"
#include "platform_os.h"
#include "racer.h"
#include "rollback_game_authority.h"
#include "rollback_limits.h"
#include "rollback_authority_view.h"
#include "rollback_audio.h"
#include "rollback_engine_registry.h"
#include "rollback_events.h"
#include "rollback_ring.h"
#include "thread3_main.h"

#define MDKR_ROLLBACK_LAB_SLOTS MDKR_ROLLBACK_SNAPSHOT_SLOTS
#define MDKR_ROLLBACK_LAB_MANIFEST UINT64_C(0x52424c4142563031)
#define MDKR_ROLLBACK_LAB_RESIM_DEPTH 4u
#ifndef MDKR_ROLLBACK_LAB_RESIM_TARGET_TICK
#define MDKR_ROLLBACK_LAB_RESIM_TARGET_TICK 120u
#endif
#if MDKR_ROLLBACK_LAB_RESIM_TARGET_TICK < MDKR_ROLLBACK_LAB_RESIM_DEPTH
#error "rollback replay target must retain the complete replay window"
#endif

typedef struct MdkrRollbackInputHistory {
    MdkrInputSample input[MDKR_INPUT_PORTS];
    MdkrInputSample received_input[MDKR_INPUT_PORTS];
    uint32_t tick;
    unsigned update_rate;
    uint8_t confirmed_mask;
    bool valid;
} MdkrRollbackInputHistory;

typedef struct MdkrRollbackGameRuntime {
    MdkrRollbackSnapshotRegistry registry;
    MdkrRollbackRing ring;
    MdkrNetInputHistory net_inputs;
    MdkrRollbackEventJournal events;
    MdkrRollbackAudioAdapter audio;
    MdkrRollbackTimingHistogram resimulation_timing;
    MdkrRollbackTimingHistogram authored_frame_timing;
    uint64_t authored_frame_started_ns;
    uint64_t validated_boundaries;
    uint32_t mutation_log_count;
    uint64_t observed_event_kind[GAMEPLAY_EVENT_KIND_COUNT];
    uint64_t observed_item_spawns;
    uint64_t observed_item_despawns;
    uint64_t item_probe_spawn_baseline;
    uint64_t item_probe_rumble_baseline;
    MdkrRollbackInputHistory input_history[MDKR_ROLLBACK_LAB_SLOTS];
    uint8_t *baseline_snapshot;
    uint8_t *replayed_snapshot;
    MdkrInputSample last_applied_input[MDKR_INPUT_PORTS];
    uint32_t replay_target_tick;
    uint32_t range_hash_tick;
    uint32_t prepared_tick;
    uint32_t effect_tick;
    uint16_t effect_ordinals[GAMEPLAY_EVENT_KIND_COUNT];
    MdkrRollbackEventId pending_sound_id;
    int item_probe_weapon;
    uint8_t item_probe_balloon;
    uint8_t item_probe_level;
    uint32_t item_probe_release_tick;
    bool roundtrip_once;
    bool roundtrip_complete;
    bool resim_once;
    bool resim_complete;
    bool mutation_control;
    bool delayed_input_control;
    bool item_probe_enabled;
    bool item_probe_effect_observed;
    bool item_probe_mutation_control;
    bool network_input;
    bool tick_prepared;
    bool side_effect_error;
    bool pending_sound;
    bool authored_frame_timing_active;
    bool active;
} MdkrRollbackGameRuntime;

static MdkrRollbackGameRuntime sRollbackGameRuntime;

static Object_Racer *item_probe_racer(void) {
    Object *object = get_racer_object_by_port(PLAYER_ONE);
    return object != NULL ? object->racer : NULL;
}

static bool arm_item_probe(
    MdkrRollbackGameRuntime *runtime, uint32_t balloon, uint32_t level,
    uint32_t release_tick, bool mutation_control) {
    Object_Racer *racer;
    const s8 *balloon_data;
    int weapon;
    if (runtime == NULL || balloon >= 5u || level >= 3u ||
        release_tick <= 1u) {
        return false;
    }
    racer = item_probe_racer();
    balloon_data = (const s8 *)get_misc_asset(ASSET_MISC_BALLOON_DATA);
    if (racer == NULL || balloon_data == NULL) {
        return false;
    }
    weapon = balloon_data[(balloon * 10u) + (level * 2u)];
    if (weapon < 0 || weapon >= NUM_WEAPON_TYPES) {
        return false;
    }
    /* Test the real inventory/use path, not a synthetic weapon spawn. One
     * charge makes consumption an exact postcondition even for ROM entries
     * that ordinarily award a multi-use quantity. This mutation happens
     * before registry freeze and tick-zero capture, so every replay starts
     * from the same ordinary Object_Racer authority bytes. */
    racer->balloon_type = (s8)balloon;
    racer->balloon_level = (s8)level;
    racer->balloon_quantity = 1;
    runtime->item_probe_weapon = weapon;
    runtime->item_probe_balloon = (uint8_t)balloon;
    runtime->item_probe_level = (uint8_t)level;
    runtime->item_probe_release_tick = release_tick;
    runtime->item_probe_mutation_control = mutation_control;
    runtime->item_probe_enabled = true;
    fprintf(stderr,
            "[ROLLBACK] item probe armed: balloon=%u level=%u weapon=%d "
            "quantity=1 release=%u mutation=%u\n",
            (unsigned)balloon, (unsigned)level, weapon,
            (unsigned)release_tick, mutation_control ? 1u : 0u);
    return true;
}

static bool validate_item_probe(MdkrRollbackGameRuntime *runtime) {
    Object_Racer *racer;
    uint64_t spawn_delta;
    uint64_t rumble_delta;
    bool effect = false;
    if (runtime == NULL || !runtime->item_probe_enabled) return true;
    racer = item_probe_racer();
    if (racer == NULL ||
        runtime->observed_item_spawns < runtime->item_probe_spawn_baseline ||
        runtime->observed_event_kind[GAMEPLAY_EVENT_RUMBLE] <
            runtime->item_probe_rumble_baseline) {
        return false;
    }
    spawn_delta = runtime->observed_item_spawns -
        runtime->item_probe_spawn_baseline;
    rumble_delta = runtime->observed_event_kind[GAMEPLAY_EVENT_RUMBLE] -
        runtime->item_probe_rumble_baseline;
    if (racer->balloon_quantity == 0) {
        switch (runtime->item_probe_weapon) {
            case WEAPON_NITRO_LEVEL_1:
            case WEAPON_NITRO_LEVEL_2:
            case WEAPON_NITRO_LEVEL_3:
                effect = racer->boostTimer > 0 && rumble_delta > 0u;
                break;
            case WEAPON_MAGNET_LEVEL_1:
            case WEAPON_MAGNET_LEVEL_2:
            case WEAPON_MAGNET_LEVEL_3:
                /* A target is deliberately not required: acquiring none is a
                 * valid deterministic result, but charge custody and the
                 * local feedback event must still occur exactly through the
                 * real item branch. */
                effect = rumble_delta > 0u;
                break;
            case WEAPON_SHIELD_LEVEL_1:
                effect = racer->shieldTimer > 0 &&
                    racer->shieldType == SHIELD_LEVEL1;
                break;
            case WEAPON_SHIELD_LEVEL_2:
                effect = racer->shieldTimer > 0 &&
                    racer->shieldType == SHIELD_LEVEL2;
                break;
            case WEAPON_SHIELD_LEVEL_3:
                effect = racer->shieldTimer > 0 &&
                    racer->shieldType == SHIELD_LEVEL3;
                break;
            default:
                effect = spawn_delta > 0u;
                break;
        }
    }
    runtime->item_probe_effect_observed = effect;
    fprintf(stderr,
            "[ROLLBACK] item probe result: balloon=%u level=%u weapon=%d "
            "quantity=%d spawns=%" PRIu64 " rumble=%" PRIu64
            " boost=%d shield=%d shieldType=%d observed=%u\n",
            (unsigned)runtime->item_probe_balloon,
            (unsigned)runtime->item_probe_level,
            runtime->item_probe_weapon, (int)racer->balloon_quantity,
            spawn_delta, rumble_delta, (int)racer->boostTimer,
            (int)racer->shieldTimer, (int)racer->shieldType,
            effect ? 1u : 0u);
    return effect;
}

static uint32_t effect_value_hash(
    int32_t a, int32_t b, int32_t c, int32_t d) {
    const uint32_t values[4] = {
        (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d};
    uint32_t hash = UINT32_C(2166136261);
    unsigned value;
    unsigned byte;
    for (value = 0u; value < 4u; value++) {
        for (byte = 0u; byte < 4u; byte++) {
            hash ^= (uint8_t)(values[value] >> (byte * 8u));
            hash *= UINT32_C(16777619);
        }
    }
    return hash;
}

static uint64_t diagnostic_range_hash(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    for (index = 0u; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void log_authority_range_hashes(
    const MdkrRollbackGameRuntime *runtime, uint32_t tick) {
    unsigned range;
    if (runtime->range_hash_tick == 0u || tick != runtime->range_hash_tick) {
        return;
    }
    for (range = 0u; range < runtime->registry.range_count; range++) {
        const MdkrRollbackRange *entry = &runtime->registry.ranges[range];
        fprintf(stderr,
                "[ROLLBACK-RANGE] tick=%u range=%u tag=%08x bytes=%zu "
                "hash=%016" PRIx64 "\n",
                tick, range, entry->tag, entry->size,
                diagnostic_range_hash(entry->address, entry->size));
    }
}

static bool bridge_input_samples(
    const MdkrInputSet *frame, MdkrInputSample out[MDKR_INPUT_PORTS]) {
    unsigned slot;
    if (frame == NULL || out == NULL ||
        (frame->present_mask & 0xf0u) != 0u ||
        (frame->confirmed_mask & 0xf0u) != 0u ||
        (frame->confirmed_mask & (uint8_t)~frame->present_mask) != 0u) {
        return false;
    }
    for (slot = 0u; slot < MDKR_INPUT_PORTS; slot++) {
        const MdkrPadSample *input = &frame->slots[slot];
        const uint8_t bit = (uint8_t)(1u << slot);
        if (input->present > 1u || input->stick_x < -80 || input->stick_x > 80 ||
            input->stick_y < -80 || input->stick_y > 80 ||
            ((frame->present_mask & bit) != 0u) != (input->present != 0u)) {
            return false;
        }
        out[slot].buttons = input->buttons;
        out[slot].stick_x = input->stick_x;
        out[slot].stick_y = input->stick_y;
        out[slot].present = input->present != 0u;
    }
    return true;
}

static void pad_input_samples(
    const MdkrInputSample input[MDKR_INPUT_PORTS],
    MdkrPadSample out[MDKR_INPUT_PORTS]) {
    unsigned slot;
    for (slot = 0u; slot < MDKR_INPUT_PORTS; slot++) {
        out[slot].buttons = input[slot].buttons;
        out[slot].stick_x = input[slot].stick_x;
        out[slot].stick_y = input[slot].stick_y;
        out[slot].present = input[slot].present ? 1u : 0u;
    }
}

static uint32_t effect_adapter_value(
    GameplayEventKind kind, int32_t a, int32_t b, int32_t c, int32_t d) {
    (void)c;
    if (kind == GAMEPLAY_EVENT_RUMBLE) {
        return ((uint32_t)a & UINT32_C(0xff)) |
            (((uint32_t)b & UINT32_C(0xff)) << 8u) |
            ((d != 0 ? UINT32_C(1) : UINT32_C(0)) << 16u);
    }
    return effect_value_hash(a, b, c, d);
}

static void preview_rollback_effect(
    const MdkrRollbackEvent *event, void *context) {
    MdkrRollbackGameRuntime *runtime = (MdkrRollbackGameRuntime *)context;
    if (event != NULL && runtime != NULL &&
        event->id.kind == GAMEPLAY_EVENT_SOUND) {
        /* Ordinary emission previews before audio.c has returned from the
         * trace call and bound its host request. The request starts that case
         * synchronously. A corrected event is already bound when end_rewrite
         * reaches this callback, so preview() starts it here. */
        (void)mdkr_rollback_audio_preview(&runtime->audio, event->id);
    }
}

static void cancel_rollback_effect(
    const MdkrRollbackEvent *event, void *context) {
    MdkrRollbackGameRuntime *runtime = (MdkrRollbackGameRuntime *)context;
    if (event == NULL) return;
    if (event->id.kind == GAMEPLAY_EVENT_SOUND && runtime != NULL &&
        !mdkr_rollback_audio_cancel(&runtime->audio, event->id)) {
        runtime->side_effect_error = true;
    } else if (event->id.kind == GAMEPLAY_EVENT_RUMBLE) {
        mdkr_rollback_rumble_cancel_preview(event->value & UINT32_C(0xff));
    }
}

static void begin_effect_tick(
    MdkrRollbackGameRuntime *runtime, uint32_t tick) {
    if (runtime->effect_tick == tick) return;
    runtime->effect_tick = tick;
    memset(runtime->effect_ordinals, 0, sizeof(runtime->effect_ordinals));
}

static void observe_rollback_effect(
    GameplayEventKind kind, int32_t a, int32_t b, int32_t c, int32_t d) {
    MdkrRollbackGameRuntime *runtime = &sRollbackGameRuntime;
    MdkrRollbackEventId id;
    MdkrRollbackEffectPolicy policy;
    uint16_t *ordinal;
    uint32_t inferred_tick;
    if (!runtime->active || kind <= 0 || kind >= GAMEPLAY_EVENT_KIND_COUNT) {
        return;
    }
    runtime->observed_event_kind[kind]++;
    if ((kind == GAMEPLAY_EVENT_SPAWN || kind == GAMEPLAY_EVENT_DESPAWN) &&
        (b == BHV_WEAPON || b == BHV_WEAPON_2 ||
         b == BHV_WEAPON_BALLOON || b == BHV_BALLOON_POP)) {
        if (kind == GAMEPLAY_EVENT_SPAWN) {
            runtime->observed_item_spawns++;
        } else {
            runtime->observed_item_despawns++;
        }
    }
    if (
        (kind != GAMEPLAY_EVENT_SOUND && kind != GAMEPLAY_EVENT_RUMBLE &&
         kind != GAMEPLAY_EVENT_SAVE)) {
        return;
    }
    inferred_tick = runtime->validated_boundaries < UINT32_MAX
        ? (uint32_t)(runtime->validated_boundaries + 1u)
        : UINT32_MAX;
    if (runtime->effect_tick == 0u) {
        begin_effect_tick(runtime, inferred_tick);
    }
    ordinal = &runtime->effect_ordinals[kind];
    if (*ordinal == UINT16_MAX) {
        runtime->side_effect_error = true;
        return;
    }
    id.tick = runtime->effect_tick;
    /* Payload participates in identity: a corrected rumble strength or sound
     * is a vanished old effect plus a new effect, not an illegal mutation of
     * one event. Ordinal still distinguishes identical repeats in one tick. */
    id.emitter = effect_value_hash(a, b, c, d);
    id.ordinal = (*ordinal)++;
    id.kind = (uint16_t)kind;
    policy = kind == GAMEPLAY_EVENT_SAVE
        ? MDKR_ROLLBACK_EFFECT_CONFIRMED_ONLY
        : MDKR_ROLLBACK_EFFECT_REVERSIBLE;
    if (!mdkr_rollback_events_emit(
            &runtime->events, id, policy,
            effect_adapter_value(kind, a, b, c, d))) {
        runtime->side_effect_error = true;
    } else if (kind == GAMEPLAY_EVENT_SOUND) {
        runtime->pending_sound_id = id;
        runtime->pending_sound = true;
    }
}

static const GameplayEventObserver kRollbackEffectObserver = {
    observe_rollback_effect, NULL};

bool mdkr_rollback_game_runtime_active(void) {
    return sRollbackGameRuntime.active;
}

bool mdkr_rollback_game_runtime_requested(void) {
    return sRollbackGameRuntime.active ||
        port_env_bool(
            "MDKR_ROLLBACK_LAB", 0,
            "freeze the real-game rollback registry and capture level tick zero") ||
        mdkr_match_input_runtime_active();
}

bool mdkr_rollback_game_runtime_host_io_allowed(bool progression_write) {
    if (!sRollbackGameRuntime.active) {
        return true;
    }
    return mdkr_rollback_events_host_io_allowed(
        &sRollbackGameRuntime.events, progression_write);
}

bool mdkr_rollback_game_runtime_presentation_allowed(void) {
    return !sRollbackGameRuntime.active ||
        !sRollbackGameRuntime.events.resimulating;
}

bool mdkr_rollback_game_runtime_defer_audio_command(
    const MdkrRollbackAudioCommand *command) {
    MdkrRollbackGameRuntime *runtime = &sRollbackGameRuntime;
    if (!runtime->active || !runtime->events.resimulating) return false;
    if (!mdkr_rollback_audio_defer_command(&runtime->audio, command)) {
        runtime->side_effect_error = true;
    }
    return true;
}

bool mdkr_rollback_game_runtime_sound_request(
    const MdkrRollbackAudioRequest *request) {
    MdkrRollbackAudioRequest bound;
    MdkrRollbackGameRuntime *runtime = &sRollbackGameRuntime;
    if (!runtime->active) return false;
    if (request == NULL || !runtime->pending_sound) {
        runtime->side_effect_error = true;
        return true;
    }
    bound = *request;
    bound.id = runtime->pending_sound_id;
    runtime->pending_sound = false;
    if (!mdkr_rollback_audio_request(
            &runtime->audio, &bound, runtime->events.resimulating)) {
        runtime->side_effect_error = true;
    }
    return true;
}

static void force_clear_effects(MdkrRollbackGameRuntime *runtime) {
    mdkr_rollback_events_force_clear(&runtime->events);
    mdkr_rollback_audio_forget_all(&runtime->audio);
    runtime->pending_sound = false;
}

static void begin_effect_rewrite(
    MdkrRollbackGameRuntime *runtime, uint32_t first_tick) {
    mdkr_rollback_audio_discard_commands(&runtime->audio);
    mdkr_rollback_events_begin_rewrite(&runtime->events, first_tick);
}

static void end_effect_rewrite(MdkrRollbackGameRuntime *runtime) {
    mdkr_rollback_events_end_rewrite(&runtime->events);
    mdkr_rollback_audio_flush_commands(&runtime->audio);
}

static void confirm_effects_through(
    MdkrRollbackGameRuntime *runtime, uint32_t tick) {
    mdkr_rollback_events_confirm_through(&runtime->events, tick);
    mdkr_rollback_audio_confirm_through(&runtime->audio, tick);
}

static uint64_t rollback_clock_now(void *context) {
    (void)context;
    return platform_perf_monotonic_ns();
}

static bool resimulate_timed(
    MdkrRollbackGameRuntime *runtime, int update_rate,
    const MdkrInputSample input[MDKR_INPUT_PORTS]) {
    const uint64_t started = rollback_clock_now(NULL);
    const bool result = mdkr_game_resimulate_tick(update_rate, input);
    const uint64_t finished = rollback_clock_now(NULL);
    mdkr_rollback_timing_record(
        &runtime->resimulation_timing,
        finished >= started ? finished - started : 0u);
    return result;
}

static void log_pool_mutation(
    MemoryPools pool_index, MdkrMempoolMutationKind kind, u64 generation,
    const void *address, size_t size, u32 colour_tag, const void *origin,
    void *context) {
    MdkrRollbackGameRuntime *runtime =
        (MdkrRollbackGameRuntime *)context;
    if (runtime == NULL || pool_index != POOL_MAIN ||
        runtime->mutation_log_count >= 64u) {
        return;
    }
    runtime->mutation_log_count++;
    fprintf(stderr,
            "[ROLLBACK-MEM] generation=%" PRIu64 " operation=%s "
            "address=%p size=%zu tag=%08x origin=%p origin_delta=%+" PRIdPTR "\n",
            (uint64_t)generation,
            kind == MDKR_MEMPOOL_MUTATION_FREE ? "free" : "assign",
            address, size, (unsigned)colour_tag, origin,
            origin != NULL
                ? (intptr_t)(uintptr_t)origin -
                      (intptr_t)(uintptr_t)&log_pool_mutation
                : (intptr_t)0);
}

void mdkr_rollback_game_runtime_level_end(void) {
    mdkr_mempool_set_mutation_observer(NULL, NULL);
    gameplay_event_trace_set_rollback_observer(NULL);
    if (sRollbackGameRuntime.active) {
        const MdkrRollbackRingStats *stats =
            &sRollbackGameRuntime.ring.stats;
        fprintf(stderr,
                "[ROLLBACK] lab stats: ticks=%" PRIu64
                " captures=%" PRIu64 " restores=%" PRIu64
                " capture_avg_ns=%" PRIu64 " capture_p50_ns=%" PRIu64
                " capture_p95_ns=%" PRIu64 " capture_p99_ns=%" PRIu64
                " capture_max_ns=%" PRIu64
                " restore_avg_ns=%" PRIu64 " restore_p50_ns=%" PRIu64
                " restore_p95_ns=%" PRIu64 " restore_p99_ns=%" PRIu64
                " restore_max_ns=%" PRIu64
                " timing_overflow=%" PRIu64 "/%" PRIu64
                " over_8333333ns=%" PRIu64 "/%" PRIu64
                " over_16666667ns=%" PRIu64 "/%" PRIu64 "\n",
                sRollbackGameRuntime.validated_boundaries,
                stats->captures, stats->restores,
                stats->captures != 0u
                    ? stats->capture_ns_total / stats->captures
                    : 0u,
                mdkr_rollback_ring_percentile_ns(stats, true, 50u),
                mdkr_rollback_ring_percentile_ns(stats, true, 95u),
                mdkr_rollback_ring_percentile_ns(stats, true, 99u),
                stats->capture_ns_max,
                stats->restores != 0u
                    ? stats->restore_ns_total / stats->restores
                    : 0u,
                mdkr_rollback_ring_percentile_ns(stats, false, 50u),
                mdkr_rollback_ring_percentile_ns(stats, false, 95u),
                mdkr_rollback_ring_percentile_ns(stats, false, 99u),
                stats->restore_ns_max,
                stats->capture_timing_overflow,
                stats->restore_timing_overflow,
                stats->capture_over_p99_budget,
                stats->restore_over_p99_budget,
                stats->capture_over_tail_budget,
                stats->restore_over_tail_budget);
        fprintf(stderr,
                "[ROLLBACK] resimulation stats: samples=%" PRIu64
                " avg_ns=%" PRIu64 " p50_ns=%" PRIu64
                " p95_ns=%" PRIu64 " p99_ns=%" PRIu64
                " max_ns=%" PRIu64 " timing_overflow=%" PRIu64
                " over_8333333ns=%" PRIu64
                " over_16666667ns=%" PRIu64 "\n",
                sRollbackGameRuntime.resimulation_timing.samples,
                sRollbackGameRuntime.resimulation_timing.samples != 0u
                    ? sRollbackGameRuntime.resimulation_timing.ns_total /
                          sRollbackGameRuntime.resimulation_timing.samples
                    : 0u,
                mdkr_rollback_timing_percentile_ns(
                    &sRollbackGameRuntime.resimulation_timing, 50u),
                mdkr_rollback_timing_percentile_ns(
                    &sRollbackGameRuntime.resimulation_timing, 95u),
                mdkr_rollback_timing_percentile_ns(
                    &sRollbackGameRuntime.resimulation_timing, 99u),
                sRollbackGameRuntime.resimulation_timing.ns_max,
                sRollbackGameRuntime.resimulation_timing.overflow,
                sRollbackGameRuntime.resimulation_timing.over_p99_budget,
                sRollbackGameRuntime.resimulation_timing.over_tail_budget);
        fprintf(stderr,
                "[ROLLBACK] authored-frame stats: samples=%" PRIu64
                " avg_ns=%" PRIu64 " p50_ns=%" PRIu64
                " p95_ns=%" PRIu64 " p99_ns=%" PRIu64
                " max_ns=%" PRIu64 " timing_overflow=%" PRIu64
                " over_8333333ns=%" PRIu64
                " over_16666667ns=%" PRIu64 "\n",
                sRollbackGameRuntime.authored_frame_timing.samples,
                sRollbackGameRuntime.authored_frame_timing.samples != 0u
                    ? sRollbackGameRuntime.authored_frame_timing.ns_total /
                          sRollbackGameRuntime.authored_frame_timing.samples
                    : 0u,
                mdkr_rollback_timing_percentile_ns(
                    &sRollbackGameRuntime.authored_frame_timing, 50u),
                mdkr_rollback_timing_percentile_ns(
                    &sRollbackGameRuntime.authored_frame_timing, 95u),
                mdkr_rollback_timing_percentile_ns(
                    &sRollbackGameRuntime.authored_frame_timing, 99u),
                sRollbackGameRuntime.authored_frame_timing.ns_max,
                sRollbackGameRuntime.authored_frame_timing.overflow,
                sRollbackGameRuntime.authored_frame_timing.over_p99_budget,
                sRollbackGameRuntime.authored_frame_timing.over_tail_budget);
        fprintf(stderr,
                "[ROLLBACK] effects: tracked=%u emitted=%" PRIu64
                " duplicates=%" PRIu64 " committed=%" PRIu64
                " cancelled=%" PRIu64 " overflows=%" PRIu64
                " forbidden_io=%" PRIu64 "\n",
                sRollbackGameRuntime.events.count,
                sRollbackGameRuntime.events.stats.emitted,
                sRollbackGameRuntime.events.stats.duplicates,
                sRollbackGameRuntime.events.stats.committed,
                sRollbackGameRuntime.events.stats.cancelled,
                sRollbackGameRuntime.events.stats.overflows,
                sRollbackGameRuntime.events.stats.forbidden_io);
        fprintf(stderr,
                "[ROLLBACK] audio: tracked=%u started=%" PRIu64
                " suppressed=%" PRIu64 " deferred=%" PRIu64
                " cancelled=%" PRIu64 " confirmed=%" PRIu64
                " rejected=%" PRIu64 " commands=%" PRIu64
                " coalesced=%" PRIu64 " applied=%" PRIu64
                " command_overflows=%" PRIu64 "\n",
                sRollbackGameRuntime.audio.count,
                sRollbackGameRuntime.audio.stats.started,
                sRollbackGameRuntime.audio.stats.suppressed,
                sRollbackGameRuntime.audio.stats.deferred,
                sRollbackGameRuntime.audio.stats.cancelled,
                sRollbackGameRuntime.audio.stats.confirmed,
                sRollbackGameRuntime.audio.stats.rejected,
                sRollbackGameRuntime.audio.stats.commands_deferred,
                sRollbackGameRuntime.audio.stats.commands_coalesced,
                sRollbackGameRuntime.audio.stats.commands_applied,
                sRollbackGameRuntime.audio.stats.command_overflows);
        fprintf(stderr,
                "[ROLLBACK] gameplay breadth: spawn=%" PRIu64
                " despawn=%" PRIu64 " item_spawn=%" PRIu64
                " item_despawn=%" PRIu64 " checkpoint=%" PRIu64
                " result=%" PRIu64 "\n",
                sRollbackGameRuntime.observed_event_kind[GAMEPLAY_EVENT_SPAWN],
                sRollbackGameRuntime.observed_event_kind[GAMEPLAY_EVENT_DESPAWN],
                sRollbackGameRuntime.observed_item_spawns,
                sRollbackGameRuntime.observed_item_despawns,
                sRollbackGameRuntime.observed_event_kind[GAMEPLAY_EVENT_CHECKPOINT],
                sRollbackGameRuntime.observed_event_kind[GAMEPLAY_EVENT_RACE_RESULT]);
    }
    mdkr_rollback_audio_forget_all(&sRollbackGameRuntime.audio);
    if (sRollbackGameRuntime.active || sRollbackGameRuntime.ring.storage != NULL) {
        mdkr_rollback_ring_destroy(&sRollbackGameRuntime.ring);
    }
    mdkr_object_assets_unpin_rollback();
    free(sRollbackGameRuntime.baseline_snapshot);
    free(sRollbackGameRuntime.replayed_snapshot);
    sRollbackGameRuntime = (MdkrRollbackGameRuntime){0};
}

bool mdkr_rollback_game_runtime_level_ready(void) {
    const bool laboratory_requested = port_env_bool(
        "MDKR_ROLLBACK_LAB", 0,
        "freeze the real-game rollback registry and capture level tick zero");
    const bool network_input = mdkr_match_input_runtime_active();
    const bool requested = laboratory_requested || network_input;
    const bool trace_allocations = port_env_bool(
        "MDKR_ROLLBACK_TRACE_ALLOCATIONS", 0,
        "log allocator mutations while the rollback laboratory is active");
    const bool roundtrip_once = port_env_bool(
        "MDKR_ROLLBACK_LAB_ROUNDTRIP", 0,
        "restore the first real-game tick snapshot at its boundary");
    const bool resim_requested = port_env_bool(
        "MDKR_ROLLBACK_LAB_RESIM", 0,
        "rewind and replay four real-game ticks with captured inputs");
    const bool mutation_control = port_env_bool(
        "MDKR_ROLLBACK_LAB_MUTATION_CONTROL", 0,
        "require changed replay input to produce a different authority snapshot");
    const bool delayed_input_control = port_env_bool(
        "MDKR_ROLLBACK_LAB_DELAYED_INPUT", 0,
        "withhold four canonical inputs, deliver them late, and reconcile");
    const uint32_t replay_target_tick = port_env_u32(
        "MDKR_ROLLBACK_LAB_TARGET_TICK",
        MDKR_ROLLBACK_LAB_RESIM_TARGET_TICK,
        MDKR_ROLLBACK_LAB_RESIM_DEPTH, UINT32_MAX,
        "authored tick at which the rollback laboratory corrects/replays");
    const uint32_t range_hash_tick = port_env_u32(
        "MDKR_ROLLBACK_RANGE_HASH_TICK", 0u, 0u, UINT32_MAX,
        "emit per-range rollback authority fingerprints at one boundary");
    const uint32_t item_probe_balloon = port_env_u32(
        "MDKR_ROLLBACK_LAB_ITEM_BALLOON", 5u, 0u, 5u,
        "grant one pre-snapshot balloon type (0..4; 5 disables)");
    const uint32_t item_probe_level = port_env_u32(
        "MDKR_ROLLBACK_LAB_ITEM_LEVEL", 0u, 0u, 2u,
        "select the granted rollback item level (0..2)");
    const bool item_probe_mutation_control = port_env_bool(
        "MDKR_ROLLBACK_LAB_ITEM_MUTATION_CONTROL", 0,
        "suppress the corrected item-release edge so breadth must fail");
    const bool resim_once =
        resim_requested || mutation_control || delayed_input_control;
    const uint64_t process_cookie =
        (uint64_t)(uintptr_t)&sRollbackGameRuntime;
    const char *failure_stage = NULL;

    mdkr_rollback_game_runtime_level_end();
    if (!requested) {
        return true;
    }
    if (network_input) {
        const MdkrMatchManifestV1 *manifest =
            mdkr_net_roster_runtime_manifest();
        const uint8_t authored_cadence_hz =
            REGION == REGION_PAL ? 25u : 30u;
        if (manifest == NULL || level_id() < 0 || level_id() > UINT16_MAX ||
            !mdkr_match_manifest_accepts_loaded_race(
                manifest, (uint16_t)level_id(),
                (uint8_t)leveltable_type(level_id()),
                (uint8_t)leveltable_vehicle_usable(level_id()),
                authored_cadence_hz)) {
            fprintf(stderr,
                    "[ROLLBACK] online race admission rejected "
                    "epoch=%u manifestTrack=%u loadedTrack=%d raceType=%d "
                    "manifestHz=%u authoredHz=%u rules=%u "
                    "manifestVehicles=0x%02x loadedVehicles=0x%02x\n",
                    manifest != NULL ? manifest->match_epoch : 0u,
                    manifest != NULL ? manifest->track_id : 0u,
                    (int)level_id(), (int)leveltable_type(level_id()),
                    manifest != NULL ? manifest->cadence_hz : 0u,
                    authored_cadence_hz,
                    manifest != NULL ? manifest->rules : 0u,
                    manifest != NULL ? manifest->vehicle_mask : 0u,
                    (unsigned)leveltable_vehicle_usable(level_id()));
            return false;
        }
    }
    if (item_probe_balloon < 5u) {
        const uint32_t release_tick = replay_target_tick -
            MDKR_ROLLBACK_LAB_RESIM_DEPTH + 1u;
        if (!laboratory_requested || network_input ||
            !delayed_input_control || release_tick <= 1u ||
            !arm_item_probe(
                &sRollbackGameRuntime, item_probe_balloon,
                item_probe_level, release_tick,
                item_probe_mutation_control)) {
            fprintf(stderr,
                    "[ROLLBACK] item probe rejected: requires a local "
                    "delayed-input lab, a pre-release tick and a valid racer\n");
            mdkr_rollback_game_runtime_level_end();
            return false;
        }
    }
    mdkr_rollback_snapshot_registry_init(
        &sRollbackGameRuntime.registry,
        process_cookie != 0u ? process_cookie : UINT64_C(1));
    if (!mdkr_object_assets_pin_rollback()) {
        failure_stage = "item-asset-residency";
    } else if (!transition_workspace_preload()) {
        failure_stage = "transition-workspace";
    } else if (!mdkr_rollback_game_authority_register(
                   &sRollbackGameRuntime.registry)) {
        failure_stage = "authority-registry";
    } else if (!mdkr_rollback_snapshot_freeze(
                   &sRollbackGameRuntime.registry,
                   MDKR_ROLLBACK_LAB_MANIFEST)) {
        failure_stage = "registry-freeze";
    } else if (!mdkr_rollback_ring_init(
                   &sRollbackGameRuntime.ring,
                   &sRollbackGameRuntime.registry,
                   MDKR_ROLLBACK_LAB_SLOTS)) {
        failure_stage = "snapshot-ring";
    }
    if (failure_stage == NULL) {
        mdkr_rollback_ring_set_clock(
            &sRollbackGameRuntime.ring, rollback_clock_now, NULL);
    }
    if (failure_stage == NULL && (resim_once || network_input)) {
        sRollbackGameRuntime.baseline_snapshot =
            (uint8_t *)malloc(sRollbackGameRuntime.ring.snapshot_bytes);
        sRollbackGameRuntime.replayed_snapshot =
            (uint8_t *)malloc(sRollbackGameRuntime.ring.snapshot_bytes);
        if (sRollbackGameRuntime.baseline_snapshot == NULL ||
            sRollbackGameRuntime.replayed_snapshot == NULL) {
            failure_stage = "replay-comparison-buffers";
        }
    }
    if (failure_stage == NULL &&
        !mdkr_rollback_ring_capture(&sRollbackGameRuntime.ring, 0u)) {
        failure_stage = "tick-zero-capture";
    } else if (failure_stage == NULL &&
               !mdkr_rollback_validate_live_allocations(
                   &sRollbackGameRuntime.registry)) {
        failure_stage = "allocation-lifetime";
    }
    if (failure_stage != NULL) {
        fprintf(stderr,
                "[ROLLBACK] lab startup rejected at stage=%s ranges=%u "
                "snapshot=%zu\n",
                failure_stage,
                (unsigned)sRollbackGameRuntime.registry.range_count,
                mdkr_rollback_snapshot_bytes(
                    &sRollbackGameRuntime.registry));
        mdkr_rollback_game_runtime_level_end();
        return false;
    }
    sRollbackGameRuntime.active = true;
    sRollbackGameRuntime.roundtrip_once = roundtrip_once;
    sRollbackGameRuntime.resim_once = resim_once;
    sRollbackGameRuntime.replay_target_tick = replay_target_tick;
    sRollbackGameRuntime.range_hash_tick = range_hash_tick;
    sRollbackGameRuntime.mutation_control = mutation_control;
    sRollbackGameRuntime.delayed_input_control = delayed_input_control;
    sRollbackGameRuntime.network_input = network_input;
    input_rollback_capture(sRollbackGameRuntime.last_applied_input);
    mdkr_net_input_init(&sRollbackGameRuntime.net_inputs, 1u);
    mdkr_rollback_events_init(
        &sRollbackGameRuntime.events, true, preview_rollback_effect, NULL,
        cancel_rollback_effect, &sRollbackGameRuntime);
    mdkr_rollback_audio_init(&sRollbackGameRuntime.audio);
    gameplay_event_trace_set_rollback_observer(&kRollbackEffectObserver);
    if (trace_allocations) {
        mdkr_mempool_set_mutation_observer(
            log_pool_mutation, &sRollbackGameRuntime);
    }
    fprintf(stderr,
            "[ROLLBACK] %s race: loadedTrack=%d raceType=%d authoredHz=%u\n",
            network_input ? "online" : "lab", (int)level_id(),
            (int)leveltable_type(level_id()),
            (unsigned)(REGION == REGION_PAL ? 25u : 30u));
    fprintf(stderr,
            "[ROLLBACK] %s ready: ranges=%u snapshot=%zu bytes ring=%zu bytes "
            "target=%u epoch=%u\n",
            network_input ? "online" : "lab",
            (unsigned)sRollbackGameRuntime.registry.range_count,
            sRollbackGameRuntime.ring.snapshot_bytes,
            sRollbackGameRuntime.ring.stats.allocated_bytes,
            (unsigned)sRollbackGameRuntime.replay_target_tick,
            (unsigned)mdkr_match_input_runtime_epoch());
    return true;
}

static bool reconcile_network_inputs(
    MdkrRollbackGameRuntime *runtime, uint32_t last_tick) {
    uint32_t dirty;
    uint32_t tick;
    if (!mdkr_match_input_runtime_take_dirty(&dirty)) return true;
    if (dirty == 0u || dirty > last_tick ||
        last_tick - dirty + 1u >= MDKR_ROLLBACK_LAB_SLOTS ||
        !mdkr_rollback_ring_has(&runtime->ring, dirty - 1u)) {
        fprintf(stderr,
                "[ROLLBACK] online correction outside retained window "
                "dirty=%u current=%u capacity=%u\n",
                dirty, last_tick, MDKR_ROLLBACK_LAB_SLOTS);
        return false;
    }
    for (tick = dirty; tick <= last_tick; tick++) {
        MdkrInputSet frame;
        MdkrRollbackInputHistory *history =
            &runtime->input_history[tick % MDKR_ROLLBACK_LAB_SLOTS];
        if (!history->valid || history->tick != tick ||
            !mdkr_match_input_runtime_inputs_for_tick(tick, &frame) ||
            !bridge_input_samples(&frame, history->input)) {
            fprintf(stderr,
                    "[ROLLBACK] online corrected input unavailable tick=%u\n",
                    tick);
            return false;
        }
        history->confirmed_mask = frame.confirmed_mask;
    }

    begin_effect_rewrite(runtime, dirty);
    if (!mdkr_rollback_ring_restore(&runtime->ring, dirty - 1u, true)) {
        force_clear_effects(runtime);
        return false;
    }
    for (tick = dirty; tick <= last_tick; tick++) {
        MdkrRollbackInputHistory *history =
            &runtime->input_history[tick % MDKR_ROLLBACK_LAB_SLOTS];
        begin_effect_tick(runtime, tick);
        if (history->update_rate == 0u || history->update_rate > INT_MAX ||
            !mdkr_match_input_runtime_begin_tick(tick) ||
            !resimulate_timed(
                runtime, (int)history->update_rate, history->input) ||
            !mdkr_rollback_validate_live_allocations(&runtime->registry) ||
            !mdkr_rollback_game_authority_validate_dynamic_coverage(
                &runtime->registry) ||
            !mdkr_rollback_ring_capture(&runtime->ring, tick)) {
            force_clear_effects(runtime);
            fprintf(stderr,
                    "[ROLLBACK] online correction replay failed tick=%u\n",
                    tick);
            return false;
        }
    }
    end_effect_rewrite(runtime);
    memcpy(runtime->last_applied_input,
           runtime->input_history[
               last_tick % MDKR_ROLLBACK_LAB_SLOTS].input,
           sizeof(runtime->last_applied_input));
    fprintf(stderr,
            "[ROLLBACK] online correction reconciled ticks=%u..%u depth=%u\n",
            dirty, last_tick, last_tick - dirty + 1u);
    return true;
}

bool mdkr_rollback_game_runtime_prepare_tick(unsigned update_rate) {
    MdkrRollbackGameRuntime *runtime = &sRollbackGameRuntime;
    MdkrRollbackInputHistory *history;
    MdkrNetInputSet set;
    MdkrInputSample polled[MDKR_INPUT_PORTS];
    uint32_t tick;
    unsigned slot;
    if (!runtime->active) {
        return true;
    }
    if (runtime->validated_boundaries >= UINT32_MAX) {
        return false;
    }
    tick = (uint32_t)(runtime->validated_boundaries + 1u);
    runtime->authored_frame_started_ns = rollback_clock_now(NULL);
    runtime->authored_frame_timing_active = true;
    begin_effect_tick(runtime, tick);
    if (runtime->network_input) {
        MdkrInputSet frame;
        MdkrPadSample physical[MDKR_INPUT_PORTS];
        if (runtime->tick_prepared && runtime->prepared_tick == tick) {
            fprintf(stderr,
                    "[ROLLBACK] online input tick prepared twice tick=%u\n",
                    tick);
            return false;
        }
        if (tick > 1u &&
            !reconcile_network_inputs(runtime, tick - 1u)) {
            return false;
        }
        input_rollback_capture(polled);
        pad_input_samples(polled, physical);
        if (!mdkr_match_input_runtime_drain(
                tick, physical, MDKR_INPUT_PORTS, &frame) ||
            !bridge_input_samples(&frame, polled) ||
            !mdkr_match_input_runtime_begin_tick(tick)) {
            fprintf(stderr,
                    "[ROLLBACK] launcher input provider rejected tick=%u\n",
                    tick);
            return false;
        }
        history = &runtime->input_history[
            tick % MDKR_ROLLBACK_LAB_SLOTS];
        memset(history, 0, sizeof(*history));
        memcpy(history->input, polled, sizeof(polled));
        memcpy(history->received_input, polled, sizeof(polled));
        history->tick = tick;
        history->update_rate = update_rate;
        history->confirmed_mask = frame.confirmed_mask;
        history->valid = true;
        input_rollback_apply_from_previous(
            polled, runtime->last_applied_input);
        memcpy(runtime->last_applied_input, polled,
               sizeof(runtime->last_applied_input));
        runtime->prepared_tick = tick;
        runtime->tick_prepared = true;
        return true;
    }
    if (!runtime->delayed_input_control) {
        return true;
    }
    if (runtime->tick_prepared && runtime->prepared_tick == tick) {
        fprintf(stderr,
                "[ROLLBACK] delayed-input tick prepared twice tick=%u\n",
                tick);
        return false;
    }
    input_rollback_capture(polled);
    if (runtime->item_probe_enabled &&
        tick == runtime->item_probe_release_tick - 1u) {
        /* The retained prefix owns Z-down. Missing frames repeat it, then the
         * late authoritative frame supplies Z-up. That puts real item use on
         * the correction rather than the originally predicted timeline. */
        polled[0].buttons |= Z_TRIG;
        polled[0].present = true;
    }
    history = &runtime->input_history[tick % MDKR_ROLLBACK_LAB_SLOTS];
    memset(history, 0, sizeof(*history));
    memcpy(history->received_input, polled, sizeof(polled));
    history->tick = tick;
    history->update_rate = update_rate;
    history->valid = true;
    mdkr_net_input_set_current_tick(&runtime->net_inputs, tick);
    for (slot = 0u; slot < MDKR_INPUT_PORTS; slot++) {
        MdkrInputSample received = polled[slot];
        const bool delayed_slot =
            slot == 0u &&
            tick >= runtime->replay_target_tick -
                        MDKR_ROLLBACK_LAB_RESIM_DEPTH + 1u &&
            tick <= runtime->replay_target_tick;
        if (delayed_slot) {
            received = (MdkrInputSample){
                (uint16_t)(A_BUTTON |
                    (runtime->item_probe_enabled &&
                     runtime->item_probe_mutation_control
                        ? Z_TRIG : 0u)),
                80, 0, true};
            history->received_input[slot] = received;
            continue;
        }
        if (mdkr_net_input_submit(
                &runtime->net_inputs, slot, tick, &received) !=
            MDKR_NET_SUBMIT_ACCEPTED) {
            fprintf(stderr,
                    "[ROLLBACK] canonical input submit rejected tick=%u "
                    "slot=%u\n",
                    tick, slot);
            return false;
        }
    }
    if (!mdkr_net_input_for_tick(&runtime->net_inputs, tick, &set)) {
        fprintf(stderr,
                "[ROLLBACK] canonical predicted input unavailable tick=%u\n",
                tick);
        return false;
    }
    memcpy(history->input, set.samples, sizeof(history->input));
    input_rollback_apply_from_previous(
        set.samples, runtime->last_applied_input);
    memcpy(runtime->last_applied_input, set.samples,
           sizeof(runtime->last_applied_input));
    runtime->prepared_tick = tick;
    runtime->tick_prepared = true;
    return true;
}

static bool record_bootstrap_input_boundary(
    MdkrRollbackGameRuntime *runtime, MdkrRollbackInputHistory *history,
    uint32_t tick, unsigned update_rate) {
    MdkrInputSample input[MDKR_INPUT_PORTS];
    unsigned slot;
    input_rollback_capture(input);
    memset(history, 0, sizeof(*history));
    memcpy(history->input, input, sizeof(input));
    memcpy(history->received_input, input, sizeof(input));
    history->tick = tick;
    history->update_rate = update_rate;
    history->valid = true;
    mdkr_net_input_set_current_tick(&runtime->net_inputs, tick);
    for (slot = 0u; slot < MDKR_INPUT_PORTS; slot++) {
        if (mdkr_net_input_submit(
                &runtime->net_inputs, slot, tick, &input[slot]) !=
            MDKR_NET_SUBMIT_ACCEPTED) {
            return false;
        }
    }
    memcpy(runtime->last_applied_input, input,
           sizeof(runtime->last_applied_input));
    return true;
}

static void log_object_list_pointer(
    const MdkrRollbackGameRuntime *runtime, const char *label,
    uintptr_t pointer) {
    MdkrObjectRollbackAddressInfo info;
    void *allocation_base = NULL;
    size_t allocation_size = 0u;
    fprintf(stderr, " %s=%p", label, (void *)pointer);
    if (pointer != 0u && mdkr_mempool_allocation_span(
            (const void *)pointer, &allocation_base, &allocation_size)) {
        fprintf(stderr, "[base=%p size=%zu", allocation_base,
                allocation_size);
        if (allocation_base == (void *)pointer &&
            mdkr_object_rollback_describe_object(
                (const void *)pointer, &info)) {
            fprintf(stderr, " index=%d behavior=%d header=%d",
                    info.object_index, info.behavior_id, info.header_type);
        }
        fputc(']', stderr);
    } else if (pointer != 0u) {
        unsigned range;
        bool matched = false;
        for (range = 0u; range < runtime->registry.range_count; range++) {
            const uintptr_t begin =
                (uintptr_t)runtime->registry.ranges[range].address;
            const uintptr_t end = begin + runtime->registry.ranges[range].size;
            if (pointer >= begin && pointer < end) {
                fprintf(stderr, "[authority_range=%u tag=%08x offset=%zu",
                        range, runtime->registry.ranges[range].tag,
                        (size_t)(pointer - begin));
                if (mdkr_object_rollback_describe_object(
                        (const void *)pointer, &info)) {
                    fprintf(stderr, " index=%d behavior=%d header=%d",
                            info.object_index, info.behavior_id,
                            info.header_type);
                }
                fputc(']', stderr);
                matched = true;
                break;
            }
        }
        if (!matched && runtime->registry.range_count > 1u) {
            fprintf(stderr, "[outside_authority object_pool=%p+%zu]",
                    runtime->registry.ranges[1].address,
                    runtime->registry.ranges[1].size);
        }
    }
}

static void log_replay_difference(const MdkrRollbackGameRuntime *runtime) {
    size_t offset = sizeof(MdkrRollbackSnapshotHeader);
    unsigned range;
    for (range = 0u; range < runtime->registry.range_count; range++) {
        const size_t size = runtime->registry.ranges[range].size;
        if (memcmp(runtime->baseline_snapshot + offset,
                   runtime->replayed_snapshot + offset, size) != 0) {
            size_t byte;
            for (byte = 0u; byte < size; byte++) {
                if (runtime->baseline_snapshot[offset + byte] !=
                    runtime->replayed_snapshot[offset + byte]) {
                    const uint8_t *address =
                        (const uint8_t *)runtime->registry.ranges[range].address +
                        byte;
                    void *allocation_base = NULL;
                    size_t allocation_size = 0u;
                    fprintf(stderr,
                            "[ROLLBACK] replay mismatch range=%u tag=%08x "
                            "offset=%zu expected=%02x actual=%02x",
                            range,
                            runtime->registry.ranges[range].tag, byte,
                            runtime->baseline_snapshot[offset + byte],
                            runtime->replayed_snapshot[offset + byte]);
                    if (runtime->registry.ranges[range].tag ==
                            MDKR_ROLLBACK_TAG_OBJECT_LIST) {
                        const size_t pointer_offset =
                            byte - (byte % sizeof(uintptr_t));
                        uintptr_t expected_pointer = 0u;
                        uintptr_t actual_pointer = 0u;
                        memcpy(&expected_pointer,
                               runtime->baseline_snapshot + offset +
                                   pointer_offset,
                               sizeof(expected_pointer));
                        memcpy(&actual_pointer,
                               runtime->replayed_snapshot + offset +
                                   pointer_offset,
                               sizeof(actual_pointer));
                        fprintf(stderr, " list_index=%zu",
                                pointer_offset / sizeof(uintptr_t));
                        log_object_list_pointer(
                            runtime, "expected_ptr", expected_pointer);
                        log_object_list_pointer(
                            runtime, "actual_ptr", actual_pointer);
                    }
                    if (mdkr_mempool_allocation_span_in_pool(
                            POOL_OBJECT, address, &allocation_base,
                            &allocation_size)) {
                        MdkrObjectRollbackAddressInfo info;
                        fprintf(stderr,
                                " allocation_base_offset=%zu "
                                "allocation_size=%zu allocation_offset=%zu",
                                (size_t)((const uint8_t *)allocation_base -
                                         (const uint8_t *)runtime->registry
                                             .ranges[range].address),
                                allocation_size,
                                (size_t)(address -
                                         (const uint8_t *)allocation_base));
                        if (mdkr_object_rollback_identify_address(
                                allocation_base, address, &info)) {
                            fprintf(stderr,
                                    " object_index=%d behavior=%d "
                                    "header=%d object_offset=%zu",
                                    info.object_index, info.behavior_id,
                                    info.header_type, info.object_offset);
                            if (info.has_behavior_offset) {
                                fprintf(stderr, " behavior_offset=%zu",
                                        info.behavior_offset);
                            }
                            fprintf(stderr,
                                    " bases[behavior=%zu shading=%zu shadow=%zu "
                                    "interaction=%zu collision=%zu]",
                                    info.behavior_base_offset,
                                    info.shading_base_offset,
                                    info.shadow_base_offset,
                                    info.interaction_base_offset,
                                    info.collision_base_offset);
                        }
                    }
                    fputc('\n', stderr);
                    return;
                }
            }
        }
        offset += size;
    }
    fprintf(stderr,
            "[ROLLBACK] replay snapshot metadata mismatch with identical "
            "authority payload\n");
}

static bool replay_payload_equal(
    const MdkrRollbackGameRuntime *runtime, const uint8_t *left,
    const uint8_t *right, bool exclude_input) {
    size_t offset = sizeof(MdkrRollbackSnapshotHeader);
    unsigned range;
    for (range = 0u; range < runtime->registry.range_count; range++) {
        const MdkrRollbackRange *entry = &runtime->registry.ranges[range];
        if ((!exclude_input ||
             !mdkr_rollback_game_authority_is_input_tag(entry->tag)) &&
            memcmp(left + offset, right + offset, entry->size) != 0) {
            return false;
        }
        offset += entry->size;
    }
    return true;
}

static bool live_payload_equal(
    const MdkrRollbackGameRuntime *runtime, const uint8_t *snapshot,
    const char *phase, uint32_t tick) {
    size_t offset = sizeof(MdkrRollbackSnapshotHeader);
    unsigned range;
    for (range = 0u; range < runtime->registry.range_count; range++) {
        const MdkrRollbackRange *entry = &runtime->registry.ranges[range];
        if (memcmp(entry->address, snapshot + offset, entry->size) != 0) {
            size_t byte;
            const uint8_t *address;
            void *allocation_base = NULL;
            size_t allocation_size = 0u;
            for (byte = 0u; byte < entry->size; byte++) {
                if (((const uint8_t *)entry->address)[byte] !=
                    snapshot[offset + byte]) break;
            }
            address = (const uint8_t *)entry->address + byte;
            fprintf(stderr,
                    "[ROLLBACK] %s mismatch tick=%u range=%u tag=%08x "
                    "offset=%zu expected=%02x actual=%02x",
                    phase, tick, range, entry->tag, byte,
                    snapshot[offset + byte],
                    ((const uint8_t *)entry->address)[byte]);
            if (mdkr_mempool_allocation_span_in_pool(
                    POOL_OBJECT, address, &allocation_base,
                    &allocation_size)) {
                MdkrObjectRollbackAddressInfo info;
                const size_t allocation_offset =
                    (size_t)(address - (const uint8_t *)allocation_base);
                fprintf(stderr,
                        " allocation_base_offset=%zu allocation_size=%zu "
                        "allocation_offset=%zu",
                        (size_t)((const uint8_t *)allocation_base -
                                 (const uint8_t *)entry->address),
                        allocation_size, allocation_offset);
                if (mdkr_object_rollback_identify_address(
                        allocation_base, address, &info)) {
                    const Object *object = (const Object *)allocation_base;
                    fprintf(stderr,
                            " object_index=%d behavior=%d header=%d "
                            "object_offset=%zu",
                            info.object_index, info.behavior_id,
                            info.header_type, info.object_offset);
                    if (allocation_size >= sizeof(Object)) {
                        Object expected_object;
                        const size_t object_snapshot_offset = offset +
                            (size_t)((const uint8_t *)allocation_base -
                                     (const uint8_t *)entry->address);
                        memcpy(&expected_object,
                               snapshot + object_snapshot_offset,
                               sizeof(expected_object));
                        fprintf(stderr,
                                " pointers[level=%p/%p header=%p/%p "
                                "verts=%p/%p model=%p/%p data=%p/%p "
                                "shadow=%p/%p shade=%p/%p interact=%p/%p] "
                                "models=%d",
                                (void *)expected_object.level_entry,
                                (void *)object->level_entry,
                                (void *)expected_object.header,
                                (void *)object->header,
                                (void *)expected_object.curVertData,
                                (void *)object->curVertData,
                                (void *)expected_object.modelInstances,
                                (void *)object->modelInstances,
                                expected_object.anyBehaviorData,
                                object->anyBehaviorData,
                                (void *)expected_object.shadow,
                                (void *)object->shadow,
                                (void *)expected_object.shading,
                                (void *)object->shading,
                                (void *)expected_object.interactObj,
                                (void *)object->interactObj,
                                object->header != NULL
                                    ? (int)object->header->numberOfModelIds
                                    : -1);
                        fprintf(stderr,
                                " embedded[collision=%p/%p emitter=%p/%p]",
                                (void *)expected_object.collisionData,
                                (void *)object->collisionData,
                                (void *)expected_object.particleEmitter,
                                (void *)object->particleEmitter);
                        if (object->particleEmitter != NULL &&
                            (const uint8_t *)object->particleEmitter <= address &&
                            address < (const uint8_t *)object->particleEmitter +
                                          sizeof(*object->particleEmitter)) {
                            ParticleEmitter expected_emitter;
                            const size_t emitter_offset =
                                (size_t)((const uint8_t *)
                                             object->particleEmitter -
                                         (const uint8_t *)allocation_base);
                            memcpy(&expected_emitter,
                                   snapshot + object_snapshot_offset +
                                       emitter_offset,
                                   sizeof(expected_emitter));
                            fprintf(stderr,
                                    " emitter[offset=%zu behaviour=%p/%p "
                                    "flags=%04x/%04x counters=%u,%u/%u,%u "
                                    "descriptor=%d/%d time=%d/%d]",
                                    emitter_offset,
                                    (void *)expected_emitter.behaviour,
                                    (void *)object->particleEmitter->behaviour,
                                    (unsigned)(u16)expected_emitter.flags,
                                    (unsigned)(u16)object->particleEmitter->flags,
                                    (unsigned)expected_emitter.sourceRotationCounter,
                                    (unsigned)expected_emitter.emissionDirRotationCounter,
                                    (unsigned)object->particleEmitter->sourceRotationCounter,
                                    (unsigned)object->particleEmitter->emissionDirRotationCounter,
                                    (int)expected_emitter.descriptorID,
                                    (int)object->particleEmitter->descriptorID,
                                    (int)expected_emitter.timeFromLastSpawn,
                                    (int)object->particleEmitter->timeFromLastSpawn);
                        }
                        fprintf(stderr,
                                " transform[pos=%.6f,%.6f,%.6f/"
                                "%.6f,%.6f,%.6f rot=%d,%d,%d/%d,%d,%d "
                                "scale=%.6f/%.6f]",
                                expected_object.trans.x_position,
                                expected_object.trans.y_position,
                                expected_object.trans.z_position,
                                object->trans.x_position,
                                object->trans.y_position,
                                object->trans.z_position,
                                expected_object.trans.rotation.x_rotation,
                                expected_object.trans.rotation.y_rotation,
                                expected_object.trans.rotation.z_rotation,
                                object->trans.rotation.x_rotation,
                                object->trans.rotation.y_rotation,
                                object->trans.rotation.z_rotation,
                                expected_object.trans.scale,
                                object->trans.scale);
                        if (object->modelInstances != NULL &&
                            object->header != NULL &&
                            object->header->modelType ==
                                OBJECT_MODEL_TYPE_3D_MODEL &&
                            object->header->numberOfModelIds > 0) {
                            ModelInstance *expected_instance = NULL;
                            ModelInstance *actual_instance =
                                object->modelInstances[0];
                            const size_t model_array_offset =
                                (size_t)((const uint8_t *)
                                             object->modelInstances -
                                         (const uint8_t *)allocation_base);
                            memcpy(&expected_instance,
                                   snapshot + object_snapshot_offset +
                                       model_array_offset,
                                   sizeof(expected_instance));
                            fprintf(stderr,
                                    " instance[arrayOffset=%zu ptr=%p/%p]",
                                    model_array_offset,
                                    (void *)expected_instance,
                                    (void *)actual_instance);
                            if (expected_instance != NULL &&
                                actual_instance != NULL) {
                                fprintf(stderr,
                                        " instanceData[model=%p/%p "
                                        "v0=%p/%p type=%d/%d]",
                                        (void *)expected_instance->objModel,
                                        (void *)actual_instance->objModel,
                                        (void *)expected_instance->vertices[0],
                                        (void *)actual_instance->vertices[0],
                                        (int)expected_instance->modelType,
                                        (int)actual_instance->modelType);
                            }
                        }
                        if (object->weapon != NULL &&
                            (const uint8_t *)object->weapon >=
                                (const uint8_t *)allocation_base &&
                            (const uint8_t *)object->weapon +
                                    sizeof(*object->weapon) <=
                                (const uint8_t *)allocation_base +
                                    allocation_size) {
                            Object_Weapon expected_weapon;
                            const size_t weapon_offset =
                                (size_t)((const uint8_t *)object->weapon -
                                         (const uint8_t *)allocation_base);
                            memcpy(&expected_weapon,
                                   snapshot + object_snapshot_offset +
                                       weapon_offset,
                                   sizeof(expected_weapon));
                            fprintf(stderr,
                                    " weapon[offset=%zu target=%p/%p "
                                    "owner=%p/%p hit=%p/%p sound=%p/%p "
                                    "id=%u/%u checkpoint=%d/%d]",
                                    weapon_offset,
                                    (void *)expected_weapon.target,
                                    (void *)object->weapon->target,
                                    (void *)expected_weapon.owner,
                                    (void *)object->weapon->owner,
                                    (void *)expected_weapon.hitObj,
                                    (void *)object->weapon->hitObj,
                                    (void *)expected_weapon.soundMask,
                                    (void *)object->weapon->soundMask,
                                    (unsigned)expected_weapon.weaponID,
                                    (unsigned)object->weapon->weaponID,
                                    (int)expected_weapon.checkpoint,
                                    (int)object->weapon->checkpoint);
                        }
                        if (object->collisionData != NULL &&
                            (const uint8_t *)object->collisionData <= address &&
                            address < (const uint8_t *)object->collisionData +
                                          sizeof(*object->collisionData)) {
                            const size_t collision_offset =
                                (size_t)((const uint8_t *)
                                             object->collisionData -
                                         (const uint8_t *)allocation_base);
                            const size_t field_offset =
                                (size_t)(address -
                                         (const uint8_t *)
                                             object->collisionData);
                            f32 expected_float = 0.0f;
                            f32 actual_float = 0.0f;
                            if (field_offset < sizeof(object->collisionData->matrices)) {
                                const size_t float_offset =
                                    field_offset & ~(sizeof(f32) - 1u);
                                memcpy(&expected_float,
                                       snapshot + object_snapshot_offset +
                                           collision_offset + float_offset,
                                       sizeof(expected_float));
                                memcpy(&actual_float,
                                       (const uint8_t *)object->collisionData +
                                           float_offset,
                                       sizeof(actual_float));
                            }
                            fprintf(stderr,
                                    " collision[offset=%zu field=%zu "
                                    "float=%zu expected=%.9g actual=%.9g "
                                    "flip=%u]",
                                    collision_offset, field_offset,
                                    field_offset / sizeof(f32),
                                    expected_float, actual_float,
                                    (unsigned)object->collisionData->mtxFlip);
                        }
                    }
                    if (allocation_offset >=
                            offsetof(Object, trans.y_position) &&
                        allocation_offset <
                            offsetof(Object, trans.y_position) +
                                sizeof(object->trans.y_position)) {
                        f32 expected_y;
                        const size_t y_offset =
                            (size_t)((const uint8_t *)allocation_base -
                                     (const uint8_t *)entry->address) +
                            offsetof(Object, trans.y_position);
                        memcpy(&expected_y, snapshot + offset + y_offset,
                               sizeof(expected_y));
                        fprintf(stderr, " expected_y=%.6f actual_y=%.6f",
                                expected_y, object->trans.y_position);
                        if (object->log != NULL &&
                            runtime->registry.range_count > 1u) {
                            const uint8_t *pool_base =
                                (const uint8_t *)runtime->registry.ranges[1]
                                    .address;
                            const size_t pool_size =
                                runtime->registry.ranges[1].size;
                            const uint8_t *log_address =
                                (const uint8_t *)object->log;
                            if (log_address >= pool_base &&
                                log_address + sizeof(*object->log) <=
                                    pool_base + pool_size) {
                                Object_Log expected_log;
                                const size_t pool_snapshot_offset =
                                    sizeof(MdkrRollbackSnapshotHeader) +
                                    runtime->registry.ranges[0].size;
                                memcpy(&expected_log,
                                       snapshot + pool_snapshot_offset +
                                           (size_t)(log_address - pool_base),
                                       sizeof(expected_log));
                                fprintf(stderr,
                                        " wave_phase[expected=%u actual=%u "
                                        "period=%u]",
                                        expected_log.unk4, object->log->unk4,
                                        object->log->unk6);
                            }
                        }
                    }
                    if (allocation_offset == offsetof(Object, animFrame) &&
                        allocation_offset + sizeof(((Object *)0)->animFrame) <=
                            allocation_size) {
                        s16 expected_frame;
                        memcpy(&expected_frame, snapshot + offset + byte,
                               sizeof(expected_frame));
                        fprintf(stderr, " expected_anim=%d actual_anim=%d",
                                expected_frame,
                                ((const Object *)allocation_base)->animFrame);
                    }
                }
            }
            fputc('\n', stderr);
            return false;
        }
        offset += entry->size;
    }
    return true;
}

static bool replay_sequence(MdkrRollbackGameRuntime *runtime,
                            uint32_t target_tick, bool mutate_input) {
    const uint32_t first_tick =
        target_tick - MDKR_ROLLBACK_LAB_RESIM_DEPTH + 1u;
    const uint32_t restore_tick = first_tick - 1u;
    uint32_t tick;
    if (mutate_input) {
        mdkr_rollback_audio_discard_commands(&runtime->audio);
        mdkr_rollback_events_set_resimulating(&runtime->events, true);
    } else {
        begin_effect_rewrite(runtime, first_tick);
    }
    if (!mdkr_rollback_ring_restore(&runtime->ring, restore_tick, true)) {
        if (mutate_input) {
            mdkr_rollback_events_set_resimulating(&runtime->events, false);
        } else {
            force_clear_effects(runtime);
        }
        fprintf(stderr,
                "[ROLLBACK] replay could not restore tick=%u for target=%u\n",
                restore_tick, target_tick);
        return false;
    }
    for (tick = first_tick; tick <= target_tick; tick++) {
        const MdkrRollbackInputHistory *history =
            &runtime->input_history[tick % MDKR_ROLLBACK_LAB_SLOTS];
        MdkrInputSample changed_input[MDKR_INPUT_PORTS];
        const MdkrInputSample *replay_input = history->input;
        const char *failure_stage = NULL;
        begin_effect_tick(runtime, tick);
        if (mutate_input) {
            memcpy(changed_input, history->input, sizeof(changed_input));
            changed_input[0].present = true;
            changed_input[0].buttons |= A_BUTTON;
            changed_input[0].stick_x = 80;
            replay_input = changed_input;
        }
        if (!history->valid || history->tick != tick ||
            history->update_rate == 0u || history->update_rate > INT_MAX) {
            failure_stage = "input-history";
        } else if (!resimulate_timed(
                       runtime, (int)history->update_rate, replay_input)) {
            failure_stage = "game-tick";
        } else if (!mdkr_rollback_validate_live_allocations(
                       &runtime->registry)) {
            failure_stage = "allocation-lifetime";
        } else if (!mdkr_rollback_game_authority_validate_dynamic_coverage(
                       &runtime->registry)) {
            failure_stage = "dynamic-coverage";
        } else if (!mdkr_rollback_ring_capture(&runtime->ring, tick)) {
            failure_stage = "snapshot-capture";
        }
        if (failure_stage != NULL) {
            fprintf(stderr,
                    "[ROLLBACK] real-game replay failed stage=%s tick=%u "
                    "target=%u mutated=%u\n",
                    failure_stage, tick, target_tick,
                    mutate_input ? 1u : 0u);
            if (mutate_input) {
                mdkr_rollback_events_set_resimulating(&runtime->events, false);
            } else {
                force_clear_effects(runtime);
            }
            return false;
        }
    }
    if (mutate_input) {
        mdkr_rollback_events_set_resimulating(&runtime->events, false);
        mdkr_rollback_audio_discard_commands(&runtime->audio);
    } else {
        end_effect_rewrite(runtime);
    }
    return true;
}

static bool replay_recent_ticks(MdkrRollbackGameRuntime *runtime,
                                uint32_t target_tick) {
    MdkrRollbackEventJournal event_checkpoint;
    MdkrRollbackAudioAdapter audio_checkpoint;
    if (!mdkr_rollback_ring_copy(
            &runtime->ring, target_tick, runtime->baseline_snapshot,
            runtime->ring.snapshot_bytes)) {
        fprintf(stderr,
                "[ROLLBACK] replay could not retain target tick=%u\n",
                target_tick);
        return false;
    }
    if (runtime->mutation_control) {
        event_checkpoint = runtime->events;
        audio_checkpoint = runtime->audio;
        if (!replay_sequence(runtime, target_tick, true) ||
            !mdkr_rollback_ring_copy(
                &runtime->ring, target_tick, runtime->replayed_snapshot,
                runtime->ring.snapshot_bytes)) {
            fprintf(stderr,
                    "[ROLLBACK] changed-input negative control could not run "
                    "tick=%u\n",
                    target_tick);
            return false;
        }
        if (replay_payload_equal(
                runtime, runtime->baseline_snapshot,
                runtime->replayed_snapshot, true)) {
            fprintf(stderr,
                    "[ROLLBACK] changed-input negative control did not "
                    "change non-input authority tick=%u\n",
                    target_tick);
            return false;
        }
        fprintf(stderr,
                "[ROLLBACK] changed-input negative control diverged as "
                "required tick=%u depth=%u\n",
                target_tick, MDKR_ROLLBACK_LAB_RESIM_DEPTH);
        /* The mutation arm is a negative control, never a candidate timeline.
         * Discard its journal rows and statistics before exact replay. */
        runtime->events = event_checkpoint;
        runtime->audio = audio_checkpoint;
    }
    if (!replay_sequence(runtime, target_tick, false)) {
        return false;
    }
    if (!mdkr_rollback_ring_copy(
            &runtime->ring, target_tick, runtime->replayed_snapshot,
            runtime->ring.snapshot_bytes)) {
        fprintf(stderr,
                "[ROLLBACK] replayed snapshot unavailable tick=%u\n",
                target_tick);
        return false;
    }
    if (memcmp(runtime->baseline_snapshot, runtime->replayed_snapshot,
               runtime->ring.snapshot_bytes) != 0) {
        log_replay_difference(runtime);
        return false;
    }
    runtime->resim_complete = true;
    fprintf(stderr,
            "[ROLLBACK] four-tick real-game rewind/replay passed tick=%u "
            "depth=%u\n",
            target_tick, MDKR_ROLLBACK_LAB_RESIM_DEPTH);
    return true;
}

static bool replay_delayed_sequence(
    MdkrRollbackGameRuntime *runtime, uint32_t target_tick,
    bool verify_existing) {
    const uint32_t first_tick =
        target_tick - MDKR_ROLLBACK_LAB_RESIM_DEPTH + 1u;
    uint32_t tick;
    begin_effect_rewrite(runtime, first_tick);
    if (!mdkr_rollback_ring_copy(
            &runtime->ring, first_tick - 1u, runtime->replayed_snapshot,
            runtime->ring.snapshot_bytes) ||
        !mdkr_rollback_ring_restore(
            &runtime->ring, first_tick - 1u, true) ||
        !live_payload_equal(
            runtime, runtime->replayed_snapshot, "immediate restore",
            first_tick - 1u)) {
        force_clear_effects(runtime);
        return false;
    }
    for (tick = first_tick; tick <= target_tick; tick++) {
        MdkrNetInputSet set;
        MdkrRollbackInputHistory *history =
            &runtime->input_history[tick % MDKR_ROLLBACK_LAB_SLOTS];
        begin_effect_tick(runtime, tick);
        if ((verify_existing && !mdkr_rollback_ring_copy(
                &runtime->ring, tick, runtime->replayed_snapshot,
                runtime->ring.snapshot_bytes)) ||
            !mdkr_net_input_for_tick(&runtime->net_inputs, tick, &set) ||
            !resimulate_timed(
                runtime, (int)history->update_rate, set.samples) ||
            (verify_existing && !live_payload_equal(
                runtime, runtime->replayed_snapshot, "replay boundary",
                tick)) ||
            !mdkr_rollback_validate_live_allocations(&runtime->registry) ||
            !mdkr_rollback_game_authority_validate_dynamic_coverage(
                &runtime->registry) ||
            !mdkr_rollback_ring_capture(&runtime->ring, tick)) {
            force_clear_effects(runtime);
            fprintf(stderr,
                    "[ROLLBACK] delayed-input replay failed tick=%u\n",
                    tick);
            return false;
        }
        memcpy(history->input, set.samples, sizeof(history->input));
        if (tick == target_tick) {
            memcpy(runtime->last_applied_input, set.samples,
                   sizeof(runtime->last_applied_input));
        }
    }
    end_effect_rewrite(runtime);
    return true;
}

static bool reconcile_delayed_inputs(
    MdkrRollbackGameRuntime *runtime, uint32_t target_tick) {
    const uint32_t first_tick =
        target_tick - MDKR_ROLLBACK_LAB_RESIM_DEPTH + 1u;
    uint32_t dirty = 0u;
    uint32_t tick;
    if (!mdkr_rollback_ring_copy(
            &runtime->ring, target_tick, runtime->baseline_snapshot,
            runtime->ring.snapshot_bytes)) {
        return false;
    }
    if (runtime->item_probe_enabled) {
        runtime->item_probe_spawn_baseline = runtime->observed_item_spawns;
        runtime->item_probe_rumble_baseline =
            runtime->observed_event_kind[GAMEPLAY_EVENT_RUMBLE];
    }
    for (tick = first_tick; tick <= target_tick; tick++) {
        const MdkrRollbackInputHistory *history =
            &runtime->input_history[tick % MDKR_ROLLBACK_LAB_SLOTS];
        const MdkrNetInputSubmitResult result = mdkr_net_input_submit(
            &runtime->net_inputs, 0u, tick, &history->received_input[0]);
        if ((tick == first_tick && result != MDKR_NET_SUBMIT_CORRECTED) ||
            (tick != first_tick && result != MDKR_NET_SUBMIT_ACCEPTED)) {
            fprintf(stderr,
                    "[ROLLBACK] delayed packet rejected tick=%u result=%u\n",
                    tick, (unsigned)result);
            return false;
        }
    }
    if (!mdkr_net_input_take_dirty(&runtime->net_inputs, &dirty) ||
        dirty != first_tick ||
        !replay_delayed_sequence(runtime, target_tick, false) ||
        !mdkr_rollback_ring_copy(
            &runtime->ring, target_tick, runtime->replayed_snapshot,
            runtime->ring.snapshot_bytes)) {
        fprintf(stderr,
                "[ROLLBACK] delayed correction did not reconcile from tick=%u\n",
                first_tick);
        return false;
    }
    if (replay_payload_equal(
            runtime, runtime->baseline_snapshot,
            runtime->replayed_snapshot, true)) {
        fprintf(stderr,
                "[ROLLBACK] delayed correction changed only input buffers "
                "tick=%u\n",
                target_tick);
        return false;
    }
    memcpy(runtime->baseline_snapshot, runtime->replayed_snapshot,
           runtime->ring.snapshot_bytes);
    if (!replay_delayed_sequence(runtime, target_tick, true) ||
        !mdkr_rollback_ring_copy(
            &runtime->ring, target_tick, runtime->replayed_snapshot,
            runtime->ring.snapshot_bytes) ||
        memcmp(runtime->baseline_snapshot, runtime->replayed_snapshot,
               runtime->ring.snapshot_bytes) != 0 ||
        !mdkr_net_input_confirm_through(&runtime->net_inputs, target_tick)) {
        log_replay_difference(runtime);
        fprintf(stderr,
                "[ROLLBACK] corrected delayed-input replay was not stable "
                "tick=%u\n",
                target_tick);
        return false;
    }
    confirm_effects_through(runtime, target_tick);
    runtime->resim_complete = true;
    fprintf(stderr,
            "[ROLLBACK] delayed-input correction passed ticks=%u..%u "
            "depth=%u non_input_divergence=1 exact_replay=1\n",
            first_tick, target_tick, MDKR_ROLLBACK_LAB_RESIM_DEPTH);
    return true;
}

bool mdkr_rollback_game_runtime_validate_boundary(unsigned update_rate) {
    uint32_t tick;
    MdkrRollbackInputHistory *history;
    if (!sRollbackGameRuntime.active) {
        return true;
    }
    if (sRollbackGameRuntime.authored_frame_timing_active) {
        const uint64_t finished = rollback_clock_now(NULL);
        mdkr_rollback_timing_record(
            &sRollbackGameRuntime.authored_frame_timing,
            finished >= sRollbackGameRuntime.authored_frame_started_ns
                ? finished - sRollbackGameRuntime.authored_frame_started_ns
                : 0u);
        sRollbackGameRuntime.authored_frame_timing_active = false;
    }
    if (sRollbackGameRuntime.pending_sound) {
        sRollbackGameRuntime.side_effect_error = true;
    }
    if (sRollbackGameRuntime.side_effect_error) {
        fprintf(stderr,
                "[ROLLBACK] side-effect journal rejected the authored stream\n");
        return false;
    }
    if (!mdkr_rollback_validate_live_allocations(
            &sRollbackGameRuntime.registry) ||
        !mdkr_rollback_game_authority_validate_dynamic_coverage(
            &sRollbackGameRuntime.registry)) {
        fprintf(stderr,
                "[ROLLBACK] registered authority allocation lifetime or "
                "coverage changed after freeze\n");
        return false;
    }
    if (sRollbackGameRuntime.validated_boundaries >= UINT32_MAX) {
        fprintf(stderr, "[ROLLBACK] lab tick counter exhausted\n");
        return false;
    }
    tick = (uint32_t)(sRollbackGameRuntime.validated_boundaries + 1u);
    if (!mdkr_rollback_ring_capture(&sRollbackGameRuntime.ring, tick)) {
        fprintf(stderr,
                "[ROLLBACK] boundary snapshot capture failed tick=%u\n",
                (unsigned)tick);
        return false;
    }
    history = &sRollbackGameRuntime
                   .input_history[tick % MDKR_ROLLBACK_LAB_SLOTS];
    if (sRollbackGameRuntime.network_input) {
        if ((!sRollbackGameRuntime.tick_prepared ||
             sRollbackGameRuntime.prepared_tick != tick) && tick == 1u) {
            MdkrInputSample physical_samples[MDKR_INPUT_PORTS];
            MdkrPadSample physical[MDKR_INPUT_PORTS];
            MdkrInputSet frame;
            input_rollback_capture(physical_samples);
            pad_input_samples(physical_samples, physical);
            memset(history, 0, sizeof(*history));
            if (!mdkr_match_input_runtime_drain(
                    tick, physical, MDKR_INPUT_PORTS, &frame) ||
                !bridge_input_samples(&frame, history->input)) {
                fprintf(stderr,
                        "[ROLLBACK] online bootstrap input unavailable tick=1\n");
                return false;
            }
            memcpy(history->received_input, history->input,
                   sizeof(history->received_input));
            history->tick = tick;
            history->update_rate = update_rate;
            history->confirmed_mask = frame.confirmed_mask;
            history->valid = true;
        } else if (!sRollbackGameRuntime.tick_prepared ||
                   sRollbackGameRuntime.prepared_tick != tick ||
                   !history->valid || history->tick != tick) {
            fprintf(stderr,
                    "[ROLLBACK] launcher input boundary was not prepared "
                    "tick=%u\n", tick);
            return false;
        }
        sRollbackGameRuntime.tick_prepared = false;
    } else if (sRollbackGameRuntime.delayed_input_control) {
        if ((!sRollbackGameRuntime.tick_prepared ||
             sRollbackGameRuntime.prepared_tick != tick) &&
            tick == 1u &&
            record_bootstrap_input_boundary(
                &sRollbackGameRuntime, history, tick, update_rate)) {
            /* Level-ready can activate the lab after this frame's pre-tick
             * hook. Treat only that pre-window boundary as fully received. */
        } else if (!sRollbackGameRuntime.tick_prepared ||
                   sRollbackGameRuntime.prepared_tick != tick ||
                   !history->valid || history->tick != tick) {
            fprintf(stderr,
                    "[ROLLBACK] canonical input boundary was not prepared "
                    "tick=%u\n",
                    tick);
            return false;
        }
        sRollbackGameRuntime.tick_prepared = false;
    } else {
        input_rollback_capture(history->input);
        history->tick = tick;
        history->update_rate = update_rate;
        history->valid = true;
    }
    if (sRollbackGameRuntime.roundtrip_once &&
        !sRollbackGameRuntime.roundtrip_complete) {
        if (!mdkr_rollback_ring_restore(
                &sRollbackGameRuntime.ring, tick, true) ||
            !mdkr_rollback_validate_live_allocations(
                &sRollbackGameRuntime.registry)) {
            fprintf(stderr,
                    "[ROLLBACK] first-boundary restore roundtrip failed "
                    "tick=%u\n",
                    (unsigned)tick);
            return false;
        }
        sRollbackGameRuntime.roundtrip_complete = true;
        fprintf(stderr,
                "[ROLLBACK] first-boundary restore roundtrip passed tick=%u\n",
                (unsigned)tick);
    }
    if (sRollbackGameRuntime.delayed_input_control &&
        tick < sRollbackGameRuntime.replay_target_tick -
                   MDKR_ROLLBACK_LAB_RESIM_DEPTH + 1u) {
        if (!mdkr_net_input_confirm_through(
                &sRollbackGameRuntime.net_inputs, tick)) {
            fprintf(stderr,
                    "[ROLLBACK] received input prefix could not confirm "
                    "tick=%u\n",
                    tick);
            return false;
        }
        confirm_effects_through(&sRollbackGameRuntime, tick);
    }
    if (sRollbackGameRuntime.delayed_input_control &&
        !sRollbackGameRuntime.resim_complete &&
        tick == sRollbackGameRuntime.replay_target_tick) {
        if (!reconcile_delayed_inputs(&sRollbackGameRuntime, tick) ||
            !validate_item_probe(&sRollbackGameRuntime)) {
            fprintf(stderr,
                    "[ROLLBACK] corrected item breadth was not observed "
                    "at tick=%u\n", (unsigned)tick);
            return false;
        }
    }
    if (!sRollbackGameRuntime.delayed_input_control &&
        sRollbackGameRuntime.resim_once &&
        !sRollbackGameRuntime.resim_complete &&
        tick == sRollbackGameRuntime.replay_target_tick &&
        !replay_recent_ticks(&sRollbackGameRuntime, tick)) {
        return false;
    }
    if (sRollbackGameRuntime.network_input) {
        /* A correction older than this retained prefix is rejected at ingress.
         * Confirming it now makes irreversible effects safe without coupling
         * the engine to peer acknowledgement or packet details. */
        if (tick >= MDKR_ROLLBACK_LAB_SLOTS) {
            confirm_effects_through(
                &sRollbackGameRuntime,
                tick - (MDKR_ROLLBACK_LAB_SLOTS - 1u));
        }
    } else if (!sRollbackGameRuntime.delayed_input_control) {
        uint32_t confirm_tick = tick;
        if (sRollbackGameRuntime.resim_once &&
            !sRollbackGameRuntime.resim_complete) {
            if (tick <= MDKR_ROLLBACK_LAB_RESIM_DEPTH) {
                confirm_tick = 0u;
            } else {
                confirm_tick = tick - MDKR_ROLLBACK_LAB_RESIM_DEPTH;
            }
        }
        if (confirm_tick != 0u) {
            confirm_effects_through(&sRollbackGameRuntime, confirm_tick);
        }
    }
    sRollbackGameRuntime.validated_boundaries = tick;
    if ((tick % 120u) == 0u) {
        const MdkrRollbackRingStats *stats = &sRollbackGameRuntime.ring.stats;
        fprintf(stderr,
                "[ROLLBACK] sample: ticks=%u capture_avg_ns=%" PRIu64
                " capture_p50_ns=%" PRIu64 " capture_p95_ns=%" PRIu64
                " capture_p99_ns=%" PRIu64 " capture_max_ns=%" PRIu64
                " restores=%" PRIu64 " restore_avg_ns=%" PRIu64
                " restore_p50_ns=%" PRIu64 " restore_p95_ns=%" PRIu64
                " restore_p99_ns=%" PRIu64 " restore_max_ns=%" PRIu64 "\n",
                (unsigned)tick,
                stats->captures != 0u
                    ? stats->capture_ns_total / stats->captures
                    : 0u,
                mdkr_rollback_ring_percentile_ns(stats, true, 50u),
                mdkr_rollback_ring_percentile_ns(stats, true, 95u),
                mdkr_rollback_ring_percentile_ns(stats, true, 99u),
                stats->capture_ns_max, stats->restores,
                stats->restores != 0u
                    ? stats->restore_ns_total / stats->restores
                    : 0u,
                mdkr_rollback_ring_percentile_ns(stats, false, 50u),
                mdkr_rollback_ring_percentile_ns(stats, false, 95u),
                mdkr_rollback_ring_percentile_ns(stats, false, 99u),
                stats->restore_ns_max);
    }
    log_authority_range_hashes(&sRollbackGameRuntime, tick);
    return true;
}
