/* Deterministic ON-03B adapter. Exercises real reducers without sockets/time. */
#ifndef MDKR_ONLINE_LOBBY_FAKE_ADAPTER_H
#define MDKR_ONLINE_LOBBY_FAKE_ADAPTER_H

#include "lobby_view_model.h"
#include "session/session_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrOnlineFakeError {
    MDKR_ONLINE_FAKE_OK = 0,
    MDKR_ONLINE_FAKE_ERROR_PROTOCOL,
    MDKR_ONLINE_FAKE_ERROR_STALE_VIEW,
    MDKR_ONLINE_FAKE_ERROR_STALE_REQUEST,
    MDKR_ONLINE_FAKE_ERROR_REQUEST_CONFLICT,
    MDKR_ONLINE_FAKE_ERROR_INVALID_ACTION,
    MDKR_ONLINE_FAKE_ERROR_PENDING,
    MDKR_ONLINE_FAKE_ERROR_STALE_CALLBACK,
    MDKR_ONLINE_FAKE_ERROR_REDUCER
} MdkrOnlineFakeError;

typedef enum MdkrOnlineFakePending {
    MDKR_ONLINE_FAKE_PENDING_NONE = 0,
    MDKR_ONLINE_FAKE_PENDING_CREATE,
    MDKR_ONLINE_FAKE_PENDING_JOIN,
    MDKR_ONLINE_FAKE_PENDING_PEER_JOIN,
    MDKR_ONLINE_FAKE_PENDING_PREFLIGHT,
    MDKR_ONLINE_FAKE_PENDING_LOAD
} MdkrOnlineFakePending;

typedef struct MdkrOnlineFakeCommand {
    uint32_t expected_revision;
    uint64_t request_id;
    MdkrOnlineViewAction action;
    uint32_t seat;
    uint32_t value;
} MdkrOnlineFakeCommand;

typedef struct MdkrOnlineFakeStep {
    bool accepted;
    bool duplicate;
    MdkrOnlineFakeError error;
    uint32_t revision;
    uint32_t pending_token;
} MdkrOnlineFakeStep;

typedef struct MdkrOnlineFakeAdapter {
    MdkrSessionCore session;
    MdkrOnlineLobby lobby;
    MdkrOnlineCompatibilityV1 compatibility;
    uint64_t local_endpoint_id;
    uint64_t peer_endpoint_id;
    uint64_t next_local_command_id;
    uint64_t next_peer_command_id;
    uint64_t last_request_id;
    uint64_t last_request_fingerprint;
    MdkrOnlineFakeStep last_request_step;
    MdkrOnlineJourney journey;
    MdkrOnlineViewFailure failure;
    MdkrOnlineFakePending pending;
    uint32_t revision;
    uint32_t next_pending_token;
    uint32_t pending_token;
    uint32_t verification_phrase_generation;
    bool have_lobby;
    bool invite_ready;
    /* Deterministic rotating stand-in for a locally derived authenticated
     * transcript. It is never sourced from the fake room snapshot. */
    bool verification_phrase_ready;
    bool race_admission_enabled;
    /* Adapter clock result, not room/service authority. The view model carries
     * the bounded outcome; UI presents it only after this local expiry. */
    bool timeout_expired;
} MdkrOnlineFakeAdapter;

bool mdkr_online_fake_init(MdkrOnlineFakeAdapter *adapter,
                           uint64_t session_id,
                           const MdkrOnlineCompatibilityV1 *compatibility,
                           bool race_admission_enabled);
bool mdkr_online_fake_view(const MdkrOnlineFakeAdapter *adapter,
                           MdkrOnlineViewModel *output);
MdkrOnlineFakeStep mdkr_online_fake_dispatch(
    MdkrOnlineFakeAdapter *adapter, const MdkrOnlineFakeCommand *command);

/* Completes only the currently published token. `failure` is a stable product
 * reason; unknown provider text has no parameter and cannot cross this seam. */
MdkrOnlineFakeStep mdkr_online_fake_complete(
    MdkrOnlineFakeAdapter *adapter, uint32_t pending_token,
    MdkrOnlineViewFailure failure);

/* Deterministic friend fixture: one atomic peer transaction over the actual
 * room reducer, not direct lobby mutation. */
MdkrOnlineFakeStep mdkr_online_fake_prepare_peer(
    MdkrOnlineFakeAdapter *adapter, uint32_t expected_revision,
    uint8_t character_id, uint8_t vehicle_id, uint16_t track_id);

/* Fake engine callback. It is versioned so a result from an earlier route or
 * rematch cannot complete the current view. */
MdkrOnlineFakeStep mdkr_online_fake_finish_race(
    MdkrOnlineFakeAdapter *adapter, uint32_t expected_revision);
MdkrOnlineFakeStep mdkr_online_fake_expire_timeout(
    MdkrOnlineFakeAdapter *adapter, uint32_t expected_revision);

/* Deterministic design/evidence gallery. Every case is constructed from a
 * fresh adapter through the same public actions, callback tokens and reducers
 * as the interactive fake journey. Cases at or beyond Start Race additionally
 * require the adapter's local race-admission flag; a gallery name can never
 * elevate that policy. */
typedef struct MdkrOnlineFakeGallerySpec {
    const char *slug;
    MdkrOnlineViewKind kind;
    MdkrOnlineViewFailure failure;
    MdkrOnlineViewAction primary_action;
    bool timeout_present;
    bool requires_race_admission;
} MdkrOnlineFakeGallerySpec;

size_t mdkr_online_fake_gallery_count(void);
const MdkrOnlineFakeGallerySpec *mdkr_online_fake_gallery_at(size_t index);
const MdkrOnlineFakeGallerySpec *mdkr_online_fake_gallery_find(
    const char *slug);
/* Fail-atomic: an unknown/ineligible/broken case leaves `adapter` untouched. */
bool mdkr_online_fake_prepare_gallery(MdkrOnlineFakeAdapter *adapter,
                                      const char *slug);

#ifdef __cplusplus
}
#endif
#endif
