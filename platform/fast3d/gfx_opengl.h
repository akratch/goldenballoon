/**
 * gfx_opengl.h — OpenGL rendering backend.
 */
#ifndef GFX_OPENGL_H
#define GFX_OPENGL_H

#include <stdbool.h>
#include <stdint.h>
#include "gfx_rendering_api.h"

extern struct GfxRenderingAPI gfx_opengl_api;

/* Consecutive diagnostic dumps read the last completed pre-swap frame. */
bool gfx_opengl_copy_captured_frame(
    int width, int height, uint8_t *rgb_out);

#endif
