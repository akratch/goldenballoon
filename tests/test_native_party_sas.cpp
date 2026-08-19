/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include "party/libdatachannel_party_transport.h"

#include <array>
#include <cassert>
#include <string>

int main() {
    std::array<uint8_t, 32> scalar{};
    scalar.back() = 1u;
    const std::string controllerKey =
        "BHzyexiNA09-ilI4AwS1GsPAiWnid_IbNaYLSPxHZpl4B3dVENuO0EApPZrGn3Qw27p9reY86YIpngS3nSJ4c9E";
    std::string hostKey;
    std::string phrase;
    assert(mdkr_party_sas_phrase_for_test(
        scalar.data(), "AAAAAAAAAAAAAAAAAAAAAA", controllerKey,
        hostKey, phrase));
    assert(hostKey ==
        "BGsX0fLhLEJH-Lzm5WOkQPJ3A32BLeszoPShOUXYmMKWT-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU");
    assert(phrase == "Royal-Penguin Nimble-Comet");
    assert(!mdkr_party_sas_phrase_for_test(
        scalar.data(), "AAAAAAAAAAAAAAAAAAAAAA", std::string(87u, 'C'),
        hostKey, phrase));

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
    return 0;
}
