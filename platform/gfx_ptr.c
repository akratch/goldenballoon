/**
 * gfx_ptr.c — single definition of the GBI pointer/segment registry globals.
 *
 * platform/gfx_ptr.h declares these `extern` and provides the (static inline)
 * store/resolve logic. Exactly one TU must DEFINE the backing storage; per
 * PLAN.md decision 3 the OS-shim side owns the store. They are zero-initialised,
 * which is the correct empty-table boot state (see gfx_ptr.h gfx_ptr_clear()).
 *
 * In mdkr64 the primary DL-address scheme is arena-low-32 reconstruction
 * (dkr_lo32_to_ptr, see gfx_pc_dkr.c dkr_resolve()); this registry backs the
 * segment table (gSPSegment / G_MW_SEGMENT sets) and any non-arena host pointer
 * that still round-trips through gfx_ptr_store()/gfx_resolve_addr().
 */
#include "gfx_ptr.h"

uintptr_t gfx_segment_table[16];

uint32_t  gfx_ptr_keys[GFX_PTR_TABLE_SIZE];
uintptr_t gfx_ptr_vals[GFX_PTR_TABLE_SIZE];
uint8_t   gfx_ptr_state[GFX_PTR_TABLE_SIZE];
uint32_t  gfx_ptr_epoch[GFX_PTR_TABLE_SIZE];
uint32_t  gfx_ptr_epoch_current = 1;

uint32_t  gfx_ptr_ambiguous;
uint32_t  gfx_ptr_full_fails;
uint32_t  gfx_ptr_max_probe;
uint32_t  gfx_ptr_live;
uint32_t  gfx_ptr_high_water;

GFX_PTR_TLS int gfx_ptr_store_suppress;
