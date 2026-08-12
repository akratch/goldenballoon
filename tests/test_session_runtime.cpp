#include "session_runtime.h"

#include <cstdio>
#include <cstring>

namespace {

int failures;

void expect(bool condition, const char *message) {
    if (condition) std::printf("ok: %s\n", message);
    else {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void completeRace(SessionRuntime &runtime) {
    expect(runtime.enginePhase(MDKR_ENGINE_READY), "runtime engine ready");
    expect(runtime.enginePhase(MDKR_ENGINE_RACING), "runtime engine racing");
    expect(runtime.enginePhase(MDKR_ENGINE_FINISHED), "runtime engine finished");
}

MdkrSessionLaunchV2 launchFor(std::uint32_t epoch, std::uint8_t localMask) {
    MdkrSessionLaunchV2 launch{};
    launch.version = MDKR_SESSION_LAUNCH_VERSION;
    launch.size = sizeof(launch);
    launch.match.match_epoch = epoch;
    launch.match.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    std::memset(launch.match.build_id, 1, sizeof(launch.match.build_id));
    std::memset(launch.match.gameplay_digest, 2,
                sizeof(launch.match.gameplay_digest));
    launch.match.slot_owner[0] = 10u;
    launch.match.slot_owner[1] = 20u;
    launch.match.rng_seed = 30u;
    launch.match.track_id = 1u;
    launch.match.rom_revision = MDKR_ROM_US_11;
    launch.match.cadence_hz = 30u;
    launch.match.slot_count = 2u;
    launch.match.rules = 1u;
    launch.match.vehicle_mask = 1u;
    launch.match.input_delay = 2u;
    launch.local_slot_mask = localMask;
    launch.viewport_slot_mask = localMask;
    return launch;
}

MdkrSessionLaunchV3 launchV3For(std::uint32_t epoch,
                               std::uint8_t localMask) {
    const MdkrSessionLaunchV2 legacy = launchFor(epoch, localMask);
    MdkrSessionLaunchV3 launch{};
    launch.version = MDKR_SESSION_LAUNCH_V3_VERSION;
    launch.size = sizeof(launch);
    launch.match.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    launch.match.manifest = legacy.match;
    for (auto &selection : launch.match.selections) {
        selection.character_id = MDKR_MATCH_NO_CHARACTER;
        selection.vehicle_id = MDKR_MATCH_NO_VEHICLE;
    }
    launch.match.selections[0] = {1u, 2u, 0u};
    launch.match.selections[1] = {2u, 5u, 0u};
    launch.local_slot_mask = localMask;
    launch.viewport_slot_mask = localMask;
    return launch;
}

void test_persistent_native_lifecycle() {
    constexpr std::uint64_t kSession = UINT64_C(0xabcdef0123456789);
    SessionRuntime runtime(kSession);
    expect(runtime.beginLocal(), "runtime begins local Party");
    expect(runtime.requestRace(), "runtime requests first race");
    completeRace(runtime);
    expect(runtime.rematch(), "runtime requests rematch");
    completeRace(runtime);
    expect(runtime.state().match_epoch == 2u, "two races own two epochs");
    expect(runtime.state().session_id == kSession,
           "native runtime identity survives two races");
    expect(runtime.returnHome(), "runtime returns Home without destruction");
    expect(runtime.state().scene == MDKR_SCENE_HOME,
           "persistent runtime reaches Home");
    expect(runtime.state().session_id == kSession,
           "Home preserves persistent runtime identity");
}

void test_overlay_policy_follows_intent() {
    SessionRuntime local(1u);
    expect(local.beginLocal() && local.requestRace(),
           "local overlay case starts");
    expect(local.enginePhase(MDKR_ENGINE_READY) &&
           local.enginePhase(MDKR_ENGINE_RACING),
           "local overlay case races");
    expect(local.overlayMayPause(), "local Party overlay may pause");

    SessionRuntime online(2u);
    expect(online.beginOnline(), "online overlay case starts");
    expect(online.setConnectivity(MDKR_CONNECTIVITY_DIRECT) &&
           online.setRoomPhase(MDKR_ROOM_LOADING) &&
           online.applyLaunch(launchFor(1u, 1u)) && online.requestRace(),
           "online overlay case reaches load");
    expect(online.enginePhase(MDKR_ENGINE_READY) &&
           online.enginePhase(MDKR_ENGINE_RACING),
           "online overlay case races");
    expect(!online.overlayMayPause(), "online Party overlay may not pause");
}

void test_launcher_owns_frozen_engine_launch() {
    SessionRuntime online(3u);
    MdkrSessionLaunchV2 launch = launchFor(1u, 2u);
    expect(online.beginOnline(), "online launch enters launcher room");
    expect(online.setConnectivity(MDKR_CONNECTIVITY_DIRECT) &&
               online.setRoomPhase(MDKR_ROOM_LOADING),
           "online launch reaches loading preflight");
    const std::uint32_t beforeMissingLaunch = online.state().generation;
    expect(!online.requestRace() &&
               online.state().generation == beforeMissingLaunch &&
               online.state().engine == MDKR_ENGINE_STOPPED,
           "missing launch refuses race without partial core mutation");
    MdkrSessionLaunchV2 wrongEpoch = launch;
    wrongEpoch.match.match_epoch = 2u;
    expect(!online.applyLaunch(wrongEpoch),
           "launcher rejects a future launch epoch");
    expect(online.applyLaunch(launch), "launcher accepts validated launch envelope");
    expect(mdkr_session_bridge_manifest(&online.bridge()) != nullptr &&
               mdkr_session_bridge_roster(&online.bridge())->
                       canonical_player_count == 2u &&
               mdkr_session_bridge_roster(&online.bridge())->
                       local_to_canonical[0] == 1u,
           "runtime exposes frozen canonical and endpoint-local roster");
    expect(online.requestRace() &&
               online.enginePhase(MDKR_ENGINE_READY) &&
               online.enginePhase(MDKR_ENGINE_RACING),
           "validated online launch reaches racing");
    expect(online.bridge().engine_phase == MDKR_ENGINE_RACING,
           "session core and bridge enter racing atomically");
    launch.match.match_epoch = 2u;
    expect(!online.applyLaunch(launch),
           "running engine cannot replace borrowed launch envelope");
    expect(online.enginePhase(MDKR_ENGINE_FINISHED),
           "first online launch reaches results");
    expect(online.bridge().engine_phase == MDKR_ENGINE_FINISHED,
           "session core and bridge enter results atomically");
    expect(online.applyLaunch(launch),
           "results may freeze the next rematch envelope");
    expect(online.rematch() && online.state().match_epoch == 2u,
           "rematch consumes exactly the newly frozen epoch");
    expect(online.bridge().engine_phase == MDKR_ENGINE_BOOTING &&
               online.bridge().manifest.match_epoch == 2u,
           "rematch starts bridge with the same new epoch");
}

void test_online_results_can_return_to_room(void) {
    SessionRuntime online(33u);
    expect(online.beginOnline() &&
               online.setConnectivity(MDKR_CONNECTIVITY_DIRECT) &&
               online.setRoomPhase(MDKR_ROOM_LOADING) &&
               online.applyLaunch(launchFor(1u, 1u)) &&
               online.requestRace(),
           "online room-return fixture starts admitted race");
    completeRace(online);
    expect(online.returnToLobby(),
           "online results return to launcher lobby without disconnecting");
    expect(online.state().scene == MDKR_SCENE_LOBBY &&
               online.state().room == MDKR_ROOM_SELECTING &&
               online.state().engine == MDKR_ENGINE_STOPPED &&
               online.bridge().engine_phase == MDKR_ENGINE_STOPPED &&
               online.transportStats() == nullptr,
           "room return atomically retires bridge and transport loan");
    expect(!online.returnToLobby(),
           "second room return rejects without another mutation");
}

void test_launcher_owns_match_transport() {
    SessionRuntime online(4u);
    MdkrSessionLaunchV2 launch = launchFor(1u, 1u);
    MdkrPadSample local{0x8000u, 12, 0, 1u};
    MdkrPadSample remote{0x1000u, -22, 0, 1u};
    std::uint32_t tick = 0u;

    expect(online.beginOnline() &&
               online.setConnectivity(MDKR_CONNECTIVITY_DIRECT) &&
               online.setRoomPhase(MDKR_ROOM_LOADING) &&
               online.applyLaunch(launch) && online.requestRace() &&
               online.enginePhase(MDKR_ENGINE_READY),
           "launcher initializes transport at admitted ready epoch");
    expect(online.transportStats() != nullptr,
           "ready online runtime owns a live match transport");
    expect(online.receiveRemoteInput(1u, 0x2u, 1u, 0u, remote) ==
               MDKR_MATCH_INGRESS_ACCEPTED &&
               online.drainMatchInputs(1u, 0u, &local, 1u),
           "authenticated remote and local seats drain through launcher");
    const MdkrInputSet *frame = mdkr_session_bridge_inputs(
        &online.bridge(), &tick);
    expect(frame != nullptr && tick == 0u &&
               frame->confirmed_mask == 0x3u &&
               frame->slots[0].buttons == local.buttons &&
               frame->slots[1].buttons == remote.buttons,
           "launcher drain publishes complete canonical bridge input");

    expect(online.drainMatchInputs(1u, 1u, &local, 1u),
           "missing remote tick uses bounded repeat-last prediction");
    remote.buttons = 0x0040u;
    expect(online.receiveRemoteInput(1u, 0x2u, 1u, 1u, remote) ==
               MDKR_MATCH_INGRESS_CORRECTED &&
               online.takeRollbackCorrection(tick) && tick == 1u,
           "late authenticated input exposes exact rollback tick");
    expect(online.receiveRemoteInput(2u, 0x2u, 1u, 1u, remote) ==
               MDKR_MATCH_INGRESS_STALE_EPOCH,
           "transport rejects a mismatched rematch epoch");
    std::uint8_t aiMask = 0xffu;
    expect(online.scheduleAiTakeover(1u, 1u, 3u) ==
               MDKR_MATCH_TAKEOVER_ACCEPTED &&
               online.scheduleAiTakeover(1u, 1u, 3u) ==
               MDKR_MATCH_TAKEOVER_DUPLICATE,
           "launcher schedules one idempotent future AI takeover");
    expect(online.engineAiMaskForTick(1u, 2u, aiMask) && aiMask == 0u &&
               online.engineAiMaskForTick(1u, 3u, aiMask) && aiMask == 0x02u,
           "engine sees the frozen AI ownership boundary by exact tick");
    expect(online.receiveRemoteInput(1u, 0x2u, 1u, 3u, remote) ==
               MDKR_MATCH_INGRESS_TAKEN_OVER,
           "peer input cannot double-own a taken-over seat");
}

void test_production_launch_owns_selections() {
    SessionRuntime online(6u);
    MdkrSessionLaunchV3 launch = launchV3For(1u, 1u);
    expect(online.beginOnline() &&
               online.setConnectivity(MDKR_CONNECTIVITY_DIRECT) &&
               online.setRoomPhase(MDKR_ROOM_LOADING),
           "V3 production launch reaches loading preflight");
    expect(online.applyLaunch(launch),
           "runtime accepts selection-complete production launch");
    const MdkrMatchLaunchDescriptorV1 *owned =
        mdkr_session_bridge_launch_descriptor(&online.bridge());
    expect(owned != nullptr && owned->selections[1].character_id == 5u,
           "runtime bridge owns the frozen seat selections");
    launch.match.selections[1].character_id = 7u;
    expect(owned != nullptr && owned->selections[1].character_id == 5u,
           "runtime does not borrow mutable launcher selection storage");
    expect(online.requestRace() && online.enginePhase(MDKR_ENGINE_READY),
           "selection-complete launch is consumable by the engine lifecycle");
}

void test_engine_provider_maps_physical_seats() {
    SessionRuntime online(5u);
    MdkrSessionLaunchV2 launch = launchFor(1u, 2u);
    MdkrPadSample physical[MDKR_SESSION_MAX_PLAYERS]{};
    MdkrPadSample remote{0x1000u, -18, 0, 1u};
    MdkrInputSet frame{};
    MdkrInputSet retained{};

    physical[0] = MdkrPadSample{0x8000u, 27, 0, 1u};
    physical[1] = MdkrPadSample{0x4000u, 55, 0, 1u};
    expect(online.beginOnline() &&
               online.setConnectivity(MDKR_CONNECTIVITY_DIRECT) &&
               online.setRoomPhase(MDKR_ROOM_LOADING) &&
               online.applyLaunch(launch) && online.requestRace() &&
               online.enginePhase(MDKR_ENGINE_READY),
           "mapped engine provider reaches admitted ready epoch");
    expect(online.receiveRemoteInput(1u, 0x1u, 0u, 0u, remote) ==
               MDKR_MATCH_INGRESS_ACCEPTED &&
               online.engineDrainInputs(1u, 0u, physical,
                                        MDKR_SESSION_MAX_PLAYERS, frame),
           "engine provider drains authenticated remote and physical local seat");
    expect(frame.slots[0].buttons == remote.buttons &&
               frame.slots[1].buttons == physical[0].buttons &&
               frame.slots[1].buttons != physical[1].buttons &&
               frame.confirmed_mask == 0x3u,
           "physical device zero maps to endpoint-owned canonical slot one");
    expect(online.engineInputsForTick(1u, 0u, retained) &&
               std::memcmp(&frame, &retained, sizeof(frame)) == 0,
           "engine provider copies retained canonical input by exact epoch/tick");
    expect(!online.engineInputsForTick(2u, 0u, retained),
           "engine provider rejects stale epoch history reads");
}

}  // namespace

int main() {
    test_persistent_native_lifecycle();
    test_overlay_policy_follows_intent();
    test_launcher_owns_frozen_engine_launch();
    test_online_results_can_return_to_room();
    test_launcher_owns_match_transport();
    test_production_launch_owns_selections();
    test_engine_provider_maps_physical_seats();
    if (failures != 0) return 1;
    std::printf("native SessionRuntime contract passed\n");
    return 0;
}
