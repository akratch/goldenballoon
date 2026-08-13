#include "lobby_view_model.h"

#include "session/session_core.h"

#include <string.h>

static bool title_word(const char *begin, const char *end) {
    const char *cursor;
    if (begin == NULL || end == NULL || begin >= end ||
        *begin < 'A' || *begin > 'Z') return false;
    for (cursor = begin + 1; cursor < end; cursor++) {
        if (*cursor < 'a' || *cursor > 'z') return false;
    }
    return true;
}

/* The peer transcript generator emits exactly 3 Title-Case compounds. Keep
 * this projection parser narrower than generic display text: it prevents an
 * adapter bug from turning cryptographic UI into an injection/overflow seam. */
static bool verification_phrase_valid(const char *phrase, size_t *length_out) {
    const char *cursor = phrase;
    unsigned compound;
    size_t length = 0u;
    if (phrase == NULL || length_out == NULL) return false;
    while (length < MDKR_ONLINE_VERIFICATION_PHRASE_BYTES &&
           phrase[length] != '\0') length++;
    if (length == 0u || length == MDKR_ONLINE_VERIFICATION_PHRASE_BYTES)
        return false;
    for (compound = 0u; compound < 3u; compound++) {
        const char *left = cursor;
        const char *hyphen;
        const char *right;
        const char *end;
        while (*cursor != '\0' && *cursor != '-' && *cursor != ' ') cursor++;
        if (*cursor != '-') return false;
        hyphen = cursor++;
        right = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            if (*cursor == '-') return false;
            cursor++;
        }
        end = cursor;
        if (!title_word(left, hyphen) || !title_word(right, end)) return false;
        if (compound < 2u) {
            if (*cursor != ' ') return false;
            cursor++;
            if (*cursor == '\0' || *cursor == ' ') return false;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    if (*cursor != '\0') return false;
    *length_out = length;
    return true;
}

static MdkrOnlineViewControl control(MdkrOnlineViewAction action,
                                     const char *label, bool enabled) {
    MdkrOnlineViewControl value;
    value.action = action;
    value.label = label;
    value.visible = action != MDKR_ONLINE_VIEW_ACTION_NONE;
    value.enabled = value.visible && enabled;
    return value;
}

static MdkrOnlineTimeoutView timeout_view(const char *title,
                                          const char *explanation,
                                          MdkrOnlineViewAction action,
                                          const char *label) {
    MdkrOnlineTimeoutView value;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.title = title;
    value.explanation = explanation;
    value.primary = control(action, label, true);
    return value;
}

static MdkrOnlineViewFailure session_failure(const MdkrSessionState *session) {
    if (session->update == MDKR_UPDATE_REQUIRED ||
        session->last_error == MDKR_SESSION_ERROR_UPDATE_REQUIRED) {
        return MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED;
    }
    switch (session->last_error) {
        case MDKR_SESSION_ERROR_CAPACITY:
            return MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL;
        case MDKR_SESSION_ERROR_ENGINE_FAILED:
            return MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED;
        case MDKR_SESSION_ERROR_CONNECTION_LOST:
            return MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE;
        case MDKR_SESSION_ERROR_PROTOCOL:
            return MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED;
        default:
            return MDKR_ONLINE_VIEW_FAILURE_NONE;
    }
}

static bool lobby_phase_matches(const MdkrSessionState *session,
                                const MdkrOnlineLobby *lobby) {
    if (session->match_epoch != lobby->match_epoch) return false;
    switch (session->room) {
        case MDKR_ROOM_OPEN:
        case MDKR_ROOM_PREFLIGHT:
        case MDKR_ROOM_SELECTING:
            return lobby->phase == MDKR_ONLINE_LOBBY;
        case MDKR_ROOM_LOADING:
        case MDKR_ROOM_COUNTDOWN:
            return lobby->phase == MDKR_ONLINE_LOADING;
        case MDKR_ROOM_RACING:
            return lobby->phase == MDKR_ONLINE_RACING;
        case MDKR_ROOM_RESULTS:
            return lobby->phase == MDKR_ONLINE_RESULTS;
        case MDKR_ROOM_CLOSED:
            return lobby->phase == MDKR_ONLINE_CLOSED;
        case MDKR_ROOM_NONE:
            return false;
    }
    return false;
}

static const MdkrOnlineMember *local_member(const MdkrOnlineLobby *lobby,
                                            uint64_t endpoint_id) {
    unsigned index;
    if (lobby == NULL || endpoint_id == 0u) return NULL;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        if (lobby->members[index].occupied &&
            lobby->members[index].endpoint_id == endpoint_id) {
            return &lobby->members[index];
        }
    }
    return NULL;
}

static void project_counts(const MdkrOnlineLobby *lobby,
                           MdkrOnlineViewModel *model) {
    unsigned index;
    if (lobby == NULL) return;
    model->member_count = lobby->member_count;
    model->seat_count = lobby->seat_count;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        if (lobby->members[index].occupied && lobby->members[index].ready) {
            model->ready_count++;
        }
    }
}

static bool selection_state(const MdkrOnlineLobby *lobby, uint64_t endpoint_id,
                            bool *character_missing, bool *vehicle_missing,
                            bool *vote_missing) {
    unsigned index;
    bool found = false;
    *character_missing = false;
    *vehicle_missing = false;
    *vote_missing = false;
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        const MdkrOnlineSeat *seat = &lobby->seats[index];
        if (!seat->occupied || seat->endpoint_id != endpoint_id) continue;
        found = true;
        if (seat->character_id == MDKR_ONLINE_NO_CHARACTER) {
            *character_missing = true;
        }
        if (seat->vehicle_id == MDKR_ONLINE_NO_VEHICLE) {
            *vehicle_missing = true;
        }
        if (seat->vote_track == MDKR_ONLINE_NO_VOTE) {
            *vote_missing = true;
        }
    }
    return found;
}

static void recovery_model(MdkrOnlineViewFailure failure,
                           MdkrOnlineViewModel *model) {
    model->kind = MDKR_ONLINE_VIEW_RECOVERY;
    model->failure = failure;
    model->announcement = MDKR_ONLINE_ANNOUNCE_ASSERTIVE;
    model->local_play_available = true;
    model->cancel = control(MDKR_ONLINE_VIEW_ACTION_RETURN_HOME,
                            "Return Home", true);
    model->secondary = control(MDKR_ONLINE_VIEW_ACTION_PLAY_HERE,
                               "Play Here", true);
    switch (failure) {
        case MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED:
            model->title = "Invite Expired";
            model->explanation =
                "That invite expired. Show a new code on the display.";
            model->primary = control(
                MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE,
                "Enter Another Code", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_INVITE_ROTATED:
            model->title = "Invite Replaced";
            model->explanation =
                "The display replaced that invite. Scan the newest QR code.";
            model->primary = control(
                MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE,
                "Enter Another Code", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL:
            model->title = "Room Full";
            model->explanation =
                "This room already has 4 racer seats in use.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_PLAY_HERE,
                                     "Play Here", true);
            model->secondary = control(MDKR_ONLINE_VIEW_ACTION_NONE, NULL, false);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_SERVICE_BUDGET_SAFE:
            model->title = "Online Rooms Are Full Right Now";
            model->explanation =
                "New online rooms are paused to keep hosting free. Local play still works.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_PLAY_HERE,
                                     "Play Here", true);
            model->secondary = control(MDKR_ONLINE_VIEW_ACTION_RETRY,
                                       "Try Again", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD:
            model->title = "Game Update Needed";
            model->explanation =
                "Everyone needs the same game version before the room can start.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME,
                                     "Update Game", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM:
            model->title = "Different ROM Version";
            model->explanation =
                "Everyone needs the same supported ROM revision.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM,
                                     "Choose ROM", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS:
            model->title = "Gameplay Settings Differ";
            model->explanation =
                "Use the room's gameplay settings before getting ready.";
            model->primary = control(
                MDKR_ONLINE_VIEW_ACTION_USE_ROOM_SETTINGS,
                "Use Room Settings", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_CONTROLLER_NEEDED:
            model->title = "Controller Needed";
            model->explanation =
                "Connect a controller for every local racer seat.";
            model->primary = control(
                MDKR_ONLINE_VIEW_ACTION_SETUP_CONTROLLER,
                "Set Up Controller", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK:
            model->title = "Connection Check Failed";
            model->explanation =
                "This room could not establish a playable connection.";
            model->primary = control(
                MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR,
                "Open Connection Doctor", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_RELAY_CAPACITY:
            model->title = "Limited Connection Is Full";
            model->explanation =
                "The free backup connection is full. Direct rooms and local play still work.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_RETRY,
                                     "Try Direct Again", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_NETWORKS_CANNOT_CONNECT:
            model->title = "Networks Could Not Connect";
            model->explanation =
                "These networks cannot make a direct connection right now.";
            model->primary = control(
                MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR,
                "Open Connection Doctor", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED:
            model->title = "Update Required";
            model->explanation =
                "Update the game before rejoining this private room.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME,
                                     "Update & Rejoin", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED:
            model->title = "Room Ended";
            model->explanation = "The room owner ended this private room.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_PLAY_HERE,
                                     "Play Here", true);
            model->secondary = control(MDKR_ONLINE_VIEW_ACTION_NONE, NULL, false);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED:
            model->title = "Room Expired";
            model->explanation = "This inactive private room has ended.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_PLAY_HERE,
                                     "Play Here", true);
            model->secondary = control(MDKR_ONLINE_VIEW_ACTION_NONE, NULL, false);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_ENGINE_FAILED:
            model->title = "Race Could Not Start";
            model->explanation =
                "The game closed safely. Your room and local settings were not changed.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
                                     "Return to Lobby", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_EPOCH_MISMATCH:
            model->title = "Race Start Changed";
            model->explanation =
                "The room prepared a newer race. Return to the lobby to sync everyone.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
                                     "Return to Lobby", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH:
            model->title = "Words Did Not Match";
            model->explanation =
                "The secure connection may have changed. Do not continue until everyone compares a new phrase.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_RETRY,
                                     "Reconnect Securely", true);
            model->cancel = control(MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM,
                                    "Leave Room", true);
            break;
        case MDKR_ONLINE_VIEW_FAILURE_NONE:
        case MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE:
        case MDKR_ONLINE_VIEW_FAILURE_COUNT:
        default:
            model->failure = MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE;
            model->title = "Could Not Reach the Room";
            model->explanation =
                "Check your connection, then try this private room again.";
            model->primary = control(MDKR_ONLINE_VIEW_ACTION_RETRY,
                                     "Try Again", true);
            break;
    }
    model->timeout = timeout_view(
        "Could Not Reconnect",
        "The room is still unavailable. Local play remains ready.",
        MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, "Play Here");
}

bool mdkr_online_view_model_build(const MdkrOnlineViewInput *input,
                                  MdkrOnlineViewModel *output) {
    MdkrOnlineViewModel next;
    const MdkrOnlineMember *member = NULL;
    MdkrOnlineViewFailure failure;
    bool character_missing = false;
    bool vehicle_missing = false;
    bool vote_missing = false;
    size_t verification_phrase_length = 0u;
    MdkrOnlineInviteState invite_state;

    if (input == NULL || output == NULL || input->session == NULL ||
        !mdkr_session_state_valid(input->session) ||
        input->journey < MDKR_ONLINE_JOURNEY_CREATE ||
        input->journey > MDKR_ONLINE_JOURNEY_REMATCH ||
        input->invite_state < MDKR_ONLINE_INVITE_PREPARING ||
        input->invite_state > MDKR_ONLINE_INVITE_REFRESH_AVAILABLE) {
        return false;
    }
    if (input->failure < MDKR_ONLINE_VIEW_FAILURE_NONE) return false;
    if (input->verification_phrase != NULL &&
        (input->session->scene != MDKR_SCENE_LOBBY ||
         input->session->room != MDKR_ROOM_PREFLIGHT ||
         !verification_phrase_valid(input->verification_phrase,
                                    &verification_phrase_length))) {
        return false;
    }
    if (input->lobby != NULL) {
        if (!mdkr_online_lobby_valid(input->lobby) ||
            input->session->intent != MDKR_INTENT_ONLINE_PRIVATE ||
            !lobby_phase_matches(input->session, input->lobby)) {
            return false;
        }
        member = local_member(input->lobby, input->local_endpoint_id);
        if (member == NULL) return false;
    }

    /* Invite custody belongs to the leader of an open room in the lobby
     * phase. Outside that exact custody the projection degrades to
     * "Preparing", which renders a disabled Share Invite control, rather
     * than refusing to build a view at all: a guest still needs its room,
     * preflight and recovery views. Only this local value reaches the
     * Share Invite control below, so a non-leader, a non-open room or a
     * stale phase can never present a live invitation. */
    invite_state = input->invite_state;
    if (input->lobby == NULL ||
        input->lobby->phase != MDKR_ONLINE_LOBBY ||
        input->session->room != MDKR_ROOM_OPEN ||
        input->lobby->leader_endpoint_id != input->local_endpoint_id) {
        invite_state = MDKR_ONLINE_INVITE_PREPARING;
    }

    memset(&next, 0, sizeof(next));
    project_counts(input->lobby, &next);
    if (input->lobby != NULL) {
        next.local_member_is_leader =
            input->lobby->leader_endpoint_id == input->local_endpoint_id;
    }

    failure = input->failure;
    if (failure == MDKR_ONLINE_VIEW_FAILURE_NONE) {
        failure = session_failure(input->session);
    }
    if (input->session->scene == MDKR_SCENE_RECOVERY ||
        failure != MDKR_ONLINE_VIEW_FAILURE_NONE) {
        recovery_model(failure, &next);
        *output = next;
        return true;
    }

    next.announcement = MDKR_ONLINE_ANNOUNCE_POLITE;
    switch (input->session->scene) {
        case MDKR_SCENE_HOME:
            next.kind = MDKR_ONLINE_VIEW_ENTRY;
            next.title = "Play Online";
            next.explanation =
                "Create a private room for friends, or join one with an invite.";
            next.primary = control(MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM,
                                   "Create Private Room", true);
            next.secondary = control(MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM,
                                     "Join Room", true);
            next.cancel = control(MDKR_ONLINE_VIEW_ACTION_RETURN_HOME,
                                  "Back", true);
            next.local_play_available = true;
            break;

        case MDKR_SCENE_JOIN:
            next.kind = MDKR_ONLINE_VIEW_CONNECTING;
            if (input->journey == MDKR_ONLINE_JOURNEY_CREATE) {
                next.title = "Creating Private Room…";
                next.explanation =
                    "Preparing a private invite for this display.";
            } else {
                next.title = "Joining Private Room…";
                next.explanation =
                    "Checking the invite before entering the room.";
            }
            next.status = "Connecting…";
            next.primary = control(
                MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS,
                "Connection Details", true);
            next.cancel = control(MDKR_ONLINE_VIEW_ACTION_RETURN_HOME,
                                  "Cancel", true);
            next.timeout = timeout_view(
                "Room Took Too Long",
                "The room did not respond. Check your connection and try again.",
                MDKR_ONLINE_VIEW_ACTION_RETRY, "Try Again");
            next.local_play_available = true;
            break;

        case MDKR_SCENE_LOBBY:
            if (input->lobby == NULL || member == NULL) return false;
            next.cancel = control(MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM,
                                  "Leave Room", true);
            next.secondary = control(
                MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS,
                "Connection Details", true);
            if (input->session->room == MDKR_ROOM_OPEN) {
                const bool invite_ready =
                    invite_state == MDKR_ONLINE_INVITE_READY;
                const bool invite_refresh =
                    invite_state == MDKR_ONLINE_INVITE_REFRESH_AVAILABLE;
                next.kind = MDKR_ONLINE_VIEW_ROOM;
                next.title = "Private Room";
                next.explanation =
                    "Invite friends, then check everyone's setup together.";
                if (next.member_count >= 2u) {
                    next.status = "Friends Joined";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
                        "Check Setup", true);
                    if (next.local_member_is_leader) {
                        next.secondary = control(
                            MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE,
                            invite_ready ? "Share Invite" :
                                invite_refresh ? "New Invitation" :
                                    "Preparing Invite…",
                            invite_ready || invite_refresh);
                    }
                } else {
                    next.status = invite_ready ? "Invite Ready" :
                        invite_refresh ? "Invitation Expired" :
                            "Preparing Invite…";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE,
                        invite_ready ? "Share Invite" :
                            invite_refresh ? "New Invitation" :
                                "Preparing Invite…",
                        invite_ready || invite_refresh);
                }
            } else if (input->session->room == MDKR_ROOM_PREFLIGHT) {
                next.kind = MDKR_ONLINE_VIEW_PREFLIGHT;
                if (input->verification_phrase != NULL) {
                    next.title = "Compare These Words";
                    next.explanation =
                        "Read all 3 groups aloud. Continue only when every display shows exactly the same words.";
                    next.status = "Secure Phrase Ready";
                    memcpy(next.verification_phrase,
                           input->verification_phrase,
                           verification_phrase_length + 1u);
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE,
                        "Words Match", true);
                    next.secondary = control(
                        MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH,
                        "Words Differ", true);
                } else {
                    next.title = "Checking Everyone's Setup";
                    next.explanation =
                        "Checking game versions, ROMs, settings, controllers and connections.";
                    next.status = "Checking…";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS,
                        "Review Checks", true);
                    next.timeout = timeout_view(
                        "Setup Check Took Too Long",
                        "One or more checks did not finish. Retry before getting ready.",
                        MDKR_ONLINE_VIEW_ACTION_RETRY, "Retry Checks");
                }
            } else if (input->session->room == MDKR_ROOM_SELECTING) {
                if (!selection_state(input->lobby, input->local_endpoint_id,
                                     &character_missing, &vehicle_missing,
                                     &vote_missing)) {
                    return false;
                }
                next.kind = MDKR_ONLINE_VIEW_SELECTING;
                next.title = "Choose Your Racers";
                next.explanation =
                    "Choose one character and vehicle for every local racer seat.";
                if (character_missing) {
                    next.status = "Character Needed";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
                        "Choose Character", true);
                } else if (vehicle_missing) {
                    next.status = "Vehicle Needed";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
                        "Choose Vehicle", true);
                } else if (vote_missing) {
                    next.status = "Track Vote Needed";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK,
                        "Choose Track", true);
                } else if (!member->ready) {
                    next.status = "Selections Complete";
                    next.primary = control(MDKR_ONLINE_VIEW_ACTION_READY,
                                           "Ready", true);
                } else if (next.local_member_is_leader &&
                           next.member_count >= 2u &&
                           next.ready_count == next.member_count &&
                           input->race_admission_enabled) {
                    next.status = "Everyone Ready";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_START_RACE,
                        "Start Race", true);
                } else {
                    next.status = next.ready_count == next.member_count
                        ? "Everyone Ready" : "Waiting for Friends";
                    next.primary = control(
                        MDKR_ONLINE_VIEW_ACTION_CHANGE_SELECTION,
                        "Change Selection", true);
                }
            } else {
                return false;
            }
            break;

        case MDKR_SCENE_LOADING:
            if (input->lobby == NULL || member == NULL) return false;
            next.cancel = control(MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
                                  "Cancel to Lobby", true);
            next.primary = control(
                MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS,
                "Connection Details", true);
            if (input->session->room == MDKR_ROOM_COUNTDOWN) {
                next.kind = MDKR_ONLINE_VIEW_COUNTDOWN;
                next.title = "Starting Together…";
                next.explanation =
                    "Keeping every display on the same race start.";
                next.status = "Starting…";
                next.timeout = timeout_view(
                    "Could Not Start Together",
                    "The race start changed or an endpoint fell behind.",
                    MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
                    "Return to Lobby");
            } else {
                next.kind = MDKR_ONLINE_VIEW_LOADING;
                next.title = input->journey == MDKR_ONLINE_JOURNEY_REMATCH
                    ? "Preparing Race Again…" : "Loading Race…";
                next.explanation =
                    "Waiting until every display has loaded the same race.";
                next.status = "Loading…";
                next.timeout = timeout_view(
                    input->journey == MDKR_ONLINE_JOURNEY_REMATCH
                        ? "Could Not Prepare the Rematch"
                        : "Race Did Not Load",
                    "Return to the lobby without ending the private room.",
                    MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
                    "Return to Lobby");
            }
            break;

        case MDKR_SCENE_RACE_CHROME:
            if (input->lobby == NULL || member == NULL) return false;
            next.kind = MDKR_ONLINE_VIEW_RACING;
            next.title = "Online Race";
            next.explanation =
                "The race keeps running while this non-pausing panel is open.";
            next.status = input->session->connectivity == MDKR_CONNECTIVITY_DIRECT
                ? "Direct Connection" : "Limited Connection";
            next.primary = control(
                MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS,
                "Connection Details", true);
            next.cancel = control(MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE,
                                  "Leave Race", true);
            break;

        case MDKR_SCENE_RESULTS:
            if (input->lobby == NULL || member == NULL) return false;
            next.kind = MDKR_ONLINE_VIEW_RESULTS;
            next.title = "Race Complete";
            next.explanation =
                "Keep the private room together for another race.";
            next.status = "Results Confirmed";
            next.primary = control(MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN,
                                   "Race Again", true);
            next.secondary = control(MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK,
                                     "Change Track", true);
            next.cancel = control(MDKR_ONLINE_VIEW_ACTION_RETURN_HOME,
                                  "Return Home", true);
            break;

        default:
            return false;
    }

    if (next.title == NULL || next.explanation == NULL ||
        !next.primary.visible || !next.cancel.visible) {
        return false;
    }
    *output = next;
    return true;
}
