#include "party_retry_policy.h"

namespace {

/* Contractual policy numbers -- see party_retry_policy.h. */
constexpr uint64_t kUnauthenticatedDeadlineMs = 20000u;
constexpr unsigned kMaxOfferAttempts = 3u;

}  // namespace

MdkrPartyRetryDecision mdkr_party_retry_decide(
    uint64_t nowMs, uint64_t offerSentMs, unsigned offerAttempts,
    bool authenticated, bool protocolMismatched, bool socketOpen) {
    MdkrPartyRetryDecision decision;

    /* A connected phone, a peer whose controller_ready declared the wrong
     * pairing-protocol version (its offer WAS answered -- connected, just
     * wrong, and no retry can close a version gap; I2), or a peer that has
     * not sent an offer yet, is never this policy's business. */
    if (authenticated || protocolMismatched || offerSentMs == 0u) {
        return decision;
    }

    /* Edge-triggered: the caller only passes true on the tick that observes
     * the socket having just reopened. Resending costs nothing and does not
     * touch offerAttempts, so it takes priority over the age-based ladder
     * below and is evaluated independently of it -- a resend neither resets
     * nor advances the attempt count the next age check will see. */
    if (socketOpen) {
        decision.resendOffer = true;
        return decision;
    }

    const uint64_t age = nowMs >= offerSentMs ? nowMs - offerSentMs : 0u;
    if (age < kUnauthenticatedDeadlineMs) return decision;

    if (offerAttempts >= kMaxOfferAttempts) decision.giveUp = true;
    else decision.recreatePeer = true;
    return decision;
}
