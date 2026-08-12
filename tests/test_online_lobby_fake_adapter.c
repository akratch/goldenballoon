#include "platform/online/lobby_fake_adapter.h"

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
        value.build_id[index] = (uint8_t)(index + 3u);
    }
    for (index = 0u; index < sizeof(value.gameplay_digest); index++) {
        value.gameplay_digest[index] = (uint8_t)(0x40u + index);
    }
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

static MdkrOnlineFakeStep send_action(MdkrOnlineFakeAdapter *adapter,
                                      uint64_t request_id,
                                      MdkrOnlineViewAction action,
                                      unsigned seat, unsigned value) {
    MdkrOnlineFakeCommand command;
    memset(&command, 0, sizeof(command));
    command.expected_revision = adapter->revision;
    command.request_id = request_id;
    command.action = action;
    command.seat = seat;
    command.value = value;
    return mdkr_online_fake_dispatch(adapter, &command);
}

static void expect_view(MdkrOnlineFakeAdapter *adapter,
                        MdkrOnlineViewKind kind,
                        MdkrOnlineViewAction primary,
                        const char *message) {
    MdkrOnlineViewModel model;
    expect(mdkr_online_fake_view(adapter, &model) && model.kind == kind &&
               model.primary.action == primary,
           message);
}

static void prepare_local(MdkrOnlineFakeAdapter *adapter,
                          uint64_t *request_id, bool choices_needed) {
    if (choices_needed) {
        expect(send_action(adapter, (*request_id)++,
                           MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
                           0u, 1u).accepted,
               "local character action reaches room reducer");
        expect(send_action(adapter, (*request_id)++,
                           MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
                           0u, 0u).accepted,
               "local vehicle action reaches room reducer");
    }
    expect(send_action(adapter, (*request_id)++,
                       MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK,
                       0u, 5u).accepted,
           "local track vote reaches room reducer");
    expect(send_action(adapter, (*request_id)++,
                       MDKR_ONLINE_VIEW_ACTION_READY,
                       0u, 1u).accepted,
           "local Ready reaches room reducer");
}

static void test_two_race_create_vertical_and_mutations(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineFakeAdapter adapter;
    MdkrOnlineFakeAdapter before;
    MdkrOnlineFakeStep step;
    MdkrOnlineFakeCommand first;
    MdkrOnlineViewModel model;
    uint64_t request_id = 1u;
    uint32_t token;

    expect(mdkr_online_fake_init(&adapter, 91u, &compat, true),
           "fake adapter initializes with explicit local race policy");
    expect_view(&adapter, MDKR_ONLINE_VIEW_ENTRY,
                MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM,
                "vertical slice starts at private online entry");

    memset(&first, 0, sizeof(first));
    first.expected_revision = adapter.revision;
    first.request_id = request_id++;
    first.action = MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM;
    step = mdkr_online_fake_dispatch(&adapter, &first);
    expect(step.accepted && step.pending_token != 0u,
           "Create publishes one cancellable callback token");
    expect(mdkr_online_fake_dispatch(&adapter, &first).accepted &&
               mdkr_online_fake_dispatch(&adapter, &first).duplicate,
           "exact double action returns the cached success");
    first.action = MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM;
    expect(mdkr_online_fake_dispatch(&adapter, &first).error ==
               MDKR_ONLINE_FAKE_ERROR_REQUEST_CONFLICT,
           "same request id with a different action fails closed");
    token = step.pending_token;
    before = adapter;
    expect(!mdkr_online_fake_complete(&adapter, token + 1u,
                                      MDKR_ONLINE_VIEW_FAILURE_NONE).accepted &&
               memcmp(&adapter, &before, sizeof(adapter)) == 0,
           "stale create callback cannot mutate the current route");
    expect(mdkr_online_fake_complete(
               &adapter, token, MDKR_ONLINE_VIEW_FAILURE_NONE).accepted,
           "matching create callback opens the room");
    expect_view(&adapter, MDKR_ONLINE_VIEW_ROOM,
                MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE,
                "created room offers its scoped invite");
    before = adapter;
    expect(!send_action(&adapter, request_id,
                        MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
                        0u, 1u).accepted &&
               memcmp(&adapter, &before, sizeof(adapter)) == 0,
           "route-confused character action cannot bypass room view");

    step = send_action(&adapter, request_id++,
                       MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE, 0u, 0u);
    token = step.pending_token;
    expect(step.accepted && mdkr_online_fake_complete(
               &adapter, token, MDKR_ONLINE_VIEW_FAILURE_NONE).accepted,
           "fake friend joins through the actual reducer");
    expect_view(&adapter, MDKR_ONLINE_VIEW_ROOM,
                MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
                "joined room advances to setup check");

    step = send_action(&adapter, request_id++,
                       MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP, 0u, 0u);
    token = step.pending_token;
    expect(step.accepted && mdkr_online_fake_view(&adapter, &model) &&
               model.kind == MDKR_ONLINE_VIEW_PREFLIGHT &&
               model.timeout.present,
           "preflight wait renders its bounded recovery contract");
    expect(mdkr_online_fake_complete(
               &adapter, token, MDKR_ONLINE_VIEW_FAILURE_NONE).accepted,
           "preflight completion enters launcher selection");
    expect(mdkr_online_fake_prepare_peer(
               &adapter, adapter.revision, 2u, 0u, 5u).accepted,
           "friend selections use one atomic fake transaction over reducer commands");
    prepare_local(&adapter, &request_id, true);
    expect_view(&adapter, MDKR_ONLINE_VIEW_SELECTING,
                MDKR_ONLINE_VIEW_ACTION_START_RACE,
                "leader sees Start only after both complete Ready states");

    step = send_action(&adapter, request_id++,
                       MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u, 1u);
    token = step.pending_token;
    expect(step.accepted && adapter.session.state.match_epoch == 1u &&
               adapter.lobby.match_epoch == 1u,
           "first loading barrier rotates matching session and room epochs");
    expect(send_action(&adapter, request_id++,
                       MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
                       0u, 0u).accepted &&
               adapter.session.state.scene == MDKR_SCENE_LOBBY &&
               adapter.lobby.phase == MDKR_ONLINE_LOBBY,
           "Cancel to Lobby retires loading without ending the room");
    before = adapter;
    expect(!mdkr_online_fake_complete(
                &adapter, token, MDKR_ONLINE_VIEW_FAILURE_NONE).accepted &&
               memcmp(&adapter, &before, sizeof(adapter)) == 0,
           "canceled load callback cannot resurrect the old route");

    expect(mdkr_online_fake_prepare_peer(
               &adapter, adapter.revision, 2u, 0u, 5u).accepted,
           "friend can prepare again after a canceled barrier");
    prepare_local(&adapter, &request_id, false);
    step = send_action(&adapter, request_id++,
                       MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u, 1u);
    token = step.pending_token;
    expect(step.accepted && adapter.session.state.match_epoch == 2u &&
               adapter.lobby.match_epoch == 2u,
           "retry consumes a fresh epoch instead of reusing canceled identity");
    expect(mdkr_online_fake_complete(
               &adapter, token, MDKR_ONLINE_VIEW_FAILURE_NONE).accepted,
           "matching load callback reaches racing");
    expect_view(&adapter, MDKR_ONLINE_VIEW_RACING,
                MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS,
                "fake vertical slice reaches non-pausing race chrome");
    before = adapter;
    expect(!mdkr_online_fake_finish_race(
                &adapter, adapter.revision - 1u).accepted &&
               memcmp(&adapter, &before, sizeof(adapter)) == 0,
           "stale engine result cannot finish the current race");
    expect(mdkr_online_fake_finish_race(&adapter, adapter.revision).accepted,
           "current fake engine result reaches confirmed Results");
    expect_view(&adapter, MDKR_ONLINE_VIEW_RESULTS,
                MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN,
                "results keeps the party and offers Race Again");

    expect(send_action(&adapter, request_id++,
                       MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN, 0u, 0u).accepted &&
               adapter.session.state.scene == MDKR_SCENE_LOBBY &&
               adapter.lobby.phase == MDKR_ONLINE_LOBBY,
           "Race Again returns to launcher-owned voting without disconnecting");
    expect(mdkr_online_fake_prepare_peer(
               &adapter, adapter.revision, 2u, 0u, 5u).accepted,
           "friend prepares second race");
    prepare_local(&adapter, &request_id, false);
    step = send_action(&adapter, request_id++,
                       MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u, 1u);
    token = step.pending_token;
    expect(mdkr_online_fake_complete(
               &adapter, token, MDKR_ONLINE_VIEW_FAILURE_NONE).accepted &&
           mdkr_online_fake_finish_race(&adapter, adapter.revision).accepted,
           "second race completes through the same persistent fake adapter");
    expect(adapter.session.state.match_epoch == 3u &&
               adapter.lobby.match_epoch == 3u &&
               adapter.lobby.member_count == 2u,
           "two races plus canceled epoch preserve party and fresh identity");
}

static void test_join_and_sanitized_failure_paths(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineFakeAdapter adapter;
    MdkrOnlineFakeStep step;
    MdkrOnlineViewModel model;

    mdkr_online_fake_init(&adapter, 92u, &compat, false);
    step = send_action(&adapter, 1u, MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM, 0u, 0u);
    expect(step.accepted && mdkr_online_fake_complete(
               &adapter, step.pending_token,
               MDKR_ONLINE_VIEW_FAILURE_NONE).accepted &&
               adapter.lobby.member_count == 2u &&
               adapter.lobby.leader_endpoint_id == adapter.peer_endpoint_id,
           "join fixture enters an existing room without stealing leadership");
    expect_view(&adapter, MDKR_ONLINE_VIEW_ROOM,
                MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
                "joined endpoint shares the same room view contract");

    mdkr_online_fake_init(&adapter, 93u, &compat, false);
    step = send_action(&adapter, 1u, MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM, 0u, 0u);
    expect(mdkr_online_fake_complete(
               &adapter, step.pending_token,
               MDKR_ONLINE_VIEW_FAILURE_SERVICE_BUDGET_SAFE).accepted &&
               mdkr_online_fake_view(&adapter, &model) &&
               model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
               model.primary.action == MDKR_ONLINE_VIEW_ACTION_PLAY_HERE &&
               strstr(model.explanation, "hosting free") != NULL,
           "$0 capacity refusal keeps exact local recovery and sanitized copy");
}

static void test_deterministic_gallery(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    bool view_seen[MDKR_ONLINE_VIEW_RECOVERY + 1u];
    bool failure_seen[MDKR_ONLINE_VIEW_FAILURE_COUNT];
    size_t index;
    size_t other;
    size_t visible_timeouts = 0u;
    memset(view_seen, 0, sizeof(view_seen));
    memset(failure_seen, 0, sizeof(failure_seen));

    expect(mdkr_online_fake_gallery_count() >= 30u,
           "gallery covers the complete journey and typed recovery catalog");
    expect(mdkr_online_fake_gallery_at(mdkr_online_fake_gallery_count()) == NULL,
           "gallery lookup rejects its exact upper bound");
    for (index = 0u; index < mdkr_online_fake_gallery_count(); index++) {
        const MdkrOnlineFakeGallerySpec *spec =
            mdkr_online_fake_gallery_at(index);
        MdkrOnlineFakeAdapter adapter;
        MdkrOnlineViewModel model;
        expect(spec != NULL && spec->slug != NULL && spec->slug[0] != '\0',
               "each gallery row has a stable nonempty slug");
        if (spec == NULL) continue;
        for (other = index + 1u;
             other < mdkr_online_fake_gallery_count(); other++) {
            const MdkrOnlineFakeGallerySpec *candidate =
                mdkr_online_fake_gallery_at(other);
            expect(candidate != NULL &&
                       strcmp(spec->slug, candidate->slug) != 0,
                   "gallery slugs are unique");
        }
        expect(mdkr_online_fake_gallery_find(spec->slug) == spec,
               "gallery slug resolves to its canonical row");
        expect(mdkr_online_fake_init(&adapter, 1000u + index, &compat,
                                     spec->requires_race_admission) &&
                   mdkr_online_fake_prepare_gallery(&adapter, spec->slug) &&
                   mdkr_online_fake_view(&adapter, &model),
               "gallery row replays through reducers into a valid view");
        expect(model.kind == spec->kind &&
                   model.failure == spec->failure &&
                   model.primary.action == spec->primary_action &&
                   model.timeout.present == spec->timeout_present &&
                   adapter.timeout_expired ==
                       (strncmp(spec->slug, "timeout-", 8u) == 0),
               "gallery output matches its published copy/action contract");
        if (strncmp(spec->slug, "timeout-", 8u) == 0) visible_timeouts++;
        if (model.kind >= MDKR_ONLINE_VIEW_ENTRY &&
            model.kind <= MDKR_ONLINE_VIEW_RECOVERY) {
            view_seen[model.kind] = true;
        }
        if (model.failure > MDKR_ONLINE_VIEW_FAILURE_NONE &&
            model.failure < MDKR_ONLINE_VIEW_FAILURE_COUNT) {
            failure_seen[model.failure] = true;
        }
    }
    for (index = MDKR_ONLINE_VIEW_ENTRY;
         index <= MDKR_ONLINE_VIEW_RECOVERY; index++) {
        expect(view_seen[index], "every player-visible view kind has a gallery row");
    }
    for (index = MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED;
         index < MDKR_ONLINE_VIEW_FAILURE_COUNT; index++) {
        expect(failure_seen[index],
               "every typed product failure has a gallery row");
    }
    expect(visible_timeouts == 6u,
           "gallery explicitly renders every distinct timeout outcome");

    {
        MdkrOnlineFakeAdapter adapter;
        MdkrOnlineFakeAdapter before;
        expect(mdkr_online_fake_init(&adapter, 2000u, &compat, false),
               "gallery negative-control adapter initializes");
        before = adapter;
        expect(!mdkr_online_fake_prepare_gallery(&adapter, "select-start") &&
                   memcmp(&adapter, &before, sizeof(adapter)) == 0,
               "gallery cannot elevate the local race-admission policy");
        expect(!mdkr_online_fake_prepare_gallery(&adapter, "not-a-case") &&
                   memcmp(&adapter, &before, sizeof(adapter)) == 0,
               "unknown gallery slug rejects fail-atomically");

        expect(mdkr_online_fake_prepare_gallery(
                   &adapter, "connecting-create"),
               "ordinary connecting wait prepares without an elapsed timeout");
        before = adapter;
        expect(!mdkr_online_fake_expire_timeout(
                    &adapter, adapter.revision - 1u).accepted &&
                   memcmp(&adapter, &before, sizeof(adapter)) == 0,
               "stale timeout callback rejects fail-atomically");
        expect(mdkr_online_fake_expire_timeout(
                   &adapter, adapter.revision).accepted &&
                   adapter.timeout_expired,
               "current local deadline exposes its bounded recovery outcome");
        before = adapter;
        expect(!mdkr_online_fake_expire_timeout(
                    &adapter, adapter.revision).accepted &&
                   memcmp(&adapter, &before, sizeof(adapter)) == 0,
               "one deadline cannot publish duplicate timeout transitions");
        {
            const uint32_t expired_token = adapter.pending_token;
            expect(send_action(&adapter, adapter.last_request_id + 1u,
                               MDKR_ONLINE_VIEW_ACTION_RETRY,
                               0u, 0u).accepted &&
                       !adapter.timeout_expired &&
                       adapter.pending_token != 0u &&
                       adapter.pending_token != expired_token,
                   "timeout Try Again publishes one fresh callback token");
            before = adapter;
            expect(!mdkr_online_fake_complete(
                        &adapter, expired_token,
                        MDKR_ONLINE_VIEW_FAILURE_NONE).accepted &&
                       memcmp(&adapter, &before, sizeof(adapter)) == 0,
                   "retry makes the expired request callback terminal");
        }
    }

    {
        MdkrOnlineFakeAdapter adapter;
        MdkrOnlineFakeStep step;
        expect(mdkr_online_fake_init(&adapter, 3000u, &compat, false) &&
                   mdkr_online_fake_prepare_gallery(
                       &adapter, "failure-invite-expired"),
               "expired-invite recovery prepares through public fake route");
        step = send_action(&adapter, adapter.last_request_id + 1u,
                           MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE,
                           0u, 0u);
        expect(step.accepted &&
                   adapter.pending == MDKR_ONLINE_FAKE_PENDING_JOIN &&
                   adapter.failure == MDKR_ONLINE_VIEW_FAILURE_NONE &&
                   adapter.session.state.scene == MDKR_SCENE_JOIN,
               "Enter Another Code begins one clean join journey");

        expect(mdkr_online_fake_init(&adapter, 3001u, &compat, false) &&
                   mdkr_online_fake_prepare_gallery(
                       &adapter, "failure-different-settings"),
               "room-settings recovery prepares inside preflight room");
        step = send_action(&adapter, adapter.last_request_id + 1u,
                           MDKR_ONLINE_VIEW_ACTION_USE_ROOM_SETTINGS,
                           0u, 0u);
        expect(step.accepted && adapter.have_lobby &&
                   adapter.pending == MDKR_ONLINE_FAKE_PENDING_PREFLIGHT &&
                   adapter.failure == MDKR_ONLINE_VIEW_FAILURE_NONE,
               "Use Room Settings preserves the room and reruns preflight");
        expect(mdkr_online_fake_complete(
                   &adapter, step.pending_token,
                   MDKR_ONLINE_VIEW_FAILURE_NONE).accepted &&
                   adapter.session.state.room == MDKR_ROOM_SELECTING,
               "successful settings recheck advances to selection");

        expect(mdkr_online_fake_init(&adapter, 3002u, &compat, false) &&
                   mdkr_online_fake_prepare_gallery(
                       &adapter, "failure-relay-capacity"),
               "relay recovery prepares inside preflight room");
        step = send_action(&adapter, adapter.last_request_id + 1u,
                           MDKR_ONLINE_VIEW_ACTION_RETRY, 0u, 0u);
        expect(step.accepted && adapter.have_lobby &&
                   adapter.pending == MDKR_ONLINE_FAKE_PENDING_PREFLIGHT,
               "preflight retry cannot accidentally create a replacement room");

        expect(mdkr_online_fake_init(&adapter, 3003u, &compat, true) &&
                   mdkr_online_fake_prepare_gallery(
                       &adapter, "racing-direct"),
               "running-race leave fixture reaches admitted race");
        step = send_action(&adapter, adapter.last_request_id + 1u,
                           MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE, 0u, 0u);
        expect(step.accepted && !adapter.have_lobby &&
                   adapter.session.state.scene == MDKR_SCENE_HOME &&
                   adapter.session.state.engine == MDKR_ENGINE_STOPPED,
               "confirmed Leave Race stops locally and disconnects once");
    }
}

int main(void) {
    test_two_race_create_vertical_and_mutations();
    test_join_and_sanitized_failure_paths();
    test_deterministic_gallery();
    if (failures != 0) {
        fprintf(stderr, "%d online fake-adapter test(s) failed\n", failures);
        return 1;
    }
    puts("online fake-adapter vertical slice passed");
    return 0;
}
