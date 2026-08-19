#include "party_retry_policy.h"

namespace {

/* Contractual policy numbers -- see party_retry_policy.h. */
constexpr uint64_t kUnauthenticatedDeadlineMs = 20000u;
constexpr unsigned kMaxOfferAttempts = 3u;
/* I4 resume ladder: the same 300 ms-doubling-to-8 s ladder the transport
 * always ran, now decided here; and the terminal gates -- at least three
 * consecutive service-refused resumes AND a refusal streak that has
 * spanned 30 s (see the header for why the floor exists: an edge
 * incident's 5xx looks exactly like a dead room's 404 through
 * libdatachannel, and only the dead room keeps refusing past it). */
constexpr unsigned kResumeBaseDelayMs = 300u;
constexpr unsigned kResumeMaxDelayMs = 8000u;
constexpr unsigned kResumeRejectionLimit = 3u;
constexpr uint64_t kResumeTerminalFloorMs = 30000u;

}  // namespace

MdkrPartyRetryDecision mdkr_party_retry_decide(
    uint64_t nowMs, uint64_t offerSentMs, unsigned offerAttempts,
    bool authenticated, bool protocolMismatched, bool socketOpen) {
    MdkrPartyRetryDecision decision;

    /* A connected phone, a peer whose controller_ready declared the wrong
     * channel-protocol version (its offer WAS answered -- connected, just
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

MdkrPartyResumeDecision mdkr_party_resume_decide(
    unsigned reconnectAttempt, bool resumeRejected,
    unsigned consecutiveResumeRejections,
    uint64_t nowMs, uint64_t firstRejectedMs) {
    MdkrPartyResumeDecision decision;

    /* Terminal only on an ACTUAL refusal that completes the streak (a
     * network-level failure carrying a stale streak proved nothing about
     * the room, so it can never be the deciding strike), and only once the
     * streak has both enough strikes AND enough wall clock behind it --
     * the floor is what separates a dead room from a service incident
     * whose refusals read identically (I-1). */
    const uint64_t streakSpanMs =
        firstRejectedMs != 0u && nowMs >= firstRejectedMs
            ? nowMs - firstRejectedMs : 0u;
    if (resumeRejected &&
        consecutiveResumeRejections >= kResumeRejectionLimit &&
        streakSpanMs >= kResumeTerminalFloorMs) {
        decision.terminal = true;
        return decision;
    }

    /* The pre-existing bounded ladder, verbatim: 300 ms doubling per
     * attempt, capped at 8 s. Attempt is 1-based; treat a caller's 0 as the
     * first attempt rather than shifting by an unsigned wraparound. */
    const unsigned attempt = reconnectAttempt == 0u ? 1u : reconnectAttempt;
    const unsigned exponent = attempt - 1u < 5u ? attempt - 1u : 5u;
    decision.retry = true;
    decision.delayMs = kResumeBaseDelayMs << exponent;
    if (decision.delayMs > kResumeMaxDelayMs) decision.delayMs = kResumeMaxDelayMs;
    return decision;
}
