#include "session_core.h"

#include <string.h>

static MdkrSessionStep rejected(MdkrSessionError error) {
    MdkrSessionStep step;
    memset(&step, 0, sizeof(step));
    step.error = error;
    return step;
}

static bool enum_in_range(const MdkrSessionState *state) {
    return state->intent >= MDKR_INTENT_NONE &&
           state->intent <= MDKR_INTENT_ONLINE_PRIVATE &&
           state->scene >= MDKR_SCENE_HOME &&
           state->scene <= MDKR_SCENE_RECOVERY &&
           state->engine >= MDKR_ENGINE_STOPPED &&
           state->engine <= MDKR_ENGINE_FAILED &&
           state->connectivity >= MDKR_CONNECTIVITY_OFFLINE &&
           state->connectivity <= MDKR_CONNECTIVITY_LOST &&
           state->room >= MDKR_ROOM_NONE && state->room <= MDKR_ROOM_CLOSED &&
           state->overlay >= MDKR_OVERLAY_CLOSED &&
           state->overlay <= MDKR_OVERLAY_LEAVE_CONFIRM &&
           state->update >= MDKR_UPDATE_NONE &&
           state->update <= MDKR_UPDATE_REQUIRED;
}

bool mdkr_session_state_valid(const MdkrSessionState *state) {
    if (state == NULL ||
        state->protocol_version != MDKR_SESSION_PROTOCOL_VERSION ||
        state->session_id == 0u || !enum_in_range(state)) {
        return false;
    }
    if (state->intent == MDKR_INTENT_NONE) {
        return state->scene == MDKR_SCENE_HOME &&
               state->engine == MDKR_ENGINE_STOPPED &&
               state->connectivity == MDKR_CONNECTIVITY_OFFLINE &&
               state->room == MDKR_ROOM_NONE &&
               state->overlay == MDKR_OVERLAY_CLOSED &&
               !state->engine_pause_requested;
    }
    if (state->intent == MDKR_INTENT_LOCAL &&
        (state->connectivity != MDKR_CONNECTIVITY_OFFLINE ||
         state->room != MDKR_ROOM_NONE)) {
        return false;
    }
    if (state->intent == MDKR_INTENT_ONLINE_PRIVATE &&
        state->scene == MDKR_SCENE_LOCAL_SETUP) {
        return false;
    }
    if (state->scene == MDKR_SCENE_LOCAL_SETUP &&
        state->intent != MDKR_INTENT_LOCAL) {
        return false;
    }
    if ((state->scene == MDKR_SCENE_JOIN ||
         state->scene == MDKR_SCENE_LOBBY) &&
        state->intent != MDKR_INTENT_ONLINE_PRIVATE) {
        return false;
    }
    if (state->engine == MDKR_ENGINE_RACING &&
        state->scene != MDKR_SCENE_RACE_CHROME) {
        return false;
    }
    if (state->scene == MDKR_SCENE_RACE_CHROME &&
        state->engine != MDKR_ENGINE_RACING) {
        return false;
    }
    if (state->scene == MDKR_SCENE_RESULTS &&
        state->engine != MDKR_ENGINE_FINISHED) {
        return false;
    }
    if (state->overlay != MDKR_OVERLAY_CLOSED &&
        state->scene != MDKR_SCENE_RACE_CHROME) {
        return false;
    }
    if (state->engine_pause_requested &&
        (state->intent != MDKR_INTENT_LOCAL ||
         state->overlay == MDKR_OVERLAY_CLOSED)) {
        return false;
    }
    if (state->intent == MDKR_INTENT_ONLINE_PRIVATE &&
        state->engine_pause_requested) {
        return false;
    }
    return true;
}

void mdkr_session_core_init(MdkrSessionCore *core, uint64_t session_id) {
    if (core == NULL) return;
    memset(core, 0, sizeof(*core));
    core->state.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    core->state.generation = 1u;
    core->state.session_id = session_id == 0u ? 1u : session_id;
    core->state.scene = MDKR_SCENE_HOME;
    core->state.engine = MDKR_ENGINE_STOPPED;
    core->state.connectivity = MDKR_CONNECTIVITY_OFFLINE;
    core->next_effect_id = 1u;
}

const MdkrSessionState *mdkr_session_core_state(const MdkrSessionCore *core) {
    return core == NULL ? NULL : &core->state;
}

static bool append_effect(MdkrSessionCore *core, MdkrSessionStep *step,
                          MdkrSessionEffectType type, uint32_t value) {
    MdkrSessionEffect *effect;
    if (step->effect_count >= MDKR_SESSION_MAX_EFFECTS) return false;
    effect = &step->effects[step->effect_count++];
    memset(effect, 0, sizeof(*effect));
    effect->protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    effect->generation = core->state.generation;
    effect->match_epoch = core->state.match_epoch;
    effect->effect_id = core->next_effect_id++;
    if (core->next_effect_id == 0u) core->next_effect_id = 1u;
    effect->type = type;
    effect->value = value;
    return true;
}

static bool engine_transition_allowed(MdkrEnginePhase from,
                                      MdkrEnginePhase to) {
    if (to == MDKR_ENGINE_FAILED) return from != MDKR_ENGINE_STOPPED;
    switch (from) {
        case MDKR_ENGINE_BOOTING: return to == MDKR_ENGINE_READY;
        case MDKR_ENGINE_READY: return to == MDKR_ENGINE_RACING;
        case MDKR_ENGINE_RACING: return to == MDKR_ENGINE_FINISHED;
        default: return false;
    }
}

static bool valid_room_phase(uint32_t value) {
    return value <= (uint32_t)MDKR_ROOM_CLOSED;
}

static bool valid_connectivity(uint32_t value) {
    return value <= (uint32_t)MDKR_CONNECTIVITY_LOST;
}

static bool valid_overlay(uint32_t value) {
    return value >= (uint32_t)MDKR_OVERLAY_CONTROLS &&
           value <= (uint32_t)MDKR_OVERLAY_LEAVE_CONFIRM;
}

MdkrSessionStep mdkr_session_core_dispatch(
    MdkrSessionCore *core, const MdkrSessionCommand *command) {
    MdkrSessionCore next;
    MdkrSessionStep step;

    if (core == NULL || command == NULL ||
        command->protocol_version != MDKR_SESSION_PROTOCOL_VERSION) {
        return rejected(MDKR_SESSION_ERROR_PROTOCOL);
    }
    if (command->expected_generation != core->state.generation) {
        return rejected(MDKR_SESSION_ERROR_STALE_COMMAND);
    }
    next = *core;
    memset(&step, 0, sizeof(step));

    switch (command->type) {
        case MDKR_SESSION_COMMAND_BEGIN_LOCAL:
            if (next.state.intent != MDKR_INTENT_NONE) goto invalid;
            next.state.intent = MDKR_INTENT_LOCAL;
            next.state.scene = MDKR_SCENE_LOCAL_SETUP;
            break;

        case MDKR_SESSION_COMMAND_BEGIN_ONLINE:
            if (next.state.intent != MDKR_INTENT_NONE) goto invalid;
            next.state.intent = MDKR_INTENT_ONLINE_PRIVATE;
            next.state.scene = MDKR_SCENE_JOIN;
            next.state.connectivity = MDKR_CONNECTIVITY_CONNECTING;
            if (!append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_CONNECT_PRIVATE_ROOM, 0u)) {
                goto invalid;
            }
            break;

        case MDKR_SESSION_COMMAND_SET_ROOM_PHASE:
            if (next.state.intent != MDKR_INTENT_ONLINE_PRIVATE ||
                !valid_room_phase(command->value)) goto invalid;
            next.state.room = (MdkrRoomPhase)command->value;
            if (next.state.room == MDKR_ROOM_OPEN ||
                next.state.room == MDKR_ROOM_PREFLIGHT ||
                next.state.room == MDKR_ROOM_SELECTING) {
                next.state.scene = MDKR_SCENE_LOBBY;
            } else if (next.state.room == MDKR_ROOM_LOADING ||
                       next.state.room == MDKR_ROOM_COUNTDOWN) {
                next.state.scene = MDKR_SCENE_LOADING;
            } else if (next.state.room == MDKR_ROOM_CLOSED) {
                next.state.last_error = MDKR_SESSION_ERROR_CONNECTION_LOST;
                next.state.connectivity = MDKR_CONNECTIVITY_LOST;
                if (next.state.engine != MDKR_ENGINE_RACING) {
                    next.state.scene = MDKR_SCENE_RECOVERY;
                }
            }
            break;

        case MDKR_SESSION_COMMAND_SET_CONNECTIVITY:
            if (next.state.intent != MDKR_INTENT_ONLINE_PRIVATE ||
                !valid_connectivity(command->value)) goto invalid;
            next.state.connectivity = (MdkrConnectivity)command->value;
            if (next.state.connectivity == MDKR_CONNECTIVITY_LOST &&
                next.state.engine != MDKR_ENGINE_RACING) {
                next.state.scene = MDKR_SCENE_RECOVERY;
                next.state.last_error = MDKR_SESSION_ERROR_CONNECTION_LOST;
            }
            break;

        case MDKR_SESSION_COMMAND_REQUEST_RACE:
            if (next.state.update == MDKR_UPDATE_REQUIRED ||
                next.state.engine != MDKR_ENGINE_STOPPED) goto invalid;
            if (next.state.intent == MDKR_INTENT_LOCAL) {
                if (next.state.scene != MDKR_SCENE_LOCAL_SETUP) goto invalid;
            } else if (next.state.intent == MDKR_INTENT_ONLINE_PRIVATE) {
                if (next.state.room != MDKR_ROOM_LOADING &&
                    next.state.room != MDKR_ROOM_COUNTDOWN) goto invalid;
            } else {
                goto invalid;
            }
            next.state.match_epoch++;
            next.state.engine = MDKR_ENGINE_BOOTING;
            next.state.scene = MDKR_SCENE_LOADING;
            if (!append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_START_ENGINE, 0u)) {
                goto invalid;
            }
            break;

        case MDKR_SESSION_COMMAND_SET_ENGINE_PHASE: {
            MdkrEnginePhase phase = (MdkrEnginePhase)command->value;
            if (command->value > (uint32_t)MDKR_ENGINE_FAILED ||
                !engine_transition_allowed(next.state.engine, phase)) {
                goto invalid;
            }
            next.state.engine = phase;
            if (phase == MDKR_ENGINE_RACING) {
                next.state.scene = MDKR_SCENE_RACE_CHROME;
                if (next.state.intent == MDKR_INTENT_ONLINE_PRIVATE) {
                    next.state.room = MDKR_ROOM_RACING;
                }
            } else if (phase == MDKR_ENGINE_FINISHED) {
                next.state.scene = MDKR_SCENE_RESULTS;
                next.state.overlay = MDKR_OVERLAY_CLOSED;
                next.state.engine_pause_requested = false;
                if (next.state.intent == MDKR_INTENT_ONLINE_PRIVATE) {
                    next.state.room = MDKR_ROOM_RESULTS;
                }
            } else if (phase == MDKR_ENGINE_FAILED) {
                next.state.scene = MDKR_SCENE_RECOVERY;
                next.state.overlay = MDKR_OVERLAY_CLOSED;
                next.state.engine_pause_requested = false;
                next.state.last_error = MDKR_SESSION_ERROR_ENGINE_FAILED;
            }
            break;
        }

        case MDKR_SESSION_COMMAND_OPEN_OVERLAY:
            if (next.state.engine != MDKR_ENGINE_RACING ||
                !valid_overlay(command->value)) goto invalid;
            next.state.overlay = (MdkrOverlayState)command->value;
            next.state.engine_pause_requested =
                next.state.intent == MDKR_INTENT_LOCAL;
            if (next.state.engine_pause_requested &&
                !append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_SET_ENGINE_PAUSED, 1u)) {
                goto invalid;
            }
            break;

        case MDKR_SESSION_COMMAND_CLOSE_OVERLAY:
            if (next.state.overlay == MDKR_OVERLAY_CLOSED) goto invalid;
            if (next.state.engine_pause_requested &&
                !append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_SET_ENGINE_PAUSED, 0u)) {
                goto invalid;
            }
            next.state.overlay = MDKR_OVERLAY_CLOSED;
            next.state.engine_pause_requested = false;
            break;

        case MDKR_SESSION_COMMAND_REMATCH:
            if (next.state.scene != MDKR_SCENE_RESULTS ||
                next.state.engine != MDKR_ENGINE_FINISHED ||
                next.state.update == MDKR_UPDATE_REQUIRED) goto invalid;
            next.state.match_epoch++;
            next.state.engine = MDKR_ENGINE_BOOTING;
            next.state.scene = MDKR_SCENE_LOADING;
            if (next.state.intent == MDKR_INTENT_ONLINE_PRIVATE) {
                next.state.room = MDKR_ROOM_LOADING;
            }
            if (!append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_START_ENGINE, 0u)) {
                goto invalid;
            }
            break;

        case MDKR_SESSION_COMMAND_RETURN_TO_LOBBY:
            if (next.state.intent != MDKR_INTENT_ONLINE_PRIVATE) goto invalid;
            if (!((next.state.scene == MDKR_SCENE_RESULTS &&
                   next.state.engine == MDKR_ENGINE_FINISHED &&
                   next.state.room == MDKR_ROOM_RESULTS) ||
                  (next.state.scene == MDKR_SCENE_LOADING &&
                   (next.state.engine == MDKR_ENGINE_STOPPED ||
                    next.state.engine == MDKR_ENGINE_BOOTING ||
                    next.state.engine == MDKR_ENGINE_READY) &&
                   (next.state.room == MDKR_ROOM_LOADING ||
                    next.state.room == MDKR_ROOM_COUNTDOWN)) ||
                  (next.state.scene == MDKR_SCENE_RECOVERY &&
                   next.state.engine == MDKR_ENGINE_FAILED &&
                   next.state.last_error == MDKR_SESSION_ERROR_ENGINE_FAILED &&
                   (next.state.room == MDKR_ROOM_LOADING ||
                    next.state.room == MDKR_ROOM_COUNTDOWN)))) goto invalid;
            if (next.state.engine != MDKR_ENGINE_STOPPED &&
                !append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_STOP_ENGINE, 0u)) {
                    goto invalid;
            }
            next.state.engine = MDKR_ENGINE_STOPPED;
            next.state.scene = MDKR_SCENE_LOBBY;
            next.state.room = MDKR_ROOM_SELECTING;
            next.state.overlay = MDKR_OVERLAY_CLOSED;
            next.state.engine_pause_requested = false;
            next.state.last_error = MDKR_SESSION_ERROR_NONE;
            break;

        case MDKR_SESSION_COMMAND_CANCEL:
            if (next.state.engine == MDKR_ENGINE_RACING) goto invalid;
            if (next.state.engine != MDKR_ENGINE_STOPPED) {
                if (!append_effect(&next, &step,
                                   MDKR_SESSION_EFFECT_STOP_ENGINE, 0u)) {
                    goto invalid;
                }
                next.state.engine = MDKR_ENGINE_STOPPED;
            }
            if (next.state.intent == MDKR_INTENT_ONLINE_PRIVATE &&
                next.state.room != MDKR_ROOM_NONE) {
                if (!append_effect(&next, &step,
                                   MDKR_SESSION_EFFECT_DISCONNECT_ROOM, 0u)) {
                    goto invalid;
                }
            }
            next.state.intent = MDKR_INTENT_NONE;
            next.state.scene = MDKR_SCENE_HOME;
            next.state.connectivity = MDKR_CONNECTIVITY_OFFLINE;
            next.state.room = MDKR_ROOM_NONE;
            next.state.overlay = MDKR_OVERLAY_CLOSED;
            next.state.engine_pause_requested = false;
            next.state.last_error = MDKR_SESSION_ERROR_NONE;
            break;

        case MDKR_SESSION_COMMAND_RETURN_HOME:
            if (next.state.engine == MDKR_ENGINE_RACING) goto invalid;
            if (next.state.engine != MDKR_ENGINE_STOPPED) {
                if (!append_effect(&next, &step,
                                   MDKR_SESSION_EFFECT_STOP_ENGINE, 0u)) {
                    goto invalid;
                }
                next.state.engine = MDKR_ENGINE_STOPPED;
            }
            if (next.state.intent == MDKR_INTENT_ONLINE_PRIVATE &&
                !append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_DISCONNECT_ROOM, 0u)) {
                goto invalid;
            }
            next.state.intent = MDKR_INTENT_NONE;
            next.state.scene = MDKR_SCENE_HOME;
            next.state.connectivity = MDKR_CONNECTIVITY_OFFLINE;
            next.state.room = MDKR_ROOM_NONE;
            next.state.overlay = MDKR_OVERLAY_CLOSED;
            next.state.engine_pause_requested = false;
            next.state.last_error = MDKR_SESSION_ERROR_NONE;
            break;

        case MDKR_SESSION_COMMAND_LEAVE_ONLINE_RACE:
            if (next.state.intent != MDKR_INTENT_ONLINE_PRIVATE ||
                next.state.scene != MDKR_SCENE_RACE_CHROME ||
                next.state.engine != MDKR_ENGINE_RACING ||
                next.state.room != MDKR_ROOM_RACING) goto invalid;
            if (!append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_STOP_ENGINE, 0u) ||
                !append_effect(&next, &step,
                               MDKR_SESSION_EFFECT_DISCONNECT_ROOM, 0u)) {
                goto invalid;
            }
            next.state.intent = MDKR_INTENT_NONE;
            next.state.scene = MDKR_SCENE_HOME;
            next.state.engine = MDKR_ENGINE_STOPPED;
            next.state.connectivity = MDKR_CONNECTIVITY_OFFLINE;
            next.state.room = MDKR_ROOM_NONE;
            next.state.overlay = MDKR_OVERLAY_CLOSED;
            next.state.engine_pause_requested = false;
            next.state.last_error = MDKR_SESSION_ERROR_NONE;
            break;

        case MDKR_SESSION_COMMAND_SET_UPDATE_STATE:
            if (command->value > (uint32_t)MDKR_UPDATE_REQUIRED) goto invalid;
            next.state.update = (MdkrUpdateState)command->value;
            break;

        case MDKR_SESSION_COMMAND_RECOVER:
            if (command->value == (uint32_t)MDKR_SESSION_ERROR_NONE ||
                command->value > (uint32_t)MDKR_SESSION_ERROR_PROTOCOL) {
                goto invalid;
            }
            next.state.scene = MDKR_SCENE_RECOVERY;
            next.state.overlay = MDKR_OVERLAY_CLOSED;
            next.state.engine_pause_requested = false;
            next.state.last_error = (MdkrSessionError)command->value;
            break;

        default:
            goto invalid;
    }

    next.state.generation++;
    if (next.state.generation == MDKR_SESSION_ANY_GENERATION) {
        next.state.generation = 1u;
    }
    next.state.last_error = command->type == MDKR_SESSION_COMMAND_RECOVER
        ? next.state.last_error : MDKR_SESSION_ERROR_NONE;
    if (!mdkr_session_state_valid(&next.state)) {
        return rejected(MDKR_SESSION_ERROR_INVALID_COMPOSITION);
    }
    /* Effects describe the state produced by this command, not the state the
     * command observed. Stamp only after the generation is committed. */
    for (uint8_t i = 0; i < step.effect_count; ++i) {
        step.effects[i].generation = next.state.generation;
        step.effects[i].match_epoch = next.state.match_epoch;
    }
    *core = next;
    step.accepted = true;
    return step;

invalid:
    return rejected(MDKR_SESSION_ERROR_INVALID_TRANSITION);
}
