#include "gfx_webgpu_callback_latch.h"

#include <stdatomic.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

static int s_failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

typedef struct PublishContext {
    uintptr_t generation;
    enum GfxWebgpuCallbackEvent event;
    atomic_int *start;
    atomic_int published;
    atomic_int finished;
} PublishContext;

static void publish(void *opaque) {
    PublishContext *context = (PublishContext *)opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire)) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    atomic_store_explicit(
        &context->published,
        gfx_webgpu_callback_latch_publish(
            context->generation, context->event) ? 1 : 0,
        memory_order_release);
    atomic_store_explicit(&context->finished, 1, memory_order_release);
}

#ifdef _WIN32
static DWORD WINAPI publish_thread(LPVOID opaque) {
    publish(opaque);
    return 0;
}
typedef HANDLE PublishThread;

static bool start_publish_thread(PublishThread *thread, PublishContext *context) {
    *thread = CreateThread(NULL, 0, publish_thread, context, 0, NULL);
    return *thread != NULL;
}

static void join_publish_thread(PublishThread thread) {
    if (thread != NULL) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}
#else
static void *publish_thread(void *opaque) {
    publish(opaque);
    return NULL;
}
typedef struct PublishThread {
    pthread_t handle;
    bool created;
} PublishThread;

static bool start_publish_thread(PublishThread *thread, PublishContext *context) {
    thread->created = pthread_create(
        &thread->handle, NULL, publish_thread, context) == 0;
    return thread->created;
}

static void join_publish_thread(PublishThread thread) {
    if (thread.created) {
        pthread_join(thread.handle, NULL);
    }
}
#endif

int main(void) {
    enum GfxWebgpuCallbackEvent event = GFX_WEBGPU_CALLBACK_NONE;
    atomic_int start = 0;
    PublishContext first = {
        41u, GFX_WEBGPU_CALLBACK_DEVICE_LOST, &start, 0, 0
    };
    PublishContext second = {
        41u, GFX_WEBGPU_CALLBACK_DEVICE_ERROR, &start, 0, 0
    };
    PublishThread first_thread = {0};
    PublishThread second_thread = {0};

    expect(gfx_webgpu_callback_latch_begin(41u),
           "first generation is reserved");
    expect(gfx_webgpu_callback_latch_commit(41u),
           "first generation is committed");
    expect(gfx_webgpu_callback_latch_active_generation() == 41u,
           "active generation is published");
    expect(start_publish_thread(&first_thread, &first),
           "first callback thread created");
    expect(start_publish_thread(&second_thread, &second),
           "second callback thread created");
    atomic_store_explicit(&start, 1, memory_order_release);
    join_publish_thread(first_thread);
    join_publish_thread(second_thread);
    expect(atomic_load(&first.published) + atomic_load(&second.published) == 1,
           "exactly one matching-generation callback wins");
    expect(gfx_webgpu_callback_latch_consume(41u, &event),
           "matching callback is consumed");
    expect(event == GFX_WEBGPU_CALLBACK_DEVICE_LOST ||
               event == GFX_WEBGPU_CALLBACK_DEVICE_ERROR,
           "consumed callback retains its event kind");
    expect(!gfx_webgpu_callback_latch_consume(41u, &event),
           "matching callback is consumed exactly once");

    expect(gfx_webgpu_callback_latch_begin(42u),
           "replacement generation is reserved");
    expect(gfx_webgpu_callback_latch_publish(
               42u, GFX_WEBGPU_CALLBACK_DEVICE_ERROR),
           "callback before activation is retained");
    expect(gfx_webgpu_callback_latch_active_generation() == 41u,
           "provisional callback does not displace the live generation");
    expect(!gfx_webgpu_callback_latch_publish(
               42u, GFX_WEBGPU_CALLBACK_DEVICE_LOST),
           "only the first provisional callback wins");
    expect(gfx_webgpu_callback_latch_commit(42u),
           "replacement generation commits");
    expect(gfx_webgpu_callback_latch_consume(42u, &event) &&
               event == GFX_WEBGPU_CALLBACK_DEVICE_ERROR,
           "callback published before activation survives commit");
    expect(!gfx_webgpu_callback_latch_publish(
               41u, GFX_WEBGPU_CALLBACK_DEVICE_LOST),
           "stale callback is ignored after replacement");
    gfx_webgpu_callback_latch_retire(42u);
    expect(gfx_webgpu_callback_latch_active_generation() == 0u,
           "host release invalidates the active generation");
    expect(!gfx_webgpu_callback_latch_consume(42u, &event),
           "release discards a pending callback without renderer mutation");

    expect(gfx_webgpu_callback_latch_begin(43u) &&
               gfx_webgpu_callback_latch_commit(43u),
           "wrong-generation scenario starts");
    expect(gfx_webgpu_callback_latch_publish(
               43u, GFX_WEBGPU_CALLBACK_DEVICE_LOST),
           "replacement callback can be published");
    expect(!gfx_webgpu_callback_latch_consume(44u, &event),
           "wrong-generation poll fails closed");
    expect(gfx_webgpu_callback_latch_consume(43u, &event),
           "wrong-generation poll cannot discard the active event");
    expect(!gfx_webgpu_callback_latch_consume(43u, &event),
           "active event is consumed exactly once");
    gfx_webgpu_callback_latch_retire(43u);

    /* A failed retry is retired without disturbing the prior live device. */
    expect(gfx_webgpu_callback_latch_begin(44u) &&
               gfx_webgpu_callback_latch_commit(44u),
           "retry baseline commits");
    expect(gfx_webgpu_callback_latch_begin(45u),
           "failed retry reserves a provisional slot");
    expect(gfx_webgpu_callback_latch_publish(
               45u, GFX_WEBGPU_CALLBACK_DEVICE_ERROR),
           "failed retry can receive an early callback");
    gfx_webgpu_callback_latch_retire(45u);
    expect(gfx_webgpu_callback_latch_active_generation() == 44u,
           "retiring a failed retry preserves the live generation");
    expect(!gfx_webgpu_callback_latch_consume(45u, &event),
           "failed retry event is not consumable");
    expect(gfx_webgpu_callback_latch_publish(
               44u, GFX_WEBGPU_CALLBACK_DEVICE_LOST),
           "live generation still accepts callbacks after failed retry");
    expect(gfx_webgpu_callback_latch_consume(44u, &event) &&
               event == GFX_WEBGPU_CALLBACK_DEVICE_LOST,
           "live callback remains exactly consumable");
    gfx_webgpu_callback_latch_retire(44u);

    /* A spontaneous callback is allowed to win or lose a teardown race, but it
     * must never leave a consumable event or reactivate released state. */
    atomic_store(&start, 0);
    PublishContext teardown = {
        51u, GFX_WEBGPU_CALLBACK_DEVICE_LOST, &start, 0, 0
    };
    PublishThread teardown_thread = {0};
    expect(gfx_webgpu_callback_latch_begin(51u) &&
               gfx_webgpu_callback_latch_commit(51u),
           "teardown-race generation starts");
    expect(start_publish_thread(&teardown_thread, &teardown),
           "teardown-race callback thread created");
    atomic_store_explicit(&start, 1, memory_order_release);
    gfx_webgpu_callback_latch_retire(51u);
    join_publish_thread(teardown_thread);
    expect(gfx_webgpu_callback_latch_active_generation() == 0u,
           "callback racing host release cannot reactivate the generation");
    expect(!gfx_webgpu_callback_latch_consume(51u, &event),
           "callback racing host release leaves no consumable event");

    /* Replacement has the same guarantee and must still admit a new-generation
     * event after the stale callback drains. */
    atomic_store(&start, 0);
    PublishContext replacement = {
        52u, GFX_WEBGPU_CALLBACK_DEVICE_ERROR, &start, 0, 0
    };
    PublishThread replacement_thread = {0};
    expect(gfx_webgpu_callback_latch_begin(52u) &&
               gfx_webgpu_callback_latch_commit(52u),
           "replacement-race baseline starts");
    expect(start_publish_thread(&replacement_thread, &replacement),
           "replacement-race callback thread created");
    atomic_store_explicit(&start, 1, memory_order_release);
    expect(gfx_webgpu_callback_latch_begin(53u) &&
               gfx_webgpu_callback_latch_commit(53u),
           "replacement-race candidate commits");
    join_publish_thread(replacement_thread);
    expect(!gfx_webgpu_callback_latch_consume(52u, &event),
           "replacement discards the stale generation's event");
    expect(gfx_webgpu_callback_latch_publish(
               53u, GFX_WEBGPU_CALLBACK_DEVICE_LOST),
           "replacement generation admits its own event");
    expect(gfx_webgpu_callback_latch_consume(53u, &event) &&
               event == GFX_WEBGPU_CALLBACK_DEVICE_LOST,
           "renderer consumes the replacement generation exactly once");
    gfx_webgpu_callback_latch_retire(53u);

    if (s_failures != 0) {
        fprintf(stderr, "%d WebGPU callback-latch test(s) failed\n", s_failures);
        return 1;
    }
    puts("webgpu callback latch: PASS");
    return 0;
}
