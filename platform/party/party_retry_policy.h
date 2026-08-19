/*
 * Pure decision policy for native WebRTC signaling retry (defect C3).
 *
 * The Worker's signaling relay is fire-and-forget: an offer addressed to a
 * phone whose socket happens to be closed at that instant is silently
 * dropped, and a PeerConnection that never receives a remote description
 * never reaches Failed -- nothing else in the transport ever notices, and
 * the phone is stranded until a human removes and re-adds it.
 *
 * This is the pure decision half of the fix: given how old the outstanding
 * offer is, how many times it has already been sent, whether the peer has
 * finished the control-channel handshake, and whether the signaling socket
 * just came back, decide what the transport's tick()/createPeer() should do
 * next. It touches no transport state, no clock, and no socket, so the
 * policy is exercised directly by a unit test instead of only through a
 * live WebSocket/WebRTC stack.
 *
 * Policy (values are contractual -- update this comment and its test
 * together with any change):
 *  - An authenticated peer (one that has completed controller_ready) is
 *    never recreated by this policy: it is a live, connected phone.
 *  - An unauthenticated peer whose outstanding offer has been unanswered
 *    for >= 20 s gets a fresh peer and a fresh offer (recreatePeer),
 *    unless it has already used all 3 attempts, in which case it gives up
 *    instead (giveUp) -- reported as an explicit per-controller error
 *    rather than silently hanging or tearing down the room.
 *  - A signaling socket that just reopened while a still-unauthenticated
 *    peer's offer is outstanding gets that same offer resent verbatim
 *    (resendOffer): the Worker dropped it because the socket was down, not
 *    because anything is wrong with the offer, so resending costs nothing
 *    and does not consume one of the 3 attempts.
 */
#ifndef MDKR_PARTY_RETRY_POLICY_H
#define MDKR_PARTY_RETRY_POLICY_H

#include <cstdint>

struct MdkrPartyRetryDecision {
    bool recreatePeer = false;
    bool resendOffer = false;
    bool giveUp = false;
};

/*
 * nowMs and offerSentMs share one steady clock (the transport's
 * steadyNowMs()). offerSentMs == 0 means no offer has gone out yet for this
 * peer (still gathering ICE candidates, say) -- there is nothing to retry,
 * resend, or give up on. offerAttempts counts offers actually sent,
 * including the first, so it starts at 1 the moment the first offer goes
 * out and only advances when recreatePeer produces a genuinely new offer
 * (a resend does not consume an attempt).
 *
 * socketOpen is edge-triggered: the caller passes true only on the call
 * that observes the signaling socket having just reopened, never on every
 * tick it happens to already be open -- otherwise an unanswered offer
 * would be resent continuously instead of once per reconnect.
 */
MdkrPartyRetryDecision mdkr_party_retry_decide(
    uint64_t nowMs, uint64_t offerSentMs, unsigned offerAttempts,
    bool authenticated, bool socketOpen);

#endif /* MDKR_PARTY_RETRY_POLICY_H */
