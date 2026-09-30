#ifndef MDKR_CUSTOM_TRACK_HEADER_H
#define MDKR_CUSTOM_TRACK_HEADER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MDKR_TRACK_HEADER_BYTES 0xC4u
/* Still-BE scalar domains: retail/world-none or exporter custom world 6,
 * finite course height, defined race kinds, base selectable vehicle and its
 * three-bit availability mask, and the documented 0..90 camera FOV domain.
 * Does not prove AI, lap/checkpoint, boss, weather or campaign semantics.
 * No normalization or writes; world 6 is normalized by relocation later. */
int mdkr_track_header_scalars_valid(const void *header, size_t size);
/* Check the seven lighting and one pulse-light references before their native
 * index-to-pointer conversion. Only -1 is an absent-record sentinel. Other
 * IDs must address the fixed retail misc inventory and fit signed indexing.
 * Does not prove that a referenced heterogeneous misc blob has the right type. */
int mdkr_track_header_misc_valid(const void *header, size_t size, uint32_t retail_misc_count);
/* Only the special sky texture used when skyDome == -1. Match load_texture's
 * low-16-bit ID decoding: bit 15 selects 3D, otherwise 2D. Inventories are
 * fixed retail counts so an authored ID cannot bind to another custom pack.
 * Does not validate the separate skybox object or the texture's payload. */
int mdkr_track_header_sky_texture_valid(const void *header, size_t size,
    uint32_t retail_2d_count, uint32_t retail_3d_count);
/* Separate waveTexID load, with the same 15-bit index/bank convention.
 * Caller invokes only when the associated model activates wave setup.
 * References the fixed retail inventory; does not validate texture payloads. */
int mdkr_track_header_wave_texture_valid(const void *header, size_t size,
    uint32_t retail_2d_count, uint32_t retail_3d_count);
/* Requested bytes for the six allocations in waves_alloc, when waves are
 * actually active. Two-player mode forces four subdivisions and four buffers;
 * one-player mode uses the authored subdivision and two buffers. Reject zero
 * divisors/invalid signed seed lengths and subdivision indices outside the
 * native 128-element weight table. No full wave geometry/index admission:
 * excludes allocator overhead, textures, wave model/grid and generators. */
int mdkr_track_header_wave_storage(const void *header, size_t size, unsigned players,
    size_t limit, size_t *required);
/* Active-wave command encoding: each strip loads 2*(subdivisions+1)
 * vertices into a 32-slot cache and emits 2*subdivisions triangles in one
 * four-bit-count G_TRIN command (maximum 16). Thus 1P needs 1..8 subdivisions;
 * 2P forces four. Double density repeats strips, not their command counts.
 * Separate from allocation sizing and not a full frame-command budget. */
int mdkr_track_header_wave_render_valid(const void *header, size_t size, unsigned players);
/* Still-BE misc payload admission: require complete frame arrays, positive
 * animated-frame durations and a sum fitting the native accumulator. Pulse
 * data needs at least one frame because initialization always reads frame 0.
 * Colour data may be static (zero frames); either one-frame format may have
 * zero duration because its update loop is skipped. No normalization occurs.
 * These do not establish every runtime interpolation/arithmetic constraint. */
int mdkr_track_colour_cycle_valid(const void *bytes, size_t size);
int mdkr_track_pulse_cycle_valid(const void *bytes, size_t size);
typedef struct MdkrTrackHeaderRelocation {
    /* -1 retains the authored retail reference. Other negative IDs fail.
     * Counts describe the final session inventories, not only appended assets. */
    int32_t model, collectables, structure;
    uint32_t model_count, object_map_count;
    /* Retained authored references may address only the original ROM prefix,
     * never accidentally resolve to another installed course's appended IDs. */
    uint32_t retail_model_count, retail_object_map_count;
} MdkrTrackHeaderRelocation;
/* Operates on still-big-endian header bytes, before asset_swap_normalize.
 * Requires a complete native LevelHeader. Checks all three final signed-16-bit
 * IDs against their inventories, then applies changes atomically in place.
 * Failure preserves every byte. Trailing exporter bytes are preserved.
 * World category 6 becomes gameplay world 1; retain catalogue metadata before
 * calling. Other worlds are unchanged and still require semantic validation.
 * This is relocation, NOT full header/payload admission: misc/texture/skybox,
 * gameplay, wave and geometry constraints must be checked before publication. */
int mdkr_track_header_relocate(void *header, size_t size,
                             const MdkrTrackHeaderRelocation *relocation);
#ifdef __cplusplus
}
#endif
#endif
