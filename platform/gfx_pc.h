/**
 * gfx_pc.h — shim for the vendored fast3d backends.
 *
 * The mgb64-derived backends (gfx_opengl.c / gfx_metal.mm) include this as
 * "../gfx_pc.h" and rely on it for `struct GfxDimensions` + the
 * `gfx_current_dimensions` global. In mdkr64 the DKR-specific front-end
 * gfx_pc_dkr.h already declares both (with mgb64's exact layout), so this shim
 * simply redirects there. Keeping the filename lets the backends stay verbatim.
 */
#ifndef GFX_PC_H
#define GFX_PC_H

#include "fast3d/gfx_pc_dkr.h"

#endif /* GFX_PC_H */
