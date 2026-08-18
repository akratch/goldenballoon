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
    return 0;
}
