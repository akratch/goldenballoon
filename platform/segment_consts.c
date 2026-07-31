/**
 * segment_consts.c — native definitions for symbols that are LINKER-generated on
 * the N64 (ROM/BSS bounds, RSP microcode blobs) plus a few libultra data tables
 * that the HLE renderer/VI shim never actually read.
 *
 * us.v80 ROM offsets come from docs/ref/dkr.us.v80.ld + the reference build map
 * (build/dkr.us.v80.map):
 *     __ASSETS_LUT_START = 0x000ED0E0   (patched numerically in asset_loading.c)
 *     __ASSETS_LUT_END   = 0x000ED1B0
 *     __ROM_END          = 0x00B8CFD0
 *
 * ===========================================================================
 *  THESE NUMBERS ARE PER-REVISION
 * ===========================================================================
 * On the N64 they are LINKER-generated, so each release has its own. From the
 * decompilation's per-version splat configs (ver/splat/dkr.*.yaml), all five
 * released revisions differ in all three:
 *
 *     build     assets.lut   assets base   ROM end
 *     us.v77     0x0ECB60     0x0ECC30     0xAC9630
 *     pal.v77    0x0ECBF0     0x0ECCC0     0xAC96C0
 *     jpn.v79    0x0EE5D0     0x0EE6A0     0xB8E4E0
 *     us.v80     0x0ED0E0     0x0ED1B0     0xB8CFD0   <-- the ROM_END below
 *     pal.v80    0x0ED170     0x0ED240     0xB8D060
 *
 * The asset-LUT pair is NO LONGER hardcoded: asset_loading.c holds it in
 * gDkrAssetsLutStart/gDkrAssetsLutEnd (defaulted to us.v80) and platform/rom_io.c
 * re-points it from the revision platform/rom_id.c identifies. Before that,
 * reading the LUT from the us.v80 offset SIGSEGV'd four of the five carts before
 * the first rendered frame — measured, per revision, in docs/ROM_REVISIONS.md.
 *
 * `gDkrRomEnd` is an explicit numeric ROM offset. It defaults to us.v80's
 * extent and platform/rom_io.c updates it from the same revision descriptor as
 * the LUT before game code can consume it.
 */
#include <ultra64.h>
#include "address_domains.h"
#include "memory.h"

/* ---- Memory / ROM bound symbols (referenced by game code) --------------- *
 * On N64 these are placed by the linker at specific VRAM/ROM addresses; here
 * they are ordinary globals. The values are irrelevant on native — the pool is
 * a malloc'd arena (see memory.c NATIVE_PORT path) and the ROM-end symbol only
 * feeds the (disabled) cheat-menu checksum. */
MemoryPoolSlot gMainMemoryPool;   /* was main_BSS_END on N64 */
u8 *main_BSS_START[1];
MdkrRomOffset gDkrRomEnd = 0x00B8CFD0u;

/* ---- RSP microcode blobs ------------------------------------------------ *
 * The F3DDKR HLE interprets display lists directly, so the actual microcode is
 * never executed. These only have to exist and be distinct non-NULL symbols so
 * the gfx-task setup in rcp_dkr.c can assign them into OSTask fields. */
long long int rspbootTextStart[1], rspbootTextEnd[1];
long long int rspF3DDKRDramStart[1],     rspF3DDKRDramEnd[1];
long long int rspF3DDKRXbusStart[1],     rspF3DDKRXbusEnd[1];
long long int rspF3DDKRDataXbusStart[1], rspF3DDKRDataXbusEnd[1];
long long int rspF3DDKRFifoStart[1],     rspF3DDKRFifoEnd[1];
long long int rspF3DDKRDataFifoStart[1], rspF3DDKRDataFifoEnd[1];

/* aspMain N-Audio microcode (audiomgr.c assigns it into the audio OSTask). The
 * synth's Acmd output is HLE'd by platform/mixer.c later; the blob is never run. */
long long int aspMainTextStart[1], aspMainTextEnd[1];
long long int aspMainDataStart[1], aspMainDataEnd[1];

/* ---- VI mode tables (data the no-op osViSetMode ignores) ---------------- */
OSViMode osViModeTable[42];
OSViMode osViModeNtscLpn1, osViModeNtscLan1;
OSViMode osViModePalLpn1,  osViModePalLan1;
OSViMode osViModeMpalLpn1, osViModeMpalLan1;

/* Anti-piracy sentinel (camera.c reads D_B0000578 & 0xFFFF == 0x8965). On the
 * N64 this is a magic cart address; seed the expected value so the check that
 * would otherwise flag piracy stays satisfied. */
s32 D_B0000578 = 0x8965;
