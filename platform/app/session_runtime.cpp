#include "session_runtime.h"

SessionRuntime::SessionRuntime(std::uint64_t sessionId) {
    mdkr_session_core_init(&core_, sessionId);
    mdkr_session_bridge_init(&bridge_);
}

bool SessionRuntime::dispatch(MdkrSessionCommandType type, std::uint32_t value) {
    const MdkrSessionCommand command = {
        MDKR_SESSION_PROTOCOL_VERSION,
        core_.state.generation,
        type,
        value,
    };
    lastStep_ = mdkr_session_core_dispatch(&core_, &command);
    return lastStep_.accepted;
}

bool SessionRuntime::dispatchWithBridgePhase(MdkrSessionCommandType type,
                                             MdkrEnginePhase phase) {
    MdkrSessionCore nextCore = core_;
    MdkrSessionBridge nextBridge = bridge_;
    MdkrMatchTransport nextTransport = transport_;
    const MdkrSessionCommand command = {
        MDKR_SESSION_PROTOCOL_VERSION,
        core_.state.generation,
        type,
        type == MDKR_SESSION_COMMAND_SET_ENGINE_PHASE
            ? static_cast<std::uint32_t>(phase) : 0u,
    };
    MdkrSessionStep step = mdkr_session_core_dispatch(&nextCore, &command);
    if (!step.accepted) {
        lastStep_ = step;
        return false;
    }
    if (nextCore.state.intent == MDKR_INTENT_ONLINE_PRIVATE &&
        !mdkr_session_bridge_set_engine_phase(&nextBridge, phase)) {
        lastStep_ = {};
        lastStep_.error = MDKR_SESSION_ERROR_INVALID_COMPOSITION;
        return false;
    }
    if (nextCore.state.intent == MDKR_INTENT_ONLINE_PRIVATE) {
        if (phase == MDKR_ENGINE_READY) {
            if (!mdkr_match_transport_init(&nextTransport, &nextBridge, 0u)) {
                lastStep_ = {};
                lastStep_.error = MDKR_SESSION_ERROR_INVALID_COMPOSITION;
                return false;
            }
        } else if (phase == MDKR_ENGINE_BOOTING ||
                   phase == MDKR_ENGINE_STOPPED) {
            nextTransport = {};
        }
    }
    core_ = nextCore;
    bridge_ = nextBridge;
    transport_ = nextTransport;
    if (transport_.ready) transport_.bridge = &bridge_;
    lastStep_ = step;
    return true;
}

bool SessionRuntime::beginLocal() {
    return dispatch(MDKR_SESSION_COMMAND_BEGIN_LOCAL);
}

bool SessionRuntime::beginOnline() {
    return dispatch(MDKR_SESSION_COMMAND_BEGIN_ONLINE);
}

bool SessionRuntime::requestRace() {
    return dispatchWithBridgePhase(MDKR_SESSION_COMMAND_REQUEST_RACE,
                                   MDKR_ENGINE_BOOTING);
}

bool SessionRuntime::enginePhase(MdkrEnginePhase phase) {
    return dispatchWithBridgePhase(MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                                   phase);
}

bool SessionRuntime::openOverlay(MdkrOverlayState overlay) {
    return dispatch(MDKR_SESSION_COMMAND_OPEN_OVERLAY,
                    static_cast<std::uint32_t>(overlay));
}

bool SessionRuntime::closeOverlay() {
    return dispatch(MDKR_SESSION_COMMAND_CLOSE_OVERLAY);
}

bool SessionRuntime::rematch() {
    return dispatchWithBridgePhase(MDKR_SESSION_COMMAND_REMATCH,
                                   MDKR_ENGINE_BOOTING);
}

bool SessionRuntime::returnToLobby() {
    MdkrSessionCore nextCore = core_;
    MdkrSessionBridge nextBridge = bridge_;
    const MdkrSessionCommand command = {
        MDKR_SESSION_PROTOCOL_VERSION,
        core_.state.generation,
        MDKR_SESSION_COMMAND_RETURN_TO_LOBBY,
        0u,
    };
    MdkrSessionStep step = mdkr_session_core_dispatch(&nextCore, &command);
    if (!step.accepted ||
        (nextBridge.have_manifest &&
         nextBridge.engine_phase != MDKR_ENGINE_STOPPED &&
         !mdkr_session_bridge_set_engine_phase(
             &nextBridge, MDKR_ENGINE_STOPPED))) {
        lastStep_ = step;
        if (step.accepted) {
            lastStep_ = {};
            lastStep_.error = MDKR_SESSION_ERROR_INVALID_COMPOSITION;
        }
        return false;
    }
    core_ = nextCore;
    bridge_ = nextBridge;
    transport_ = {};
    lastStep_ = step;
    return true;
}

bool SessionRuntime::returnHome() {
    MdkrSessionCore nextCore = core_;
    MdkrSessionBridge nextBridge = bridge_;
    const MdkrSessionCommand command = {
        MDKR_SESSION_PROTOCOL_VERSION,
        core_.state.generation,
        MDKR_SESSION_COMMAND_RETURN_HOME,
        0u,
    };
    MdkrSessionStep step = mdkr_session_core_dispatch(&nextCore, &command);
    if (!step.accepted) {
        lastStep_ = step;
        return false;
    }
    if (core_.state.intent == MDKR_INTENT_ONLINE_PRIVATE &&
        nextBridge.have_manifest &&
        nextBridge.engine_phase != MDKR_ENGINE_STOPPED &&
        !mdkr_session_bridge_set_engine_phase(
            &nextBridge, MDKR_ENGINE_STOPPED)) {
        lastStep_ = {};
        lastStep_.error = MDKR_SESSION_ERROR_INVALID_COMPOSITION;
        return false;
    }
    core_ = nextCore;
    bridge_ = nextBridge;
    transport_ = {};
    lastStep_ = step;
    return true;
}

bool SessionRuntime::setConnectivity(MdkrConnectivity connectivity) {
    return dispatch(MDKR_SESSION_COMMAND_SET_CONNECTIVITY,
                    static_cast<std::uint32_t>(connectivity));
}

bool SessionRuntime::setRoomPhase(MdkrRoomPhase phase) {
    return dispatch(MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    static_cast<std::uint32_t>(phase));
}

bool SessionRuntime::recover(MdkrSessionError error) {
    return dispatch(MDKR_SESSION_COMMAND_RECOVER,
                    static_cast<std::uint32_t>(error));
}

bool SessionRuntime::applyLaunch(const MdkrSessionLaunchV2 &launch) {
    /* Matchmaking may prepare this while the launcher is in setup/loading,
     * but it may never replace an envelope borrowed by a running engine. */
    if (core_.state.intent != MDKR_INTENT_ONLINE_PRIVATE ||
        (core_.state.engine != MDKR_ENGINE_STOPPED &&
         core_.state.engine != MDKR_ENGINE_BOOTING &&
         core_.state.engine != MDKR_ENGINE_FINISHED)) {
        return false;
    }
    const std::uint32_t expectedEpoch =
        core_.state.engine == MDKR_ENGINE_BOOTING
            ? core_.state.match_epoch : core_.state.match_epoch + 1u;
    if (launch.match.match_epoch != expectedEpoch) return false;
    MdkrSessionBridge nextBridge = bridge_;
    if (nextBridge.have_manifest &&
        nextBridge.engine_phase != MDKR_ENGINE_STOPPED &&
        !mdkr_session_bridge_set_engine_phase(
            &nextBridge, MDKR_ENGINE_STOPPED)) {
        return false;
    }
    if (!mdkr_session_bridge_apply_launch(&nextBridge, &launch)) return false;
    bridge_ = nextBridge;
    transport_ = {};
    return true;
}

bool SessionRuntime::applyLaunch(const MdkrSessionLaunchV3 &launch) {
    /* V3 is the player-visible path: the launcher freezes both deterministic
     * match identity and per-seat race selections before engine admission. */
    if (core_.state.intent != MDKR_INTENT_ONLINE_PRIVATE ||
        (core_.state.engine != MDKR_ENGINE_STOPPED &&
         core_.state.engine != MDKR_ENGINE_BOOTING &&
         core_.state.engine != MDKR_ENGINE_FINISHED)) {
        return false;
    }
    const std::uint32_t expectedEpoch =
        core_.state.engine == MDKR_ENGINE_BOOTING
            ? core_.state.match_epoch : core_.state.match_epoch + 1u;
    if (launch.match.manifest.match_epoch != expectedEpoch) return false;
    MdkrSessionBridge nextBridge = bridge_;
    if (nextBridge.have_manifest &&
        nextBridge.engine_phase != MDKR_ENGINE_STOPPED &&
        !mdkr_session_bridge_set_engine_phase(
            &nextBridge, MDKR_ENGINE_STOPPED)) {
        return false;
    }
    if (!mdkr_session_bridge_apply_launch_v3(&nextBridge, &launch)) {
        return false;
    }
    bridge_ = nextBridge;
    transport_ = {};
    return true;
}

MdkrMatchTransportIngressResult SessionRuntime::receiveRemoteInput(
    std::uint32_t matchEpoch, std::uint8_t authenticatedSlotMask,
    unsigned slot, std::uint32_t tick, const MdkrPadSample &sample) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return mdkr_match_transport_receive(
        &transport_, matchEpoch, authenticatedSlotMask, slot, tick, &sample);
}

MdkrMatchTakeoverResult SessionRuntime::scheduleAiTakeover(
    std::uint32_t matchEpoch, unsigned slot,
    std::uint32_t activationTick) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return mdkr_match_transport_schedule_ai_takeover(
        &transport_, matchEpoch, slot, activationTick);
}

bool SessionRuntime::drainMatchInputs(
    std::uint32_t matchEpoch, std::uint32_t tick,
    const MdkrPadSample *localSamples, unsigned localSampleCount) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return mdkr_match_transport_drain_tick(
        &transport_, matchEpoch, tick, localSamples, localSampleCount);
}

bool SessionRuntime::takeRollbackCorrection(std::uint32_t &tick) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return mdkr_match_transport_take_dirty(&transport_, &tick);
}

bool SessionRuntime::engineDrainInputs(
    std::uint32_t matchEpoch, std::uint32_t tick,
    const MdkrPadSample *physicalInputs, unsigned physicalInputCount,
    MdkrInputSet &out, bool canonicalMirrorForTest) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    if (!transport_.ready || physicalInputs == nullptr ||
        physicalInputCount != MDKR_SESSION_MAX_PLAYERS ||
        matchEpoch != transport_.match_epoch) {
        return false;
    }
    const MdkrNetRoster *roster = mdkr_session_bridge_roster(&bridge_);
    if (roster == nullptr || roster->local_seat_count == 0u) return false;

    MdkrPadSample local[MDKR_SESSION_MAX_PLAYERS]{};
    for (unsigned seat = 0u; seat < roster->local_seat_count; ++seat) {
        const unsigned source = canonicalMirrorForTest
            ? roster->local_to_canonical[seat] : seat;
        local[seat] = physicalInputs[source];
    }
    if (!mdkr_match_transport_drain_tick(
            &transport_, matchEpoch, tick, local,
            roster->local_seat_count)) {
        return false;
    }
    std::uint32_t acceptedTick = 0u;
    const MdkrInputSet *accepted =
        mdkr_session_bridge_inputs(&bridge_, &acceptedTick);
    if (accepted == nullptr || acceptedTick != tick) return false;
    out = *accepted;
    return true;
}

bool SessionRuntime::engineInputsForTick(
    std::uint32_t matchEpoch, std::uint32_t tick, MdkrInputSet &out) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return mdkr_match_transport_inputs_for_tick(
        &transport_, matchEpoch, tick, &out);
}

bool SessionRuntime::engineTakeDirty(
    std::uint32_t matchEpoch, std::uint32_t &tick) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return transport_.ready && matchEpoch == transport_.match_epoch &&
        mdkr_match_transport_take_dirty(&transport_, &tick);
}

bool SessionRuntime::engineRecovery(
    std::uint32_t matchEpoch, MdkrMatchRecovery &recovery) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return transport_.ready && matchEpoch == transport_.match_epoch &&
        mdkr_match_transport_recovery(&transport_, &recovery);
}

bool SessionRuntime::engineAiMaskForTick(
    std::uint32_t matchEpoch, std::uint32_t tick,
    std::uint8_t &slotMask) {
    const std::lock_guard<std::mutex> lock(transportMutex_);
    return mdkr_match_transport_ai_takeover_mask_for_tick(
        &transport_, matchEpoch, tick, &slotMask);
}
