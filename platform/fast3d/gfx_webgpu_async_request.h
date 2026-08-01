#ifndef GFX_WEBGPU_ASYNC_REQUEST_H
#define GFX_WEBGPU_ASYNC_REQUEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lifetime-safe landing site for WebGPU adapter/device request callbacks.
 *
 * A bounded bring-up wait may return before WebGPU delivers its callback. Each
 * attempt therefore owns a distinct heap context with one caller reference and
 * one callback reference. Abandoning an attempt transfers any eventual handle
 * to the callback for release; a completed attempt transfers it exactly once to
 * the caller. No context or result slot is ever reused by a retry or recovery.
 */
typedef struct GfxWebgpuAsyncRequest GfxWebgpuAsyncRequest;
typedef void (*GfxWebgpuAsyncHandleRelease)(void *handle);

GfxWebgpuAsyncRequest *gfx_webgpu_async_request_create(void);

/* Add the callback's lifetime reference immediately before submitting the
 * asynchronous request. Every successful retain must be paired with one call
 * to gfx_webgpu_async_request_complete(), including injected completions. */
bool gfx_webgpu_async_request_retain_callback(
    GfxWebgpuAsyncRequest *request);

/* Callback-thread completion. If the caller already abandoned the attempt, a
 * non-null handle is released here. */
void gfx_webgpu_async_request_complete(
    GfxWebgpuAsyncRequest *request, int status, void *handle,
    GfxWebgpuAsyncHandleRelease release_handle);

bool gfx_webgpu_async_request_completed(
    const GfxWebgpuAsyncRequest *request);

/* Finish the caller side. Returns true only when a callback result was claimed;
 * otherwise marks the still-pending attempt abandoned. On true, the caller owns
 * the returned handle, even when status denotes failure. */
bool gfx_webgpu_async_request_finish(
    GfxWebgpuAsyncRequest *request, int *status, void **handle);

/* Drop the caller reference after finish. */
void gfx_webgpu_async_request_release(GfxWebgpuAsyncRequest *request);

#ifdef __cplusplus
}
#endif

#endif
