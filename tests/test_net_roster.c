/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/net_roster.h"
#include "platform/net/net_roster_runtime.h"

typedef struct World { int32_t position[4]; uint32_t tick; } World;

static MdkrMatchManifestV1 manifest(void) {
    MdkrMatchManifestV1 value;
    memset(&value, 0, sizeof(value));
    value.match_epoch = 1u; value.protocol_version = 1u;
    memset(value.build_id, 1, sizeof(value.build_id));
    memset(value.gameplay_digest, 2, sizeof(value.gameplay_digest));
    for (unsigned slot = 0u; slot < 4u; slot++) value.slot_owner[slot] = 100u + slot;
    value.rng_seed = 3u; value.track_id = 1u;
    value.rom_revision = MDKR_ROM_US_11; value.cadence_hz = 30u;
    value.slot_count = 4u; value.rules = 1u; value.vehicle_mask = 1u;
    value.input_delay = 2u;
    return value;
}

static MdkrMatchLaunchDescriptorV1 launch_descriptor(
    const MdkrMatchManifestV1 *spec) {
    MdkrMatchLaunchDescriptorV1 launch;
    memset(&launch, 0, sizeof(launch));
    launch.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    launch.manifest = *spec;
    for (unsigned slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) {
        launch.selections[slot].selection_revision = slot + 1u;
        launch.selections[slot].character_id = (uint8_t)slot;
        launch.selections[slot].vehicle_id = 0u;
    }
    return launch;
}

static void authoritative_tick(World *world) {
    /* Deliberately canonical: no viewport or local-seat count is observable. */
    for (unsigned slot = 0u; slot < 4u; slot++) {
        world->position[slot] += (int32_t)(slot + 1u) * (int32_t)(world->tick + 1u);
    }
    world->tick++;
}

int main(void) {
    const uint8_t endpoint_a[] = {0u};
    const uint8_t endpoint_b[] = {1u};
    const uint8_t endpoint_c[] = {0u, 2u};
    MdkrMatchManifestV1 spec = manifest();
    MdkrNetRoster a, b, c, headless;
    World worlds[4] = {{{0}, 0u}, {{0}, 0u}, {{0}, 0u}, {{0}, 0u}};
    uint8_t mapped = MDKR_NET_ROSTER_UNMAPPED;

    assert(mdkr_net_roster_init(&a, &spec));
    assert(mdkr_net_roster_init(&b, &spec));
    assert(mdkr_net_roster_init(&c, &spec));
    assert(mdkr_net_roster_init(&headless, &spec));
    assert(mdkr_net_roster_configure_local(&a, endpoint_a, 1u));
    assert(mdkr_net_roster_configure_local(&b, endpoint_b, 1u));
    assert(mdkr_net_roster_configure_local(&c, endpoint_c, 2u));
    assert(mdkr_net_roster_configure_local(&headless, NULL, 0u));
    assert(mdkr_net_roster_set_viewports(&a, endpoint_a, 1u));
    assert(mdkr_net_roster_set_viewports(&b, endpoint_b, 1u));
    assert(mdkr_net_roster_set_viewports(&c, endpoint_c, 2u));
    assert(mdkr_net_roster_set_viewports(&headless, NULL, 0u));

    /* The launcher publishes a copy, never a borrowed mutable pointer. */
    assert(!mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_canonical_player_count(1u) == 1u);
    assert(mdkr_net_roster_runtime_install(&spec, &b));
    assert(mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_canonical_player_count(1u) == 4u);
    assert(mdkr_net_roster_runtime_viewport_count(3u) == 1u);
    assert(mdkr_net_roster_runtime_local_to_canonical(0u, &mapped) &&
           mapped == 1u);
    mapped = MDKR_NET_ROSTER_UNMAPPED;
    assert(mdkr_net_roster_runtime_viewport_to_canonical(0u, &mapped) &&
           mapped == 1u);
    assert(!mdkr_net_roster_runtime_local_to_canonical(1u, &mapped));
    assert(!mdkr_net_roster_runtime_viewport_to_canonical(1u, &mapped));
    mapped = MDKR_NET_ROSTER_UNMAPPED;
    assert(mdkr_net_roster_runtime_canonical_to_local(1u, &mapped) &&
           mapped == 0u);
    assert(!mdkr_net_roster_runtime_canonical_to_local(0u, &mapped));
    b.canonical_player_count = 2u;
    assert(mdkr_net_roster_runtime_get()->canonical_player_count == 4u);
    assert(mdkr_net_roster_runtime_manifest()->track_id == spec.track_id);
    assert(!mdkr_net_roster_runtime_install(&spec, &a));
    mdkr_net_roster_runtime_clear();
    assert(!mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_canonical_player_count(3u) == 3u);
    assert(mdkr_net_roster_runtime_viewport_count(2u) == 2u);

    /* Production launch additionally copy-owns deterministic selections. */
    b.canonical_player_count = 4u;
    {
        MdkrMatchLaunchDescriptorV1 launch = launch_descriptor(&spec);
        assert(mdkr_net_roster_runtime_install_launch(&launch, &b));
        assert(mdkr_net_roster_runtime_launch_descriptor() != NULL);
        assert(mdkr_net_roster_runtime_selection(0u)->character_id == 0u);
        assert(mdkr_net_roster_runtime_selection(3u)->character_id == 3u);
        assert(mdkr_net_roster_runtime_selection(4u) == NULL);
        launch.selections[0].character_id = 9u;
        assert(mdkr_net_roster_runtime_selection(0u)->character_id == 0u);
        assert(!mdkr_net_roster_runtime_install_launch(&launch, &a));
        mdkr_net_roster_runtime_clear();
        assert(mdkr_net_roster_runtime_launch_descriptor() == NULL);
        assert(mdkr_net_roster_runtime_selection(0u) == NULL);
    }

    /* Manifest and presentation roster are one atomic engine publication. */
    { MdkrMatchManifestV1 mismatched = spec;
      mismatched.slot_owner[0]++;
      assert(!mdkr_net_roster_runtime_install(&mismatched, &headless)); }

    /* A no-render verifier is a valid engine endpoint even though player
     * launch envelopes currently require at least one owned local seat. */
    assert(mdkr_net_roster_runtime_install(&spec, &headless));
    assert(mdkr_net_roster_runtime_get()->viewport_count == 0u);
    assert(mdkr_net_roster_runtime_viewport_count(4u) == 0u);
    mdkr_net_roster_runtime_clear();
    assert(mdkr_net_roster_runtime_manifest() == NULL);
    for (unsigned tick = 0u; tick < 1000u; tick++) {
        for (unsigned endpoint = 0u; endpoint < 4u; endpoint++) {
            authoritative_tick(&worlds[endpoint]);
        }
    }
    assert(memcmp(&worlds[0], &worlds[1], sizeof(World)) == 0);
    assert(memcmp(&worlds[0], &worlds[2], sizeof(World)) == 0);
    assert(memcmp(&worlds[0], &worlds[3], sizeof(World)) == 0);

    /* Broken-direction controls: duplicate/remote views and owner aliases fail. */
    assert(!mdkr_net_roster_set_viewports(&a, endpoint_c, 2u));
    { const uint8_t duplicate[] = {0u, 0u};
      assert(!mdkr_net_roster_configure_local(&a, duplicate, 2u)); }
    spec.slot_owner[3] = spec.slot_owner[0];
    assert(!mdkr_net_roster_init(&a, &spec));
    puts("test_net_roster: PASS");
    return 0;
}
