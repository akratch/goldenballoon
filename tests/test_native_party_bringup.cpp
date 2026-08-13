/*
 * Packaged-build Phone Party bring-up smoke.
 *
 * This gate exists so a release lane can qualify, on every platform and with
 * no GPU, no network, no ROM and no window, the pairing surface a phone
 * actually needs before it can join: the secure-origin bootstrap, the invite
 * URL and fallback code the host is willing to surface, the QR encoding of
 * that URL, the pairing phrase the host accepts, and the fail-closed
 * behaviour when the compiled service origin is missing or insecure.
 *
 * It drives the real MdkrNativePartyHost through a recording transport. It
 * deliberately does NOT re-test the seat-custody lifecycle -- that is
 * tests/test_native_party_host.cpp -- and it does not claim anything about
 * the launcher's on-screen party panel, which has no headless seam.
 *
 * The QR oracle below was produced by the pinned browser encoder in
 * dist/web/party/qrcodegen.js for the same vector that
 * tests/test_party_qr.cjs decodes back to the URL with a real QR decoder
 * (jsQR). Pinning the native encoder to it means a phone camera reading the
 * native launcher's code lands on the same string a browser would produce.
 */

/* Assert-driven test: NDEBUG (the Release default) would compile every check
 * away -- and delete the calls the asserts wrap. */
#undef NDEBUG

#include "party/native_party_host.h"
#include "party/native_remote_pad_ingress.h"

#include "qrcodegen.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

/* The invite capability the Party service mints is a 43-character base64url
 * value; the transport rejects any other length. */
constexpr size_t kCapabilityLength = 43u;
constexpr const char *kDefaultOrigin = "https://party.example.invalid";

/* Cross-language oracle: dist/web/party/qrcodegen.js at ECC QUARTILE for
 * kDefaultOrigin + "/controller/#" + 43 'A'. */
constexpr int kOracleSize = 45;
constexpr unsigned kOracleDark = 1020u;
constexpr uint32_t kOracleHash = 0x1870b43fu;

class RecordingTransport final : public MdkrPartyTransport {
public:
    bool availableValue = true;
    bool openResult = true;
    std::string reason;
    std::deque<MdkrPartyTransportEvent> events;
    std::vector<std::string> calls;

    bool available() const override { return availableValue; }
    const char *unavailableReason() const override { return reason.c_str(); }
    bool open(const std::string &origin) override {
        calls.push_back("open:" + origin);
        return openResult;
    }
    bool approve(const std::string &id, unsigned seat) override {
        calls.push_back("approve:" + id + ":" + std::to_string(seat));
        return true;
    }
    bool reject(const std::string &id) override {
        calls.push_back("reject:" + id);
        return true;
    }
    bool remove(const std::string &id) override {
        calls.push_back("remove:" + id);
        return true;
    }
    bool rotateInvite(unsigned generation) override {
        calls.push_back("rotate:" + std::to_string(generation));
        return true;
    }
    bool revokeInvite() override {
        calls.push_back("revoke");
        return true;
    }
    bool closeRoom() override {
        calls.push_back("close");
        return true;
    }
    bool sendRumble(const std::string &id, uint16_t strength) override {
        calls.push_back("rumble:" + id + ":" + std::to_string(strength));
        return true;
    }
    bool poll(MdkrPartyTransportEvent &event) override {
        if (events.empty()) return false;
        event = std::move(events.front());
        events.pop_front();
        return true;
    }
    void shutdown() override { calls.push_back("shutdown"); }

    size_t openCalls() const {
        size_t count = 0u;
        for (const std::string &call : calls) {
            if (call.rfind("open:", 0u) == 0u) count++;
        }
        return count;
    }
};

std::string configuredOrigin() {
    const char *value = std::getenv("MDKR_PARTY_BRINGUP_ORIGIN");
    if (value == nullptr || value[0] == '\0') return kDefaultOrigin;
    const std::string origin(value);
    /* Same rule CMake enforces on MDKR_PARTY_ORIGIN. A lane that passes a
     * non-HTTPS origin has a broken build, not a skippable test. */
    assert(origin.rfind("https://", 0u) == 0u);
    assert(origin.back() != '/');
    return origin;
}

std::string inviteUrlFor(const std::string &origin) {
    return origin + "/controller/#" + std::string(kCapabilityLength, 'A');
}

MdkrPartyTransportEvent inviteRoom(
    const std::string &url, const std::string &code, uint64_t expiresAtMs) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::RoomState;
    event.room.transitionId = 1u;
    event.room.inviteGeneration = 1u;
    event.room.inviteExpiresAtMs = expiresAtMs;
    event.room.controllerUrl = url;
    event.room.fallbackCode = code;
    event.room.inviteActive = true;
    return event;
}

struct QrDigest {
    int size = 0;
    unsigned dark = 0u;
    uint32_t hash = 2166136261u;
};

QrDigest encodeInvite(const std::string &url) {
    const auto qr = qrcodegen::QrCode::encodeText(
        url.c_str(), qrcodegen::QrCode::Ecc::QUARTILE);
    QrDigest digest;
    digest.size = qr.getSize();
    for (int y = 0; y < digest.size; y++) {
        for (int x = 0; x < digest.size; x++) {
            const uint32_t value = qr.getModule(x, y) ? 1u : 0u;
            digest.dark += value;
            digest.hash = (digest.hash ^ value) * 16777619u;
        }
    }
    return digest;
}

/*
 * A build whose compiled MDKR_PARTY_ORIGIN is empty, or is not HTTPS, must
 * refuse to bootstrap and must never reach the network.
 */
void unsetOrInsecureOriginFailsClosed() {
    mdkr_native_remote_pad_reset_all();
    const char *const refused[] = {
        "",
        "http://party.example.invalid",
        "https:/party.example.invalid",
        "wss://party.example.invalid",
        "party.example.invalid",
        "HTTPS://party.example.invalid",
    };
    for (const char *origin : refused) {
        RecordingTransport transport;
        MdkrNativePartyHost host(transport);
        assert(!host.open(origin));
        assert(host.view().phase == MdkrNativePartyPhase::Error);
        assert(!host.view().inviteVisible);
        assert(host.view().controllerUrl.empty());
        assert(host.view().fallbackCode.empty());
        assert(!host.view().message.empty());
        /* Fail closed BEFORE any transport bootstrap. */
        assert(transport.openCalls() == 0u);
    }

    /* An over-long origin is refused for the same reason. */
    RecordingTransport oversize;
    MdkrNativePartyHost oversizeHost(oversize);
    assert(!oversizeHost.open("https://" + std::string(4096u, 'a')));
    assert(oversizeHost.view().phase == MdkrNativePartyPhase::Error);
    assert(oversize.openCalls() == 0u);

    /* A build without the native WebRTC transport must say so and stay shut. */
    RecordingTransport unavailable;
    unavailable.availableValue = false;
    unavailable.reason = "Phone controllers are not included in this build.";
    MdkrNativePartyHost unavailableHost(unavailable);
    assert(!unavailableHost.open(kDefaultOrigin));
    assert(unavailableHost.view().phase == MdkrNativePartyPhase::Error);
    assert(unavailableHost.view().message == unavailable.reason);
    assert(unavailable.openCalls() == 0u);

    /* A bootstrap the service refuses must not leave a half-open room. */
    RecordingTransport refusedOpen;
    refusedOpen.openResult = false;
    MdkrNativePartyHost refusedHost(refusedOpen);
    assert(!refusedHost.open(kDefaultOrigin));
    assert(refusedHost.view().phase == MdkrNativePartyPhase::Error);
    assert(refusedOpen.openCalls() == 1u);
}

/* The bootstrap must reach exactly the configured service origin, unedited. */
void bootstrapReachesConfiguredOrigin(const std::string &origin) {
    mdkr_native_remote_pad_reset_all();
    RecordingTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open(origin));
    assert(transport.calls.size() == 1u);
    assert(transport.calls.front() == "open:" + origin);
    assert(host.view().phase == MdkrNativePartyPhase::Opening);
    assert(host.view().busy);
    assert(!host.view().inviteVisible);
    assert(!host.view().message.empty());

    /* Re-entrant open while a room is coming up must not double-bootstrap. */
    assert(!host.open(origin));
    assert(transport.openCalls() == 1u);
}

/* The invite the host is willing to show a player: HTTPS URL on the configured
 * origin, six-digit fallback code, and a QR that encodes that exact URL. */
QrDigest inviteSurfaceIsPairable(const std::string &origin) {
    mdkr_native_remote_pad_reset_all();
    RecordingTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open(origin));

    const std::string url = inviteUrlFor(origin);
    transport.events.push_back(inviteRoom(url, "406913", 121000u));
    host.service(1000u);

    const MdkrNativePartyView &view = host.view();
    assert(view.phase == MdkrNativePartyPhase::Open);
    assert(view.inviteVisible);
    assert(view.controllerUrl == url);
    assert(view.controllerUrl.rfind(origin + "/", 0u) == 0u);
    assert(view.controllerUrl.rfind("https://", 0u) == 0u);
    /* The capability rides in the fragment so it is never sent to the server
     * by the phone's browser. */
    const size_t fragment = view.controllerUrl.find('#');
    assert(fragment != std::string::npos);
    assert(view.controllerUrl.size() - fragment - 1u == kCapabilityLength);

    assert(view.fallbackCode.size() == 6u);
    for (const char digit : view.fallbackCode) {
        assert(digit >= '0' && digit <= '9');
    }
    assert(view.inviteGeneration == 1u);
    assert(view.inviteExpiresAtMs == 121000u);

    const QrDigest digest = encodeInvite(view.controllerUrl);
    /* Non-degenerate: a real code, not a blank or saturated field. */
    assert(digest.size >= 21 && digest.size <= 177);
    assert(digest.dark > 0u);
    assert(digest.dark < static_cast<unsigned>(digest.size) *
                         static_cast<unsigned>(digest.size));
    /* Deterministic, and sensitive to the capability it carries -- a QR that
     * ignored the URL would pair every phone with the same room. */
    const QrDigest again = encodeInvite(view.controllerUrl);
    assert(again.hash == digest.hash && again.dark == digest.dark);
    std::string altered = view.controllerUrl;
    altered.back() = altered.back() == 'B' ? 'C' : 'B';
    const QrDigest other = encodeInvite(altered);
    assert(other.hash != digest.hash);

    /* The pairing phrase the service derives for a joining phone is carried
     * through to the surface a host reads aloud. */
    MdkrNativePartyController pending;
    pending.id = "phone-a";
    pending.name = "A friend's phone";
    pending.publicKey = std::string(87u, 'C');
    pending.pairingPhrase = "Royal-Penguin Nimble-Comet";
    pending.connectionSequence = 1u;
    MdkrPartyTransportEvent joined = inviteRoom(url, "406913", 121000u);
    joined.room.transitionId = 2u;
    joined.room.controllers.push_back(pending);
    transport.events.push_back(std::move(joined));
    host.service(1001u);
    assert(host.view().controllers.size() == 1u);
    assert(host.view().controllers[0].pairingPhrase ==
           "Royal-Penguin Nimble-Comet");

    MdkrPartyTransportEvent phrase;
    phrase.type = MdkrPartyTransportEventType::ControllerPhrase;
    phrase.controllerId = "phone-a";
    phrase.message = "Amber-Comet Steady-Falcon";
    transport.events.push_back(phrase);
    host.service(1002u);
    assert(host.view().controllers[0].pairingPhrase ==
           "Amber-Comet Steady-Falcon");

    /* An unprintable phrase must never reach a player's screen. */
    MdkrPartyTransportEvent hostile;
    hostile.type = MdkrPartyTransportEventType::ControllerPhrase;
    hostile.controllerId = "phone-a";
    hostile.message = std::string("Amber\x01Comet");
    transport.events.push_back(hostile);
    host.service(1003u);
    assert(host.view().controllers[0].pairingPhrase ==
           "Amber-Comet Steady-Falcon");

    /* Once the invite window closes, nothing pairable stays on screen. */
    host.service(121000u);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
    assert(!host.view().inviteVisible);
    assert(host.view().controllerUrl.empty());
    assert(host.view().fallbackCode.empty());

    return digest;
}

/* A malformed invite must be refused rather than shown to a player. */
void malformedInvitesAreRefused(const std::string &origin) {
    const std::string url = inviteUrlFor(origin);
    struct Case {
        std::string url;
        std::string code;
    };
    const Case refused[] = {
        {"http://party.example.invalid/controller/#a", "406913"},
        {url, "40691"},
        {url, "4069133"},
        {url, "40691A"},
        {url, ""},
        {std::string("https://party.example.invalid/controller/#\x01"), "406913"},
    };
    for (const Case &candidate : refused) {
        mdkr_native_remote_pad_reset_all();
        RecordingTransport transport;
        MdkrNativePartyHost host(transport);
        assert(host.open(origin));
        transport.events.push_back(
            inviteRoom(candidate.url, candidate.code, 121000u));
        host.service(1000u);
        assert(host.view().phase == MdkrNativePartyPhase::Error);
        assert(!host.view().inviteVisible);
        assert(host.view().controllerUrl.empty());
        assert(host.view().fallbackCode.empty());
    }
}

/* Bind the native encoder to the browser encoder a phone camera reads. */
void qrMatchesBrowserOracle() {
    const QrDigest digest = encodeInvite(inviteUrlFor(kDefaultOrigin));
    assert(digest.size == kOracleSize);
    assert(digest.dark == kOracleDark);
    assert(digest.hash == kOracleHash);
}

}  // namespace

int main() {
    const std::string origin = configuredOrigin();

    unsetOrInsecureOriginFailsClosed();
    bootstrapReachesConfiguredOrigin(origin);
    const QrDigest digest = inviteSurfaceIsPairable(origin);
    malformedInvitesAreRefused(origin);
    qrMatchesBrowserOracle();
    mdkr_native_remote_pad_reset_all();

    const char *version = std::getenv("MDKR_PARTY_BRINGUP_VERSION");
    /*
     * Evidence line. A release lane reads the version out of the packaged
     * artifact it just qualified and passes it in here, so this record is
     * bound to that artifact rather than to a rebuilt tree.
     */
    std::printf("[party-bringup] version=%s origin=%s qr=%dx%d ecc=Q ok=1\n",
                version != nullptr && version[0] != '\0' ? version : "unset",
                origin.c_str(), digest.size, digest.size);
    return 0;
}
