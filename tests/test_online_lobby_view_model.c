#include "platform/online/lobby_view_model.h"
#include "platform/session/session_core.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static MdkrOnlineCompatibilityV1 compatibility(void) {
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

static MdkrSessionStep session_command(MdkrSessionCore *core,
                                       MdkrSessionCommandType type,
                                       uint32_t value) {
    MdkrSessionCommand command;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    command.expected_generation = core->state.generation;
    command.type = type;
    command.value = value;
    return mdkr_session_core_dispatch(core, &command);
}

static MdkrOnlineStep lobby_command_as(MdkrOnlineLobby *lobby,
                                       uint64_t actor,
                                       uint64_t command_id,
                                       MdkrOnlineCommandType type,
                                       unsigned seat, unsigned value) {
    MdkrOnlineCommand command;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    command.expected_revision = lobby->revision;
    command.command_id = command_id;
    command.actor_endpoint_id = actor;
    command.type = type;
    command.target_endpoint_id = seat;
    command.value = value;
    return mdkr_online_lobby_dispatch(lobby, &command);
}

static MdkrOnlineStep lobby_command(MdkrOnlineLobby *lobby,
                                    uint64_t command_id,
                                    MdkrOnlineCommandType type,
                                    unsigned seat, unsigned value) {
    return lobby_command_as(lobby, 10u, command_id, type, seat, value);
}

static MdkrOnlineStep lobby_join(MdkrOnlineLobby *lobby, uint64_t actor,
                                 unsigned seats,
                                 const MdkrOnlineCompatibilityV1 *compat) {
    MdkrOnlineCommand command;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    command.expected_revision = lobby->revision;
    command.command_id = 1u;
    command.actor_endpoint_id = actor;
    command.type = MDKR_ONLINE_JOIN;
    command.value = seats;
    command.compatibility = *compat;
    return mdkr_online_lobby_dispatch(lobby, &command);
}

static MdkrOnlineViewInput input_for(const MdkrSessionCore *session,
                                     const MdkrOnlineLobby *lobby) {
    MdkrOnlineViewInput input;
    memset(&input, 0, sizeof(input));
    input.session = &session->state;
    input.lobby = lobby;
    input.local_endpoint_id = 10u;
    input.journey = MDKR_ONLINE_JOURNEY_CREATE;
    return input;
}

static void expect_complete(const MdkrOnlineViewModel *model,
                            const char *message) {
    expect(model->title != NULL && model->title[0] != '\0', message);
    expect(model->explanation != NULL && model->explanation[0] != '\0', message);
    expect(model->primary.visible && model->primary.enabled &&
           model->primary.label != NULL, message);
    expect(model->cancel.visible && model->cancel.enabled &&
           model->cancel.label != NULL, message);
}

static void test_entry_connecting_and_timeouts(void) {
    MdkrSessionCore session;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;

    mdkr_session_core_init(&session, 1u);
    input = input_for(&session, NULL);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_ENTRY &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM &&
           model.local_play_available,
           "home projects both private-room routes without hiding local play");
    expect_complete(&model, "entry copy/control contract is complete");

    expect(session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u).accepted,
           "online journey begins through session reducer");
    input = input_for(&session, NULL);
    input.journey = MDKR_ONLINE_JOURNEY_JOIN;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_CONNECTING && model.timeout.present &&
           model.timeout.primary.action == MDKR_ONLINE_VIEW_ACTION_RETRY &&
           model.cancel.action == MDKR_ONLINE_VIEW_ACTION_RETURN_HOME,
           "joining wait has progress, cancel and bounded timeout recovery");
    expect_complete(&model, "connecting copy/control contract is complete");
}

static void test_room_selection_and_release_gate(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrSessionCore session;
    MdkrOnlineLobby lobby;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;

    mdkr_session_core_init(&session, 2u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    expect(mdkr_online_lobby_init(&lobby, 99u, 10u, &compat, 1u),
           "single-endpoint lobby initializes");
    expect(lobby_join(&lobby, 20u, 1u, &compat).accepted,
           "friend joins release-gate fixture");

    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_OPEN);
    input = input_for(&session, &lobby);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_ROOM &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP &&
           !model.secondary.enabled,
           "joined room advances to setup while invite preparation stays explicit");
    input.invite_ready = true;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE &&
           model.secondary.enabled,
           "room enables secondary sharing only after launcher owns an invite");

    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_PREFLIGHT);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_PREFLIGHT && model.timeout.present &&
           model.announcement == MDKR_ONLINE_ANNOUNCE_POLITE,
           "preflight has non-blocking status and timeout recovery");
    expect_complete(&model, "preflight copy/control contract is complete");

    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_SELECTING);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_SELECTING &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
           "selection points to the first incomplete launcher-owned choice");
    expect(lobby_command(&lobby, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 2u).accepted,
           "character selection accepted");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
           "vehicle follows character in one-decision selection flow");
    expect(lobby_command(&lobby, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 0u).accepted,
           "vehicle selection accepted");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK,
           "track vote follows canonical racer choices");
    expect(lobby_command(&lobby, 3u, MDKR_ONLINE_SET_VOTE, 0u, 5u).accepted,
           "track vote accepted");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_READY,
           "complete selection enables Ready");
    expect(lobby_command(&lobby, 4u, MDKR_ONLINE_SET_READY, 0u, 1u).accepted,
           "member becomes ready");
    expect(lobby_command_as(
               &lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 3u).accepted &&
           lobby_command_as(
               &lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u).accepted &&
           lobby_command_as(
               &lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE, 1u, 5u).accepted &&
           lobby_command_as(
               &lobby, 20u, 5u, MDKR_ONLINE_SET_READY, 1u, 1u).accepted,
           "friend completes selections and becomes ready");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.status != NULL && strcmp(model.status, "Everyone Ready") == 0 &&
           model.primary.action != MDKR_ONLINE_VIEW_ACTION_START_RACE,
           "service state cannot expose Start before local rollback GO");
    input.race_admission_enabled = true;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_START_RACE,
           "reviewed local release policy can expose Start without view rewiring");
}

static void test_loading_racing_and_results(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrSessionCore session;
    MdkrOnlineLobby lobby;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;

    mdkr_session_core_init(&session, 3u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    mdkr_online_lobby_init(&lobby, 100u, 10u, &compat, 1u);
    lobby_join(&lobby, 20u, 1u, &compat);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_SELECTING);
    lobby_command(&lobby, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 1u);
    lobby_command(&lobby, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 0u);
    lobby_command(&lobby, 3u, MDKR_ONLINE_SET_VOTE, 0u, 5u);
    lobby_command(&lobby, 4u, MDKR_ONLINE_SET_READY, 0u, 1u);
    lobby_command_as(&lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 2u);
    lobby_command_as(&lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u);
    lobby_command_as(&lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE, 1u, 5u);
    lobby_command_as(&lobby, 20u, 5u, MDKR_ONLINE_SET_READY, 1u, 1u);
    expect(lobby_command(&lobby, 5u, MDKR_ONLINE_BEGIN_LOADING, 0u, 1u).accepted,
           "lobby begins loading");
    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_LOADING);
    expect(session_command(&session, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u).accepted,
           "session begins matching engine loan");
    input = input_for(&session, &lobby);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_LOADING && model.timeout.present &&
           model.cancel.action == MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
           "load barrier has cancel and room-preserving timeout");

    session.state.room = MDKR_ROOM_COUNTDOWN;
    expect(mdkr_session_state_valid(&session.state) &&
           mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_COUNTDOWN && model.timeout.present,
           "countdown has explicit mismatch recovery");
    session.state.room = MDKR_ROOM_LOADING;

    lobby_command(&lobby, 6u, MDKR_ONLINE_ACK_LOADED, 0u, 0u);
    lobby_command_as(&lobby, 20u, 6u, MDKR_ONLINE_ACK_LOADED, 0u, 0u);
    lobby_command(&lobby, 7u, MDKR_ONLINE_BEGIN_RACE, 0u, 0u);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                    MDKR_ENGINE_READY);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                    MDKR_ENGINE_RACING);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RACING &&
           model.cancel.action == MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE &&
           strstr(model.explanation, "non-pausing") != NULL,
           "race chrome states non-pausing behavior and deliberate leave");

    lobby_command(&lobby, 8u, MDKR_ONLINE_PUBLISH_RESULTS, 0u, 0u);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                    MDKR_ENGINE_FINISHED);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RESULTS &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK,
           "results keep the party and offer a clear rematch path");
}

static void test_failure_catalog_and_atomicity(void) {
    static const char *forbidden[] = { "ICE", "STUN", "TURN", "HTTP",
                                       "provider", "quota", "unknown error" };
    MdkrSessionCore session;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;
    unsigned failure;
    unsigned term;

    mdkr_session_core_init(&session, 4u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    input = input_for(&session, NULL);
    for (failure = MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED;
         failure <= MDKR_ONLINE_VIEW_FAILURE_COUNT + 1u; failure++) {
        input.failure = (MdkrOnlineViewFailure)failure;
        memset(&model, 0, sizeof(model));
        expect(mdkr_online_view_model_build(&input, &model),
               "every typed or unrecognized adapter failure projects safely");
        expect_complete(&model, "recovery copy/control contract is complete");
        expect(model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
               model.announcement == MDKR_ONLINE_ANNOUNCE_ASSERTIVE &&
               model.local_play_available && model.timeout.present,
               "recovery is assertive, bounded and preserves local escape");
        for (term = 0u; term < sizeof(forbidden) / sizeof(forbidden[0]); term++) {
            expect(strstr(model.title, forbidden[term]) == NULL &&
                   strstr(model.explanation, forbidden[term]) == NULL,
                   "player copy excludes provider jargon and raw diagnostics");
        }
    }
    input.failure = (MdkrOnlineViewFailure)999;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.failure == MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE,
           "unrecognized failure fails safely into one actionable copy path");

    {
        MdkrOnlineCompatibilityV1 compat = compatibility();
        MdkrOnlineLobby lobby;
        MdkrOnlineViewModel sentinel;
        mdkr_online_lobby_init(&lobby, 88u, 10u, &compat, 1u);
        input.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
        input.lobby = &lobby; /* Session is still JOIN/room NONE: mismatched. */
        memset(&sentinel, 0x5a, sizeof(sentinel));
        model = sentinel;
        expect(!mdkr_online_view_model_build(&input, &model) &&
               memcmp(&model, &sentinel, sizeof(model)) == 0,
               "mismatched session/lobby snapshots reject fail-atomically");
    }
}

int main(void) {
    test_entry_connecting_and_timeouts();
    test_room_selection_and_release_gate();
    test_loading_racing_and_results();
    test_failure_catalog_and_atomicity();
    if (failures != 0) {
        fprintf(stderr, "%d online lobby view-model test(s) failed\n", failures);
        return 1;
    }
    puts("online lobby view-model tests passed");
    return 0;
}
