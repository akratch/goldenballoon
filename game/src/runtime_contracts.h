#ifndef _RUNTIME_CONTRACTS_H_
#define _RUNTIME_CONTRACTS_H_

#include "types.h"
#include <stddef.h>

#define MDKR_SOUND_GROUP_COUNT 5
#define MDKR_TEXTURE_COMMAND_BYTES (12u * 8u)
#define MDKR_VEHICLE_SOUND_ROW_COUNT 30
#define MDKR_AI_NODE_COUNT 128
/* Longest link between two nodes at signed-16 map coordinates:
 * ceil(sqrt(3) * 65535) = 113510. */
#define MDKR_AI_MAP_LINK_MAX 113510
/* Pop budget of the challenge-arena route search (func_8001CD28). The
 * original search keeps no closed set: a node that left the frontier is added
 * again whenever a neighbour is expanded, which is how it finds a target on
 * the start node and turns back through the node the racer came from. So a
 * search whose target is absent from a cyclic graph never empties its
 * frontier; the N64 spins until the s32 cost wraps. The budget lets each of
 * the 128 node slots be expanded 128 times. Each pop adds at most one link
 * to a path, so after the budget no cost on a map-coordinate graph exceeds
 * 16384 * 113510 = 1,859,747,840 < INT32_MAX (runtime_contracts.c asserts
 * it). No retail search comes near it: run exhaustively over every start,
 * previous node and node/tag target on the three arenas with AI nodes
 * (Smokey Castle, Darkwater Beach, Icicle Pyramid; US and PAL), the original
 * search always finishes, in at most 234 pops. A search that reaches the
 * budget ends the way the original ends one whose frontier empties: with the
 * first hop of the node it popped last. */
#define MDKR_AI_ROUTE_POP_BUDGET (MDKR_AI_NODE_COUNT * MDKR_AI_NODE_COUNT)
/* First object for an explicit node ID wins. Check before appending to the
 * fixed-size rebuild arrays; occupied must describe that ID's current slot. */
s32 mdkr_ainode_can_append(s32 nodeId, s32 count, s32 occupied);
/* Truncated Euclidean cost, preserving the original float calculation for
 * ordinary links. -1 rejects nonfinite/over-budget distances. The per-edge
 * bound leaves signed-32 headroom for an entire 128-node simple route. */
s32 mdkr_ainode_distance_cost(f32 dx, f32 dy, f32 dz);

s32 mdkr_audio_group_valid(s32 groupId);
s32 mdkr_sound_id_valid(u32 soundId, u32 soundCount);
s32 mdkr_vehicle_sound_model(s32 vehicleId);
s32 mdkr_vehicle_sound_row(s32 characterId, s32 vehicleId, s32 *assetRow, s32 *soundVehicleId);
s32 mdkr_asset_rows_fit(s32 startOffset, s32 endOffset, s32 rowCount, s32 rowSize);
s32 mdkr_normalize_xz(f32 x, f32 z, f32 *normalizedX, f32 *normalizedZ);
s32 mdkr_model_index_resolve(s32 requested, s32 modelCount, s32 *resolved);
/* Preserve the retail high-ID fallback to map zero; negative IDs and an
 * empty inventory are invalid, not optional-map sentinels. */
s32 mdkr_object_map_index_resolve(s32 requested, s32 mapCount, s32 *resolved);
/* Five retail background rows; unsupported worlds use Dino Domain's row.
 * Presentation fallback only: does not change a course's world identity. */
s32 mdkr_postrace_background_index(s32 worldId);
/* Preserve the weather consumer's high-type fallback, reject negative types
 * or an invalid final preset index. Returns -1 when no access is permitted. */
s32 mdkr_weather_preset_index(s32 requested, s32 lastPreset);
/* Bounded nonnegative fixed-point fraction, with widened multiplication.
 * Invalid/nonpositive time yields zero; elapsed beyond duration saturates.
 * This does not advance or validate the owning cycle's frame state. */
u32 mdkr_light_fraction(u32 scale, s32 elapsed, s32 duration);
/* Forward cycle advance before narrowing to native frame-time storage.
 * Returns -1 for negative time/delta or a nonpositive cycle duration. */
s32 mdkr_light_cycle_time(s32 elapsed, s32 delta, s32 duration);
/* Select one of four NUL-terminated localized names within one asset entry.
 * Includes the selected string's terminator in the bounds check. */
const char *mdkr_level_name_bounded(const char *bytes, size_t size, unsigned languageIndex);
s32 mdkr_model_load_selection(s32 requested, s32 modelCount, s32 *resolved,
                              s32 *loadCount);
s32 mdkr_course_flag(s32 index, u32 *flag);
/* Read any of the five championship fields; reject non-championship worlds. */
s32 mdkr_trophy_state(u32 trophies, s32 worldId, u32 *state);
/* Furthest medal per world, not bitwise union of encoded medal ranks. */
u32 mdkr_trophy_records_merge(u32 left, u32 right);
s32 mdkr_extension_bit(char extension, u32 *bit);
typedef struct MdkrPulseTextureLayout {
    unsigned width, halfHeight, maskS, maskT;
    size_t halfBytes;
} MdkrPulseTextureLayout;
/* Two contiguous RGBA16 images. Rows must fit whole 64-bit TMEM words;
 * both halves together fit 4 KiB. Output unchanged on invalid dimensions. */
s32 mdkr_texture_pulse_layout(unsigned width, unsigned height, unsigned format,
    MdkrPulseTextureLayout *out);
s32 mdkr_texture_allocation_size(size_t dataBytes, size_t frameCount,
                                 size_t textureCommandBytes,
                                 size_t paletteCommandBytes,
                                 size_t *allocationBytes);
s32 mdkr_texture_frame_advance(size_t decodedBytes, size_t cursorOffset,
                               size_t headerBytes, size_t frameBytes,
                               s32 finalFrame, size_t *nextOffset);
s32 mdkr_palette_reservation(size_t usedBytes, size_t paletteBytes,
                             size_t capacityBytes, size_t *paletteOffset);
s32 mdkr_audio_fx_span(const u32 *table, size_t tableBytes,
                       size_t audioSectionBytes, s32 *offset, s32 *size);
s32 mdkr_mips_trunc_w_d(f64 value);
s32 mdkr_mips_round_w_s(f32 value);
u32 mdkr_mips_atan_index(u32 numerator, u32 denominator);
s16 mdkr_mips_f64_to_s16(f64 value);
u16 mdkr_mips_f64_to_u16(f64 value);
f32 mdkr_audio_rate_value(s16 rateMajor, u16 rateMinor);

#endif
