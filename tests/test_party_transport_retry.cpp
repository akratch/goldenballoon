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

/* ---- mdkr_party_resume_decide: the signaling-socket resume ladder (I4).
 * Same pure-decision seam as the offer policy above; the transport's
 * socketClosed() supplies the inputs and applies the verdict. See
 * party_retry_policy.h for the classification writeup (what a refused
 * resume looks like through libdatachannel, and why the terminal verdict
 * needs BOTH three consecutive refusals AND a refusal streak that has
 * spanned at least 30 s of wall clock -- a dead room refuses
 * deterministically forever, so it can afford to wait out a correlated
 * edge incident whose 5xx answers look identical through libdatachannel).
 * nowMs/firstRejectedMs share the transport's one steady clock, exactly
 * like nowMs/offerSentMs above; firstRejectedMs == 0 means no refusal
 * streak is standing. ---- */

constexpr uint64_t kTerminalFloorMs = 30000u;

/* (5) Regression: a transient close -- a live socket dropping, or any
 * network-level failure (TCP/TLS/timeout) -- keeps the existing bounded
 * ladder exactly: every attempt schedules another resume, the delay starts
 * at 300 ms, never shrinks, and never exceeds 8 s. Never terminal. */
void transientClosesKeepTheBoundedLadderUnderEightSeconds() {
    unsigned previousDelay = 0u;
    for (unsigned attempt = 1u; attempt <= 12u; attempt++) {
        const MdkrPartyResumeDecision decision = mdkr_party_resume_decide(
            attempt, /*resumeRejected=*/false,
            /*consecutiveResumeRejections=*/0u,
            /*nowMs=*/1000u + attempt * 8000u, /*firstRejectedMs=*/0u);
        assert(decision.retry);
        assert(!decision.terminal);
        assert(decision.delayMs > 0u && decision.delayMs <= 8000u);
        assert(decision.delayMs >= previousDelay);
        previousDelay = decision.delayMs;
    }
    assert(mdkr_party_resume_decide(1u, false, 0u, 1000u, 0u).delayMs == 300u);
    assert(mdkr_party_resume_decide(6u, false, 0u, 1000u, 0u).delayMs == 8000u);
}

/* (6) One or two refused resumes could still be a service blip mid-deploy;
 * the ladder keeps going, on its usual bounded delays -- even if the thin
 * streak has somehow been standing longer than the terminal floor. */
void rejectedResumesBelowTheLimitStillRetry() {
    for (unsigned rejections = 1u; rejections <= 2u; rejections++) {
        const MdkrPartyResumeDecision decision = mdkr_party_resume_decide(
            /*reconnectAttempt=*/rejections, /*resumeRejected=*/true,
            /*consecutiveResumeRejections=*/rejections,
            /*nowMs=*/1000u + 2u * kTerminalFloorMs,
            /*firstRejectedMs=*/1000u);
        assert(decision.retry);
        assert(!decision.terminal);
        assert(decision.delayMs > 0u && decision.delayMs <= 8000u);
    }
}

/* (7) I-1 fix: three refusals arriving FAST -- the shape of a correlated
 * edge incident, where live sockets drop and upgrades answer 5xx at the
 * same moment, completing the streak on the ladder's 600 + 1200 ms rungs
 * about 2.1 s after the drop -- must NOT be terminal. The room may be
 * perfectly healthy behind a service that recovers in seconds; keep
 * climbing the bounded ladder until the refusals have lasted. */
void rapidRefusalStreakInsideTheFloorKeepsRetrying() {
    const uint64_t firstRejectedMs = 1000u;
    const unsigned counts[] = {3u, 4u, 5u};
    for (const unsigned count : counts) {
        const MdkrPartyResumeDecision decision = mdkr_party_resume_decide(
            /*reconnectAttempt=*/count, /*resumeRejected=*/true,
            /*consecutiveResumeRejections=*/count,
            /*nowMs=*/firstRejectedMs + 2100u, firstRejectedMs);
        assert(decision.retry);
        assert(!decision.terminal);
        assert(decision.delayMs > 0u && decision.delayMs <= 8000u);
    }
    /* One tick under the floor is still under the floor. */
    assert(!mdkr_party_resume_decide(9u, true, 9u,
        firstRejectedMs + (kTerminalFloorMs - 1u), firstRejectedMs).terminal);
}

/* (8) The terminal verdict: at least three consecutive refusals AND the
 * refusal streak has spanned the 30 s floor from its first refusal to this
 * deciding one. The service has now answered "no" to separate fresh
 * handshakes across half a minute -- a deleted/expired room (or an equally
 * unrecoverable credential), not an incident blip. No next delay -- the
 * ladder ends here. */
void refusalsSpanningTheFloorAreTerminal() {
    const uint64_t firstRejectedMs = 1000u;
    const MdkrPartyResumeDecision decision = mdkr_party_resume_decide(
        /*reconnectAttempt=*/9u, /*resumeRejected=*/true,
        /*consecutiveResumeRejections=*/9u,
        /*nowMs=*/firstRejectedMs + kTerminalFloorMs, firstRejectedMs);
    assert(decision.terminal);
    assert(!decision.retry);
    assert(decision.delayMs == 0u);
    /* Well past the floor, and with a count overshoot, still terminal. */
    assert(mdkr_party_resume_decide(12u, true, 40u,
        firstRejectedMs + 10u * kTerminalFloorMs, firstRejectedMs).terminal);
    /* Exactly the minimum count at exactly the floor is the earliest
     * possible verdict. */
    assert(mdkr_party_resume_decide(9u, true, 3u,
        firstRejectedMs + kTerminalFloorMs, firstRejectedMs).terminal);
}

/* (9) A network-level failure is never terminal and never the deciding
 * strike, even when it lands on top of a standing refusal streak that
 * already satisfies both the count and the floor: this attempt never
 * reached the service, so it proved nothing about the room. Only an actual
 * refusal may end the ladder. */
void networkFailuresNeverGoTerminalEvenWithAStaleRejectionStreak() {
    const uint64_t firstRejectedMs = 1000u;
    const unsigned staleStreaks[] = {2u, 3u, 7u};
    for (const unsigned staleStreak : staleStreaks) {
        const MdkrPartyResumeDecision decision = mdkr_party_resume_decide(
            /*reconnectAttempt=*/8u, /*resumeRejected=*/false,
            staleStreak,
            /*nowMs=*/firstRejectedMs + 4u * kTerminalFloorMs,
            firstRejectedMs);
        assert(decision.retry);
        assert(!decision.terminal);
        assert(decision.delayMs > 0u && decision.delayMs <= 8000u);
    }
}

/* ---- mdkr_party_socket_cycle_due: proactive host-socket recycling (M7).
 * The Worker closes any one signaling socket for good at 512 lifetime
 * messages (services/party/src/party-room.ts SIGNAL_LIFETIME_MESSAGES) -- a
 * quota, not a fault. The transport counts its own outbound sends per
 * socket and recycles at 480, leaving 32 messages of headroom, through the
 * exact resume ladder a network drop uses. ---- */

/* (10) The trigger is edge-shaped: false all the way up to the threshold,
 * true at exactly 480, and false for every count above it -- so however a
 * late send races the close, only ONE cycle can ever fire per socket. The
 * replacement socket resets the count to zero, which is what re-arms it. */
void socketCycleFiresExactlyOnceAtTheHeadroomThreshold() {
    for (unsigned socketLife = 0u; socketLife < 2u; socketLife++) {
        for (unsigned sent = 0u; sent < 480u; sent++) {
            assert(!mdkr_party_socket_cycle_due(sent));
        }
        assert(mdkr_party_socket_cycle_due(480u));
        /* Sends that raced the close on this same socket: never a second
         * cycle, all the way past the Worker's own 512 hard cap. */
        for (unsigned sent = 481u; sent <= 600u; sent++) {
            assert(!mdkr_party_socket_cycle_due(sent));
        }
    }
}

/* (11) A proactive cycle is a CLEAN close, not a refusal: the transport
 * feeds socketClosed() resumeRejected=false for it, so it rides the prompt
 * first-attempt rung of the ladder and must never advance -- let alone
 * complete -- the I4 refusal streak, even when one is already standing at
 * terminal depth and age. */
void proactiveCycleRidesTheLadderWithoutTouchingTheRefusalStreak() {
    const MdkrPartyResumeDecision fresh = mdkr_party_resume_decide(
        /*reconnectAttempt=*/1u, /*resumeRejected=*/false,
        /*consecutiveResumeRejections=*/0u,
        /*nowMs=*/1000u, /*firstRejectedMs=*/0u);
    assert(fresh.retry);
    assert(!fresh.terminal);
    assert(fresh.delayMs == 300u);
    /* Atop a stale streak that satisfies both terminal gates: still just a
     * retry, because a clean close proves nothing about the room. */
    const MdkrPartyResumeDecision amidStreak = mdkr_party_resume_decide(
        /*reconnectAttempt=*/4u, /*resumeRejected=*/false,
        /*consecutiveResumeRejections=*/3u,
        /*nowMs=*/1000u + kTerminalFloorMs, /*firstRejectedMs=*/1000u);
    assert(amidStreak.retry);
    assert(!amidStreak.terminal);
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
    transientClosesKeepTheBoundedLadderUnderEightSeconds();
    rejectedResumesBelowTheLimitStillRetry();
    rapidRefusalStreakInsideTheFloorKeepsRetrying();
    refusalsSpanningTheFloorAreTerminal();
    networkFailuresNeverGoTerminalEvenWithAStaleRejectionStreak();
    socketCycleFiresExactlyOnceAtTheHeadroomThreshold();
    proactiveCycleRidesTheLadderWithoutTouchingTheRefusalStreak();
    return 0;
}
