#ifndef MDKR_BOUNDS_H
#define MDKR_BOUNDS_H

/*
 * Native-port bounds instrumentation shared by game code and the headless
 * reporter. Keep this declaration-only header libc-free: implicit declarations
 * change the wasm function ABI, while ordinary host headers can conflict with
 * libultra's libc declarations in game translation units.
 */
void mdkr_bound_probe(int slot, int count, int bound);
int mdkr_bound_slack(int slot);
int mdkr_bound_max(int slot);
int mdkr_bound_min(int slot);
long mdkr_bound_clamped(int slot);
int mdkr_seg_margin(void);
int mdkr_segbound_legacy(void);
int mdkr_shadow_cap(int kind, int fallback);
int mdkr_shadow_decal_enabled(void);
void mdkr_shadow_stats(
    int *dataPeak, int *triPeak, int *vtxPeak,
    int *overflowDrops, int *emptyMeshes,
    int *drawGroups, int *nonDecalDrawGroups,
    int *dataCap, int *triCap, int *vtxCap);

#endif
