/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include "party/libdatachannel_party_transport.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

int main() {
    /* SAS v2 interoperability vectors. Expected words come from the web
     * page's own derivation (dist/web/party/party-sas.js run under Node with
     * the transcript extended to the v2 layout), so a native transcript that
     * drifted by even one separator byte cannot match. Transcript:
     *   ECDH_secret ‖ "golden-balloon-party-sas-v2" ‖ 0x00 ‖ roomId ‖ 0x00 ‖
     *   hostPublicKey ‖ 0x00 ‖ controllerPublicKey ‖ 0x00 ‖
     *   hostFingerprint ‖ 0x00 ‖ controllerFingerprint
     * The private scalar 1 puts the host key at the P-256 generator, so the
     * ECDH secret is the controller key's own X coordinate. */
    std::array<uint8_t, 32> scalar{};
    scalar.back() = 1u;
    const std::string controllerKey =
        "BHzyexiNA09-ilI4AwS1GsPAiWnid_IbNaYLSPxHZpl4B3dVENuO0EApPZrGn3Qw27p9reY86YIpngS3nSJ4c9E";
    const std::string hostFingerprint =
        "sha-256 00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:"
        "10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F";
    const std::string controllerFingerprint =
        "sha-256 E0:E1:E2:E3:E4:E5:E6:E7:E8:E9:EA:EB:EC:ED:EE:EF:"
        "F0:F1:F2:F3:F4:F5:F6:F7:F8:F9:FA:FB:FC:FD:FE:FF";
    std::string hostKey;
    std::string phrase;
    assert(mdkr_party_sas_phrase_for_test(
        scalar.data(), "AAAAAAAAAAAAAAAAAAAAAA", controllerKey,
        hostFingerprint, controllerFingerprint, hostKey, phrase));
    assert(hostKey ==
        "BGsX0fLhLEJH-Lzm5WOkQPJ3A32BLeszoPShOUXYmMKWT-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU");
    assert(phrase == "Gentle-Star Royal-Pilot");

    /* Swapping the two fingerprints must move the words: the transcript
     * binds each fingerprint to its role, not the pair as an unordered set,
     * or a relay could cross-substitute certificates undetected. */
    assert(mdkr_party_sas_phrase_for_test(
        scalar.data(), "AAAAAAAAAAAAAAAAAAAAAA", controllerKey,
        controllerFingerprint, hostFingerprint, hostKey, phrase));
    assert(phrase == "Mighty-Kite Wild-Kite");

    /* Fail closed: with either fingerprint missing there is NO phrase at
     * all -- never a silent fall back to the v1 transcript, which is what a
     * fingerprint-substituting relay would need. */
    phrase = "sentinel";
    assert(!mdkr_party_sas_phrase_for_test(
        scalar.data(), "AAAAAAAAAAAAAAAAAAAAAA", controllerKey,
        std::string{}, controllerFingerprint, hostKey, phrase));
    assert(phrase == "sentinel");
    assert(!mdkr_party_sas_phrase_for_test(
        scalar.data(), "AAAAAAAAAAAAAAAAAAAAAA", controllerKey,
        hostFingerprint, std::string{}, hostKey, phrase));
    assert(phrase == "sentinel");

    /* An invalid controller key is still refused outright. */
    assert(!mdkr_party_sas_phrase_for_test(
        scalar.data(), "AAAAAAAAAAAAAAAAAAAAAA", std::string(87u, 'C'),
        hostFingerprint, controllerFingerprint, hostKey, phrase));

    /* Canonical fingerprint capture from an SDP description: the value
     * string after "a=fingerprint:", algorithm token verbatim, hex
     * uppercased, single spaces. Anything else is refused as the empty
     * string, which the transport treats as "no phrase" (fail closed). */
    assert(mdkr_party_sdp_fingerprint_for_test(
        "v=0\r\no=rtc 111 0 IN IP4 127.0.0.1\r\n"
        "a=fingerprint:sha-256 ab:cd:ef:01:23:45:67:89:"
        "ab:cd:ef:01:23:45:67:89\r\n"
        "a=setup:actpass\r\n") ==
        "sha-256 AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89");
    /* Runs of blanks collapse to the single canonical space; bare-\n line
     * endings parse the same as \r\n. */
    assert(mdkr_party_sdp_fingerprint_for_test(
        "v=0\na=fingerprint:sha-256 \t AB:CD\na=setup:actpass\n") ==
        "sha-256 AB:CD");
    /* A session-level and media-level repeat of the SAME fingerprint is one
     * certificate and stays accepted. */
    assert(mdkr_party_sdp_fingerprint_for_test(
        "a=fingerprint:sha-256 ab:cd\r\nm=application 9 UDP/DTLS/SCTP\r\n"
        "a=fingerprint:sha-256 AB:CD\r\n") == "sha-256 AB:CD");
    /* Two DIFFERENT fingerprints in one description are ambiguous: refused
     * rather than guessed at. */
    assert(mdkr_party_sdp_fingerprint_for_test(
        "a=fingerprint:sha-256 AB:CD\r\na=fingerprint:sha-256 AB:CE\r\n")
        .empty());
    /* Absent, malformed, or non-attribute-line shapes are all refused. */
    assert(mdkr_party_sdp_fingerprint_for_test("v=0\r\na=setup:actpass\r\n")
        .empty());
    assert(mdkr_party_sdp_fingerprint_for_test(
        "a=fingerprint:sha-256\r\n").empty());
    assert(mdkr_party_sdp_fingerprint_for_test(
        "a=fingerprint:sha-256 AB:C\r\n").empty());
    assert(mdkr_party_sdp_fingerprint_for_test(
        "a=fingerprint:sha-256 AB::CD\r\n").empty());
    assert(mdkr_party_sdp_fingerprint_for_test(
        "a=fingerprint:sha-256 AB:CG\r\n").empty());
    assert(mdkr_party_sdp_fingerprint_for_test(
        "a=fingerprint:sha-256 AB:CD extra\r\n").empty());
    assert(mdkr_party_sdp_fingerprint_for_test(
        "x=a=fingerprint:sha-256 AB:CD\r\n").empty());

    /* Signaling-URL pins. The https form matters because libdatachannel's
     * URL parser rejects anything else ("wss:://host" — the shape the old
     * five-character in-place rewrite produced from an https origin — never
     * even reaches the network); the ws form is the loopback lane
     * tests/check_party_native_e2e.py drives against wrangler dev. */
    assert(mdkr_party_signaling_url_for_test(
               "https://party.example.com", "/api/party/native-create") ==
           "wss://party.example.com/api/party/native-create");
    assert(mdkr_party_signaling_url_for_test(
               "http://127.0.0.1:8787", "/api/party/native-create") ==
           "ws://127.0.0.1:8787/api/party/native-create");

    /* I3: a failed host_command_result carries the worker's typed error
     * code VERBATIM into the CommandRejected event, alongside the same
     * generic prose as before (the host owns the per-code copy table). */
    MdkrPartyTransportEvent rejected;
    assert(mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result","ok":false,"error":"room_full"})",
        rejected));
    assert(rejected.type == MdkrPartyTransportEventType::CommandRejected);
    assert(rejected.errorCode == "room_full");
    assert(rejected.message ==
        "That controller action did not complete. Try again.");
    assert(rejected.controllerId.empty());

    /* ok:true results and non-results produce no rejection event at all. */
    MdkrPartyTransportEvent ignored;
    assert(!mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result","ok":true})", ignored));
    assert(!mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result"})", ignored));
    assert(!mdkr_party_host_command_rejection_for_test(
        R"({"type":"room_state","ok":false})", ignored));

    /* A missing, non-string, or oversized error field degrades to the empty
     * "unknown" code -- it must never hide the failure itself. */
    MdkrPartyTransportEvent missing;
    assert(mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result","ok":false})", missing));
    assert(missing.errorCode.empty());
    MdkrPartyTransportEvent numeric;
    assert(mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result","ok":false,"error":7})", numeric));
    assert(numeric.errorCode.empty());
    MdkrPartyTransportEvent oversized;
    assert(mdkr_party_host_command_rejection_for_test(
        std::string(R"({"type":"host_command_result","ok":false,"error":")") +
            std::string(65u, 'x') + R"("})",
        oversized));
    assert(oversized.errorCode.empty());

    /* Task-1 residual closure: the worker echoes the failed command's
     * identity -- the command name always, the controller id when the
     * command targeted one -- and the parser carries both into the event
     * verbatim so the host can scope its cleanup to exactly the command
     * that failed. */
    MdkrPartyTransportEvent scoped;
    assert(mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result","ok":false,"error":"room_full",)"
        R"("command":"approve","controllerId":"phone-b"})", scoped));
    assert(scoped.command == "approve");
    assert(scoped.controllerId == "phone-b");
    assert(scoped.errorCode == "room_full");
    MdkrPartyTransportEvent roomLevel;
    assert(mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result","ok":false,"error":"invalid_state",)"
        R"("command":"rotate"})", roomLevel));
    assert(roomLevel.command == "rotate");
    assert(roomLevel.controllerId.empty());
    /* Malformed or oversized identity degrades to absent exactly like the
     * error code -- identity must never hide the failure itself. */
    MdkrPartyTransportEvent malformedIdentity;
    assert(mdkr_party_host_command_rejection_for_test(
        R"({"type":"host_command_result","ok":false,"error":"invalid_state",)"
        R"("command":7,"controllerId":9})", malformedIdentity));
    assert(malformedIdentity.command.empty());
    assert(malformedIdentity.controllerId.empty());
    MdkrPartyTransportEvent oversizedIdentity;
    assert(mdkr_party_host_command_rejection_for_test(
        std::string(R"({"type":"host_command_result","ok":false,)"
        R"("error":"room_full","controllerId":")") +
            std::string(65u, 'c') + R"("})",
        oversizedIdentity));
    assert(oversizedIdentity.controllerId.empty());

    /* I2: a controller_ready that declares any pairing-protocol version but
     * this build's own must surface as ControllerProtocolMismatch instead of
     * being dropped in silence (the pre-fix behavior: the phone looked
     * healthy at the WebRTC layer while its seat sat in an
     * indistinguishable "Reconnecting" forever). The peer identity checks
     * still gate it: only a controller_ready addressed to this exact
     * peer/connection produces any event at all. */
    MdkrPartyTransportEvent mismatch;
    assert(mdkr_party_controller_ready_event_for_test(
        R"({"type":"controller_ready","protocol":3,"controllerId":"phone-a",)"
        R"("connectionSequence":7})",
        "phone-a", 7u, mismatch));
    assert(mismatch.type ==
        MdkrPartyTransportEventType::ControllerProtocolMismatch);
    assert(mismatch.controllerId == "phone-a");
    assert(mismatch.theirProtocol == 3u);

    /* A declared-nothing page is a mismatch too, reported as version 0. */
    MdkrPartyTransportEvent unversioned;
    assert(mdkr_party_controller_ready_event_for_test(
        R"({"type":"controller_ready","controllerId":"phone-a",)"
        R"("connectionSequence":7})",
        "phone-a", 7u, unversioned));
    assert(unversioned.type ==
        MdkrPartyTransportEventType::ControllerProtocolMismatch);
    assert(unversioned.theirProtocol == 0u);

    /* This build's own protocol still connects, capabilities intact. */
    MdkrPartyTransportEvent connected;
    assert(mdkr_party_controller_ready_event_for_test(
        R"({"type":"controller_ready","protocol":1,"controllerId":"phone-a",)"
        R"("connectionSequence":7,"capabilities":{"vibration":true}})",
        "phone-a", 7u, connected));
    assert(connected.type ==
        MdkrPartyTransportEventType::ControllerConnected);
    assert(connected.controllerId == "phone-a");
    assert(connected.haptics);

    /* Addressed to another peer or another connection epoch: no event,
     * matched or mismatched protocol alike. */
    MdkrPartyTransportEvent misaddressed;
    assert(!mdkr_party_controller_ready_event_for_test(
        R"({"type":"controller_ready","protocol":3,"controllerId":"phone-b",)"
        R"("connectionSequence":7})",
        "phone-a", 7u, misaddressed));
    assert(!mdkr_party_controller_ready_event_for_test(
        R"({"type":"controller_ready","protocol":1,"controllerId":"phone-a",)"
        R"("connectionSequence":8})",
        "phone-a", 7u, misaddressed));
    assert(!mdkr_party_controller_ready_event_for_test(
        R"({"type":"pong","protocol":3,"controllerId":"phone-a",)"
        R"("connectionSequence":7})",
        "phone-a", 7u, misaddressed));

    /* M4: quitting must say goodbye without hanging the app. closeRoom()
     * queues the `close` command (the worker relays it to every phone as
     * host_closed) and then waits, bounded, for the socket write to flush
     * before the caller tears the socket down. The seam runs the transport's
     * exact wait loop against an injectable clock/buffer/sleep, so the
     * 250 ms cap is proven here without a socket. */
    {
        /* A socket that never drains: the full cap and not one millisecond
         * of quit-blocking more. */
        uint64_t fakeNowMs = 0u;
        const uint64_t waited = mdkr_party_close_flush_wait_for_test(
            [&fakeNowMs]() { return fakeNowMs; },
            []() -> size_t { return 64u; },
            [&fakeNowMs](uint64_t ms) { fakeNowMs += ms; });
        assert(kMdkrPartyCloseFlushDeadlineMs == 250u);
        assert(waited == kMdkrPartyCloseFlushDeadlineMs);
        assert(fakeNowMs == 250u);
    }
    {
        /* Already flushed: quit pays nothing at all. */
        uint64_t fakeNowMs = 0u;
        const uint64_t waited = mdkr_party_close_flush_wait_for_test(
            [&fakeNowMs]() { return fakeNowMs; },
            []() -> size_t { return 0u; },
            [&fakeNowMs](uint64_t ms) { fakeNowMs += ms; });
        assert(waited == 0u);
        assert(fakeNowMs == 0u);
    }
    {
        /* Drains partway through: the wait ends the moment the write
         * completes (three 5 ms polls saw bytes, the fourth saw none). */
        uint64_t fakeNowMs = 0u;
        unsigned polls = 0u;
        const uint64_t waited = mdkr_party_close_flush_wait_for_test(
            [&fakeNowMs]() { return fakeNowMs; },
            [&polls]() -> size_t { return ++polls <= 3u ? 64u : 0u; },
            [&fakeNowMs](uint64_t ms) { fakeNowMs += ms; });
        assert(waited == 15u);
        assert(fakeNowMs == 15u);
    }
    {
        /* An oversleeping host (every requested 5 ms nap really costs 50):
         * the cap is on the observed WALL clock, not the sum of requests,
         * so the loop still exits at 250 ms elapsed even though only 25 ms
         * of sleep was ever asked for. */
        uint64_t fakeNowMs = 0u;
        uint64_t requestedMs = 0u;
        const uint64_t waited = mdkr_party_close_flush_wait_for_test(
            [&fakeNowMs]() { return fakeNowMs; },
            []() -> size_t { return 64u; },
            [&fakeNowMs, &requestedMs](uint64_t ms) {
                requestedMs += ms;
                fakeNowMs += ms * 10u;
            });
        assert(waited == 25u);
        assert(requestedMs == 25u);
        assert(fakeNowMs == 250u);
    }
    return 0;
}
