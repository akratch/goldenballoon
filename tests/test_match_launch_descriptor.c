#include "platform/online/match_launch_builder.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static MdkrOnlineCompatibilityV1 compatibility(void) {
    MdkrOnlineCompatibilityV1 value = {0};
    unsigned index;
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (index = 0u; index < sizeof(value.build_id); index++)
        value.build_id[index] = (uint8_t)(index + 1u);
    for (index = 0u; index < sizeof(value.gameplay_digest); index++)
        value.gameplay_digest[index] = (uint8_t)(0xa0u + index);
    value.rom_revision = MDKR_ROM_US_11;
    value.cadence_hz = 30u;
    return value;
}

static MdkrOnlineCommand command(
    const MdkrOnlineLobby *lobby, uint64_t actor, uint64_t command_id,
    MdkrOnlineCommandType type, uint64_t target, uint32_t selected) {
    MdkrOnlineCommand value = {0};
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    value.expected_revision = lobby->revision;
    value.command_id = command_id;
    value.actor_endpoint_id = actor;
    value.type = type;
    value.target_endpoint_id = target;
    value.value = selected;
    return value;
}

static int dispatch(
    MdkrOnlineLobby *lobby, uint64_t actor, uint64_t command_id,
    MdkrOnlineCommandType type, uint64_t target, uint32_t selected) {
    MdkrOnlineCommand value = command(
        lobby, actor, command_id, type, target, selected);
    return mdkr_online_lobby_dispatch(lobby, &value).accepted;
}

static int loading_lobby(
    MdkrOnlineLobby *lobby, MdkrOnlineCompatibilityV1 *compatibility_out) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineCommand join;
    if (!mdkr_online_lobby_init(lobby, 0x1234u, 10u, &compat, 1u)) return 0;
    join = command(lobby, 20u, 1u, MDKR_ONLINE_JOIN, 0u, 1u);
    join.compatibility = compat;
    if (!mdkr_online_lobby_dispatch(lobby, &join).accepted ||
        !dispatch(lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 9u) ||
        !dispatch(lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 1u) ||
        !dispatch(lobby, 10u, 3u, MDKR_ONLINE_SET_VOTE, 0u, 7u) ||
        !dispatch(lobby, 10u, 4u, MDKR_ONLINE_SET_READY, 0u, 1u) ||
        !dispatch(lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 7u) ||
        !dispatch(lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 2u) ||
        !dispatch(lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE, 1u, 7u) ||
        !dispatch(lobby, 20u, 5u, MDKR_ONLINE_SET_READY, 0u, 1u) ||
        !dispatch(lobby, 10u, 5u, MDKR_ONLINE_BEGIN_LOADING, 0u, 0x06u))
        return 0;
    *compatibility_out = compat;
    return 1;
}

static MdkrMatchManifestV1 manifest_for(
    const MdkrOnlineLobby *lobby,
    const MdkrOnlineCompatibilityV1 *compatibility) {
    MdkrMatchManifestV1 manifest = {0};
    manifest.match_epoch = lobby->match_epoch;
    manifest.protocol_version = 1u;
    memcpy(manifest.build_id, compatibility->build_id, sizeof(manifest.build_id));
    memcpy(manifest.gameplay_digest, compatibility->gameplay_digest,
           sizeof(manifest.gameplay_digest));
    manifest.slot_owner[0] = 101u;
    manifest.slot_owner[1] = 202u;
    manifest.rng_seed = UINT64_C(0x1122334455667788);
    manifest.track_id = lobby->selected_track;
    manifest.rom_revision = compatibility->rom_revision;
    manifest.cadence_hz = compatibility->cadence_hz;
    manifest.slot_count = lobby->seat_count;
    manifest.rules = MDKR_MATCH_RULES_STANDARD_RACE;
    manifest.vehicle_mask = lobby->selected_vehicle_mask;
    manifest.input_delay = 2u;
    return manifest;
}

int main(void) {
    MdkrOnlineLobby lobby;
    MdkrOnlineCompatibilityV1 compat;
    MdkrMatchManifestV1 manifest;
    MdkrMatchLaunchDescriptorV1 descriptor;
    MdkrMatchLaunchDescriptorV1 decoded;
    uint8_t encoded[MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES];
    uint8_t mutated[MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES];
    unsigned byte;

    expect(loading_lobby(&lobby, &compat),
           "launcher reducer reaches Loading with frozen selections");
    manifest = manifest_for(&lobby, &compat);
    expect(mdkr_match_launch_descriptor_from_lobby(
               &lobby, &manifest, &descriptor),
           "loading lobby freezes one canonical launch descriptor");
    expect(descriptor.selections[0].character_id == 9u &&
           descriptor.selections[0].vehicle_id == 1u &&
           descriptor.selections[1].character_id == 7u &&
           descriptor.selections[1].vehicle_id == 2u &&
           descriptor.selections[2].character_id == MDKR_ONLINE_NO_CHARACTER,
           "descriptor preserves canonical seat order and neutral tail");
    expect(mdkr_match_launch_descriptor_encode(
               &descriptor, encoded, sizeof(encoded)) &&
           mdkr_match_launch_descriptor_decode(
               encoded, sizeof(encoded), &decoded) &&
           mdkr_match_launch_descriptor_digest(&descriptor) != 0u &&
           mdkr_match_launch_descriptor_digest(&descriptor) ==
               mdkr_match_launch_descriptor_digest(&decoded),
           "fixed bytes round-trip to one stable digest");

    for (byte = 0u; byte < sizeof(encoded); byte++) {
        memcpy(mutated, encoded, sizeof(mutated));
        mutated[byte] ^= 1u;
        if (mdkr_match_launch_descriptor_decode(
                mutated, sizeof(mutated), &decoded)) {
            fprintf(stderr, "FAIL: mutated byte %u survived checksum\n", byte);
            failures++;
            break;
        }
    }

    decoded = descriptor;
    decoded.selections[1].character_id = decoded.selections[0].character_id;
    expect(!mdkr_match_launch_descriptor_validate(&decoded),
           "duplicate character fails validation");
    decoded = descriptor;
    decoded.selections[0].vehicle_id = 0u;
    expect(!mdkr_match_launch_descriptor_validate(&decoded),
           "vehicle outside frozen track mask fails validation");
    decoded = descriptor;
    decoded.selections[2].selection_revision = 1u;
    expect(!mdkr_match_launch_descriptor_validate(&decoded),
           "unused canonical slot must remain neutral");
    decoded = descriptor;
    decoded.manifest.track_id++;
    expect(!mdkr_match_launch_descriptor_from_lobby(
               &lobby, &decoded.manifest, &decoded),
           "manifest/lobby track mismatch fails before output mutation");
    lobby.phase = MDKR_ONLINE_RACING;
    expect(!mdkr_match_launch_descriptor_from_lobby(
               &lobby, &manifest, &decoded),
           "descriptor can freeze only at the Loading barrier");

    if (failures != 0) return 1;
    puts("match launch descriptor contract passed");
    return 0;
}
