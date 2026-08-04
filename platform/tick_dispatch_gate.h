/* A future authoritative game pass requires both a live process and a ticket
 * from the host clock. The adapter completes/accounting the pass that just
 * returned to osRecvMesg before applying this gate to the next one. */
#ifndef MDKR_TICK_DISPATCH_GATE_H
#define MDKR_TICK_DISPATCH_GATE_H

#include <stdbool.h>

static inline bool mdkr_next_tick_dispatch_allowed(bool exit_requested,
                                                   bool ticket_issued) {
    return !exit_requested && ticket_issued;
}

#endif /* MDKR_TICK_DISPATCH_GATE_H */
