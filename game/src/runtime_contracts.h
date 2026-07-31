#ifndef _RUNTIME_CONTRACTS_H_
#define _RUNTIME_CONTRACTS_H_

#include "types.h"
#include <stddef.h>

#define MDKR_SOUND_GROUP_COUNT 5
#define MDKR_VEHICLE_SOUND_ROW_COUNT 30

s32 mdkr_audio_group_valid(s32 groupId);
s32 mdkr_sound_id_valid(u32 soundId, u32 soundCount);
s32 mdkr_vehicle_sound_row(s32 characterId, s32 vehicleId, s32 *assetRow, s32 *soundVehicleId);
s32 mdkr_asset_rows_fit(s32 startOffset, s32 endOffset, s32 rowCount, s32 rowSize);
s32 mdkr_normalize_xz(f32 x, f32 z, f32 *normalizedX, f32 *normalizedZ);
s32 mdkr_model_index_resolve(s32 requested, s32 modelCount, s32 *resolved);
s32 mdkr_model_load_selection(s32 requested, s32 modelCount, s32 *resolved,
                              s32 *loadCount);
s32 mdkr_course_flag(s32 index, u32 *flag);
s32 mdkr_trophy_state(u32 trophies, s32 worldId, u32 *state);
s32 mdkr_extension_bit(char extension, u32 *bit);
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
