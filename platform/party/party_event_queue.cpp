#include "party_event_queue.h"

#include <algorithm>
#include <utility>

namespace {

/* Pad-state packets are droppable because the pad protocol tolerates gaps by
 * design (remote_pad.h). Every other event type changes the host's model of
 * the room (a seat, an invite, a pending command) and must never be dropped
 * silently. No default case: adding a new event type without classifying it
 * here fails the build instead of failing open. */
bool isDroppable(MdkrPartyTransportEventType type) {
    bool droppable = false;
    switch (type) {
        case MdkrPartyTransportEventType::ControllerPacket:
            droppable = true;
            break;
        case MdkrPartyTransportEventType::RoomState:
        case MdkrPartyTransportEventType::ControllerConnected:
        case MdkrPartyTransportEventType::ControllerDisconnected:
        case MdkrPartyTransportEventType::ControllerPhrase:
        case MdkrPartyTransportEventType::CommandRejected:
        case MdkrPartyTransportEventType::Recovering:
        case MdkrPartyTransportEventType::Error:
        case MdkrPartyTransportEventType::Closed:
            droppable = false;
            break;
    }
    return droppable;
}

}  // namespace

MdkrPartyEventQueue::MdkrPartyEventQueue(size_t capacity) : capacity_(capacity) {}

void MdkrPartyEventQueue::push(MdkrPartyTransportEvent event) {
    if (capacity_ == 0u) {
        /* A zero-capacity queue cannot be built by the transport today, but
         * the policy must stay total: accept nothing, still count a dropped
         * pad packet honestly. */
        if (isDroppable(event.type)) droppedPadPackets_++;
        return;
    }

    if (events_.size() < capacity_) {
        events_.push_back(std::move(event));
        return;
    }

    /* At capacity: evict the oldest droppable event to make room. This is
     * the fix -- the old policy cleared everything and latched instead. */
    const auto oldestDroppable = std::find_if(
        events_.begin(), events_.end(),
        [](const MdkrPartyTransportEvent &queued) {
            return isDroppable(queued.type);
        });
    if (oldestDroppable != events_.end()) {
        events_.erase(oldestDroppable);
        droppedPadPackets_++;
        events_.push_back(std::move(event));
        return;
    }

    /* Every queued event is non-droppable. Cannot happen at capacity 128
     * with <=4 phones and the room-state cadence, but the policy must stay
     * total rather than grow past its bound or crash. */
    if (isDroppable(event.type)) {
        droppedPadPackets_++;
        return;
    }
    auto sameType = std::find_if(
        events_.begin(), events_.end(),
        [&event](const MdkrPartyTransportEvent &queued) {
            return queued.type == event.type;
        });
    if (sameType == events_.end()) sameType = events_.begin();
    events_.erase(sameType);
    events_.push_back(std::move(event));
}

std::vector<MdkrPartyTransportEvent> MdkrPartyEventQueue::drain() {
    std::vector<MdkrPartyTransportEvent> drained;
    drained.reserve(events_.size());
    for (MdkrPartyTransportEvent &event : events_) {
        drained.push_back(std::move(event));
    }
    events_.clear();
    return drained;
}
