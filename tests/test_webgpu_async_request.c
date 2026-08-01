#include "gfx_webgpu_async_request.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

static int s_failures;
static atomic_int s_releases;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++s_failures;
    }
}

static void release_handle(void *handle) {
    if (handle != NULL) {
        atomic_fetch_add_explicit(&s_releases, 1, memory_order_relaxed);
    }
}

typedef struct DelayedCompletion {
    GfxWebgpuAsyncRequest *request;
    atomic_int *start;
    int status;
    void *handle;
} DelayedCompletion;

static void complete_delayed(void *opaque) {
    DelayedCompletion *completion = (DelayedCompletion *)opaque;
    while (!atomic_load_explicit(completion->start, memory_order_acquire)) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    gfx_webgpu_async_request_complete(
        completion->request, completion->status, completion->handle,
        release_handle);
}

#ifdef _WIN32
typedef HANDLE CompletionThread;
static DWORD WINAPI completion_thread(LPVOID opaque) {
    complete_delayed(opaque);
    return 0;
}
static bool start_thread(CompletionThread *thread, DelayedCompletion *context) {
    *thread = CreateThread(NULL, 0, completion_thread, context, 0, NULL);
    return *thread != NULL;
}
static void join_thread(CompletionThread thread) {
    if (thread != NULL) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}
#else
typedef struct CompletionThread {
    pthread_t handle;
    bool created;
} CompletionThread;
static void *completion_thread(void *opaque) {
    complete_delayed(opaque);
    return NULL;
}
static bool start_thread(CompletionThread *thread, DelayedCompletion *context) {
    thread->created = pthread_create(
        &thread->handle, NULL, completion_thread, context) == 0;
    return thread->created;
}
static void join_thread(CompletionThread thread) {
    if (thread.created) pthread_join(thread.handle, NULL);
}
#endif

int main(void) {
    atomic_int first_start = 0;
    atomic_int second_start = 0;
    GfxWebgpuAsyncRequest *first = gfx_webgpu_async_request_create();
    GfxWebgpuAsyncRequest *second = gfx_webgpu_async_request_create();
    expect(first != NULL && second != NULL, "distinct request contexts allocate");
    expect(gfx_webgpu_async_request_retain_callback(first),
           "timed-out attempt retains its callback lifetime");
    expect(gfx_webgpu_async_request_retain_callback(second),
           "retry retains an independent callback lifetime");

    DelayedCompletion first_completion = {
        first, &first_start, 0, (void *)(uintptr_t)0x11u
    };
    DelayedCompletion second_completion = {
        second, &second_start, 0, (void *)(uintptr_t)0x22u
    };
    CompletionThread first_thread = {0};
    CompletionThread second_thread = {0};
    expect(start_thread(&first_thread, &first_completion),
           "late first callback thread starts");
    expect(start_thread(&second_thread, &second_completion),
           "retry callback thread starts");

    int status = -1;
    void *handle = NULL;
    expect(!gfx_webgpu_async_request_finish(first, &status, &handle),
           "first attempt is abandoned before its delayed callback");
    gfx_webgpu_async_request_release(first);

    atomic_store_explicit(&second_start, 1, memory_order_release);
    join_thread(second_thread);
    expect(gfx_webgpu_async_request_completed(second),
           "retry completes independently");
    expect(gfx_webgpu_async_request_finish(second, &status, &handle) &&
               status == 0 && handle == (void *)(uintptr_t)0x22u,
           "retry claims only its own result");
    gfx_webgpu_async_request_release(second);
    expect(atomic_load_explicit(&s_releases, memory_order_relaxed) == 0,
           "claimed retry handle remains caller-owned");

    /* Deliver the timed-out success after the retry has already completed.
     * Its handle must be reaped, never written into the retry's record. */
    atomic_store_explicit(&first_start, 1, memory_order_release);
    join_thread(first_thread);
    expect(atomic_load_explicit(&s_releases, memory_order_relaxed) == 1,
           "late successful handle is released exactly once");

    /* Model a later recovery attempt and prove the prior late callback cannot
     * overwrite it. */
    GfxWebgpuAsyncRequest *recovery = gfx_webgpu_async_request_create();
    expect(recovery != NULL &&
               gfx_webgpu_async_request_retain_callback(recovery),
           "later recovery owns a fresh context");
    gfx_webgpu_async_request_complete(
        recovery, 7, (void *)(uintptr_t)0x33u, release_handle);
    handle = NULL;
    expect(gfx_webgpu_async_request_finish(recovery, &status, &handle) &&
               status == 7 && handle == (void *)(uintptr_t)0x33u,
           "later recovery retains its own status and handle");
    gfx_webgpu_async_request_release(recovery);

    if (s_failures != 0) {
        fprintf(stderr, "%d WebGPU async-request test(s) failed\n", s_failures);
        return 1;
    }
    puts("webgpu async request: PASS");
    return 0;
}
