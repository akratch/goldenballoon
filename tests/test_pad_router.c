#include "pad_router.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

int main(void) {
    MdkrPadRouter router;
    MdkrPadLease keyboard;
    MdkrPadLease phone;
    MdkrPadLease reclaimed;
    MdkrInputSample sample = {0x8000u, 40, -20, true};
    MdkrInputSample readback;

    mdkr_pad_router_init(&router);
    CHECK(mdkr_pad_router_claim(
        &router, 0u, MDKR_PAD_KEYBOARD, 1u, &keyboard));
    CHECK(!mdkr_pad_router_claim(
        &router, 0u, MDKR_PAD_REMOTE_PHONE, 2u, &phone));
    CHECK(router.stats.rejected_claims == 1u);
    CHECK(mdkr_pad_router_publish(&router, &keyboard, sample));
    CHECK(mdkr_pad_router_sample(&router, 0u, &readback));
    CHECK(memcmp(&readback, &sample, sizeof(sample)) == 0);

    CHECK(mdkr_pad_router_claim_first_free(
        &router, MDKR_PAD_REMOTE_PHONE, 2u, 0u, &phone));
    CHECK(phone.port == 1u);
    CHECK(mdkr_pad_router_publish(&router, &phone, sample));
    CHECK(mdkr_pad_router_release(&router, &phone));
    CHECK(!mdkr_pad_router_sample(&router, 1u, &readback));
    CHECK(!readback.present && readback.buttons == 0u);

    CHECK(mdkr_pad_router_claim(
        &router, 1u, MDKR_PAD_REMOTE_PHONE, 3u, &reclaimed));
    CHECK(reclaimed.generation != phone.generation);
    CHECK(!mdkr_pad_router_publish(&router, &phone, sample));
    CHECK(!mdkr_pad_router_release(&router, &phone));
    CHECK(router.stats.stale_operations == 2u);

    sample.stick_x = 81;
    CHECK(!mdkr_pad_router_publish(&router, &reclaimed, sample));
    CHECK(mdkr_pad_router_sample(&router, 1u, &readback));
    CHECK(readback.present && readback.buttons == 0u &&
          readback.stick_x == 0 && readback.stick_y == 0);

    if (failures != 0) return 1;
    puts("pad_router: PASS");
    return 0;
}
