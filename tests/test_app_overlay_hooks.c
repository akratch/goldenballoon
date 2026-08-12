#include "app_overlay_hooks.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;
static int processed;
static int serviced;

static void expect(int condition, const char *message) {
    if (condition) {
        printf("ok: %s\n", message);
    } else {
        printf("FAIL: %s\n", message);
        failures++;
    }
}

static int process_event(const void *event) {
    processed += event != NULL;
    return event != NULL;
}

static void service(void) { serviced++; }

static int yes(void) { return 1; }
static int no(void) { return 0; }

static void set_overlay_test_environment(int enabled) {
#if defined(_WIN32)
    _putenv_s("MDKR_TEST_WEBGPU_OVERLAY", enabled ? "1" : "");
#else
    if (enabled) setenv("MDKR_TEST_WEBGPU_OVERLAY", "1", 1);
    else unsetenv("MDKR_TEST_WEBGPU_OVERLAY");
#endif
}

int main(void) {
    AppOverlayHooks hooks = {process_event, service, yes, no, no, yes};
    const int event = 7;

    platformSetOverlayHooks(NULL);
    set_overlay_test_environment(1);
    expect(platformOverlayWantsRender() == 1,
           "renderer diagnostic requests an overlay pass");
    expect(platformOverlayWantsInput() == 0,
           "renderer diagnostic never captures input or pauses simulation");
    expect(platformOverlayWantsPause() == 0,
           "renderer diagnostic never requests a pause");
    expect(platformOverlayRender() == 1,
           "absent app renderer is a successful no-op");

    platformSetOverlayHooks(&hooks);
    expect(platformOverlayProcessEvent(&event) == 1,
           "registered hook forwards event-consumed result");
    expect(processed == 1, "registered hook receives events");
    platformOverlayService();
    expect(serviced == 1, "registered hook services safe-boundary work");
    expect(platformOverlayWantsInput() == 1,
           "registered app hook owns input capture");
    expect(platformOverlayWantsPause() == 0,
           "registered online-style hook captures without pausing");
    expect(platformOverlayWantsRender() == 0,
           "registered app hook overrides the diagnostic fallback");
    expect(platformOverlayRender() == 1,
           "registered app renderer result is forwarded");

    hooks.wants_pause = yes;
    platformSetOverlayHooks(&hooks);
    expect(platformOverlayWantsInput() == 1 &&
           platformOverlayWantsPause() == 1,
           "local-style hook may capture input and pause independently");

    platformSetOverlayHooks(NULL);
    set_overlay_test_environment(0);
    expect(platformOverlayWantsRender() == 0,
           "unregistered production path stays inert");
    expect(platformOverlayWantsInput() == 0,
           "unregistered production input path stays inert");
    expect(platformOverlayWantsPause() == 0,
           "unregistered production pause path stays inert");

    if (failures) return 1;
    printf("overlay hook boundary passed\n");
    return 0;
}
