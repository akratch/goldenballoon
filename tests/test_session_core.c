#include "session/session_core.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (condition) {
        printf("ok: %s\n", message);
    } else {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static MdkrSessionStep dispatch(MdkrSessionCore *core,
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

static void engine_phase(MdkrSessionCore *core, MdkrEnginePhase phase) {
    MdkrSessionStep step = dispatch(
        core, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE, (uint32_t)phase);
    expect(step.accepted, "engine phase accepted");
}

static void test_local_round_trip(void) {
    MdkrSessionCore core;
    MdkrSessionStep step;
    uint64_t session_id = UINT64_C(0x1122334455667788);

    mdkr_session_core_init(&core, session_id);
    expect(mdkr_session_state_valid(&core.state), "initial state is valid");
    expect(core.state.scene == MDKR_SCENE_HOME, "initial scene is Home");

    step = dispatch(&core, MDKR_SESSION_COMMAND_BEGIN_LOCAL, 0u);
    expect(step.accepted && step.effect_count == 0u,
           "local intent opens setup without service effect");
    expect(core.state.scene == MDKR_SCENE_LOCAL_SETUP,
           "local intent opens controller setup");

    step = dispatch(&core, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u);
    expect(step.accepted && step.effect_count == 1u,
           "local start emits one engine effect");
    expect(step.effects[0].type == MDKR_SESSION_EFFECT_START_ENGINE,
           "local start effect boots engine");
    expect(step.effects[0].generation == core.state.generation,
           "effect is stamped with committed generation");
    expect(core.state.match_epoch == 1u, "first race owns epoch one");

    engine_phase(&core, MDKR_ENGINE_READY);
    engine_phase(&core, MDKR_ENGINE_RACING);
    step = dispatch(&core, MDKR_SESSION_COMMAND_OPEN_OVERLAY,
                    MDKR_OVERLAY_CONTROLS);
    expect(step.accepted && core.state.engine_pause_requested,
           "local Controls requests engine pause");
    expect(step.effect_count == 1u &&
           step.effects[0].type == MDKR_SESSION_EFFECT_SET_ENGINE_PAUSED &&
           step.effects[0].value == 1u,
           "local overlay emits pause effect");
    step = dispatch(&core, MDKR_SESSION_COMMAND_CLOSE_OVERLAY, 0u);
    expect(step.accepted && !core.state.engine_pause_requested,
           "closing local overlay clears pause");
    expect(step.effect_count == 1u && step.effects[0].value == 0u,
           "closing local overlay emits resume effect");

    engine_phase(&core, MDKR_ENGINE_FINISHED);
    expect(core.state.scene == MDKR_SCENE_RESULTS,
           "finished engine enters results");
    step = dispatch(&core, MDKR_SESSION_COMMAND_REMATCH, 0u);
    expect(step.accepted && core.state.match_epoch == 2u,
           "rematch rotates match epoch");
    expect(core.state.session_id == session_id,
           "rematch preserves launcher session id");
    engine_phase(&core, MDKR_ENGINE_READY);
    engine_phase(&core, MDKR_ENGINE_RACING);
    engine_phase(&core, MDKR_ENGINE_FINISHED);
    step = dispatch(&core, MDKR_SESSION_COMMAND_RETURN_HOME, 0u);
    expect(step.accepted && core.state.scene == MDKR_SCENE_HOME,
           "results return to Home");
    expect(core.state.session_id == session_id,
           "return Home preserves runtime identity");
    expect(mdkr_session_state_valid(&core.state),
           "complete local round trip remains valid");
}

static void test_online_overlay_never_pauses(void) {
    MdkrSessionCore core;
    MdkrSessionStep step;

    mdkr_session_core_init(&core, 9u);
    step = dispatch(&core, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    expect(step.accepted && step.effect_count == 1u &&
           step.effects[0].type == MDKR_SESSION_EFFECT_CONNECT_PRIVATE_ROOM,
           "online intent delegates room connection");
    expect(dispatch(&core, MDKR_SESSION_COMMAND_SET_CONNECTIVITY,
                    MDKR_CONNECTIVITY_DIRECT).accepted,
           "direct connectivity accepted");
    expect(dispatch(&core, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_LOADING).accepted,
           "online room reaches loading");
    expect(dispatch(&core, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u).accepted,
           "online room requests race");
    engine_phase(&core, MDKR_ENGINE_READY);
    engine_phase(&core, MDKR_ENGINE_RACING);

    step = dispatch(&core, MDKR_SESSION_COMMAND_OPEN_OVERLAY,
                    MDKR_OVERLAY_CONNECTION);
    expect(step.accepted, "online Connection details opens");
    expect(!core.state.engine_pause_requested && step.effect_count == 0u,
           "online overlay cannot emit a pause effect");
    expect(core.state.engine == MDKR_ENGINE_RACING,
           "online overlay leaves engine racing");

    step = dispatch(&core, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_CLOSED);
    expect(step.accepted && core.state.scene == MDKR_SCENE_RACE_CHROME,
           "control loss does not counterfeit an in-race scene transition");
    expect(core.state.connectivity == MDKR_CONNECTIVITY_LOST,
           "closed room records lost control connectivity");
    engine_phase(&core, MDKR_ENGINE_FAILED);
    step = dispatch(&core, MDKR_SESSION_COMMAND_RECOVER,
                    MDKR_SESSION_ERROR_CONNECTION_LOST);
    expect(step.accepted && core.state.scene == MDKR_SCENE_RECOVERY &&
               core.state.last_error == MDKR_SESSION_ERROR_CONNECTION_LOST,
           "launcher classifies a clean network unwind as connection recovery");
}

static void test_stale_and_invalid_commands_are_atomic(void) {
    MdkrSessionCore core;
    MdkrSessionCore before;
    MdkrSessionCommand command;
    MdkrSessionStep step;

    mdkr_session_core_init(&core, 42u);
    before = core;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    command.expected_generation = core.state.generation - 1u;
    command.type = MDKR_SESSION_COMMAND_BEGIN_LOCAL;
    step = mdkr_session_core_dispatch(&core, &command);
    expect(!step.accepted && step.error == MDKR_SESSION_ERROR_STALE_COMMAND,
           "stale command has typed rejection");
    expect(memcmp(&core, &before, sizeof(core)) == 0,
           "stale command cannot partially mutate state");

    step = dispatch(&core, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u);
    expect(!step.accepted &&
           step.error == MDKR_SESSION_ERROR_INVALID_TRANSITION,
           "race cannot start without play intent");
    expect(memcmp(&core, &before, sizeof(core)) == 0,
           "invalid transition is atomic");

    core.state.intent = MDKR_INTENT_ONLINE_PRIVATE;
    core.state.scene = MDKR_SCENE_RACE_CHROME;
    core.state.engine = MDKR_ENGINE_RACING;
    core.state.engine_pause_requested = true;
    expect(!mdkr_session_state_valid(&core.state),
           "online pause mutation fails composition validator");
}

static void test_online_results_return_to_lobby(void) {
    MdkrSessionCore core;
    MdkrSessionStep step;
    mdkr_session_core_init(&core, 55u);
    dispatch(&core, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    dispatch(&core, MDKR_SESSION_COMMAND_SET_CONNECTIVITY,
             MDKR_CONNECTIVITY_DIRECT);
    dispatch(&core, MDKR_SESSION_COMMAND_SET_ROOM_PHASE, MDKR_ROOM_LOADING);
    dispatch(&core, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u);
    engine_phase(&core, MDKR_ENGINE_READY);
    engine_phase(&core, MDKR_ENGINE_RACING);
    engine_phase(&core, MDKR_ENGINE_FINISHED);

    step = dispatch(&core, MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
    expect(step.accepted && step.effect_count == 1u &&
               step.effects[0].type == MDKR_SESSION_EFFECT_STOP_ENGINE,
           "online results retire the engine without disconnecting the room");
    expect(core.state.intent == MDKR_INTENT_ONLINE_PRIVATE &&
               core.state.scene == MDKR_SCENE_LOBBY &&
               core.state.room == MDKR_ROOM_SELECTING &&
               core.state.engine == MDKR_ENGINE_STOPPED &&
               core.state.connectivity == MDKR_CONNECTIVITY_DIRECT,
           "online Race Again returns to launcher selection with room intact");
    expect(mdkr_session_state_valid(&core.state),
           "returned online lobby composition validates");
    step = dispatch(&core, MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
    expect(!step.accepted,
           "duplicate results return cannot advance the lobby twice");

    core.state.scene = MDKR_SCENE_LOADING;
    core.state.room = MDKR_ROOM_LOADING;
    core.state.engine = MDKR_ENGINE_BOOTING;
    step = dispatch(&core, MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
    expect(step.accepted && core.state.scene == MDKR_SCENE_LOBBY &&
               core.state.engine == MDKR_ENGINE_STOPPED &&
               step.effect_count == 1u &&
               step.effects[0].type == MDKR_SESSION_EFFECT_STOP_ENGINE,
           "loading cancel retires an engine loan but preserves the room");

    core.state.scene = MDKR_SCENE_RECOVERY;
    core.state.room = MDKR_ROOM_LOADING;
    core.state.engine = MDKR_ENGINE_FAILED;
    core.state.last_error = MDKR_SESSION_ERROR_ENGINE_FAILED;
    expect(mdkr_session_state_valid(&core.state),
           "failed online engine remains a valid room-owned recovery state");
    step = dispatch(&core, MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
    expect(step.accepted && core.state.scene == MDKR_SCENE_LOBBY &&
               core.state.room == MDKR_ROOM_SELECTING &&
               core.state.engine == MDKR_ENGINE_STOPPED &&
               core.state.last_error == MDKR_SESSION_ERROR_NONE &&
               step.effect_count == 1u &&
               step.effects[0].type == MDKR_SESSION_EFFECT_STOP_ENGINE,
           "engine-start failure returns to the intact room and clears recovery");

    {
        MdkrSessionCore before = core;
        core.state.scene = MDKR_SCENE_RECOVERY;
        core.state.room = MDKR_ROOM_SELECTING;
        core.state.last_error = MDKR_SESSION_ERROR_CONNECTION_LOST;
        before = core;
        step = dispatch(&core, MDKR_SESSION_COMMAND_RETURN_TO_LOBBY, 0u);
        expect(!step.accepted && memcmp(&core, &before, sizeof(core)) == 0,
               "unrelated recovery cannot counterfeit room-preserving engine return");
    }
}

static void test_confirmed_online_race_leave(void) {
    MdkrSessionCore core;
    MdkrSessionCore before;
    MdkrSessionStep step;
    mdkr_session_core_init(&core, 56u);
    dispatch(&core, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    dispatch(&core, MDKR_SESSION_COMMAND_SET_CONNECTIVITY,
             MDKR_CONNECTIVITY_DIRECT);
    dispatch(&core, MDKR_SESSION_COMMAND_SET_ROOM_PHASE, MDKR_ROOM_LOADING);
    dispatch(&core, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u);
    engine_phase(&core, MDKR_ENGINE_READY);
    engine_phase(&core, MDKR_ENGINE_RACING);

    step = dispatch(&core, MDKR_SESSION_COMMAND_LEAVE_ONLINE_RACE, 0u);
    expect(step.accepted && step.effect_count == 2u &&
               step.effects[0].type == MDKR_SESSION_EFFECT_STOP_ENGINE &&
               step.effects[1].type == MDKR_SESSION_EFFECT_DISCONNECT_ROOM,
           "confirmed online leave orders engine stop before room disconnect");
    expect(core.state.intent == MDKR_INTENT_NONE &&
               core.state.scene == MDKR_SCENE_HOME &&
               core.state.engine == MDKR_ENGINE_STOPPED &&
               core.state.room == MDKR_ROOM_NONE &&
               core.state.connectivity == MDKR_CONNECTIVITY_OFFLINE &&
               mdkr_session_state_valid(&core.state),
           "confirmed online leave commits one valid Home composition");

    before = core;
    step = dispatch(&core, MDKR_SESSION_COMMAND_LEAVE_ONLINE_RACE, 0u);
    expect(!step.accepted && memcmp(&core, &before, sizeof(core)) == 0,
           "stale leave confirmation cannot stop or disconnect twice");

    mdkr_session_core_init(&core, 57u);
    dispatch(&core, MDKR_SESSION_COMMAND_BEGIN_LOCAL, 0u);
    dispatch(&core, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u);
    engine_phase(&core, MDKR_ENGINE_READY);
    engine_phase(&core, MDKR_ENGINE_RACING);
    before = core;
    step = dispatch(&core, MDKR_SESSION_COMMAND_LEAVE_ONLINE_RACE, 0u);
    expect(!step.accepted && memcmp(&core, &before, sizeof(core)) == 0,
           "online leave command cannot terminate a local race");
}

static void test_update_required_blocks_new_match(void) {
    MdkrSessionCore core;
    MdkrSessionStep step;
    mdkr_session_core_init(&core, 77u);
    expect(dispatch(&core, MDKR_SESSION_COMMAND_BEGIN_LOCAL, 0u).accepted,
           "local setup begins for update case");
    expect(dispatch(&core, MDKR_SESSION_COMMAND_SET_UPDATE_STATE,
                    MDKR_UPDATE_REQUIRED).accepted,
           "required update is recorded");
    step = dispatch(&core, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u);
    expect(!step.accepted, "required update blocks a new race");
    expect(core.state.scene == MDKR_SCENE_LOCAL_SETUP,
           "blocked update leaves setup recoverable");
}

int main(void) {
    test_local_round_trip();
    test_online_overlay_never_pauses();
    test_online_results_return_to_lobby();
    test_confirmed_online_race_leave();
    test_stale_and_invalid_commands_are_atomic();
    test_update_required_blocks_new_match();
    if (failures != 0) return 1;
    printf("session core contract passed\n");
    return 0;
}
