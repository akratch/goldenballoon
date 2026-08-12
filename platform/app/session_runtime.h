// Persistent launcher-owned session lifetime for native application flows.
#ifndef MDKR_APP_SESSION_RUNTIME_H
#define MDKR_APP_SESSION_RUNTIME_H

#include "session/session_core.h"
#include "session/session_bridge.h"
#include "net/match_transport.h"

#include <cstdint>
#include <mutex>

class SessionRuntime {
public:
    explicit SessionRuntime(std::uint64_t sessionId);

    const MdkrSessionState &state() const { return core_.state; }
    const MdkrSessionStep &lastStep() const { return lastStep_; }
    const MdkrSessionBridge &bridge() const { return bridge_; }
    const MdkrMatchTransportStats *transportStats() const {
        return mdkr_match_transport_stats(&transport_);
    }

    bool beginLocal();
    bool beginOnline();
    bool requestRace();
    bool enginePhase(MdkrEnginePhase phase);
    bool openOverlay(MdkrOverlayState overlay);
    bool closeOverlay();
    bool rematch();
    bool returnToLobby();
    bool returnHome();
    bool setConnectivity(MdkrConnectivity connectivity);
    bool setRoomPhase(MdkrRoomPhase phase);
    bool recover(MdkrSessionError error);
    bool applyLaunch(const MdkrSessionLaunchV2 &launch);
    bool applyLaunch(const MdkrSessionLaunchV3 &launch);
    MdkrMatchTransportIngressResult receiveRemoteInput(
        std::uint32_t matchEpoch, std::uint8_t authenticatedSlotMask,
        unsigned slot, std::uint32_t tick, const MdkrPadSample &sample);
    MdkrMatchTakeoverResult scheduleAiTakeover(
        std::uint32_t matchEpoch, unsigned slot,
        std::uint32_t activationTick);
    bool drainMatchInputs(std::uint32_t matchEpoch, std::uint32_t tick,
                          const MdkrPadSample *localSamples,
                          unsigned localSampleCount);
    bool takeRollbackCorrection(std::uint32_t &tick);
    /* Copy-out callback surface for the engine-lifetime provider. The ordinary
     * path maps physical device order to frozen local-seat order. The canonical
     * mirror is an explicit process-test mapping for scripted canonical ports;
     * the test carrier still feeds remote ports through authenticated ingress. */
    bool engineDrainInputs(std::uint32_t matchEpoch, std::uint32_t tick,
                           const MdkrPadSample *physicalInputs,
                           unsigned physicalInputCount, MdkrInputSet &out,
                           bool canonicalMirrorForTest = false);
    bool engineInputsForTick(std::uint32_t matchEpoch, std::uint32_t tick,
                             MdkrInputSet &out);
    bool engineTakeDirty(std::uint32_t matchEpoch, std::uint32_t &tick);
    bool engineRecovery(std::uint32_t matchEpoch,
                        MdkrMatchRecovery &recovery);
    bool engineAiMaskForTick(std::uint32_t matchEpoch, std::uint32_t tick,
                             std::uint8_t &slotMask);

    bool overlayMayPause() const {
        return core_.state.intent == MDKR_INTENT_LOCAL;
    }

private:
    bool dispatch(MdkrSessionCommandType type, std::uint32_t value = 0u);
    bool dispatchWithBridgePhase(MdkrSessionCommandType type,
                                 MdkrEnginePhase phase);

    MdkrSessionCore core_{};
    MdkrSessionStep lastStep_{};
    MdkrSessionBridge bridge_{};
    MdkrMatchTransport transport_{};
    std::mutex transportMutex_;
};

#endif  // MDKR_APP_SESSION_RUNTIME_H
