#include "party/native_remote_pad_ingress.h"

/* This test is assert-driven. NDEBUG (the Release default) would compile
 * every check away and leave a test that can only pass — and, as a tell,
 * -Werror unused-variable failures on the values the asserts consume. */
#undef NDEBUG

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

std::vector<uint8_t> packet(uint32_t connection, uint32_t sequence,
                            uint16_t buttons = 0x8000u) {
    MdkrPartyPadPacket source{};
    source.flags = MDKR_PARTY_PAD_FLAG_PRESENT;
    source.connection_sequence = connection;
    source.sample_sequence = sequence;
    source.sender_time_ms = sequence;
    source.buttons = buttons;
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> bytes{};
    size_t length = 0u;
    assert(mdkr_party_pad_encode(
        &source, bytes.data(), bytes.size(), &length));
    return std::vector<uint8_t>(bytes.begin(), bytes.begin() + length);
}

void identityAndValidation() {
    mdkr_native_remote_pad_reset_all();
    assert(!mdkr_native_remote_pad_bind(4u, 1u, 1u));
    assert(!mdkr_native_remote_pad_bind(0u, 0u, 1u));
    assert(mdkr_native_remote_pad_bind(0u, 41u, 7u));
    assert(mdkr_native_remote_pad_bind(0u, 41u, 7u));

    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(0u, &owner, &connection));
    assert(owner == 41u && connection == 7u);

    std::vector<uint8_t> good = packet(7u, 1u);
    assert(!mdkr_native_remote_pad_push(
        0u, 42u, 7u, good.data(), good.size()));
    assert(!mdkr_native_remote_pad_push(
        0u, 41u, 8u, good.data(), good.size()));
    std::vector<uint8_t> wrongConnection = packet(8u, 2u);
    assert(!mdkr_native_remote_pad_push(
        0u, 41u, 7u, wrongConnection.data(), wrongConnection.size()));
    good.back() ^= 0x01u;
    assert(!mdkr_native_remote_pad_push(
        0u, 41u, 7u, good.data(), good.size()));

    good = packet(7u, 3u);
    assert(mdkr_native_remote_pad_push(
        0u, 41u, 7u, good.data(), good.size()));
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    const size_t length = mdkr_native_remote_pad_pop(
        0u, 41u, 7u, output.data(), output.size());
    assert(length == good.size());
    assert(std::equal(good.begin(), good.end(), output.begin()));

    assert(!mdkr_native_remote_pad_release(0u, 41u, 6u));
    assert(mdkr_native_remote_pad_info(0u, &owner, &connection));
    assert(mdkr_native_remote_pad_release(0u, 41u, 7u));
    assert(!mdkr_native_remote_pad_info(0u, &owner, &connection));
}

void overflowFailsNeutral() {
    mdkr_native_remote_pad_reset_all();
    assert(mdkr_native_remote_pad_bind(1u, 90u, 12u));
    for (uint32_t index = 0u;
         index < MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY; ++index) {
        const std::vector<uint8_t> bytes = packet(12u, index + 1u);
        assert(mdkr_native_remote_pad_push(
            1u, 90u, 12u, bytes.data(), bytes.size()));
    }
    const std::vector<uint8_t> extra = packet(12u, 100u);
    assert(!mdkr_native_remote_pad_push(
        1u, 90u, 12u, extra.data(), extra.size()));
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(1u, &owner, &connection));
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    assert(mdkr_native_remote_pad_pop(
        1u, 90u, 12u, output.data(), output.size()) == 0u);
    MdkrNativeRemotePadIngressStats stats{};
    mdkr_native_remote_pad_stats(1u, &stats);
    assert(stats.packets == MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY);
    assert(stats.overflows == 1u);
}

void rebindAndRumble() {
    mdkr_native_remote_pad_reset_all();
    assert(mdkr_native_remote_pad_bind(2u, 5u, 2u));
    const std::vector<uint8_t> old = packet(2u, 1u);
    assert(mdkr_native_remote_pad_push(
        2u, 5u, 2u, old.data(), old.size()));
    assert(!mdkr_native_remote_pad_request_rumble(2u, 100u));
    assert(mdkr_native_remote_pad_set_haptics(2u, 5u, 2u, true));
    assert(mdkr_native_remote_pad_request_rumble(2u, 100u));
    assert(mdkr_native_remote_pad_request_rumble(2u, 600u));
    assert(mdkr_native_remote_pad_bind(2u, 5u, 3u));

    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    assert(mdkr_native_remote_pad_pop(
        2u, 5u, 2u, output.data(), output.size()) == 0u);
    uint16_t strength = 1u;
    assert(!mdkr_native_remote_pad_take_rumble(2u, 5u, 2u, &strength));
    assert(strength == 0u);
    assert(mdkr_native_remote_pad_set_haptics(2u, 5u, 3u, true));
    assert(mdkr_native_remote_pad_request_rumble(2u, 321u));
    assert(mdkr_native_remote_pad_take_rumble(2u, 5u, 3u, &strength));
    assert(strength == 321u);
    assert(!mdkr_native_remote_pad_take_rumble(2u, 5u, 3u, &strength));
}

void concurrentProducerConsumer() {
    mdkr_native_remote_pad_reset_all();
    assert(mdkr_native_remote_pad_bind(3u, 700u, 44u));
    std::atomic<bool> finished{false};
    std::atomic<unsigned> accepted{0u};
    std::atomic<unsigned> consumed{0u};
    std::thread producer([&] {
        for (uint32_t sequence = 1u; sequence <= 2000u; ++sequence) {
            const std::vector<uint8_t> bytes = packet(44u, sequence);
            while (!mdkr_native_remote_pad_push(
                3u, 700u, 44u, bytes.data(), bytes.size())) {
                uint64_t owner = 0u;
                uint32_t connection = 0u;
                if (!mdkr_native_remote_pad_info(3u, &owner, &connection)) {
                    assert(mdkr_native_remote_pad_bind(3u, 700u, 44u));
                }
                std::this_thread::yield();
            }
            accepted.fetch_add(1u, std::memory_order_relaxed);
        }
        finished.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
        while (!finished.load(std::memory_order_acquire)) {
            if (mdkr_native_remote_pad_pop(
                    3u, 700u, 44u, output.data(), output.size()) > 0u) {
                consumed.fetch_add(1u, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        while (mdkr_native_remote_pad_pop(
                   3u, 700u, 44u, output.data(), output.size()) > 0u) {
            consumed.fetch_add(1u, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();
    assert(accepted.load() == 2000u);
    assert(consumed.load() <= accepted.load());
    MdkrNativeRemotePadIngressStats stats{};
    mdkr_native_remote_pad_stats(3u, &stats);
    assert(stats.packets == accepted.load());
}

}  // namespace

int main() {
    identityAndValidation();
    overflowFailsNeutral();
    rebindAndRumble();
    concurrentProducerConsumer();
    mdkr_native_remote_pad_reset_all();
    return 0;
}
