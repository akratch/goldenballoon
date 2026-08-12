#include "libdatachannel_party_transport.h"

#include <memory>

namespace {
class UnavailableTransport final : public MdkrPartyTransport {
public:
    bool available() const override { return false; }
    const char *unavailableReason() const override {
        return "Phone controllers are not included in this build.";
    }
    bool open(const std::string &) override { return false; }
    bool approve(const std::string &, unsigned) override { return false; }
    bool reject(const std::string &) override { return false; }
    bool remove(const std::string &) override { return false; }
    bool rotateInvite(unsigned) override { return false; }
    bool revokeInvite() override { return false; }
    bool closeRoom() override { return false; }
    bool sendRumble(const std::string &, uint16_t) override { return false; }
    bool poll(MdkrPartyTransportEvent &) override { return false; }
    void shutdown() override {}
};
}  // namespace

std::unique_ptr<MdkrPartyTransport> mdkr_create_native_party_transport() {
    return std::make_unique<UnavailableTransport>();
}
