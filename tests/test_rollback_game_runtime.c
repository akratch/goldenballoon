/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "PR/os_cont.h"
#include "platform/input_tick_queue.h"
#include "platform/net/match_input_runtime.h"
#include "platform/net/net_roster_runtime.h"
#include "platform/rollback/rollback_game_runtime.h"
#include "platform/rollback/rollback_ring.h"
#include "objects.h"
#include "rollback_authority_view.h"

static int sRequested;
static int sAuthorityOk = 1;
static int sFreezeOk = 1;
static int sRingOk = 1;
static int sCaptureOk = 1;
static int sLiveAllocationsOk = 1;
static int sTraceAllocations;
static int sRoundtripOnce;
static int sResimOnce;
static int sMutationControl;
static int sDelayedInputControl;
static int sCaptureCalls;
static int sRestoreCalls;
static int sResimCalls;
static int sWorldState;
static int sSnapshotWorld[8];
static MdkrInputSample sAppliedInput[MDKR_INPUT_PORTS];
static int sInitCalls;
static int sAuthorityCalls;
static int sDestroyCalls;
static MdkrMempoolMutationObserver sMutationObserver;
static void *sMutationContext;
static unsigned sRumbleCancelCalls;
static unsigned sRumbleCancelController;
static int sNetworkInput;
static int sLoadedTrack = 5;
static int sLoadedRaceType = MDKR_MATCH_RACE_TYPE_STANDARD;
static int sLoadedVehicleMask = 1;
static MdkrMatchManifestV1 sManifest;

s32 transition_workspace_preload(void) { return TRUE; }
uint64_t platform_perf_monotonic_ns(void) { return 1u; }

bool mdkr_match_input_runtime_active(void) { return sNetworkInput != 0; }
uint32_t mdkr_match_input_runtime_epoch(void) { return 0u; }
bool mdkr_match_input_runtime_drain(
    uint32_t tick, const MdkrPadSample physical[MDKR_SESSION_MAX_PLAYERS],
    unsigned count, MdkrInputSet *out) {
    (void)tick; (void)physical; (void)count; (void)out;
    return false;
}
bool mdkr_match_input_runtime_inputs_for_tick(
    uint32_t tick, MdkrInputSet *out) {
    (void)tick; (void)out;
    return false;
}
bool mdkr_match_input_runtime_take_dirty(uint32_t *tick) {
    (void)tick;
    return false;
}
bool mdkr_match_input_runtime_begin_tick(uint32_t tick) {
    (void)tick;
    return true;
}

const MdkrMatchManifestV1 *mdkr_net_roster_runtime_manifest(void) {
    return sNetworkInput ? &sManifest : NULL;
}

s32 level_id(void) { return sLoadedTrack; }
s8 leveltable_type(s32 mapId) {
    assert(mapId == sLoadedTrack);
    return (s8)sLoadedRaceType;
}
s32 leveltable_vehicle_usable(s32 mapId) {
    assert(mapId == sLoadedTrack);
    return sLoadedVehicleMask;
}

s32 *get_misc_asset(s32 index) {
    (void)index;
    return NULL;
}

Object *get_racer_object_by_port(s32 index) {
    (void)index;
    return NULL;
}

s32 mdkr_object_assets_pin_rollback(void) { return TRUE; }
void mdkr_object_assets_unpin_rollback(void) {}

void mdkr_rollback_rumble_cancel_preview(unsigned controller_index) {
    sRumbleCancelCalls++;
    sRumbleCancelController = controller_index;
}

int port_env_bool(const char *name, int default_on, const char *help) {
    (void)default_on;
    (void)help;
    if (strcmp(name, "MDKR_ROLLBACK_TRACE_ALLOCATIONS") == 0) {
        return sTraceAllocations;
    }
    if (strcmp(name, "MDKR_ROLLBACK_LAB_ROUNDTRIP") == 0) {
        return sRoundtripOnce;
    }
    if (strcmp(name, "MDKR_ROLLBACK_LAB_RESIM") == 0) {
        return sResimOnce;
    }
    if (strcmp(name, "MDKR_ROLLBACK_LAB_MUTATION_CONTROL") == 0) {
        return sMutationControl;
    }
    if (strcmp(name, "MDKR_ROLLBACK_LAB_DELAYED_INPUT") == 0) {
        return sDelayedInputControl;
    }
    return sRequested;
}

uint32_t port_env_u32(
    const char *name, uint32_t default_value, uint32_t minimum,
    uint32_t maximum, const char *help) {
    (void)name;
    (void)minimum;
    (void)maximum;
    (void)help;
    return default_value;
}

void mdkr_rollback_snapshot_registry_init(
    MdkrRollbackSnapshotRegistry *registry, uint64_t process_cookie) {
    memset(registry, 0, sizeof(*registry));
    registry->process_cookie = process_cookie;
    sInitCalls++;
}

bool mdkr_rollback_game_authority_register(
    MdkrRollbackSnapshotRegistry *registry) {
    sAuthorityCalls++;
    if (!sAuthorityOk) return false;
    registry->range_count = 1u;
    registry->total_bytes = 4u;
    registry->ranges[0].address = &sWorldState;
    registry->ranges[0].size = sizeof(sWorldState);
    registry->ranges[0].tag = 1u;
    return true;
}

bool mdkr_rollback_game_authority_validate_dynamic_coverage(
    const MdkrRollbackSnapshotRegistry *registry) {
    return registry != NULL;
}

bool mdkr_rollback_game_authority_is_input_tag(uint32_t tag) {
    return tag >= 1000u;
}

bool mdkr_rollback_snapshot_freeze(
    MdkrRollbackSnapshotRegistry *registry, uint64_t manifest_digest) {
    if (!sFreezeOk) return false;
    registry->frozen = true;
    registry->manifest_digest = manifest_digest;
    return true;
}

size_t mdkr_rollback_snapshot_bytes(
    const MdkrRollbackSnapshotRegistry *registry) {
    return registry->frozen ? 100u : 0u;
}

bool mdkr_rollback_ring_init(
    MdkrRollbackRing *ring, const MdkrRollbackSnapshotRegistry *registry,
    unsigned slots) {
    (void)registry;
    assert(slots == MDKR_ROLLBACK_SNAPSHOT_SLOTS);
    if (!sRingOk) return false;
    memset(ring, 0, sizeof(*ring));
    ring->storage = (uint8_t *)(uintptr_t)1u;
    ring->snapshot_bytes = 100u;
    ring->stats.allocated_bytes =
        100u * MDKR_ROLLBACK_SNAPSHOT_SLOTS;
    return true;
}

bool mdkr_rollback_ring_capture(MdkrRollbackRing *ring, uint32_t tick) {
    assert(ring->storage != NULL);
    if (sCaptureCalls <= 4) {
        assert(tick == (uint32_t)sCaptureCalls);
    } else {
        assert(tick >= 1u && tick <= 4u);
    }
    sSnapshotWorld[tick % 8u] = sWorldState;
    sCaptureCalls++;
    return sCaptureOk != 0;
}

bool mdkr_rollback_ring_restore(
    MdkrRollbackRing *ring, uint32_t tick, bool at_tick_boundary) {
    assert(ring->storage != NULL && (tick == 0u || tick == 1u) &&
           at_tick_boundary);
    sWorldState = sSnapshotWorld[tick % 8u];
    sRestoreCalls++;
    return true;
}

bool mdkr_rollback_ring_has(const MdkrRollbackRing *ring, uint32_t tick) {
    return ring != NULL && ring->storage != NULL && tick <= 4u;
}

bool mdkr_rollback_ring_copy(
    const MdkrRollbackRing *ring, uint32_t tick, void *output,
    size_t output_size) {
    assert(ring->storage != NULL && output != NULL && output_size == 100u);
    memset(output, 0, output_size);
    memcpy(output, &tick, sizeof(tick));
    memcpy((uint8_t *)output + sizeof(MdkrRollbackSnapshotHeader),
           &sSnapshotWorld[tick % 8u], sizeof(sWorldState));
    return true;
}

uint64_t mdkr_rollback_ring_percentile_ns(
    const MdkrRollbackRingStats *stats, bool capture, unsigned percentile) {
    (void)stats;
    (void)capture;
    (void)percentile;
    return 0u;
}

void mdkr_rollback_timing_record(
    MdkrRollbackTimingHistogram *histogram, uint64_t duration_ns) {
    assert(histogram != NULL);
    histogram->samples++;
    histogram->ns_total += duration_ns;
    if (duration_ns > histogram->ns_max) histogram->ns_max = duration_ns;
}

uint64_t mdkr_rollback_timing_percentile_ns(
    const MdkrRollbackTimingHistogram *histogram, unsigned percentile) {
    assert(histogram != NULL && percentile > 0u && percentile <= 100u);
    return histogram->ns_max;
}

void input_rollback_capture(MdkrInputSample out[MDKR_INPUT_PORTS]) {
    memset(out, 0, sizeof(*out) * MDKR_INPUT_PORTS);
    out[0].present = true;
}

void input_rollback_apply_from_previous(
    const MdkrInputSample input[MDKR_INPUT_PORTS],
    const MdkrInputSample previous[MDKR_INPUT_PORTS]) {
    assert(input != NULL && previous != NULL);
    memcpy(sAppliedInput, input, sizeof(sAppliedInput));
}

s32 mdkr_game_resimulate_tick(
    s32 update_rate, const MdkrInputSample input[MDKR_INPUT_PORTS]) {
    assert(update_rate == 1 && input != NULL && input[0].present);
    assert(!mdkr_rollback_game_runtime_host_io_allowed(false));
    assert(!mdkr_rollback_game_runtime_host_io_allowed(true));
    sWorldState += input[0].stick_x;
    if ((input[0].buttons & A_BUTTON) != 0u) sWorldState++;
    sResimCalls++;
    return TRUE;
}

void mdkr_rollback_ring_destroy(MdkrRollbackRing *ring) {
    if (ring->storage != NULL) sDestroyCalls++;
    memset(ring, 0, sizeof(*ring));
}

void mdkr_rollback_ring_set_clock(
    MdkrRollbackRing *ring, MdkrRollbackClockFn clock, void *context) {
    assert(ring != NULL && clock != NULL && context == NULL);
    ring->clock = clock;
}

bool mdkr_rollback_validate_live_allocations(
    MdkrRollbackSnapshotRegistry *registry) {
    (void)registry;
    return sLiveAllocationsOk != 0;
}

s32 mdkr_mempool_allocation_span_in_pool(
    MemoryPools pool_index, const void *address, void **allocation_base,
    size_t *allocation_size) {
    (void)pool_index;
    (void)address;
    *allocation_base = NULL;
    *allocation_size = 0u;
    return FALSE;
}

s32 mdkr_mempool_allocation_span(
    const void *address, void **allocation_base, size_t *allocation_size) {
    (void)address;
    *allocation_base = NULL;
    *allocation_size = 0u;
    return FALSE;
}

s32 mdkr_object_rollback_identify_address(
    const void *allocation_base, const void *address,
    MdkrObjectRollbackAddressInfo *info) {
    (void)allocation_base;
    (void)address;
    (void)info;
    return FALSE;
}

s32 mdkr_object_rollback_describe_object(
    const void *object_pointer, MdkrObjectRollbackAddressInfo *info) {
    (void)object_pointer;
    (void)info;
    return FALSE;
}

void mdkr_mempool_set_mutation_observer(
    MdkrMempoolMutationObserver observer, void *context) {
    sMutationObserver = observer;
    sMutationContext = context;
}

static void reset_controls(void) {
    mdkr_rollback_game_runtime_level_end();
    sRequested = 0;
    sAuthorityOk = 1;
    sFreezeOk = 1;
    sRingOk = 1;
    sCaptureOk = 1;
    sLiveAllocationsOk = 1;
    sTraceAllocations = 0;
    sRoundtripOnce = 0;
    sResimOnce = 0;
    sMutationControl = 0;
    sDelayedInputControl = 0;
    sCaptureCalls = 0;
    sRestoreCalls = 0;
    sResimCalls = 0;
    sWorldState = 0;
    memset(sSnapshotWorld, 0, sizeof(sSnapshotWorld));
    memset(sAppliedInput, 0, sizeof(sAppliedInput));
    sInitCalls = 0;
    sAuthorityCalls = 0;
    sDestroyCalls = 0;
    sMutationObserver = NULL;
    sMutationContext = NULL;
    sRumbleCancelCalls = 0u;
    sRumbleCancelController = UINT32_MAX;
    sNetworkInput = 0;
    sLoadedTrack = 5;
    sLoadedRaceType = MDKR_MATCH_RACE_TYPE_STANDARD;
    sLoadedVehicleMask = 1;
    memset(&sManifest, 0, sizeof(sManifest));
    sManifest.match_epoch = 1u;
    sManifest.protocol_version = 1u;
    sManifest.build_id[0] = 1u;
    sManifest.gameplay_digest[0] = 1u;
    sManifest.slot_owner[0] = 1u;
    sManifest.slot_owner[1] = 2u;
    sManifest.rng_seed = 1u;
    sManifest.track_id = 5u;
    sManifest.rom_revision = MDKR_ROM_US_11;
    sManifest.cadence_hz = 30u;
    sManifest.slot_count = 2u;
    sManifest.rules = MDKR_MATCH_RULES_STANDARD_RACE;
    sManifest.vehicle_mask = 1u;
}

int main(void) {
    reset_controls();
    assert(mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 0 && sAuthorityCalls == 0 && sDestroyCalls == 0);
    assert(mdkr_rollback_game_runtime_host_io_allowed(true));
    assert(mdkr_rollback_game_runtime_host_io_allowed(false));

    reset_controls();
    sNetworkInput = 1;
    assert(mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 1 && sAuthorityCalls == 1);
    mdkr_rollback_game_runtime_level_end();

    reset_controls();
    sNetworkInput = 1;
    sLoadedTrack = 6;
    assert(!mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 0 && sAuthorityCalls == 0);

    reset_controls();
    sNetworkInput = 1;
    sLoadedRaceType = 64;
    assert(!mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 0 && sAuthorityCalls == 0);

    reset_controls();
    sNetworkInput = 1;
    sManifest.cadence_hz = 25u;
    assert(!mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 0 && sAuthorityCalls == 0);

    reset_controls();
    sNetworkInput = 1;
    sLoadedVehicleMask = 3;
    assert(!mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 0 && sAuthorityCalls == 0);

    reset_controls();
    sRequested = 1;
    assert(mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 1 && sAuthorityCalls == 1 && sDestroyCalls == 0);
    assert(sMutationObserver == NULL && sMutationContext == NULL);
    assert(!mdkr_rollback_game_runtime_host_io_allowed(true));
    assert(mdkr_rollback_game_runtime_host_io_allowed(false));
    assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    assert(sCaptureCalls == 2 && sRestoreCalls == 0);
    sLiveAllocationsOk = 0;
    assert(!mdkr_rollback_game_runtime_validate_boundary(1u));
    mdkr_rollback_game_runtime_level_end();

    reset_controls();
    sRequested = 1;
    sDelayedInputControl = 1;
    assert(mdkr_rollback_game_runtime_level_ready());
    for (unsigned tick = 1u; tick <= 4u; tick++) {
        assert(mdkr_rollback_game_runtime_prepare_tick(1u));
        sWorldState += sAppliedInput[0].stick_x;
        if ((sAppliedInput[0].buttons & A_BUTTON) != 0u) sWorldState++;
        assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    }
    assert(sResimCalls == 8 && sCaptureCalls == 13 && sRestoreCalls == 2);
    assert(mdkr_rollback_game_runtime_host_io_allowed(false));
    assert(!mdkr_rollback_game_runtime_host_io_allowed(true));
    mdkr_rollback_game_runtime_level_end();
    assert(sDestroyCalls == 1);

    reset_controls();
    sRequested = 1;
    sTraceAllocations = 1;
    assert(mdkr_rollback_game_runtime_level_ready());
    assert(sMutationObserver != NULL && sMutationContext != NULL);
    mdkr_rollback_game_runtime_level_end();

    reset_controls();
    sRequested = 1;
    sRoundtripOnce = 1;
    assert(mdkr_rollback_game_runtime_level_ready());
    assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    assert(sCaptureCalls == 3 && sRestoreCalls == 1);
    mdkr_rollback_game_runtime_level_end();

    reset_controls();
    sRequested = 1;
    sResimOnce = 1;
    assert(mdkr_rollback_game_runtime_level_ready());
    assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    assert(mdkr_rollback_game_runtime_validate_boundary(1u));
    assert(sResimCalls == 4 && sCaptureCalls == 9 && sRestoreCalls == 1);
    mdkr_rollback_game_runtime_level_end();

    reset_controls();
    sRequested = 1;
    sAuthorityOk = 0;
    assert(!mdkr_rollback_game_runtime_level_ready());
    assert(sInitCalls == 1 && sAuthorityCalls == 1 && sDestroyCalls == 0);

    reset_controls();
    sRequested = 1;
    sCaptureOk = 0;
    assert(!mdkr_rollback_game_runtime_level_ready());
    assert(sDestroyCalls == 1);
    puts("test_rollback_game_runtime: PASS");
    return 0;
}
