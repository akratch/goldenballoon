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
        /*protocolMismatched=*/false, /*socketOpen=*/false);
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
        /*protocolMismatched=*/false, /*socketOpen=*/false);
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
        /*protocolMismatched=*/false, /*socketOpen=*/true);
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
        /*protocolMismatched=*/false, /*socketOpen=*/false));
}

/* Negative (a): authenticated peers are never recreated by this policy, no
 * matter how ancient the offer, how many attempts, or whether the socket
 * just reopened -- a connected phone is never this policy's business. */
void authenticatedPeersNeverRecreate() {
    const uint64_t offerSentMs = 1000u;
    const uint64_t now = offerSentMs + 999999u;
    allDecisionsAreFalse(mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/9u, /*authenticated=*/true,
        /*protocolMismatched=*/false, /*socketOpen=*/false));
    allDecisionsAreFalse(mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/9u, /*authenticated=*/true,
        /*protocolMismatched=*/false, /*socketOpen=*/true));
}

/* Negative (b): a young, unanswered offer with no socket reopen does
 * nothing -- restated at a much younger age than case (4) to make sure the
 * policy is not merely "not yet exactly at the boundary". */
void youngOffersDoNothing() {
    const uint64_t offerSentMs = 3000u;
    const uint64_t now = offerSentMs + 250u;
    allDecisionsAreFalse(mdkr_party_retry_decide(
        now, offerSentMs, /*offerAttempts=*/1u, /*authenticated=*/false,
        /*protocolMismatched=*/false, /*socketOpen=*/false));
}

/* Negative (c), I2: a peer whose controller_ready declared the wrong
 * pairing-protocol version is connected-but-wrong-version, not stranded --
 * its offer WAS answered, so there is nothing for this ladder to rescue and
 * nothing a recreated peer, resent offer, or give-up could fix. Walk every
 * rung the pre-fix ladder would have fired: the 20 s recreate (attempts 1
 * and 2), the 60 s give-up (attempt 3), far beyond it, and the
 * socket-reopen resend. The peer stays untouched on all of them; its only
 * exit is replacement by a fresh peer when the phone reloads. */
void wrongVersionPeersAreNeverRetriedResentOrGivenUpOn() {
    const uint64_t offerSentMs = 1000u;
    const unsigned attempts[] = {1u, 2u, 3u};
    const uint64_t ages[] = {kDeadlineMs, 2u * kDeadlineMs, 3u * kDeadlineMs,
                             999999u};
    for (const unsigned attempt : attempts) {
        for (const uint64_t age : ages) {
            allDecisionsAreFalse(mdkr_party_retry_decide(
                offerSentMs + age, offerSentMs, attempt,
                /*authenticated=*/false, /*protocolMismatched=*/true,
                /*socketOpen=*/false));
        }
        allDecisionsAreFalse(mdkr_party_retry_decide(
            offerSentMs + kDeadlineMs, offerSentMs, attempt,
            /*authenticated=*/false, /*protocolMismatched=*/true,
            /*socketOpen=*/true));
    }
}

/* No offer sent yet (still gathering ICE candidates, say) is never a
 * reason to recreate, resend, or give up, even on a socket reopen. */
void noOutstandingOfferDoesNothing() {
    allDecisionsAreFalse(mdkr_party_retry_decide(
        999999u, /*offerSentMs=*/0u, /*offerAttempts=*/0u,
        /*authenticated=*/false, /*protocolMismatched=*/false,
        /*socketOpen=*/true));
}

}  // namespace

int main() {
    unansweredOfferAtDeadlineRecreatesThePeer();
    thirdUnansweredOfferAtDeadlineGivesUp();
    socketReopenWithPendingOfferResendsRegardlessOfAge();
    freshOfferUnderTheDeadlineDoesNothing();
    authenticatedPeersNeverRecreate();
    youngOffersDoNothing();
    wrongVersionPeersAreNeverRetriedResentOrGivenUpOn();
    noOutstandingOfferDoesNothing();
    return 0;
}
