/**
 * vi.h — shim for the vendored mgb64 gfx_opengl.c backend.
 *
 * gfx_opengl.c reads viGetX()/viGetY() (the N64 VI output resolution) to scale
 * N64-filter thresholds and detect special low-res strips. DKR renders at the
 * standard 320x240 VI grid, so the shim returns those constants.
 */
#ifndef GFX_SHIM_VI_H
#define GFX_SHIM_VI_H

#include <stdint.h>

int16_t viGetX(void);
int16_t viGetY(void);

#endif
