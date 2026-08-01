#ifndef GFX_WEBGPU_CALLBACK_LATCH_H
#define GFX_WEBGPU_CALLBACK_LATCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device callbacks may run on a WebGPU implementation thread.  They publish
 * only this small immutable record; the renderer thread remains the sole owner
 * of GPU objects and mutable renderer state.
 */
enum GfxWebgpuCallbackEvent {
    GFX_WEBGPU_CALLBACK_NONE = 0,
    GFX_WEBGPU_CALLBACK_DEVICE_ERROR = 1,
    GFX_WEBGPU_CALLBACK_DEVICE_LOST = 2,
};

/* Reserve a callback slot before requesting a device. A provisional device may
 * report an error (or even be lost) before its request callback returns; that
 * event remains attached to the candidate without displacing the live device.
 * At most one live and one provisional generation are supported. */
bool gfx_webgpu_callback_latch_begin(uintptr_t generation);

/* Promote a reserved generation after the complete device transaction
 * succeeds. The former active generation is retired only at this boundary. */
bool gfx_webgpu_callback_latch_commit(uintptr_t generation);

/* Retire either a failed provisional generation or the active generation being
 * destroyed. Retiring a provisional generation leaves the live one intact. */
void gfx_webgpu_callback_latch_retire(uintptr_t generation);
uintptr_t gfx_webgpu_callback_latch_active_generation(void);

/* Returns true only for the first event published by a reserved generation. */
bool gfx_webgpu_callback_latch_publish(
    uintptr_t generation, enum GfxWebgpuCallbackEvent event);

/* Consumes a matching event exactly once on the renderer thread. */
bool gfx_webgpu_callback_latch_consume(
    uintptr_t generation, enum GfxWebgpuCallbackEvent *event);

#ifdef __cplusplus
}
#endif

#endif
