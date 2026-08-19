#include "native_remote_pad_ingress.h"

#include <array>
#include <cstring>
#include <mutex>

namespace {

struct Packet {
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> bytes{};
    size_t length = 0u;
};

struct Port {
    std::mutex mutex;
    uint64_t owner = 0u;
    uint32_t connectionSequence = 0u;
    std::array<Packet, MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY> packets{};
    size_t head = 0u;
    size_t count = 0u;
    uint16_t rumbleStrength = 0u;
    bool reserved = false;
    bool haptics = false;
    bool rumblePending = false;
    MdkrNativeRemotePadIngressStats stats{};
};

std::array<Port, MDKR_NATIVE_REMOTE_PAD_PORTS> g_ports;

Port *portAt(unsigned port) {
    return port < g_ports.size() ? &g_ports[port] : nullptr;
}

void clearPayload(Port &port) {
    for (Packet &packet : port.packets) {
        packet.length = 0u;
    }
    port.head = 0u;
    port.count = 0u;
    port.rumbleStrength = 0u;
    port.rumblePending = false;
    port.haptics = false;
}

bool sameBinding(
    const Port &port, uint64_t owner, uint32_t connectionSequence) {
    return port.reserved && port.owner == owner &&
        port.connectionSequence == connectionSequence;
}

}  // namespace

extern "C" bool mdkr_native_remote_pad_bind(
    unsigned index, uint64_t owner, uint32_t connectionSequence) {
    Port *port = portAt(index);
    if (port == nullptr || owner == 0u || connectionSequence == 0u) {
        return false;
    }
    std::lock_guard<std::mutex> lock(port->mutex);
    if (sameBinding(*port, owner, connectionSequence)) return true;
    clearPayload(*port);
    port->owner = owner;
    port->connectionSequence = connectionSequence;
    port->reserved = true;
    port->stats.binds++;
    return true;
}

extern "C" bool mdkr_native_remote_pad_release(
    unsigned index, uint64_t owner, uint32_t connectionSequence) {
    Port *port = portAt(index);
    if (port == nullptr || owner == 0u || connectionSequence == 0u) {
        return false;
    }
    std::lock_guard<std::mutex> lock(port->mutex);
    if (!sameBinding(*port, owner, connectionSequence)) {
        port->stats.stale++;
        return false;
    }
    clearPayload(*port);
    port->owner = 0u;
    port->connectionSequence = 0u;
    port->reserved = false;
    port->stats.releases++;
    return true;
}

extern "C" bool mdkr_native_remote_pad_push(
    unsigned index, uint64_t owner, uint32_t connectionSequence,
    const uint8_t *bytes, size_t length) {
    Port *port = portAt(index);
    MdkrPartyPadPacket decoded{};
    if (port == nullptr || bytes == nullptr || length == 0u ||
        length > MDKR_PARTY_PAD_MAX_BYTES ||
        mdkr_party_pad_decode(bytes, length, &decoded) != MDKR_PARTY_DECODE_OK ||
        decoded.connection_sequence != connectionSequence) {
        if (port != nullptr) {
            std::lock_guard<std::mutex> lock(port->mutex);
            port->stats.malformed++;
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(port->mutex);
    if (!sameBinding(*port, owner, connectionSequence)) {
        port->stats.stale++;
        return false;
    }
    if (port->count >= port->packets.size()) {
        /* Congestion is a custody event, not a packet drop. Revocation makes
         * the engine publish neutral and requires a fresh reliable rebind. */
        clearPayload(*port);
        port->owner = 0u;
        port->connectionSequence = 0u;
        port->reserved = false;
        port->stats.overflows++;
        return false;
    }
    const size_t tail = (port->head + port->count) % port->packets.size();
    Packet &packet = port->packets[tail];
    std::memcpy(packet.bytes.data(), bytes, length);
    packet.length = length;
    port->count++;
    port->stats.packets++;
    return true;
}

extern "C" bool mdkr_native_remote_pad_set_haptics(
    unsigned index, uint64_t owner, uint32_t connectionSequence,
    bool supported) {
    Port *port = portAt(index);
    if (port == nullptr) return false;
    std::lock_guard<std::mutex> lock(port->mutex);
    if (!sameBinding(*port, owner, connectionSequence)) {
        port->stats.stale++;
        return false;
    }
    port->haptics = supported;
    if (!supported) {
        port->rumbleStrength = 0u;
        port->rumblePending = false;
    }
    return true;
}

extern "C" bool mdkr_native_remote_pad_haptics_supported(unsigned index) {
    Port *port = portAt(index);
    if (port == nullptr) return false;
    std::lock_guard<std::mutex> lock(port->mutex);
    return port->reserved && port->haptics;
}

extern "C" bool mdkr_native_remote_pad_info(
    unsigned index, uint64_t *outOwner, uint32_t *outConnectionSequence) {
    Port *port = portAt(index);
    if (outOwner != nullptr) *outOwner = 0u;
    if (outConnectionSequence != nullptr) *outConnectionSequence = 0u;
    if (port == nullptr || outOwner == nullptr ||
        outConnectionSequence == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(port->mutex);
    if (!port->reserved) return false;
    *outOwner = port->owner;
    *outConnectionSequence = port->connectionSequence;
    return true;
}

extern "C" size_t mdkr_native_remote_pad_pop(
    unsigned index, uint64_t owner, uint32_t connectionSequence,
    uint8_t *output, size_t capacity) {
    Port *port = portAt(index);
    if (port == nullptr || output == nullptr) return 0u;
    std::lock_guard<std::mutex> lock(port->mutex);
    if (!sameBinding(*port, owner, connectionSequence) || port->count == 0u) {
        return 0u;
    }
    Packet &packet = port->packets[port->head];
    if (packet.length == 0u || packet.length > capacity) return 0u;
    const size_t length = packet.length;
    std::memcpy(output, packet.bytes.data(), length);
    packet.length = 0u;
    port->head = (port->head + 1u) % port->packets.size();
    port->count--;
    return length;
}

extern "C" bool mdkr_native_remote_pad_request_rumble(
    unsigned index, uint16_t strength) {
    Port *port = portAt(index);
    if (port == nullptr) return false;
    std::lock_guard<std::mutex> lock(port->mutex);
    if (!port->reserved || !port->haptics) return false;
    port->rumbleStrength = strength;
    port->rumblePending = true;
    port->stats.rumble_requests++;
    return true;
}

extern "C" bool mdkr_native_remote_pad_take_rumble(
    unsigned index, uint64_t owner, uint32_t connectionSequence,
    uint16_t *outStrength) {
    Port *port = portAt(index);
    if (outStrength != nullptr) *outStrength = 0u;
    if (port == nullptr || outStrength == nullptr) return false;
    std::lock_guard<std::mutex> lock(port->mutex);
    if (!sameBinding(*port, owner, connectionSequence) ||
        !port->rumblePending) {
        return false;
    }
    *outStrength = port->rumbleStrength;
    port->rumblePending = false;
    return true;
}

extern "C" bool mdkr_native_remote_pad_peek_rumble(
    unsigned index, uint64_t owner, uint32_t connectionSequence,
    uint16_t *outStrength) {
    Port *port = portAt(index);
    if (outStrength != nullptr) *outStrength = 0u;
    if (port == nullptr || outStrength == nullptr) return false;
    std::lock_guard<std::mutex> lock(port->mutex);
    if (!sameBinding(*port, owner, connectionSequence)) return false;
    *outStrength = port->rumbleStrength;
    return true;
}

extern "C" void mdkr_native_remote_pad_stats(
    unsigned index, MdkrNativeRemotePadIngressStats *outStats) {
    if (outStats == nullptr) return;
    *outStats = MdkrNativeRemotePadIngressStats{};
    Port *port = portAt(index);
    if (port == nullptr) return;
    std::lock_guard<std::mutex> lock(port->mutex);
    *outStats = port->stats;
}

extern "C" void mdkr_native_remote_pad_reset_all(void) {
    for (Port &port : g_ports) {
        std::lock_guard<std::mutex> lock(port.mutex);
        clearPayload(port);
        port.owner = 0u;
        port.connectionSequence = 0u;
        port.reserved = false;
        port.stats = MdkrNativeRemotePadIngressStats{};
    }
}
