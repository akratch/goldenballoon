#include "asset_swap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VehicleSoundAsset stride. platform/asset_swap.h exposes no symbolic size, so
 * the value is pinned behaviourally below: the swapper accepts exactly this
 * size and refuses one byte less. */
#define RECORD_SIZE 0x4Cu

/* Position-distinct filler: every offset holds a different byte from its
 * neighbour, so a swap of the wrong pair is observable rather than idempotent. */
#define FILL_BYTE(offset) ((uint8_t) ((offset) ^ 0xA5u))

#define REQUIRE(condition)                                                          \
    do {                                                                            \
        if (!(condition)) {                                                         \
            fprintf(stderr, "test_vehicle_audio_contract:%d: failed: %s\n",       \
                    __LINE__, #condition);                                          \
            return EXIT_FAILURE;                                                    \
        }                                                                           \
    } while (0)

static uint16_t load_u16(const uint8_t *bytes, uint32_t offset) {
    uint16_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static int16_t load_s16(const uint8_t *bytes, uint32_t offset) {
    int16_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

int main(void) {
    static const int16_t scales[7] = {
        -12000, -100, 0, 210, 500, 4000, 8000
    };
    uint8_t record[RECORD_SIZE];
    uint8_t before[RECORD_SIZE];
    uint32_t i;

    for (i = 0; i < RECORD_SIZE; i++) {
        record[i] = FILL_BYTE(i);
    }
    record[0x00] = 0x00; record[0x01] = 0x73;
    record[0x02] = 0x01; record[0x03] = 0x23;
    for (i = 0; i < 10; i++) {
        uint16_t value = (uint16_t)(5000u + i * 1000u);
        record[0x18 + i * 2] = (uint8_t)(value >> 8);
        record[0x19 + i * 2] = (uint8_t)value;
    }
    for (i = 0; i < 7; i++) {
        uint16_t encoded = (uint16_t)scales[i];
        record[0x3C + i * 2] = (uint8_t)(encoded >> 8);
        record[0x3D + i * 2] = (uint8_t)encoded;
    }

    REQUIRE(asset_swap_vehicle_sound(record, sizeof(record)));
    REQUIRE(load_u16(record, 0x00) == 115u);
    REQUIRE(load_u16(record, 0x02) == 0x0123u);
    for (i = 0; i < 10; i++) {
        REQUIRE(load_u16(record, 0x18 + i * 2) == 5000u + i * 1000u);
    }
    for (i = 0; i < 7; i++) {
        REQUIRE(load_s16(record, 0x3C + i * 2) == scales[i]);
    }
    /* Everything outside the three serialized multi-byte runs is byte-sized
     * control data (intensity table, idle sound ID, plane/hover base pitch) and
     * must keep both its value and its offset. */
    for (i = 0; i < RECORD_SIZE; i++) {
        if ((i < 0x04u) || (i >= 0x18u && i < 0x2Cu) ||
            (i >= 0x3Cu && i < 0x4Au)) {
            continue;
        }
        REQUIRE(record[i] == FILL_BYTE(i));
    }

    for (i = 0; i < RECORD_SIZE; i++) {
        record[i] = (uint8_t) (i ^ 0x5Au);
    }
    memcpy(before, record, sizeof(record));
    REQUIRE(!asset_swap_vehicle_sound(record, RECORD_SIZE - 1));
    REQUIRE(memcmp(record, before, sizeof(record)) == 0);
    /* Acceptance threshold pins RECORD_SIZE to the production stride. */
    REQUIRE(asset_swap_vehicle_sound(record, RECORD_SIZE));
    REQUIRE(!asset_swap_vehicle_sound(NULL, RECORD_SIZE));

    puts("vehicle audio contract tests passed");
    return EXIT_SUCCESS;
}
