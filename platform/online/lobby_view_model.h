/* Pure launcher projection for private-room UI. No sockets, clocks or game UI. */
#ifndef MDKR_ONLINE_LOBBY_VIEW_MODEL_H
#define MDKR_ONLINE_LOBBY_VIEW_MODEL_H

#include "lobby_core.h"
#include "session/session_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrOnlineJourney {
    MDKR_ONLINE_JOURNEY_CREATE = 1,
    MDKR_ONLINE_JOURNEY_JOIN,
    MDKR_ONLINE_JOURNEY_REMATCH
} MdkrOnlineJourney;

typedef enum MdkrOnlineViewKind {
    MDKR_ONLINE_VIEW_ENTRY = 1,
    MDKR_ONLINE_VIEW_CONNECTING,
    MDKR_ONLINE_VIEW_ROOM,
    MDKR_ONLINE_VIEW_PREFLIGHT,
    MDKR_ONLINE_VIEW_SELECTING,
    MDKR_ONLINE_VIEW_LOADING,
    MDKR_ONLINE_VIEW_COUNTDOWN,
    MDKR_ONLINE_VIEW_RACING,
    MDKR_ONLINE_VIEW_RESULTS,
    MDKR_ONLINE_VIEW_RECOVERY
} MdkrOnlineViewKind;

/* Stable product reasons. Adapters translate provider/network failures into
 * this bounded vocabulary; raw messages and transport terms never become UI. */
typedef enum MdkrOnlineViewFailure {
    MDKR_ONLINE_VIEW_FAILURE_NONE = 0,
    MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED,
    MDKR_ONLINE_VIEW_FAILURE_INVITE_ROTATED,
    MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL,
    MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE,
    MDKR_ONLINE_VIEW_FAILURE_SERVICE_BUDGET_SAFE,
    MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD,
    MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM,
    MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS,
    MDKR_ONLINE_VIEW_FAILURE_CONTROLLER_NEEDED,
    MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK,
    MDKR_ONLINE_VIEW_FAILURE_RELAY_CAPACITY,
    MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT,
    MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED,
    MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED,
    MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED,
    MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED,
    MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH,
    MDKR_ONLINE_VIEW_FAILURE_COUNT
} MdkrOnlineViewFailure;

typedef enum MdkrOnlineViewAction {
    MDKR_ONLINE_VIEW_ACTION_NONE = 0,
    MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM,
    MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM,
    MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE,
    MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
    MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS,
    MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
    MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
    MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK,
    MDKR_ONLINE_VIEW_ACTION_READY,
    MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION,
    MDKR_ONLINE_VIEW_ACTION_START_RACE,
    MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN,
    MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK,
    MDKR_ONLINE_VIEW_ACTION_RETRY,
    MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE,
    MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME,
    MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM,
    MDKR_ONLINE_VIEW_ACTION_USE_ROOM_SETTINGS,
    MDKR_ONLINE_VIEW_ACTION_SETUP_CONTROLLER,
    MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR,
    MDKR_ONLINE_VIEW_ACTION_PLAY_HERE,
    MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
    MDKR_ONLINE_VIEW_ACTION_RETURN_HOME,
    MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM,
    MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE
} MdkrOnlineViewAction;

typedef enum MdkrOnlineAnnouncement {
    MDKR_ONLINE_ANNOUNCE_NONE = 0,
    MDKR_ONLINE_ANNOUNCE_POLITE,
    MDKR_ONLINE_ANNOUNCE_ASSERTIVE
} MdkrOnlineAnnouncement;

typedef struct MdkrOnlineViewInput {
    const MdkrSessionState *session;
    const MdkrOnlineLobby *lobby; /* Optional before a room snapshot exists. */
    uint64_t local_endpoint_id;
    MdkrOnlineJourney journey;
    MdkrOnlineViewFailure failure;
    bool invite_ready;
    /* Local release configuration only. Never derive this from room/service
     * data. It remains false until the separately reviewed rollback GO. */
    bool race_admission_enabled;
} MdkrOnlineViewInput;

typedef struct MdkrOnlineViewControl {
    MdkrOnlineViewAction action;
    const char *label;
    bool visible;
    bool enabled;
} MdkrOnlineViewControl;

typedef struct MdkrOnlineTimeoutView {
    bool present;
    const char *title;
    const char *explanation;
    MdkrOnlineViewControl primary;
} MdkrOnlineTimeoutView;

typedef struct MdkrOnlineViewModel {
    MdkrOnlineViewKind kind;
    const char *title;
    const char *explanation;
    const char *status;
    MdkrOnlineViewControl primary;
    MdkrOnlineViewControl secondary;
    MdkrOnlineViewControl cancel;
    MdkrOnlineTimeoutView timeout;
    MdkrOnlineAnnouncement announcement;
    MdkrOnlineViewFailure failure;
    uint8_t member_count;
    uint8_t ready_count;
    uint8_t seat_count;
    bool local_play_available;
    bool local_member_is_leader;
} MdkrOnlineViewModel;

/* Fail-atomic: invalid session/lobby compositions leave output untouched. */
bool mdkr_online_view_model_build(const MdkrOnlineViewInput *input,
                                  MdkrOnlineViewModel *output);

#ifdef __cplusplus
}
#endif
#endif
