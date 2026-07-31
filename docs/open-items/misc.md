# Open items — Everything else

> Part of the split [open-items index](README.md). Every defect this port has
> hit is recorded with its mechanism, the measurement that found it, the fix, and
> the check that would catch a regression. **Nothing is deleted when it is
> fixed** — a closed entry is the only warning the next person gets that the same
> trap exists, and retractions are recorded in place rather than removed.


## Integration pending
- [x] Wire `asset_swap_normalize` / `asset_swap_lut` hooks into game/src/asset_loading.c per contract in platform/asset_swap.h. (M2 wired the LUT + asset_table_load hooks; M3 wired the per-caller post-inflate/whole-record hooks: LEVEL_MODELS, OBJECT_MODELS + asset_swap_object_animation, OBJECTS, SPRITES, LEVEL_OBJECT_MAPS, LEVEL_HEADERS, and the ASSET_MISC LevelHeader_70 swap.) Gzip'd sections swap AFTER inflate via gzip_inflate_output sizing.
