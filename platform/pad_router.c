#include "pad_router.h"

#include <string.h>

static MdkrInputSample neutral_sample(void) {
    const MdkrInputSample sample = {0, 0, 0, false};
    return sample;
}

static bool valid_kind(MdkrPadSourceKind kind) {
    return kind > MDKR_PAD_NONE && kind < MDKR_PAD_SOURCE_KIND_COUNT;
}

static bool lease_equal(const MdkrPadLease *left, const MdkrPadLease *right) {
    return left != NULL && right != NULL &&
        left->port == right->port && left->kind == right->kind &&
        left->generation == right->generation && left->owner == right->owner;
}

static bool live_route(
    MdkrPadRouter *router, const MdkrPadLease *lease, MdkrPadRoute **out) {
    MdkrPadRoute *route;
    if (router == NULL || lease == NULL ||
        lease->port >= MDKR_PAD_ROUTER_PORTS) {
        if (router != NULL) router->stats.stale_operations++;
        return false;
    }
    route = &router->routes[lease->port];
    if (!route->occupied || !lease_equal(&route->lease, lease)) {
        router->stats.stale_operations++;
        return false;
    }
    if (out != NULL) *out = route;
    return true;
}

void mdkr_pad_router_init(MdkrPadRouter *router) {
    if (router == NULL) return;
    memset(router, 0, sizeof(*router));
    router->next_generation = 1u;
}

bool mdkr_pad_router_claim(
    MdkrPadRouter *router, unsigned port, MdkrPadSourceKind kind,
    uint64_t owner, MdkrPadLease *out_lease) {
    MdkrPadRoute *route;
    if (router == NULL || port >= MDKR_PAD_ROUTER_PORTS ||
        !valid_kind(kind) || owner == 0u) {
        if (router != NULL) router->stats.rejected_claims++;
        return false;
    }
    route = &router->routes[port];
    if (route->occupied) {
        if (route->lease.kind == (uint8_t)kind && route->lease.owner == owner) {
            if (out_lease != NULL) *out_lease = route->lease;
            return true;
        }
        router->stats.rejected_claims++;
        return false;
    }
    if (router->next_generation == 0u) router->next_generation = 1u;
    route->lease.port = (uint8_t)port;
    route->lease.kind = (uint8_t)kind;
    route->lease.reserved = 0u;
    route->lease.generation = router->next_generation++;
    route->lease.owner = owner;
    route->sample = neutral_sample();
    route->sample.present = true;
    route->occupied = true;
    router->stats.claims++;
    if (out_lease != NULL) *out_lease = route->lease;
    return true;
}

bool mdkr_pad_router_claim_first_free(
    MdkrPadRouter *router, MdkrPadSourceKind kind, uint64_t owner,
    unsigned first_port, MdkrPadLease *out_lease) {
    unsigned offset;
    if (router == NULL || first_port >= MDKR_PAD_ROUTER_PORTS) return false;
    for (offset = 0; offset < MDKR_PAD_ROUTER_PORTS; offset++) {
        const unsigned port = (first_port + offset) % MDKR_PAD_ROUTER_PORTS;
        if (!router->routes[port].occupied) {
            return mdkr_pad_router_claim(router, port, kind, owner, out_lease);
        }
    }
    router->stats.rejected_claims++;
    return false;
}

bool mdkr_pad_router_publish(
    MdkrPadRouter *router, const MdkrPadLease *lease, MdkrInputSample sample) {
    MdkrPadRoute *route;
    if (!live_route(router, lease, &route)) return false;
    if (sample.stick_x < -80 || sample.stick_x > 80 ||
        sample.stick_y < -80 || sample.stick_y > 80) {
        router->stats.invalid_samples++;
        route->sample = neutral_sample();
        route->sample.present = true;
        return false;
    }
    if (!sample.present) {
        sample.buttons = 0u;
        sample.stick_x = 0;
        sample.stick_y = 0;
    }
    route->sample = sample;
    return true;
}

bool mdkr_pad_router_release(
    MdkrPadRouter *router, const MdkrPadLease *lease) {
    MdkrPadRoute *route;
    if (!live_route(router, lease, &route)) return false;
    /* Neutralize before ownership disappears. Consumers sampling between these
     * statements can only observe the old owner as neutral, never held input. */
    route->sample = neutral_sample();
    memset(&route->lease, 0, sizeof(route->lease));
    route->occupied = false;
    router->stats.releases++;
    return true;
}

bool mdkr_pad_router_sample(
    const MdkrPadRouter *router, unsigned port, MdkrInputSample *out_sample) {
    if (out_sample == NULL) return false;
    *out_sample = neutral_sample();
    if (router == NULL || port >= MDKR_PAD_ROUTER_PORTS ||
        !router->routes[port].occupied) return false;
    *out_sample = router->routes[port].sample;
    return true;
}

bool mdkr_pad_router_lease(
    const MdkrPadRouter *router, unsigned port, MdkrPadLease *out_lease) {
    if (out_lease == NULL) return false;
    memset(out_lease, 0, sizeof(*out_lease));
    if (router == NULL || port >= MDKR_PAD_ROUTER_PORTS ||
        !router->routes[port].occupied) return false;
    *out_lease = router->routes[port].lease;
    return true;
}
