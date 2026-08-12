#include "lobby_fake_adapter.h"

#include <string.h>

static MdkrOnlineFakeStep fake_step(const MdkrOnlineFakeAdapter *adapter,
                                    bool accepted, bool duplicate,
                                    MdkrOnlineFakeError error) {
    MdkrOnlineFakeStep step;
    memset(&step, 0, sizeof(step));
    step.accepted = accepted;
    step.duplicate = duplicate;
    step.error = error;
    if (adapter != NULL) {
        step.revision = adapter->revision;
        step.pending_token = adapter->pending_token;
    }
    return step;
}

static uint64_t request_fingerprint(const MdkrOnlineFakeCommand *command) {
    uint64_t value = UINT64_C(1469598103934665603);
    const uint32_t fields[] = {
        (uint32_t)command->action, command->seat, command->value
    };
    unsigned field;
    unsigned byte;
    for (field = 0u; field < sizeof(fields) / sizeof(fields[0]); field++) {
        for (byte = 0u; byte < 4u; byte++) {
            value ^= (fields[field] >> (byte * 8u)) & 0xffu;
            value *= UINT64_C(1099511628211);
        }
    }
    return value;
}

static bool session_dispatch(MdkrOnlineFakeAdapter *adapter,
                             MdkrSessionCommandType type, uint32_t value) {
    MdkrSessionCommand command;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    command.expected_generation = adapter->session.state.generation;
    command.type = type;
    command.value = value;
    return mdkr_session_core_dispatch(&adapter->session, &command).accepted;
}

static bool lobby_dispatch(MdkrOnlineFakeAdapter *adapter, uint64_t actor,
                           MdkrOnlineCommandType type, uint32_t seat,
                           uint32_t value) {
    MdkrOnlineCommand command;
    uint64_t *next_id = actor == adapter->local_endpoint_id
        ? &adapter->next_local_command_id : &adapter->next_peer_command_id;
    MdkrOnlineStep step;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    command.expected_revision = adapter->lobby.revision;
    command.command_id = *next_id;
    command.actor_endpoint_id = actor;
    command.type = type;
    command.target_endpoint_id = seat;
    command.value = value;
    if (type == MDKR_ONLINE_JOIN) command.compatibility = adapter->compatibility;
    step = mdkr_online_lobby_dispatch(&adapter->lobby, &command);
    if (!step.accepted) return false;
    (*next_id)++;
    return true;
}

static bool action_allowed(const MdkrOnlineFakeAdapter *adapter,
                           MdkrOnlineViewAction action) {
    MdkrOnlineViewModel model;
    if (!mdkr_online_fake_view(adapter, &model)) return false;
    return (model.primary.visible && model.primary.enabled &&
            model.primary.action == action) ||
           (model.secondary.visible && model.secondary.enabled &&
            model.secondary.action == action) ||
           (model.cancel.visible && model.cancel.enabled &&
            model.cancel.action == action) ||
           (adapter->timeout_expired && model.timeout.present &&
            model.timeout.primary.visible && model.timeout.primary.enabled &&
            model.timeout.primary.action == action);
}

static void publish_pending(MdkrOnlineFakeAdapter *adapter,
                            MdkrOnlineFakePending pending) {
    adapter->pending = pending;
    adapter->pending_token = adapter->next_pending_token++;
    if (adapter->next_pending_token == 0u) adapter->next_pending_token = 1u;
}

static bool local_seat_owned(const MdkrOnlineFakeAdapter *adapter,
                             uint32_t seat) {
    return adapter->have_lobby && seat < MDKR_ONLINE_MAX_SEATS &&
        adapter->lobby.seats[seat].occupied &&
        adapter->lobby.seats[seat].endpoint_id == adapter->local_endpoint_id;
}

static bool begin_room(MdkrOnlineFakeAdapter *adapter,
                       MdkrOnlineJourney journey) {
    if (!session_dispatch(adapter, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u)) {
        return false;
    }
    adapter->journey = journey;
    publish_pending(adapter, journey == MDKR_ONLINE_JOURNEY_CREATE
        ? MDKR_ONLINE_FAKE_PENDING_CREATE : MDKR_ONLINE_FAKE_PENDING_JOIN);
    return true;
}

static bool enter_loading(MdkrOnlineFakeAdapter *adapter,
                          uint8_t legal_vehicle_mask) {
    if (!lobby_dispatch(adapter, adapter->local_endpoint_id,
                        MDKR_ONLINE_BEGIN_LOADING, 0u,
                        legal_vehicle_mask) ||
        !session_dispatch(adapter, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                          MDKR_ROOM_LOADING) ||
        !session_dispatch(adapter, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u)) {
        return false;
    }
    publish_pending(adapter, MDKR_ONLINE_FAKE_PENDING_LOAD);
    return true;
}

bool mdkr_online_fake_init(MdkrOnlineFakeAdapter *adapter,
                           uint64_t session_id,
                           const MdkrOnlineCompatibilityV1 *compatibility,
                           bool race_admission_enabled) {
    MdkrOnlineFakeAdapter next;
    if (adapter == NULL ||
        !mdkr_online_compatibility_valid(compatibility)) return false;
    memset(&next, 0, sizeof(next));
    mdkr_session_core_init(&next.session, session_id);
    next.compatibility = *compatibility;
    next.local_endpoint_id = UINT64_C(0x101);
    next.peer_endpoint_id = UINT64_C(0x202);
    next.next_local_command_id = 1u;
    next.next_peer_command_id = 1u;
    next.journey = MDKR_ONLINE_JOURNEY_CREATE;
    next.revision = 1u;
    next.next_pending_token = 1u;
    next.race_admission_enabled = race_admission_enabled;
    *adapter = next;
    return true;
}

bool mdkr_online_fake_view(const MdkrOnlineFakeAdapter *adapter,
                           MdkrOnlineViewModel *output) {
    MdkrOnlineViewInput input;
    if (adapter == NULL || output == NULL) return false;
    memset(&input, 0, sizeof(input));
    input.session = &adapter->session.state;
    input.lobby = adapter->have_lobby ? &adapter->lobby : NULL;
    input.local_endpoint_id = adapter->local_endpoint_id;
    input.journey = adapter->journey;
    input.failure = adapter->failure;
    input.invite_ready = adapter->invite_ready;
    input.race_admission_enabled = adapter->race_admission_enabled;
    return mdkr_online_view_model_build(&input, output);
}

MdkrOnlineFakeStep mdkr_online_fake_dispatch(
    MdkrOnlineFakeAdapter *adapter, const MdkrOnlineFakeCommand *command) {
    MdkrOnlineFakeAdapter next;
    MdkrOnlineFakeStep step;
    uint64_t fingerprint;
    bool accepted = false;
    bool timeout_expired;

    if (adapter == NULL || command == NULL || command->request_id == 0u) {
        return fake_step(adapter, false, false, MDKR_ONLINE_FAKE_ERROR_PROTOCOL);
    }
    fingerprint = request_fingerprint(command);
    if (command->request_id == adapter->last_request_id) {
        if (fingerprint != adapter->last_request_fingerprint) {
            return fake_step(adapter, false, false,
                             MDKR_ONLINE_FAKE_ERROR_REQUEST_CONFLICT);
        }
        step = adapter->last_request_step;
        step.duplicate = true;
        return step;
    }
    if (command->request_id < adapter->last_request_id) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_STALE_REQUEST);
    }
    if (command->expected_revision != adapter->revision) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_STALE_VIEW);
    }
    if (adapter->pending != MDKR_ONLINE_FAKE_PENDING_NONE &&
        command->action != MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY &&
        !(adapter->timeout_expired &&
          command->action == MDKR_ONLINE_VIEW_ACTION_RETRY)) {
        return fake_step(adapter, false, false, MDKR_ONLINE_FAKE_ERROR_PENDING);
    }
    if (!action_allowed(adapter, command->action)) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_INVALID_ACTION);
    }

    next = *adapter;
    timeout_expired = next.timeout_expired;
    next.timeout_expired = false;
    switch (command->action) {
        case MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM:
            accepted = begin_room(&next, MDKR_ONLINE_JOURNEY_CREATE);
            break;
        case MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM:
            accepted = begin_room(&next, MDKR_ONLINE_JOURNEY_JOIN);
            break;
        case MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE:
            if (next.journey != MDKR_ONLINE_JOURNEY_CREATE ||
                next.lobby.member_count != 1u) break;
            publish_pending(&next, MDKR_ONLINE_FAKE_PENDING_PEER_JOIN);
            accepted = true;
            break;
        case MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP:
            accepted = session_dispatch(
                &next, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                MDKR_ROOM_PREFLIGHT);
            if (accepted) publish_pending(
                &next, MDKR_ONLINE_FAKE_PENDING_PREFLIGHT);
            break;
        case MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER:
            accepted = local_seat_owned(&next, command->seat) &&
                lobby_dispatch(&next, next.local_endpoint_id,
                               MDKR_ONLINE_SET_CHARACTER,
                               command->seat, command->value);
            break;
        case MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE:
            accepted = local_seat_owned(&next, command->seat) &&
                lobby_dispatch(&next, next.local_endpoint_id,
                               MDKR_ONLINE_SET_VEHICLE,
                               command->seat, command->value);
            break;
        case MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK:
            accepted = local_seat_owned(&next, command->seat) &&
                lobby_dispatch(&next, next.local_endpoint_id,
                               MDKR_ONLINE_SET_VOTE,
                               command->seat, command->value);
            break;
        case MDKR_ONLINE_VIEW_ACTION_READY:
            accepted = lobby_dispatch(&next, next.local_endpoint_id,
                                      MDKR_ONLINE_SET_READY, 0u, 1u);
            break;
        case MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION:
            accepted = lobby_dispatch(&next, next.local_endpoint_id,
                                      MDKR_ONLINE_SET_READY, 0u, 0u);
            break;
        case MDKR_ONLINE_VIEW_ACTION_START_RACE:
            accepted = enter_loading(&next, (uint8_t)command->value);
            break;
        case MDKR_ONLINE_VIEW_ACTION_RETRY:
            if (!timeout_expired &&
                next.failure == MDKR_ONLINE_VIEW_FAILURE_NONE) break;
            if (next.failure != MDKR_ONLINE_VIEW_FAILURE_NONE) {
                next.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
                if (next.have_lobby &&
                    next.session.state.room == MDKR_ROOM_PREFLIGHT) {
                    publish_pending(&next, MDKR_ONLINE_FAKE_PENDING_PREFLIGHT);
                } else {
                    publish_pending(
                        &next, next.journey == MDKR_ONLINE_JOURNEY_JOIN
                            ? MDKR_ONLINE_FAKE_PENDING_JOIN
                            : MDKR_ONLINE_FAKE_PENDING_CREATE);
                }
                accepted = true;
            } else if (next.pending == MDKR_ONLINE_FAKE_PENDING_CREATE ||
                       next.pending == MDKR_ONLINE_FAKE_PENDING_JOIN ||
                       next.pending == MDKR_ONLINE_FAKE_PENDING_PREFLIGHT) {
                publish_pending(&next, next.pending);
                accepted = true;
            }
            break;
        case MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE:
            accepted = session_dispatch(
                &next, MDKR_SESSION_COMMAND_RETURN_HOME, 0u);
            if (accepted) {
                next.have_lobby = false;
                next.invite_ready = false;
                next.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
                accepted = begin_room(&next, MDKR_ONLINE_JOURNEY_JOIN);
            }
            break;
        case MDKR_ONLINE_VIEW_ACTION_USE_ROOM_SETTINGS:
            if (!next.have_lobby ||
                next.session.state.room != MDKR_ROOM_PREFLIGHT ||
                next.failure != MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS) {
                break;
            }
            next.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
            publish_pending(&next, MDKR_ONLINE_FAKE_PENDING_PREFLIGHT);
            accepted = true;
            break;
        case MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY:
            accepted = next.have_lobby &&
                lobby_dispatch(&next, next.local_endpoint_id,
                               MDKR_ONLINE_CANCEL_LOADING, 0u, 0u) &&
                session_dispatch(&next,
                                 MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
            if (accepted) {
                next.pending = MDKR_ONLINE_FAKE_PENDING_NONE;
                next.pending_token = 0u;
                next.journey = MDKR_ONLINE_JOURNEY_REMATCH;
            }
            break;
        case MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN:
            accepted = next.have_lobby &&
                next.lobby.leader_endpoint_id == next.local_endpoint_id &&
                lobby_dispatch(&next, next.local_endpoint_id,
                               MDKR_ONLINE_REMATCH, 0u, 0u) &&
                session_dispatch(&next,
                                 MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
            if (accepted) next.journey = MDKR_ONLINE_JOURNEY_REMATCH;
            break;
        case MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM:
        case MDKR_ONLINE_VIEW_ACTION_PLAY_HERE:
        case MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM:
        case MDKR_ONLINE_VIEW_ACTION_RETURN_HOME:
            accepted = session_dispatch(&next,
                command->action == MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM
                    ? MDKR_SESSION_COMMAND_CANCEL
                    : MDKR_SESSION_COMMAND_RETURN_HOME, 0u);
            if (accepted) {
                next.have_lobby = false;
                next.invite_ready = false;
                next.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
            }
            break;
        case MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE:
            accepted = session_dispatch(
                &next, MDKR_SESSION_COMMAND_LEAVE_ONLINE_RACE, 0u);
            if (accepted) {
                next.have_lobby = false;
                next.invite_ready = false;
                next.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
            }
            break;
        default:
            break;
    }
    if (!accepted) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_REDUCER);
    }
    next.revision++;
    if (next.revision == 0u) next.revision = 1u;
    next.last_request_id = command->request_id;
    next.last_request_fingerprint = fingerprint;
    step = fake_step(&next, true, false, MDKR_ONLINE_FAKE_OK);
    next.last_request_step = step;
    *adapter = next;
    return step;
}

MdkrOnlineFakeStep mdkr_online_fake_complete(
    MdkrOnlineFakeAdapter *adapter, uint32_t pending_token,
    MdkrOnlineViewFailure failure) {
    MdkrOnlineFakeAdapter next;
    bool accepted = false;
    if (adapter == NULL || adapter->pending == MDKR_ONLINE_FAKE_PENDING_NONE ||
        pending_token == 0u || pending_token != adapter->pending_token) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_STALE_CALLBACK);
    }
    next = *adapter;
    next.timeout_expired = false;
    if (failure != MDKR_ONLINE_VIEW_FAILURE_NONE) {
        next.failure = failure;
        next.pending = MDKR_ONLINE_FAKE_PENDING_NONE;
        next.pending_token = 0u;
        accepted = true;
    } else {
        switch (next.pending) {
            case MDKR_ONLINE_FAKE_PENDING_CREATE:
                accepted = mdkr_online_lobby_init(
                    &next.lobby, UINT64_C(0xabcdef), next.local_endpoint_id,
                    &next.compatibility, 1u);
                break;
            case MDKR_ONLINE_FAKE_PENDING_JOIN:
                accepted = mdkr_online_lobby_init(
                    &next.lobby, UINT64_C(0xabcdef), next.peer_endpoint_id,
                    &next.compatibility, 1u);
                if (accepted) {
                    accepted = lobby_dispatch(
                        &next, next.local_endpoint_id,
                        MDKR_ONLINE_JOIN, 0u, 1u);
                }
                break;
            case MDKR_ONLINE_FAKE_PENDING_PEER_JOIN:
                accepted = lobby_dispatch(
                    &next, next.peer_endpoint_id,
                    MDKR_ONLINE_JOIN, 0u, 1u);
                break;
            case MDKR_ONLINE_FAKE_PENDING_PREFLIGHT:
                accepted = session_dispatch(
                    &next, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_SELECTING);
                break;
            case MDKR_ONLINE_FAKE_PENDING_LOAD: {
                uint64_t leader = next.lobby.leader_endpoint_id;
                accepted = lobby_dispatch(
                    &next, next.local_endpoint_id,
                    MDKR_ONLINE_ACK_LOADED, 0u, 0u) &&
                    lobby_dispatch(
                        &next, next.peer_endpoint_id,
                        MDKR_ONLINE_ACK_LOADED, 0u, 0u) &&
                    lobby_dispatch(&next, leader,
                                   MDKR_ONLINE_BEGIN_RACE, 0u, 0u) &&
                    session_dispatch(&next,
                                     MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                                     MDKR_ENGINE_READY) &&
                    session_dispatch(&next,
                                     MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                                     MDKR_ENGINE_RACING);
                break;
            }
            case MDKR_ONLINE_FAKE_PENDING_NONE:
                break;
        }
        if (accepted && (next.pending == MDKR_ONLINE_FAKE_PENDING_CREATE ||
                         next.pending == MDKR_ONLINE_FAKE_PENDING_JOIN)) {
            next.have_lobby = true;
            next.invite_ready = true;
            accepted = session_dispatch(
                &next, MDKR_SESSION_COMMAND_SET_CONNECTIVITY,
                MDKR_CONNECTIVITY_DIRECT) &&
                session_dispatch(&next, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                                 MDKR_ROOM_OPEN);
        }
        if (accepted) {
            next.pending = MDKR_ONLINE_FAKE_PENDING_NONE;
            next.pending_token = 0u;
        }
    }
    if (!accepted) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_REDUCER);
    }
    next.revision++;
    if (next.revision == 0u) next.revision = 1u;
    *adapter = next;
    return fake_step(adapter, true, false, MDKR_ONLINE_FAKE_OK);
}

MdkrOnlineFakeStep mdkr_online_fake_prepare_peer(
    MdkrOnlineFakeAdapter *adapter, uint32_t expected_revision,
    uint8_t character_id, uint8_t vehicle_id, uint16_t track_id) {
    MdkrOnlineFakeAdapter next;
    unsigned seat;
    if (adapter == NULL || expected_revision != adapter->revision) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_STALE_VIEW);
    }
    if (adapter->pending != MDKR_ONLINE_FAKE_PENDING_NONE ||
        adapter->session.state.room != MDKR_ROOM_SELECTING) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_INVALID_ACTION);
    }
    for (seat = 0u; seat < MDKR_ONLINE_MAX_SEATS; seat++) {
        if (adapter->lobby.seats[seat].occupied &&
            adapter->lobby.seats[seat].endpoint_id == adapter->peer_endpoint_id) {
            break;
        }
    }
    if (seat == MDKR_ONLINE_MAX_SEATS) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_INVALID_ACTION);
    }
    next = *adapter;
    if (!lobby_dispatch(&next, next.peer_endpoint_id,
                        MDKR_ONLINE_SET_CHARACTER, seat, character_id) ||
        !lobby_dispatch(&next, next.peer_endpoint_id,
                        MDKR_ONLINE_SET_VEHICLE, seat, vehicle_id) ||
        !lobby_dispatch(&next, next.peer_endpoint_id,
                        MDKR_ONLINE_SET_VOTE, seat, track_id) ||
        !lobby_dispatch(&next, next.peer_endpoint_id,
                        MDKR_ONLINE_SET_READY, 0u, 1u)) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_REDUCER);
    }
    next.revision++;
    *adapter = next;
    return fake_step(adapter, true, false, MDKR_ONLINE_FAKE_OK);
}

MdkrOnlineFakeStep mdkr_online_fake_finish_race(
    MdkrOnlineFakeAdapter *adapter, uint32_t expected_revision) {
    MdkrOnlineFakeAdapter next;
    if (adapter == NULL || expected_revision != adapter->revision) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_STALE_VIEW);
    }
    if (!adapter->have_lobby ||
        adapter->session.state.scene != MDKR_SCENE_RACE_CHROME ||
        adapter->lobby.phase != MDKR_ONLINE_RACING) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_INVALID_ACTION);
    }
    next = *adapter;
    if (!lobby_dispatch(&next, next.lobby.leader_endpoint_id,
                        MDKR_ONLINE_PUBLISH_RESULTS, 0u, 0u) ||
        !session_dispatch(&next, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                          MDKR_ENGINE_FINISHED)) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_REDUCER);
    }
    next.revision++;
    *adapter = next;
    return fake_step(adapter, true, false, MDKR_ONLINE_FAKE_OK);
}

MdkrOnlineFakeStep mdkr_online_fake_expire_timeout(
    MdkrOnlineFakeAdapter *adapter, uint32_t expected_revision) {
    MdkrOnlineFakeAdapter next;
    MdkrOnlineViewModel model;
    if (adapter == NULL || expected_revision != adapter->revision) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_STALE_VIEW);
    }
    if (adapter->timeout_expired ||
        !mdkr_online_fake_view(adapter, &model) || !model.timeout.present) {
        return fake_step(adapter, false, false,
                         MDKR_ONLINE_FAKE_ERROR_INVALID_ACTION);
    }
    next = *adapter;
    next.timeout_expired = true;
    next.revision++;
    if (next.revision == 0u) next.revision = 1u;
    *adapter = next;
    return fake_step(adapter, true, false, MDKR_ONLINE_FAKE_OK);
}

static const MdkrOnlineFakeGallerySpec k_gallery[] = {
    {"entry", MDKR_ONLINE_VIEW_ENTRY, MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM, false, false},
    {"connecting-create", MDKR_ONLINE_VIEW_CONNECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, false},
    {"connecting-join", MDKR_ONLINE_VIEW_CONNECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, false},
    {"room-solo", MDKR_ONLINE_VIEW_ROOM, MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE, false, false},
    {"room-friends", MDKR_ONLINE_VIEW_ROOM, MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP, false, false},
    {"preflight", MDKR_ONLINE_VIEW_PREFLIGHT, MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, false},
    {"select-character", MDKR_ONLINE_VIEW_SELECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER, false, false},
    {"select-vehicle", MDKR_ONLINE_VIEW_SELECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE, false, false},
    {"select-track", MDKR_ONLINE_VIEW_SELECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK, false, false},
    {"select-ready", MDKR_ONLINE_VIEW_SELECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_READY, false, false},
    {"select-waiting", MDKR_ONLINE_VIEW_SELECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION, false, false},
    {"select-everyone-ready", MDKR_ONLINE_VIEW_SELECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION, false, false},
    {"select-start", MDKR_ONLINE_VIEW_SELECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_START_RACE, false, true},
    {"loading", MDKR_ONLINE_VIEW_LOADING, MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, true},
    {"loading-rematch", MDKR_ONLINE_VIEW_LOADING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, true},
    {"countdown", MDKR_ONLINE_VIEW_COUNTDOWN, MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, true},
    {"racing-direct", MDKR_ONLINE_VIEW_RACING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, false, true},
    {"racing-limited", MDKR_ONLINE_VIEW_RACING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, false, true},
    {"results", MDKR_ONLINE_VIEW_RESULTS, MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN, false, true},
    {"timeout-connecting", MDKR_ONLINE_VIEW_CONNECTING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, false},
    {"timeout-preflight", MDKR_ONLINE_VIEW_PREFLIGHT,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, false},
    {"timeout-loading", MDKR_ONLINE_VIEW_LOADING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, true},
    {"timeout-loading-rematch", MDKR_ONLINE_VIEW_LOADING,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, true},
    {"timeout-countdown", MDKR_ONLINE_VIEW_COUNTDOWN,
     MDKR_ONLINE_VIEW_FAILURE_NONE,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS, true, true},
    {"timeout-recovery", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE,
     MDKR_ONLINE_VIEW_ACTION_RETRY, true, false},
    {"failure-invite-expired", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED,
     MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE, true, false},
    {"failure-invite-rotated", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_INVITE_ROTATED,
     MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE, true, false},
    {"failure-room-full", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL,
     MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, true, false},
    {"failure-service-unavailable", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE,
     MDKR_ONLINE_VIEW_ACTION_RETRY, true, false},
    {"failure-zero-cost-capacity", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_SERVICE_BUDGET_SAFE,
     MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, true, false},
    {"failure-different-build", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD,
     MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME, true, false},
    {"failure-different-rom", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM,
     MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM, true, false},
    {"failure-different-settings", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS,
     MDKR_ONLINE_VIEW_ACTION_USE_ROOM_SETTINGS, true, false},
    {"failure-controller-needed", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_CONTROLLER_NEEDED,
     MDKR_ONLINE_VIEW_ACTION_SETUP_CONTROLLER, true, false},
    {"failure-connection-check", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR, true, false},
    {"failure-relay-capacity", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_RELAY_CAPACITY,
     MDKR_ONLINE_VIEW_ACTION_RETRY, true, false},
    {"failure-networks-cannot-connect", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT,
     MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR, true, false},
    {"failure-update-required", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED,
     MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME, true, false},
    {"failure-host-closed", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED,
     MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, true, false},
    {"failure-room-expired", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED,
     MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, true, false},
    {"failure-engine-failed", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED,
     MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY, true, true},
    {"failure-epoch-mismatch", MDKR_ONLINE_VIEW_RECOVERY,
     MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH,
     MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY, true, true},
};

size_t mdkr_online_fake_gallery_count(void) {
    return sizeof(k_gallery) / sizeof(k_gallery[0]);
}

const MdkrOnlineFakeGallerySpec *mdkr_online_fake_gallery_at(size_t index) {
    return index < mdkr_online_fake_gallery_count() ? &k_gallery[index] : NULL;
}

const MdkrOnlineFakeGallerySpec *mdkr_online_fake_gallery_find(
    const char *slug) {
    size_t index;
    if (slug == NULL || slug[0] == '\0') return NULL;
    for (index = 0u; index < mdkr_online_fake_gallery_count(); index++) {
        if (strcmp(k_gallery[index].slug, slug) == 0) return &k_gallery[index];
    }
    return NULL;
}

static bool gallery_action(MdkrOnlineFakeAdapter *adapter,
                           MdkrOnlineViewAction action,
                           uint32_t seat, uint32_t value) {
    MdkrOnlineFakeCommand command;
    uint64_t request_id = adapter->last_request_id + 1u;
    if (request_id == 0u) request_id = 1u;
    memset(&command, 0, sizeof(command));
    command.expected_revision = adapter->revision;
    command.request_id = request_id;
    command.action = action;
    command.seat = seat;
    command.value = value;
    return mdkr_online_fake_dispatch(adapter, &command).accepted;
}

static bool gallery_complete(MdkrOnlineFakeAdapter *adapter,
                             MdkrOnlineViewFailure failure) {
    uint32_t token = adapter->pending_token;
    return token != 0u &&
        mdkr_online_fake_complete(adapter, token, failure).accepted;
}

static bool gallery_room(MdkrOnlineFakeAdapter *adapter, bool with_peer) {
    if (!gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM, 0u, 0u) ||
        !gallery_complete(adapter, MDKR_ONLINE_VIEW_FAILURE_NONE)) {
        return false;
    }
    if (!with_peer) return true;
    return gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE, 0u, 0u) &&
           gallery_complete(adapter, MDKR_ONLINE_VIEW_FAILURE_NONE);
}

static bool gallery_selecting(MdkrOnlineFakeAdapter *adapter) {
    return gallery_room(adapter, true) &&
           gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP, 0u, 0u) &&
           gallery_complete(adapter, MDKR_ONLINE_VIEW_FAILURE_NONE);
}

static bool gallery_local_choices(MdkrOnlineFakeAdapter *adapter,
                                  bool character, bool vehicle,
                                  bool track, bool ready) {
    if (character &&
        !gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
                        0u, 1u)) return false;
    if (vehicle &&
        !gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
                        0u, 0u)) return false;
    if (track &&
        !gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK,
                        0u, 5u)) return false;
    if (ready &&
        !gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_READY,
                        0u, 1u)) return false;
    return true;
}

static bool gallery_everyone_ready(MdkrOnlineFakeAdapter *adapter) {
    return gallery_selecting(adapter) &&
           mdkr_online_fake_prepare_peer(adapter, adapter->revision,
                                         2u, 0u, 5u).accepted &&
           gallery_local_choices(adapter, true, true, true, true);
}

static bool gallery_loading(MdkrOnlineFakeAdapter *adapter) {
    return gallery_everyone_ready(adapter) &&
           gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_START_RACE,
                          0u, 1u);
}

static bool gallery_racing(MdkrOnlineFakeAdapter *adapter) {
    return gallery_loading(adapter) &&
           gallery_complete(adapter, MDKR_ONLINE_VIEW_FAILURE_NONE);
}

static bool gallery_results(MdkrOnlineFakeAdapter *adapter) {
    return gallery_racing(adapter) &&
           mdkr_online_fake_finish_race(adapter, adapter->revision).accepted;
}

static bool gallery_rematch_loading(MdkrOnlineFakeAdapter *adapter) {
    return gallery_results(adapter) &&
           gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN, 0u, 0u) &&
           mdkr_online_fake_prepare_peer(adapter, adapter->revision,
                                         2u, 0u, 5u).accepted &&
           gallery_local_choices(adapter, false, false, true, true) &&
           gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_START_RACE,
                          0u, 1u);
}

static bool gallery_failure(MdkrOnlineFakeAdapter *adapter,
                            MdkrOnlineViewFailure failure) {
    return gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM, 0u, 0u) &&
           gallery_complete(adapter, failure);
}

static bool gallery_preflight_failure(MdkrOnlineFakeAdapter *adapter,
                                      MdkrOnlineViewFailure failure) {
    return gallery_room(adapter, true) &&
           gallery_action(adapter, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
                          0u, 0u) &&
           gallery_complete(adapter, failure);
}

static bool gallery_engine_failure(MdkrOnlineFakeAdapter *adapter) {
    if (!gallery_loading(adapter) ||
        !session_dispatch(adapter, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                          MDKR_ENGINE_FAILED)) return false;
    adapter->pending = MDKR_ONLINE_FAKE_PENDING_NONE;
    adapter->pending_token = 0u;
    adapter->failure = MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED;
    adapter->revision++;
    return true;
}

static bool gallery_epoch_failure(MdkrOnlineFakeAdapter *adapter) {
    if (!gallery_loading(adapter) ||
        !session_dispatch(adapter, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                          MDKR_ROOM_COUNTDOWN)) return false;
    adapter->pending = MDKR_ONLINE_FAKE_PENDING_NONE;
    adapter->pending_token = 0u;
    adapter->failure = MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH;
    adapter->revision++;
    return true;
}

static bool gallery_timeout(MdkrOnlineFakeAdapter *adapter) {
    return mdkr_online_fake_expire_timeout(
        adapter, adapter->revision).accepted;
}

static bool gallery_timeout_visible(const MdkrOnlineFakeGallerySpec *spec) {
    return spec != NULL && strncmp(spec->slug, "timeout-", 8u) == 0;
}

bool mdkr_online_fake_prepare_gallery(MdkrOnlineFakeAdapter *adapter,
                                      const char *slug) {
    const MdkrOnlineFakeGallerySpec *spec =
        mdkr_online_fake_gallery_find(slug);
    MdkrOnlineFakeAdapter next;
    MdkrOnlineViewModel model;
    bool built = false;
    if (adapter == NULL || spec == NULL ||
        (spec->requires_race_admission &&
         !adapter->race_admission_enabled)) return false;
    if (!mdkr_online_fake_init(&next, adapter->session.state.session_id,
                               &adapter->compatibility,
                               adapter->race_admission_enabled)) return false;

    if (strcmp(slug, "entry") == 0) {
        built = true;
    } else if (strcmp(slug, "connecting-create") == 0) {
        built = gallery_action(&next, MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM,
                               0u, 0u);
    } else if (strcmp(slug, "connecting-join") == 0) {
        built = gallery_action(&next, MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM,
                               0u, 0u);
    } else if (strcmp(slug, "room-solo") == 0) {
        built = gallery_room(&next, false);
    } else if (strcmp(slug, "room-friends") == 0) {
        built = gallery_room(&next, true);
    } else if (strcmp(slug, "preflight") == 0) {
        built = gallery_room(&next, true) &&
            gallery_action(&next, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP, 0u, 0u);
    } else if (strcmp(slug, "select-character") == 0) {
        built = gallery_selecting(&next);
    } else if (strcmp(slug, "select-vehicle") == 0) {
        built = gallery_selecting(&next) &&
            gallery_local_choices(&next, true, false, false, false);
    } else if (strcmp(slug, "select-track") == 0) {
        built = gallery_selecting(&next) &&
            gallery_local_choices(&next, true, true, false, false);
    } else if (strcmp(slug, "select-ready") == 0) {
        built = gallery_selecting(&next) &&
            gallery_local_choices(&next, true, true, true, false);
    } else if (strcmp(slug, "select-waiting") == 0) {
        built = gallery_selecting(&next) &&
            gallery_local_choices(&next, true, true, true, true);
    } else if (strcmp(slug, "select-everyone-ready") == 0 ||
               strcmp(slug, "select-start") == 0) {
        built = gallery_everyone_ready(&next);
    } else if (strcmp(slug, "loading") == 0) {
        built = gallery_loading(&next);
    } else if (strcmp(slug, "loading-rematch") == 0) {
        built = gallery_rematch_loading(&next);
    } else if (strcmp(slug, "countdown") == 0) {
        built = gallery_loading(&next) &&
            session_dispatch(&next, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                             MDKR_ROOM_COUNTDOWN);
    } else if (strcmp(slug, "racing-direct") == 0) {
        built = gallery_racing(&next);
    } else if (strcmp(slug, "racing-limited") == 0) {
        built = gallery_racing(&next) &&
            session_dispatch(&next, MDKR_SESSION_COMMAND_SET_CONNECTIVITY,
                             MDKR_CONNECTIVITY_RELAYED);
    } else if (strcmp(slug, "results") == 0) {
        built = gallery_results(&next);
    } else if (strcmp(slug, "timeout-connecting") == 0) {
        built = gallery_action(&next, MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM,
                               0u, 0u) && gallery_timeout(&next);
    } else if (strcmp(slug, "timeout-preflight") == 0) {
        built = gallery_room(&next, true) &&
            gallery_action(&next, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
                           0u, 0u) && gallery_timeout(&next);
    } else if (strcmp(slug, "timeout-loading") == 0) {
        built = gallery_loading(&next) && gallery_timeout(&next);
    } else if (strcmp(slug, "timeout-loading-rematch") == 0) {
        built = gallery_rematch_loading(&next) && gallery_timeout(&next);
    } else if (strcmp(slug, "timeout-countdown") == 0) {
        built = gallery_loading(&next) &&
            session_dispatch(&next, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                             MDKR_ROOM_COUNTDOWN) && gallery_timeout(&next);
    } else if (strcmp(slug, "timeout-recovery") == 0) {
        built = gallery_failure(
            &next, MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE) &&
            gallery_timeout(&next);
    } else if (spec->failure == MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED) {
        built = gallery_engine_failure(&next);
    } else if (spec->failure == MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH) {
        built = gallery_epoch_failure(&next);
    } else if (spec->failure >= MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD &&
               spec->failure <= MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED) {
        built = gallery_preflight_failure(&next, spec->failure);
    } else if (spec->failure != MDKR_ONLINE_VIEW_FAILURE_NONE) {
        built = gallery_failure(&next, spec->failure);
    }

    if (!built || !mdkr_online_fake_view(&next, &model) ||
        model.kind != spec->kind || model.failure != spec->failure ||
        model.primary.action != spec->primary_action ||
        model.timeout.present != spec->timeout_present ||
        next.timeout_expired != gallery_timeout_visible(spec)) {
        return false;
    }
    *adapter = next;
    return true;
}
