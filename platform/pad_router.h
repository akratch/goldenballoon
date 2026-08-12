/* Launcher-owned controller-port custody. Pure C; no SDL, DOM or network. */
#ifndef MDKR_PAD_ROUTER_H
#define MDKR_PAD_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#include "input_tick_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_PAD_ROUTER_PORTS MDKR_INPUT_PORTS

typedef enum MdkrPadSourceKind {
    MDKR_PAD_NONE = 0,
    MDKR_PAD_KEYBOARD,
    MDKR_PAD_SDL,
    MDKR_PAD_LOCAL_TOUCH,
    MDKR_PAD_REMOTE_PHONE,
    MDKR_PAD_SCRIPT,
    MDKR_PAD_SOURCE_KIND_COUNT
} MdkrPadSourceKind;

typedef struct MdkrPadLease {
    uint8_t port;
    uint8_t kind;
    uint16_t reserved;
    uint32_t generation;
    uint64_t owner;
} MdkrPadLease;

typedef struct MdkrPadRoute {
    MdkrPadLease lease;
    MdkrInputSample sample;
    bool occupied;
} MdkrPadRoute;

typedef struct MdkrPadRouterStats {
    uint64_t claims;
    uint64_t rejected_claims;
    uint64_t releases;
    uint64_t stale_operations;
    uint64_t invalid_samples;
} MdkrPadRouterStats;

typedef struct MdkrPadRouter {
    MdkrPadRoute routes[MDKR_PAD_ROUTER_PORTS];
    uint32_t next_generation;
    MdkrPadRouterStats stats;
} MdkrPadRouter;

void mdkr_pad_router_init(MdkrPadRouter *router);

/* Claim is atomic and never evicts. Repeating the same owner claim returns the
 * existing lease; a competing source must be approved onto another free port. */
bool mdkr_pad_router_claim(
    MdkrPadRouter *router, unsigned port, MdkrPadSourceKind kind,
    uint64_t owner, MdkrPadLease *out_lease);

bool mdkr_pad_router_claim_first_free(
    MdkrPadRouter *router, MdkrPadSourceKind kind, uint64_t owner,
    unsigned first_port, MdkrPadLease *out_lease);

/* Every mutation is generation checked. A delayed disconnect from an old phone
 * therefore cannot release or overwrite the controller that reclaimed a seat. */
bool mdkr_pad_router_publish(
    MdkrPadRouter *router, const MdkrPadLease *lease, MdkrInputSample sample);
bool mdkr_pad_router_release(
    MdkrPadRouter *router, const MdkrPadLease *lease);

bool mdkr_pad_router_sample(
    const MdkrPadRouter *router, unsigned port, MdkrInputSample *out_sample);
bool mdkr_pad_router_lease(
    const MdkrPadRouter *router, unsigned port, MdkrPadLease *out_lease);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PAD_ROUTER_H */
