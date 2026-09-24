#ifndef _ASSET_LOADING_H_
#define _ASSET_LOADING_H_

#include "types.h"

#ifdef NATIVE_PORT
struct MdkrCustomTrackSession;
/* Borrowed session, prepared against this ROM's normalized retail tables.
 * Call only with the engine stopped, before asset consumers initialize (or
 * after all consumers unload). Detach before destroying the session. This is
 * not a live-reload API; the DMA mutex alone cannot invalidate cached IDs.
 * After pi_init only. Returns 1 on success; mismatched active-ROM tables or
 * read/allocation failure return 0 and preserve the installed session. NULL
 * detaches. Table matching is not authored-asset or ROM-content validation. */
int asset_custom_track_session_set(const struct MdkrCustomTrackSession *session);
#endif

void pi_init(void);
u32 *asset_table_load(u32 assetIndex);
s32 asset_load(u32 assetIndex, uintptr_t address, s32 assetOffset, s32 size);
u8 *asset_rom_offset(u32 assetIndex, u32 assetOffset);
s32 asset_table_size(u32 assetIndex);
s32 dmacopy(u32 romOffset, uintptr_t ramAddress, s32 numBytes);
s32 dmacopy_v1(u32 romOffset, uintptr_t ramAddress, s32 numBytes);

#endif
