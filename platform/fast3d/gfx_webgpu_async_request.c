#include "gfx_webgpu_async_request.h"

#include <stdatomic.h>
#include <stdlib.h>

enum GfxWebgpuAsyncState {
    GFX_WEBGPU_ASYNC_PENDING = 0,
    GFX_WEBGPU_ASYNC_READY,
    GFX_WEBGPU_ASYNC_CLAIMED,
    GFX_WEBGPU_ASYNC_ABANDONED,
};

struct GfxWebgpuAsyncRequest {
    atomic_uint references;
    atomic_int state;
    int status;
    void *handle;
};

static void release_reference(GfxWebgpuAsyncRequest *request) {
    if (request != NULL && atomic_fetch_sub_explicit(
            &request->references, 1u, memory_order_acq_rel) == 1u) {
        free(request);
    }
}

GfxWebgpuAsyncRequest *gfx_webgpu_async_request_create(void) {
    GfxWebgpuAsyncRequest *request =
        (GfxWebgpuAsyncRequest *)calloc(1, sizeof(*request));
    if (request == NULL) return NULL;
    atomic_init(&request->references, 1u);
    atomic_init(&request->state, GFX_WEBGPU_ASYNC_PENDING);
    return request;
}

bool gfx_webgpu_async_request_retain_callback(
        GfxWebgpuAsyncRequest *request) {
    if (request == NULL || atomic_load_explicit(
            &request->state, memory_order_acquire) !=
            GFX_WEBGPU_ASYNC_PENDING) {
        return false;
    }
    atomic_fetch_add_explicit(
        &request->references, 1u, memory_order_relaxed);
    return true;
}

void gfx_webgpu_async_request_complete(
        GfxWebgpuAsyncRequest *request, int status, void *handle,
        GfxWebgpuAsyncHandleRelease release_handle) {
    if (request == NULL) {
        if (handle != NULL && release_handle != NULL) release_handle(handle);
        return;
    }

    request->status = status;
    request->handle = handle;
    int pending = GFX_WEBGPU_ASYNC_PENDING;
    if (!atomic_compare_exchange_strong_explicit(
            &request->state, &pending, GFX_WEBGPU_ASYNC_READY,
            memory_order_release, memory_order_acquire)) {
        if (handle != NULL && release_handle != NULL) release_handle(handle);
    }
    release_reference(request);
}

bool gfx_webgpu_async_request_completed(
        const GfxWebgpuAsyncRequest *request) {
    return request != NULL && atomic_load_explicit(
        &request->state, memory_order_acquire) != GFX_WEBGPU_ASYNC_PENDING;
}

bool gfx_webgpu_async_request_finish(
        GfxWebgpuAsyncRequest *request, int *status, void **handle) {
    if (request == NULL) return false;

    int state = atomic_load_explicit(&request->state, memory_order_acquire);
    for (;;) {
        if (state == GFX_WEBGPU_ASYNC_READY) {
            if (atomic_compare_exchange_weak_explicit(
                    &request->state, &state, GFX_WEBGPU_ASYNC_CLAIMED,
                    memory_order_acq_rel, memory_order_acquire)) {
                if (status != NULL) *status = request->status;
                if (handle != NULL) *handle = request->handle;
                return true;
            }
            continue;
        }
        if (state == GFX_WEBGPU_ASYNC_PENDING) {
            if (atomic_compare_exchange_weak_explicit(
                    &request->state, &state, GFX_WEBGPU_ASYNC_ABANDONED,
                    memory_order_acq_rel, memory_order_acquire)) {
                return false;
            }
            continue;
        }
        return false;
    }
}

void gfx_webgpu_async_request_release(GfxWebgpuAsyncRequest *request) {
    release_reference(request);
}
