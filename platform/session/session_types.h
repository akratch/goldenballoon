/* Shared, wire-stable multiplayer session vocabulary. */
#ifndef MDKR_SESSION_TYPES_H
#define MDKR_SESSION_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_SESSION_PROTOCOL_VERSION 1u
#define MDKR_SESSION_ANY_GENERATION UINT32_MAX
#define MDKR_SESSION_MAX_EFFECTS 2u
#define MDKR_SESSION_MAX_PLAYERS 4u

typedef enum MdkrPlayIntent {
    MDKR_INTENT_NONE = 0,
    MDKR_INTENT_LOCAL = 1,
    MDKR_INTENT_ONLINE_PRIVATE = 2
} MdkrPlayIntent;

typedef enum MdkrSessionScene {
    MDKR_SCENE_HOME = 0,
    MDKR_SCENE_LOCAL_SETUP = 1,
    MDKR_SCENE_JOIN = 2,
    MDKR_SCENE_LOBBY = 3,
    MDKR_SCENE_LOADING = 4,
    MDKR_SCENE_RACE_CHROME = 5,
    MDKR_SCENE_RESULTS = 6,
    MDKR_SCENE_RECOVERY = 7
} MdkrSessionScene;

typedef enum MdkrEnginePhase {
    MDKR_ENGINE_STOPPED = 0,
    MDKR_ENGINE_BOOTING = 1,
    MDKR_ENGINE_READY = 2,
    MDKR_ENGINE_RACING = 3,
    MDKR_ENGINE_FINISHED = 4,
    MDKR_ENGINE_FAILED = 5
} MdkrEnginePhase;

typedef enum MdkrConnectivity {
    MDKR_CONNECTIVITY_OFFLINE = 0,
    MDKR_CONNECTIVITY_CONNECTING = 1,
    MDKR_CONNECTIVITY_DIRECT = 2,
    MDKR_CONNECTIVITY_FORWARDED = 3,
    MDKR_CONNECTIVITY_RELAYED = 4,
    MDKR_CONNECTIVITY_DEGRADED = 5,
    MDKR_CONNECTIVITY_LOST = 6
} MdkrConnectivity;

typedef enum MdkrRoomPhase {
    MDKR_ROOM_NONE = 0,
    MDKR_ROOM_OPEN = 1,
    MDKR_ROOM_PREFLIGHT = 2,
    MDKR_ROOM_SELECTING = 3,
    MDKR_ROOM_LOADING = 4,
    MDKR_ROOM_COUNTDOWN = 5,
    MDKR_ROOM_RACING = 6,
    MDKR_ROOM_RESULTS = 7,
    MDKR_ROOM_CLOSED = 8
} MdkrRoomPhase;

typedef enum MdkrOverlayState {
    MDKR_OVERLAY_CLOSED = 0,
    MDKR_OVERLAY_CONTROLS = 1,
    MDKR_OVERLAY_CONNECTION = 2,
    MDKR_OVERLAY_LEAVE_CONFIRM = 3
} MdkrOverlayState;

typedef enum MdkrUpdateState {
    MDKR_UPDATE_NONE = 0,
    MDKR_UPDATE_WAITING = 1,
    MDKR_UPDATE_REQUIRED = 2
} MdkrUpdateState;

typedef enum MdkrSessionError {
    MDKR_SESSION_ERROR_NONE = 0,
    MDKR_SESSION_ERROR_STALE_COMMAND = 1,
    MDKR_SESSION_ERROR_INVALID_TRANSITION = 2,
    MDKR_SESSION_ERROR_INVALID_COMPOSITION = 3,
    MDKR_SESSION_ERROR_UPDATE_REQUIRED = 4,
    MDKR_SESSION_ERROR_ENGINE_FAILED = 5,
    MDKR_SESSION_ERROR_CONNECTION_LOST = 6,
    MDKR_SESSION_ERROR_UNSUPPORTED = 7,
    MDKR_SESSION_ERROR_CAPACITY = 8,
    MDKR_SESSION_ERROR_PROTOCOL = 9
} MdkrSessionError;

typedef enum MdkrSessionCommandType {
    MDKR_SESSION_COMMAND_BEGIN_LOCAL = 1,
    MDKR_SESSION_COMMAND_BEGIN_ONLINE = 2,
    MDKR_SESSION_COMMAND_SET_ROOM_PHASE = 3,
    MDKR_SESSION_COMMAND_SET_CONNECTIVITY = 4,
    MDKR_SESSION_COMMAND_REQUEST_RACE = 5,
    MDKR_SESSION_COMMAND_SET_ENGINE_PHASE = 6,
    MDKR_SESSION_COMMAND_OPEN_OVERLAY = 7,
    MDKR_SESSION_COMMAND_CLOSE_OVERLAY = 8,
    MDKR_SESSION_COMMAND_REMATCH = 9,
    MDKR_SESSION_COMMAND_CANCEL = 10,
    MDKR_SESSION_COMMAND_RETURN_HOME = 11,
    MDKR_SESSION_COMMAND_SET_UPDATE_STATE = 12,
    MDKR_SESSION_COMMAND_RECOVER = 13,
    MDKR_SESSION_COMMAND_RETURN_TO_LOBBY = 14,
    /* Confirmed online-race exit. Kept distinct from RETURN_HOME so an
     * unconfirmed Back action can never stop a running engine. */
    MDKR_SESSION_COMMAND_LEAVE_ONLINE_RACE = 15
} MdkrSessionCommandType;

typedef enum MdkrSessionEffectType {
    MDKR_SESSION_EFFECT_NONE = 0,
    MDKR_SESSION_EFFECT_START_ENGINE = 1,
    MDKR_SESSION_EFFECT_STOP_ENGINE = 2,
    MDKR_SESSION_EFFECT_SET_ENGINE_PAUSED = 3,
    MDKR_SESSION_EFFECT_CONNECT_PRIVATE_ROOM = 4,
    MDKR_SESSION_EFFECT_DISCONNECT_ROOM = 5,
    MDKR_SESSION_EFFECT_ACTIVATE_UPDATE = 6
} MdkrSessionEffectType;

typedef struct MdkrSessionCommand {
    uint32_t protocol_version;
    uint32_t expected_generation;
    MdkrSessionCommandType type;
    uint32_t value;
} MdkrSessionCommand;

typedef struct MdkrSessionEffect {
    uint32_t protocol_version;
    uint32_t generation;
    uint32_t match_epoch;
    uint32_t effect_id;
    MdkrSessionEffectType type;
    uint32_t value;
} MdkrSessionEffect;

typedef struct MdkrSessionState {
    uint32_t protocol_version;
    uint32_t generation;
    uint32_t match_epoch;
    uint64_t session_id;
    MdkrPlayIntent intent;
    MdkrSessionScene scene;
    MdkrEnginePhase engine;
    MdkrConnectivity connectivity;
    MdkrRoomPhase room;
    MdkrOverlayState overlay;
    MdkrUpdateState update;
    MdkrSessionError last_error;
    bool engine_pause_requested;
} MdkrSessionState;

typedef struct MdkrSessionStep {
    bool accepted;
    MdkrSessionError error;
    uint8_t effect_count;
    MdkrSessionEffect effects[MDKR_SESSION_MAX_EFFECTS];
} MdkrSessionStep;

#ifdef __cplusplus
}
#endif

#endif /* MDKR_SESSION_TYPES_H */
