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
    return 0;
}
