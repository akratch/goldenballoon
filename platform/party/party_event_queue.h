/*
 * Bounded queue for MdkrPartyTransportEvent with a survivable overflow
 * policy.
 *
 * The transport's callbacks (WebSocket/WebRTC threads) publish events faster
 * than the launcher thread is guaranteed to drain them -- a minimized
 * window, an OS hiccup, anything that delays a service() call. Phones
 * heartbeat their pad state every 50 ms, so a pad-packet event is by far the
 * most common one and, by the Party pad protocol's own design, droppable: a
 * missed sample is just a gap the next sample fills. Room/control events
 * (RoomState, connect/disconnect, command results, errors) are not --
 * dropping one of those silently desyncs the host's view of the room from
 * the truth the service holds.
 *
 * This queue never grows past capacity, never clears itself, and never
 * fabricates an event. At capacity it evicts the oldest droppable event to
 * make room for the newest one; only in the (practically unreachable, but
 * still-handled) case where every queued event is non-droppable does it
 * fall back to evicting a same-typed non-droppable event, so the bound
 * holds unconditionally without ever destroying the whole backlog.
 */
#ifndef MDKR_PARTY_EVENT_QUEUE_H
#define MDKR_PARTY_EVENT_QUEUE_H

#include "native_party_host.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/*
 * Not thread-safe on its own -- callers serialize access externally. The
 * transport holds the one mutex that already guards its other state and
 * takes it around every push()/drain(), so this queue does not need (and
 * must not add) a second lock.
 */
class MdkrPartyEventQueue {
public:
    explicit MdkrPartyEventQueue(size_t capacity);

    void push(MdkrPartyTransportEvent event);

    /* Empties the queue into caller order (oldest first). Draining is not
     * an overflow condition and never blocks a future push: there is no
     * latch. */
    std::vector<MdkrPartyTransportEvent> drain();

    uint64_t droppedPadPackets() const { return droppedPadPackets_; }

private:
    size_t capacity_;
    std::deque<MdkrPartyTransportEvent> events_;
    uint64_t droppedPadPackets_ = 0u;
};

#endif /* MDKR_PARTY_EVENT_QUEUE_H */
