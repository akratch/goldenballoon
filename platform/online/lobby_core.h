/* Launcher-owned private-room reducer. Pure C: no sockets, clocks, UI or game. */
#ifndef MDKR_ONLINE_LOBBY_CORE_H
#define MDKR_ONLINE_LOBBY_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ONLINE_PROTOCOL_VERSION 1u
#define MDKR_ONLINE_MAX_ENDPOINTS 4u
#define MDKR_ONLINE_MAX_SEATS 4u
#define MDKR_ONLINE_MAX_SEATS_PER_ENDPOINT 2u
#define MDKR_ONLINE_RECEIPT_WINDOW 8u
#define MDKR_ONLINE_NO_VOTE UINT16_MAX
#define MDKR_ONLINE_NO_CHARACTER UINT8_MAX
#define MDKR_ONLINE_NO_VEHICLE UINT8_MAX
#define MDKR_ONLINE_CHARACTER_COUNT 10u
#define MDKR_ONLINE_PLAYER_VEHICLE_COUNT 3u
#define MDKR_ONLINE_PLAYER_VEHICLE_MASK 0x07u

typedef enum MdkrOnlinePhase {
    MDKR_ONLINE_LOBBY = 1,
    MDKR_ONLINE_LOADING,
    MDKR_ONLINE_RACING,
    MDKR_ONLINE_RESULTS,
    MDKR_ONLINE_CLOSED
} MdkrOnlinePhase;

typedef enum MdkrOnlineCommandType {
    MDKR_ONLINE_JOIN = 1,
    MDKR_ONLINE_LEAVE,
    MDKR_ONLINE_DISCONNECT,
    MDKR_ONLINE_RECONNECT,
    MDKR_ONLINE_SET_READY,
    MDKR_ONLINE_SET_VOTE,
    MDKR_ONLINE_BEGIN_LOADING,
    MDKR_ONLINE_ACK_LOADED,
    MDKR_ONLINE_BEGIN_RACE,
    MDKR_ONLINE_PUBLISH_RESULTS,
    MDKR_ONLINE_REMATCH,
    MDKR_ONLINE_TRANSFER_LEADER,
    MDKR_ONLINE_CLOSE,
    MDKR_ONLINE_SET_CHARACTER,
    MDKR_ONLINE_SET_VEHICLE,
    MDKR_ONLINE_CANCEL_LOADING
} MdkrOnlineCommandType;

typedef enum MdkrOnlineError {
    MDKR_ONLINE_OK = 0,
    MDKR_ONLINE_ERROR_PROTOCOL,
    MDKR_ONLINE_ERROR_STALE_REVISION,
    MDKR_ONLINE_ERROR_STALE_COMMAND,
    MDKR_ONLINE_ERROR_COMMAND_CONFLICT,
    MDKR_ONLINE_ERROR_INVALID_STATE,
    MDKR_ONLINE_ERROR_UNAUTHORIZED,
    MDKR_ONLINE_ERROR_NOT_FOUND,
    MDKR_ONLINE_ERROR_ALREADY_JOINED,
    MDKR_ONLINE_ERROR_INCOMPATIBLE,
    MDKR_ONLINE_ERROR_CAPACITY,
    MDKR_ONLINE_ERROR_NOT_READY,
    MDKR_ONLINE_ERROR_DISCONNECTED,
    MDKR_ONLINE_ERROR_SELECTION_CONFLICT,
    MDKR_ONLINE_ERROR_ILLEGAL_VEHICLE
} MdkrOnlineError;

typedef struct MdkrOnlineCompatibilityV1 {
    uint32_t protocol_version;
    uint8_t build_id[16];
    uint8_t gameplay_digest[32];
    uint8_t rom_revision;
    uint8_t cadence_hz;
} MdkrOnlineCompatibilityV1;

typedef struct MdkrOnlineMember {
    uint64_t endpoint_id;
    uint64_t last_command_id;
    uint64_t last_command_fingerprint;
    uint8_t seat_count;
    bool occupied;
    bool connected;
    bool ready;
    bool loaded;
} MdkrOnlineMember;

typedef struct MdkrOnlineSeat {
    uint64_t endpoint_id;
    uint32_t selection_revision;
    uint16_t vote_track;
    uint8_t local_index;
    uint8_t character_id;
    uint8_t vehicle_id;
    bool occupied;
} MdkrOnlineSeat;

typedef struct MdkrOnlineCommandReceipt {
    uint64_t actor_endpoint_id;
    uint64_t command_id;
    uint64_t fingerprint;
} MdkrOnlineCommandReceipt;

typedef struct MdkrOnlineLobby {
    uint32_t protocol_version;
    uint32_t revision;
    uint32_t match_epoch;
    uint32_t leader_generation;
    uint64_t room_id;
    uint64_t leader_endpoint_id;
    MdkrOnlinePhase phase;
    MdkrOnlineCompatibilityV1 compatibility;
    MdkrOnlineMember members[MDKR_ONLINE_MAX_ENDPOINTS];
    MdkrOnlineSeat seats[MDKR_ONLINE_MAX_SEATS];
    MdkrOnlineCommandReceipt receipts[MDKR_ONLINE_RECEIPT_WINDOW];
    uint16_t selected_track;
    uint8_t selected_vehicle_mask;
    uint8_t member_count;
    uint8_t seat_count;
    uint8_t next_receipt;
} MdkrOnlineLobby;

typedef struct MdkrOnlineCommand {
    uint32_t protocol_version;
    uint32_t expected_revision;
    uint64_t command_id;
    uint64_t actor_endpoint_id;
    MdkrOnlineCommandType type;
    uint32_t value;
    uint64_t target_endpoint_id;
    MdkrOnlineCompatibilityV1 compatibility;
} MdkrOnlineCommand;

typedef struct MdkrOnlineStep {
    bool accepted;
    bool duplicate;
    bool leader_changed;
    MdkrOnlineError error;
    uint32_t revision;
    uint32_t match_epoch;
    uint64_t leader_endpoint_id;
    uint16_t selected_track;
    uint8_t selected_vehicle_mask;
} MdkrOnlineStep;

bool mdkr_online_compatibility_valid(const MdkrOnlineCompatibilityV1 *value);
bool mdkr_online_lobby_valid(const MdkrOnlineLobby *lobby);
bool mdkr_online_lobby_init(
    MdkrOnlineLobby *lobby, uint64_t room_id, uint64_t leader_endpoint_id,
    const MdkrOnlineCompatibilityV1 *compatibility, unsigned leader_seats);
MdkrOnlineStep mdkr_online_lobby_dispatch(
    MdkrOnlineLobby *lobby, const MdkrOnlineCommand *command);

#ifdef __cplusplus
}
#endif
#endif
