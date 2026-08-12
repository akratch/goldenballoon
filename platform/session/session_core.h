/* Allocation-free session state reducer shared by native and wasm shells. */
#ifndef MDKR_SESSION_CORE_H
#define MDKR_SESSION_CORE_H

#include "session_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrSessionCore {
    MdkrSessionState state;
    uint32_t next_effect_id;
} MdkrSessionCore;

void mdkr_session_core_init(MdkrSessionCore *core, uint64_t session_id);
const MdkrSessionState *mdkr_session_core_state(const MdkrSessionCore *core);
MdkrSessionStep mdkr_session_core_dispatch(
    MdkrSessionCore *core, const MdkrSessionCommand *command);
bool mdkr_session_state_valid(const MdkrSessionState *state);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_SESSION_CORE_H */
