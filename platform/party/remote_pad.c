#include "remote_pad.h"

#include <string.h>

static MdkrInputSample neutral_sample(void) {
    const MdkrInputSample sample = {0, 0, 0, false};
    return sample;
}

static bool sample_equal(MdkrInputSample left, MdkrInputSample right) {
    return left.buttons == right.buttons && left.stick_x == right.stick_x &&
        left.stick_y == right.stick_y && left.present == right.present;
}

static bool sequence_newer(uint32_t candidate, uint32_t reference) {
    const uint32_t difference = candidate - reference;
    return difference != 0u && difference < 0x80000000u;
}

static void queue_neutral(MdkrRemotePad *pad, uint32_t sequence) {
    MdkrRemotePadTransition *entry;
    pad->head = 0u;
    pad->count = 1u;
    entry = &pad->transitions[0];
    entry->sequence = sequence;
    entry->sample = neutral_sample();
    pad->latest = entry->sample;
}

static bool queue_sample(
    MdkrRemotePad *pad, uint32_t sequence, MdkrInputSample sample) {
    unsigned tail;
    if (sample_equal(sample, pad->latest)) return true;
    if (pad->count >= MDKR_REMOTE_PAD_TRANSITION_CAPACITY) {
        queue_neutral(pad, sequence);
        pad->stats.overflow_neutralizations++;
        return false;
    }
    tail = ((unsigned)pad->head + (unsigned)pad->count) %
        MDKR_REMOTE_PAD_TRANSITION_CAPACITY;
    pad->transitions[tail].sequence = sequence;
    pad->transitions[tail].sample = sample;
    pad->count++;
    pad->latest = sample;
    return true;
}

static MdkrInputSample packet_sample(const MdkrPartyPadPacket *packet) {
    MdkrInputSample sample;
    sample.buttons = packet->buttons;
    sample.stick_x = packet->stick_x;
    sample.stick_y = packet->stick_y;
    sample.present = (packet->flags & MDKR_PARTY_PAD_FLAG_PRESENT) != 0u;
    if (!sample.present) sample = neutral_sample();
    return sample;
}

static MdkrInputSample edge_sample(const MdkrPartyPadEdge *edge) {
    const MdkrInputSample sample = {
        edge->buttons, edge->stick_x, edge->stick_y, true
    };
    return sample;
}

void mdkr_remote_pad_init(MdkrRemotePad *pad, uint32_t connection_sequence) {
    if (pad == NULL) return;
    memset(pad, 0, sizeof(*pad));
    pad->connection_sequence = connection_sequence;
    pad->latest = neutral_sample();
}

void mdkr_remote_pad_rebind(
    MdkrRemotePad *pad, uint32_t connection_sequence, uint64_t now_ms) {
    MdkrRemotePadStats stats;
    uint32_t sequence;
    if (pad == NULL) return;
    stats = pad->stats;
    sequence = pad->has_sequence ? pad->last_sequence + 1u : 0u;
    mdkr_remote_pad_init(pad, connection_sequence);
    pad->stats = stats;
    pad->last_receive_ms = now_ms;
    queue_neutral(pad, sequence);
}

MdkrPartyDecodeResult mdkr_remote_pad_accept(
    MdkrRemotePad *pad, const uint8_t *bytes, size_t length, uint64_t now_ms) {
    MdkrPartyPadPacket packet;
    MdkrPartyDecodeResult result;
    uint8_t index;
    if (pad == NULL) return MDKR_PARTY_DECODE_NULL;
    result = mdkr_party_pad_decode(bytes, length, &packet);
    if (result != MDKR_PARTY_DECODE_OK) {
        pad->stats.malformed++;
        return result;
    }
    pad->stats.packets++;
    if (packet.connection_sequence != pad->connection_sequence) {
        pad->stats.stale_connections++;
        return MDKR_PARTY_DECODE_EDGE_SEQUENCE;
    }
    if (pad->has_sequence &&
        !sequence_newer(packet.sample_sequence, pad->last_sequence)) {
        /* No newer sample, but a well-formed frame from the bound connection is
         * still proof of life. A held button emits no state change, so the
         * phone's heartbeat resends the current sample_sequence; counting those
         * keepalives as liveness is what stops the 250 ms silence timeout from
         * neutralizing a legitimate hold. */
        const bool duplicate = packet.sample_sequence == pad->last_sequence;
        if (duplicate) {
            pad->stats.duplicates++;
            /* A duplicate carries the phone's current state. If the silence
             * timeout already latched a neutral, re-assert it so a long hold
             * resumes instead of staying stuck neutral. queue_sample dedupes,
             * so this is a no-op whenever the held state is already published. */
            if (pad->timeout_latched) {
                (void)queue_sample(
                    pad, packet.sample_sequence, packet_sample(&packet));
            }
        } else {
            pad->stats.stale_packets++;
        }
        pad->last_receive_ms = now_ms;
        pad->received = true;
        pad->timeout_latched = false;
        return MDKR_PARTY_DECODE_EDGE_SEQUENCE;
    }
    /* Edges are oldest first. Each is a complete state, so a tap whose current
     * packet is already released still becomes press then release exactly once. */
    for (index = 0u; index < packet.edge_count; index++) {
        const MdkrPartyPadEdge *edge = &packet.edges[index];
        const uint32_t sequence = packet.sample_sequence - edge->sequence_delta;
        if (pad->has_sequence && !sequence_newer(sequence, pad->last_sequence)) {
            pad->stats.history_deduped++;
            continue;
        }
        if (!queue_sample(pad, sequence, edge_sample(edge))) break;
    }
    (void)queue_sample(pad, packet.sample_sequence, packet_sample(&packet));
    pad->last_sequence = packet.sample_sequence;
    pad->last_receive_ms = now_ms;
    pad->has_sequence = true;
    pad->received = true;
    pad->timeout_latched = false;
    return MDKR_PARTY_DECODE_OK;
}

bool mdkr_remote_pad_pop(
    MdkrRemotePad *pad, MdkrRemotePadTransition *out_transition) {
    if (pad == NULL || pad->count == 0u) return false;
    if (out_transition != NULL) *out_transition = pad->transitions[pad->head];
    pad->head = (uint8_t)(((unsigned)pad->head + 1u) %
                          MDKR_REMOTE_PAD_TRANSITION_CAPACITY);
    pad->count--;
    return true;
}

bool mdkr_remote_pad_expire(MdkrRemotePad *pad, uint64_t now_ms) {
    if (pad == NULL || !pad->received || pad->timeout_latched ||
        now_ms < pad->last_receive_ms ||
        now_ms - pad->last_receive_ms < MDKR_REMOTE_PAD_TIMEOUT_MS) {
        return false;
    }
    queue_neutral(pad, pad->has_sequence ? pad->last_sequence + 1u : 0u);
    pad->timeout_latched = true;
    pad->stats.timeouts++;
    return true;
}
