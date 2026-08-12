/* Narrow launcher/session-to-engine ABI. No service or UI types belong here. */
#ifndef MDKR_SESSION_BRIDGE_H
#define MDKR_SESSION_BRIDGE_H

#include "net/match_manifest.h"
#include "net/match_launch_descriptor.h"
#include "net/net_roster.h"
#include "session_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_SESSION_LAUNCH_VERSION 2u
#define MDKR_SESSION_LAUNCH_V3_VERSION 3u
#define MDKR_MATCH_RESULT_VERSION 1u
#define MDKR_SESSION_EVENT_CAPACITY 16u

typedef struct MdkrPadSample {
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
    uint8_t present;
} MdkrPadSample;

typedef struct MdkrInputSet {
    MdkrPadSample slots[MDKR_SESSION_MAX_PLAYERS];
    uint8_t present_mask;
    uint8_t confirmed_mask;
    uint8_t reserved[2];
} MdkrInputSet;

/* Endpoint-local launch data deliberately wraps, rather than extends, the
 * canonical wire manifest. Two peers may own different local slots while the
 * byte-exact match identity and digest remain identical. */
typedef struct MdkrSessionLaunchV2 {
    uint32_t version;
    uint32_t size;
    MdkrMatchManifestV1 match;
    uint8_t local_slot_mask;
    uint8_t viewport_slot_mask;
    uint8_t reserved[6];
} MdkrSessionLaunchV2;

/* Production-ready launch shape. V2 remains for the isolated rollback
 * laboratory until every launcher path constructs canonical selections; a
 * player-visible online race must use V3. */
typedef struct MdkrSessionLaunchV3 {
    uint32_t version;
    uint32_t size;
    MdkrMatchLaunchDescriptorV1 match;
    uint8_t local_slot_mask;
    uint8_t viewport_slot_mask;
    uint8_t reserved[6];
} MdkrSessionLaunchV3;

typedef struct MdkrMatchResultV1 {
    uint32_t version;
    uint32_t size;
    uint32_t match_epoch;
    uint32_t terminal_tick;
    uint64_t authority_hash;
    uint8_t finishing_order[MDKR_SESSION_MAX_PLAYERS];
    uint8_t player_count;
    uint8_t synchronized;
    uint8_t reserved[2];
} MdkrMatchResultV1;

typedef enum MdkrSessionEventType {
    MDKR_SESSION_EVENT_MANIFEST_APPLIED = 1,
    MDKR_SESSION_EVENT_INPUT_ACCEPTED = 2,
    MDKR_SESSION_EVENT_ENGINE_PHASE = 3,
    MDKR_SESSION_EVENT_RESULT_READY = 4,
    MDKR_SESSION_EVENT_REJECTED = 5
} MdkrSessionEventType;

typedef struct MdkrSessionEvent {
    uint32_t protocol_version;
    MdkrSessionEventType type;
    uint32_t match_epoch;
    uint32_t tick;
    uint32_t value;
} MdkrSessionEvent;

typedef struct MdkrSessionBridge {
    MdkrMatchManifestV1 manifest;
    MdkrMatchLaunchDescriptorV1 launch_descriptor;
    MdkrNetRoster roster;
    uint8_t local_slot_mask;
    uint8_t viewport_slot_mask;
    MdkrInputSet latest_inputs;
    MdkrMatchResultV1 result;
    MdkrSessionEvent events[MDKR_SESSION_EVENT_CAPACITY];
    uint32_t latest_tick;
    uint8_t event_head;
    uint8_t event_count;
    MdkrEnginePhase engine_phase;
    bool have_manifest;
    bool have_inputs;
    bool have_result;
    bool have_launch_descriptor;
} MdkrSessionBridge;

void mdkr_session_bridge_init(MdkrSessionBridge *bridge);
bool mdkr_session_bridge_apply_launch(
    MdkrSessionBridge *bridge, const MdkrSessionLaunchV2 *launch);
bool mdkr_session_bridge_apply_launch_v3(
    MdkrSessionBridge *bridge, const MdkrSessionLaunchV3 *launch);
bool mdkr_session_bridge_submit_inputs(
    MdkrSessionBridge *bridge, uint32_t tick, const MdkrInputSet *inputs);
/* Launcher-owned transport drain. `transport_base` is canonical and may carry
 * remote/predicted slots; `local_samples` is indexed only by this endpoint's
 * frozen local-seat order. The merge can replace local-owned slots only,
 * preserves remote slots exactly, confirms locally authored slots, and is
 * atomic on rejection. */
bool mdkr_session_bridge_submit_local_inputs(
    MdkrSessionBridge *bridge, uint32_t tick,
    const MdkrPadSample *local_samples, unsigned local_sample_count,
    const MdkrInputSet *transport_base);
bool mdkr_session_bridge_poll_event(
    MdkrSessionBridge *bridge, MdkrSessionEvent *event);
bool mdkr_session_bridge_set_engine_phase(
    MdkrSessionBridge *bridge, MdkrEnginePhase phase);
bool mdkr_session_bridge_publish_result(
    MdkrSessionBridge *bridge, const MdkrMatchResultV1 *result);
bool mdkr_session_bridge_export_result(
    const MdkrSessionBridge *bridge, MdkrMatchResultV1 *result);
const MdkrMatchManifestV1 *mdkr_session_bridge_manifest(
    const MdkrSessionBridge *bridge);
const MdkrMatchLaunchDescriptorV1 *mdkr_session_bridge_launch_descriptor(
    const MdkrSessionBridge *bridge);
uint8_t mdkr_session_bridge_local_slot_mask(
    const MdkrSessionBridge *bridge);
uint8_t mdkr_session_bridge_viewport_slot_mask(
    const MdkrSessionBridge *bridge);
const MdkrNetRoster *mdkr_session_bridge_roster(
    const MdkrSessionBridge *bridge);
const MdkrInputSet *mdkr_session_bridge_inputs(
    const MdkrSessionBridge *bridge, uint32_t *tick);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_SESSION_BRIDGE_H */
