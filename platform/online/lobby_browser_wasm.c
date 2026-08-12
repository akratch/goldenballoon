/* Small ROM/engine/service-free browser ABI over the authoritative room model.
 *
 * This module exists so the browser launcher can render the exact C projection
 * used by native without growing a JavaScript room reducer. It exposes only
 * bounded gallery/model fields and typed fake-adapter actions. The production
 * browser does not load it while online admission is closed.
 */
#include "lobby_fake_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MDKR_ONLINE_BROWSER_ABI_VERSION 1u

static MdkrOnlineFakeAdapter s_adapter;
static MdkrOnlineViewModel s_model;
static const MdkrOnlineFakeGallerySpec *s_spec;
static uint64_t s_request_id;
static int s_selected;
static int s_live;

static MdkrOnlineCompatibilityV1 browser_compatibility(void) {
    MdkrOnlineCompatibilityV1 value;
    unsigned index;
    memset(&value, 0, sizeof(value));
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (index = 0u; index < sizeof(value.build_id); index++) {
        value.build_id[index] = (uint8_t)(index + 1u);
    }
    for (index = 0u; index < sizeof(value.gameplay_digest); index++) {
        value.gameplay_digest[index] = (uint8_t)(0x80u + index);
    }
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

static const MdkrOnlineViewControl *control_at(unsigned slot) {
    if (!s_selected) return NULL;
    switch (slot) {
        case 0u: return &s_model.primary;
        case 1u: return &s_model.secondary;
        case 2u: return &s_model.cancel;
        case 3u:
            return s_adapter.timeout_expired && s_model.timeout.present
                ? &s_model.timeout.primary : NULL;
        default: return NULL;
    }
}

static int visible_action(unsigned action) {
    unsigned slot;
    for (slot = 0u; slot < 4u; slot++) {
        const MdkrOnlineViewControl *control = control_at(slot);
        if (control != NULL && control->visible && control->enabled &&
            (unsigned)control->action == action) return 1;
    }
    return 0;
}

unsigned mdkr_online_browser_version(void) {
    return MDKR_ONLINE_BROWSER_ABI_VERSION;
}

unsigned mdkr_online_browser_count(void) {
    return (unsigned)mdkr_online_fake_gallery_count();
}

int mdkr_online_browser_select(unsigned index) {
    MdkrOnlineCompatibilityV1 compatibility;
    const MdkrOnlineFakeGallerySpec *spec =
        mdkr_online_fake_gallery_at((size_t)index);
    MdkrOnlineFakeAdapter adapter;
    MdkrOnlineViewModel model;
    if (spec == NULL) return 0;
    compatibility = browser_compatibility();
    if (!mdkr_online_fake_init(
            &adapter, UINT64_C(0x42524f57534552) + index, &compatibility,
            spec->requires_race_admission) ||
        !mdkr_online_fake_prepare_gallery(&adapter, spec->slug) ||
        !mdkr_online_fake_view(&adapter, &model)) return 0;
    s_adapter = adapter;
    s_model = model;
    s_spec = spec;
    s_request_id = s_adapter.last_request_id + 1u;
    if (s_request_id == 0u) s_request_id = 1u;
    s_selected = 1;
    s_live = 0;
    return 1;
}

const char *mdkr_online_browser_slug(void) {
    return s_live ? "live-room" :
        (s_selected && s_spec != NULL ? s_spec->slug : "");
}

const char *mdkr_online_browser_title(void) {
    return s_selected && s_model.title != NULL ? s_model.title : "";
}

const char *mdkr_online_browser_explanation(void) {
    return s_selected && s_model.explanation != NULL
        ? s_model.explanation : "";
}

const char *mdkr_online_browser_status(void) {
    return s_selected && s_model.status != NULL ? s_model.status : "";
}

const char *mdkr_online_browser_timeout_title(void) {
    return s_selected && s_adapter.timeout_expired && s_model.timeout.present &&
            s_model.timeout.title != NULL ? s_model.timeout.title : "";
}

const char *mdkr_online_browser_timeout_explanation(void) {
    return s_selected && s_adapter.timeout_expired && s_model.timeout.present &&
            s_model.timeout.explanation != NULL
        ? s_model.timeout.explanation : "";
}

unsigned mdkr_online_browser_kind(void) {
    return s_selected ? (unsigned)s_model.kind : 0u;
}

unsigned mdkr_online_browser_failure(void) {
    return s_selected ? (unsigned)s_model.failure : 0u;
}

unsigned mdkr_online_browser_announcement(void) {
    return s_selected ? (unsigned)s_model.announcement : 0u;
}

unsigned mdkr_online_browser_member_count(void) {
    return s_selected ? (unsigned)s_model.member_count : 0u;
}

unsigned mdkr_online_browser_seat_count(void) {
    return s_selected ? (unsigned)s_model.seat_count : 0u;
}

unsigned mdkr_online_browser_ready_count(void) {
    return s_selected ? (unsigned)s_model.ready_count : 0u;
}

unsigned mdkr_online_browser_requires_admission(void) {
    return !s_live && s_selected && s_spec != NULL && s_spec->requires_race_admission
        ? 1u : 0u;
}

unsigned mdkr_online_browser_local_play_available(void) {
    return s_selected && s_model.local_play_available ? 1u : 0u;
}

unsigned mdkr_online_browser_timeout_visible(void) {
    return s_selected && s_adapter.timeout_expired ? 1u : 0u;
}

unsigned mdkr_online_browser_control_action(unsigned slot) {
    const MdkrOnlineViewControl *control = control_at(slot);
    return control != NULL && control->visible
        ? (unsigned)control->action : 0u;
}

const char *mdkr_online_browser_control_label(unsigned slot) {
    const MdkrOnlineViewControl *control = control_at(slot);
    return control != NULL && control->visible && control->label != NULL
        ? control->label : "";
}

unsigned mdkr_online_browser_control_enabled(unsigned slot) {
    const MdkrOnlineViewControl *control = control_at(slot);
    return control != NULL && control->visible && control->enabled ? 1u : 0u;
}

/* 0 rejected, 1 reducer action accepted, 2 launcher-owned route accepted. */
unsigned mdkr_online_browser_dispatch(unsigned action, unsigned supplied_value) {
    MdkrOnlineFakeCommand command;
    MdkrOnlineFakeStep step;
    MdkrOnlineViewAction dispatched;
    unsigned value = supplied_value;
    if (!s_selected || s_live || action == MDKR_ONLINE_VIEW_ACTION_NONE ||
        action > MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE ||
        !visible_action(action)) return 0u;

    switch ((MdkrOnlineViewAction)action) {
        case MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS:
        case MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME:
        case MDKR_ONLINE_VIEW_ACTION_SETUP_CONTROLLER:
        case MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR:
            return 2u;
        case MDKR_ONLINE_VIEW_ACTION_RETURN_HOME:
            if (s_adapter.session.state.intent == MDKR_INTENT_NONE) return 2u;
            break;
        case MDKR_ONLINE_VIEW_ACTION_READY:
        case MDKR_ONLINE_VIEW_ACTION_START_RACE:
            value = 1u;
            break;
        default:
            break;
    }
    dispatched = (MdkrOnlineViewAction)action;
    if (dispatched == MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK) {
        dispatched = MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN;
    }
    memset(&command, 0, sizeof(command));
    command.expected_revision = s_adapter.revision;
    command.request_id = s_request_id++;
    command.action = dispatched;
    command.value = value;
    step = mdkr_online_fake_dispatch(&s_adapter, &command);
    if (!step.accepted || !mdkr_online_fake_view(&s_adapter, &s_model)) return 0u;
    return 1u;
}

unsigned mdkr_online_browser_pending(void) {
    return s_selected ? (unsigned)s_adapter.pending : 0u;
}

int mdkr_online_browser_complete(void) {
    MdkrOnlineFakeStep step;
    if (!s_selected || s_adapter.pending == MDKR_ONLINE_FAKE_PENDING_NONE ||
        s_adapter.pending_token == 0u) return 0;
    step = mdkr_online_fake_complete(
        &s_adapter, s_adapter.pending_token, MDKR_ONLINE_VIEW_FAILURE_NONE);
    return step.accepted && mdkr_online_fake_view(&s_adapter, &s_model);
}

/* Project a validated service snapshot through the same native view model.
 * Endpoint ids are deliberately remapped to 1..4: identity strings stay in JS
 * transport custody and the C projection needs only ownership/equality. Packed
 * fields use two bits per member/seat; boolean masks use one bit per row. The
 * live path can never enable race admission through service data. */
int mdkr_online_browser_live_project(
    unsigned room_phase, unsigned lobby_phase, unsigned revision,
    unsigned match_epoch, unsigned leader_index, unsigned local_index,
    unsigned member_count, unsigned seat_count,
    unsigned member_seat_counts, unsigned ready_mask,
    unsigned connected_mask, unsigned loaded_mask, unsigned seat_owners,
    unsigned character_mask, unsigned vehicle_mask, unsigned vote_mask,
    unsigned selected_track, unsigned selected_vehicle_mask,
    unsigned journey, unsigned failure, unsigned invite_ready) {
    MdkrOnlineCompatibilityV1 compatibility = browser_compatibility();
    MdkrOnlineLobby lobby;
    MdkrSessionState session;
    MdkrOnlineViewInput input;
    unsigned local_indices[MDKR_ONLINE_MAX_ENDPOINTS] = {0};
    unsigned index;

    if (member_count == 0u || member_count > MDKR_ONLINE_MAX_ENDPOINTS ||
        seat_count == 0u || seat_count > MDKR_ONLINE_MAX_SEATS ||
        leader_index >= member_count || local_index >= member_count ||
        room_phase < MDKR_ROOM_OPEN || room_phase > MDKR_ROOM_CLOSED ||
        lobby_phase < MDKR_ONLINE_LOBBY || lobby_phase > MDKR_ONLINE_CLOSED ||
        revision == 0u || journey < MDKR_ONLINE_JOURNEY_CREATE ||
        journey > MDKR_ONLINE_JOURNEY_REMATCH ||
        failure >= MDKR_ONLINE_VIEW_FAILURE_COUNT) return 0;

    memset(&lobby, 0, sizeof(lobby));
    lobby.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    lobby.revision = revision;
    lobby.match_epoch = match_epoch;
    lobby.leader_generation = 1u;
    lobby.room_id = 1u;
    lobby.leader_endpoint_id = leader_index + 1u;
    lobby.phase = (MdkrOnlinePhase)lobby_phase;
    lobby.compatibility = compatibility;
    lobby.member_count = (uint8_t)member_count;
    lobby.seat_count = (uint8_t)seat_count;
    lobby.selected_track = lobby_phase == MDKR_ONLINE_LOBBY
        ? MDKR_ONLINE_NO_VOTE : (uint16_t)selected_track;
    lobby.selected_vehicle_mask = lobby_phase == MDKR_ONLINE_LOBBY
        ? 0u : (uint8_t)selected_vehicle_mask;
    for (index = 0u; index < member_count; index++) {
        MdkrOnlineMember *member = &lobby.members[index];
        member->endpoint_id = index + 1u;
        member->seat_count = (uint8_t)((member_seat_counts >> (index * 2u)) & 3u);
        member->occupied = true;
        member->connected = (connected_mask & (1u << index)) != 0u;
        member->ready = (ready_mask & (1u << index)) != 0u;
        member->loaded = (loaded_mask & (1u << index)) != 0u;
    }
    for (index = 0u; index < seat_count; index++) {
        MdkrOnlineSeat *seat = &lobby.seats[index];
        unsigned owner = (seat_owners >> (index * 2u)) & 3u;
        if (owner >= member_count) return 0;
        seat->endpoint_id = owner + 1u;
        seat->local_index = (uint8_t)local_indices[owner]++;
        seat->character_id = (character_mask & (1u << index)) != 0u
            ? (uint8_t)index : MDKR_ONLINE_NO_CHARACTER;
        seat->vehicle_id = (vehicle_mask & (1u << index)) != 0u
            ? 0u : MDKR_ONLINE_NO_VEHICLE;
        seat->vote_track = (vote_mask & (1u << index)) != 0u
            ? 0u : MDKR_ONLINE_NO_VOTE;
        seat->selection_revision =
            ((character_mask | vehicle_mask) & (1u << index)) != 0u ? 1u : 0u;
        seat->occupied = true;
    }
    if (!mdkr_online_lobby_valid(&lobby)) return 0;

    memset(&session, 0, sizeof(session));
    session.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    session.generation = 1u;
    session.match_epoch = match_epoch;
    session.session_id = 1u;
    session.intent = MDKR_INTENT_ONLINE_PRIVATE;
    session.connectivity = lobby_phase == MDKR_ONLINE_CLOSED
        ? MDKR_CONNECTIVITY_LOST : MDKR_CONNECTIVITY_DIRECT;
    session.room = (MdkrRoomPhase)room_phase;
    session.scene = MDKR_SCENE_LOBBY;
    session.engine = MDKR_ENGINE_STOPPED;
    if (room_phase == MDKR_ROOM_LOADING || room_phase == MDKR_ROOM_COUNTDOWN) {
        session.scene = MDKR_SCENE_LOADING;
    } else if (room_phase == MDKR_ROOM_RACING) {
        session.scene = MDKR_SCENE_RACE_CHROME;
        session.engine = MDKR_ENGINE_RACING;
    } else if (room_phase == MDKR_ROOM_RESULTS) {
        session.scene = MDKR_SCENE_RESULTS;
        session.engine = MDKR_ENGINE_FINISHED;
    } else if (room_phase == MDKR_ROOM_CLOSED) {
        session.scene = MDKR_SCENE_RECOVERY;
        session.last_error = MDKR_SESSION_ERROR_CONNECTION_LOST;
    }
    if (!mdkr_session_state_valid(&session)) return 0;

    memset(&input, 0, sizeof(input));
    input.session = &session;
    input.lobby = &lobby;
    input.local_endpoint_id = local_index + 1u;
    input.journey = (MdkrOnlineJourney)journey;
    input.failure = (MdkrOnlineViewFailure)failure;
    input.invite_ready = invite_ready != 0u;
    input.race_admission_enabled = false;
    if (!mdkr_online_view_model_build(&input, &s_model)) return 0;
    s_spec = NULL;
    s_selected = 1;
    s_live = 1;
    return 1;
}
