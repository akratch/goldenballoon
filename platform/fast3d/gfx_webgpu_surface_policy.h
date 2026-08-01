#ifndef GFX_WEBGPU_SURFACE_POLICY_H
#define GFX_WEBGPU_SURFACE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Pure surface-capability selection shared by production configuration and the
 * generation-recovery unit test. Values are uint32_t so this helper remains
 * independent of a particular WebGPU header dialect; the caller derives the
 * advertised booleans from the current generation's capability query. */
uint32_t gfx_webgpu_surface_select_alpha(
    uint32_t automatic_mode,
    uint32_t opaque_mode,
    bool opaque_advertised);

uint32_t gfx_webgpu_surface_select_present(
    uint32_t fifo_mode,
    uint32_t requested_mode,
    bool requested_advertised);

#endif
