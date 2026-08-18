/*
 * mdkr_party_retry_decide -- the pure decision half of defect C3's fix
 * (native WebRTC signaling retry, unauthenticated-peer deadline, offer
 * resend after socket recovery). See platform/party/party_retry_policy.h
 * for the full policy writeup; this is the direct regression test for its
 * four decision rows plus the two negative cases that must never fire a
 * decision.
 */
#include "party/party_retry_policy.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <cassert>
#include <cstdint>

namespace {

constexpr uint64_t kDeadlineMs = 20000u;

void allDecisionsAreFalse(const MdkrPartyRetryDecision &decision) {
    assert(!decision.recreatePeer);
    assert(!decision.resendOffer);
    assert(!decision.giveUp);
}

/* (1) An unauthenticated peer whose offer has been outstanding for exactly
 * the 20 s deadline, with attempts still available, gets a fresh peer and
 * fresh offer. */
void unansweredOfferAtDeadlineRecreatesThePeer() {
    const uint64_t offerSentMs = 1000u;
    const uint64_t now = offerSentMs + kDeadlineMs;
    const MdkrPartyRetryDecision decision = mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/1u, /*authenticated=*/false,
        /*socketOpen=*/false);
    assert(decision.recreatePeer);
    assert(!decision.resendOffer);
    assert(!decision.giveUp);
}

/* (2) Once the 3rd offer has also gone unanswered past the deadline, the
 * peer gives up instead of trying a 4th time. */
void thirdUnansweredOfferAtDeadlineGivesUp() {
    const uint64_t offerSentMs = 5000u;
    const uint64_t now = offerSentMs + kDeadlineMs;
    const MdkrPartyRetryDecision decision = mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/3u, /*authenticated=*/false,
        /*socketOpen=*/false);
    assert(decision.giveUp);
    assert(!decision.recreatePeer);
    assert(!decision.resendOffer);
}

/* (3) A socket that just reopened while an unauthenticated peer's offer is
 * still outstanding resends that same offer -- regardless of the offer's
 * age, because what failed was the Worker relay, not the offer itself. */
void socketReopenWithPendingOfferResendsRegardlessOfAge() {
    const uint64_t offerSentMs = 10000u;
    const uint64_t now = offerSentMs + 500u;  // well within the deadline
    const MdkrPartyRetryDecision decision = mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/1u, /*authenticated=*/false,
        /*socketOpen=*/true);
    assert(decision.resendOffer);
    assert(!decision.recreatePeer);
    assert(!decision.giveUp);
}

/* (4) A fresh offer under the 20 s deadline, socket not reopening, does
 * nothing at all. */
void freshOfferUnderTheDeadlineDoesNothing() {
    const uint64_t offerSentMs = 2000u;
    const uint64_t now = offerSentMs + (kDeadlineMs - 1u);
    allDecisionsAreFalse(mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/1u, /*authenticated=*/false,
        /*socketOpen=*/false));
}

/* Negative (a): authenticated peers are never recreated by this policy, no
 * matter how ancient the offer, how many attempts, or whether the socket
 * just reopened -- a connected phone is never this policy's business. */
void authenticatedPeersNeverRecreate() {
    const uint64_t offerSentMs = 1000u;
    const uint64_t now = offerSentMs + 999999u;
    allDecisionsAreFalse(mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/9u, /*authenticated=*/true,
        /*socketOpen=*/false));
    allDecisionsAreFalse(mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/9u, /*authenticated=*/true,
        /*socketOpen=*/true));
}

/* Negative (b): a young, unanswered offer with no socket reopen does
 * nothing -- restated at a much younger age than case (4) to make sure the
 * policy is not merely "not yet exactly at the boundary". */
void youngOffersDoNothing() {
    const uint64_t offerSentMs = 3000u;
    const uint64_t now = offerSentMs + 250u;
    allDecisionsAreFalse(mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/1u, /*authenticated=*/false,
        /*socketOpen=*/false));
}

/* No offer sent yet (still gathering ICE candidates, say) is never a
 * reason to recreate, resend, or give up, even on a socket reopen. */
void noOutstandingOfferDoesNothing() {
    allDecisionsAreFalse(mdkr_party_retry_decide(
        999999u, /*offerSentMs=*/0u, /*offerAttempts=*/0u,
        /*authenticated=*/false, /*socketOpen=*/true));
}

}  // namespace

int main() {
    unansweredOfferAtDeadlineRecreatesThePeer();
    thirdUnansweredOfferAtDeadlineGivesUp();
    socketReopenWithPendingOfferResendsRegardlessOfAge();
    freshOfferUnderTheDeadlineDoesNothing();
    authenticatedPeersNeverRecreate();
    youngOffersDoNothing();
    noOutstandingOfferDoesNothing();
    return 0;
}
