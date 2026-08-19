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
 *  - A protocol-mismatched peer (one whose controller_ready declared a
 *    different pairing-protocol version, I2) is never this policy's
 *    business either: its offer WAS answered, so it is connected-but-
 *    wrong-version, not stranded, and no recreated peer, resent offer, or
 *    give-up can close a version gap. The recovery is the phone reloading
 *    into a matching page, which arrives as a fresh peer with the flag
 *    clear.
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
 * protocolMismatched is the transport's per-peer I2 latch: set the moment a
 * controller_ready arrives with any pairing-protocol version but this
 * build's own, cleared only by the peer's replacement (a reloaded phone is
 * a fresh peer).
 *
 * socketOpen is edge-triggered: the caller passes true only on the call
 * that observes the signaling socket having just reopened, never on every
 * tick it happens to already be open -- otherwise an unanswered offer
 * would be resent continuously instead of once per reconnect.
 */
MdkrPartyRetryDecision mdkr_party_retry_decide(
    uint64_t nowMs, uint64_t offerSentMs, unsigned offerAttempts,
    bool authenticated, bool protocolMismatched, bool socketOpen);

/*
 * Signaling-socket resume policy (I4): the decision half of the transport's
 * reconnect ladder, including when to stop climbing it forever.
 *
 * A party room can be gone for good: its Durable Object's 24 h alarm ran
 * deleteAll, or its stored expiry passed (services/party/src/party-room.ts
 * rejects the resume upgrade with HTTP 404 in both cases), or the credential
 * can no longer validate (the Worker answers 401 before the room is even
 * consulted, services/party/src/worker.ts connect()). None of those states
 * ever heals within a session, yet the pre-fix ladder retried them every
 * <= 8 s forever.
 *
 * What the native side can actually SEE of that: libdatachannel folds every
 * HTTP-refused WebSocket upgrade -- whatever the status code -- into the one
 * onError string "WebSocket connection failed" (the non-101 status is
 * logged and swallowed, wshandshake.cpp/wstransport.cpp), and discards the
 * code and reason of typed close frames entirely. Network-level failures
 * are distinct strings ("TCP connection failed", "TLS connection failed",
 * "Connection timed out"). So the honest classification is not "reason ==
 * room_gone" but "the service itself answered this fresh handshake with a
 * refusal", counted consecutively:
 *
 *  - resumeRejected: this close followed the service refusing the resume
 *    upgrade with an HTTP response. False for live-socket drops and for
 *    network-level failures that never reached the service.
 *  - consecutiveResumeRejections: refused resumes since the last successful
 *    upgrade, including this one when resumeRejected is true. Network-level
 *    failures neither advance nor reset it (they proved nothing about the
 *    room); only a completed upgrade resets it.
 *  - reconnectAttempt: 1-based closes since the last successful upgrade,
 *    driving the existing 300 ms..8 s delay ladder, unchanged.
 *
 * Verdict (values are contractual -- update this comment and the test
 * together with any change): the THIRD consecutive refusal is terminal --
 * three separate TCP+TLS+HTTP round trips where the service said "no" is a
 * dead room, not weather; a one-off refusal mid-deploy retries as before.
 * Terminal means the ladder ends: no next delay, no further socket opens,
 * and the transport reports the room gone so the host can offer a new
 * invite (a brand-new room) as the way forward. Everything else keeps the
 * pre-existing bounded ladder: delays 300 ms doubling to a 8000 ms cap.
 */
struct MdkrPartyResumeDecision {
    bool retry = false;      /* schedule another resume after delayMs */
    bool terminal = false;   /* the room is gone for good: stop opening */
    unsigned delayMs = 0u;   /* valid only when retry is true */
};

MdkrPartyResumeDecision mdkr_party_resume_decide(
    unsigned reconnectAttempt, bool resumeRejected,
    unsigned consecutiveResumeRejections);

#endif /* MDKR_PARTY_RETRY_POLICY_H */
