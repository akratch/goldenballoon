#include "platform/online/lobby_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static MdkrOnlineCompatibilityV1 compatibility(void) {
    MdkrOnlineCompatibilityV1 value;
    unsigned index;
    memset(&value, 0, sizeof(value));
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (index = 0u; index < sizeof(value.build_id); index++)
        value.build_id[index] = (uint8_t)(index + 1u);
    for (index = 0u; index < sizeof(value.gameplay_digest); index++)
        value.gameplay_digest[index] = (uint8_t)(0xa0u + index);
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

static MdkrOnlineCommand command(
    const MdkrOnlineLobby *lobby, uint64_t actor, uint64_t command_id,
    MdkrOnlineCommandType type) {
    MdkrOnlineCommand value;
    memset(&value, 0, sizeof(value));
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    value.expected_revision = lobby->revision;
    value.command_id = command_id;
    value.actor_endpoint_id = actor;
    value.type = type;
    return value;
}

static const char *phase_name(MdkrOnlinePhase phase) {
    static const char *const names[] = {
        "invalid", "lobby", "loading", "racing", "results", "closed"};
    return phase <= MDKR_ONLINE_CLOSED ? names[phase] : names[0];
}

static const char *error_name(MdkrOnlineError error) {
    static const char *const names[] = {
        "ok", "protocol", "stale_revision", "stale_command",
        "command_conflict", "invalid_state", "unauthorized", "not_found",
        "already_joined", "incompatible", "capacity", "not_ready",
        "disconnected", "selection_conflict", "illegal_vehicle"};
    return error <= MDKR_ONLINE_ERROR_ILLEGAL_VEHICLE ? names[error] : "unknown";
}

static MdkrOnlineCommandType command_type(const char *name) {
    static const char *const names[] = {
        "invalid", "join", "leave", "disconnect", "reconnect", "set_ready",
        "set_vote", "begin_loading", "ack_loaded", "begin_race",
        "publish_results", "rematch", "transfer_leader", "close",
        "set_character", "set_vehicle", "cancel_loading"};
    unsigned index;
    for (index = 1u; index <= MDKR_ONLINE_CANCEL_LOADING; index++) {
        if (strcmp(name, names[index]) == 0) return (MdkrOnlineCommandType)index;
    }
    return (MdkrOnlineCommandType)0;
}

static void append_text(char *output, size_t capacity, size_t *used,
                        const char *format, ...) {
    va_list arguments;
    int count;
    if (*used >= capacity) return;
    va_start(arguments, format);
    count = vsnprintf(output + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= capacity - *used) {
        *used = capacity;
        return;
    }
    *used += (size_t)count;
}

static void canonical_parity_state(const MdkrOnlineLobby *lobby,
                                   MdkrOnlineStep result, char *output,
                                   size_t capacity) {
    size_t used = 0u;
    unsigned index;
    bool first = true;
    output[0] = '\0';
    append_text(output, capacity, &used,
                "%u,%u,%u,%s,%u,%u,%s,%llu,%u,",
                result.accepted ? 1u : 0u, result.duplicate ? 1u : 0u,
                result.leader_changed ? 1u : 0u, error_name(result.error),
                lobby->revision, lobby->match_epoch, phase_name(lobby->phase),
                (unsigned long long)lobby->leader_endpoint_id,
                lobby->leader_generation);
    if (lobby->selected_track == MDKR_ONLINE_NO_VOTE)
        append_text(output, capacity, &used, "-,%u,", lobby->selected_vehicle_mask);
    else
        append_text(output, capacity, &used, "%u,%u,", lobby->selected_track,
                    lobby->selected_vehicle_mask);
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        const MdkrOnlineMember *item = &lobby->members[index];
        if (!item->occupied) continue;
        append_text(output, capacity, &used, "%s%llu:%u:%u:%u:%u:%llu",
                    first ? "" : ";", (unsigned long long)item->endpoint_id,
                    item->connected ? 1u : 0u, item->ready ? 1u : 0u,
                    item->loaded ? 1u : 0u, item->seat_count,
                    (unsigned long long)item->last_command_id);
        first = false;
    }
    append_text(output, capacity, &used, ",");
    first = true;
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        const MdkrOnlineSeat *item = &lobby->seats[index];
        if (!item->occupied) continue;
        append_text(output, capacity, &used, "%s%llu:%u:%u:", first ? "" : ";",
                    (unsigned long long)item->endpoint_id, item->local_index,
                    item->selection_revision);
        if (item->vote_track == MDKR_ONLINE_NO_VOTE)
            append_text(output, capacity, &used, "-:");
        else
            append_text(output, capacity, &used, "%u:", item->vote_track);
        if (item->character_id == MDKR_ONLINE_NO_CHARACTER)
            append_text(output, capacity, &used, "-:");
        else
            append_text(output, capacity, &used, "%u:", item->character_id);
        if (item->vehicle_id == MDKR_ONLINE_NO_VEHICLE)
            append_text(output, capacity, &used, "-");
        else
            append_text(output, capacity, &used, "%u", item->vehicle_id);
        first = false;
    }
}

static void test_shared_service_parity_trace(void) {
    const char *path = MDKR_SOURCE_DIR "/tests/fixtures/online_lobby_reducer_v1.tsv";
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    FILE *file = fopen(path, "rb");
    char line[4096];
    unsigned line_number = 0u;
    expect(file != NULL, "shared reducer parity trace opens");
    if (file == NULL) return;
    expect(mdkr_online_lobby_init(&lobby, 42u, 100u, &compat, 1u),
           "shared reducer parity lobby initializes");
    while (fgets(line, sizeof(line), file) != NULL) {
        char *fields[9];
        char *cursor = line;
        char *tab;
        unsigned field_count = 0u;
        MdkrOnlineCommand value;
        MdkrOnlineStep result;
        char actual[2048];
        char message[256];
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        while (field_count < 9u) {
            fields[field_count++] = cursor;
            tab = strchr(cursor, '\t');
            if (tab == NULL) break;
            *tab = '\0';
            cursor = tab + 1;
        }
        snprintf(message, sizeof(message), "parity trace line %u has nine fields",
                 line_number);
        expect(field_count == 9u && strchr(fields[8], '\t') == NULL, message);
        if (field_count != 9u) continue;
        value = command(&lobby, strtoull(fields[2], NULL, 10),
                        strtoull(fields[3], NULL, 10), command_type(fields[1]));
        value.expected_revision = (uint32_t)strtoul(fields[4], NULL, 10);
        value.value = (uint32_t)strtoul(fields[5], NULL, 10);
        value.target_endpoint_id = strtoull(fields[6], NULL, 10);
        if (strcmp(fields[7], "same") == 0) value.compatibility = compat;
        else if (strcmp(fields[7], "unsupported_rom") == 0) {
            value.compatibility = compat;
            value.compatibility.rom_revision = 3u;
        } else if (strcmp(fields[7], "mismatch") == 0) {
            value.compatibility = compat;
            value.compatibility.gameplay_digest[3] ^= 1u;
        }
        result = mdkr_online_lobby_dispatch(&lobby, &value);
        canonical_parity_state(&lobby, result, actual, sizeof(actual));
        snprintf(message, sizeof(message), "shared parity row %s matches", fields[0]);
        expect(strcmp(actual, fields[8]) == 0, message);
        if (strcmp(actual, fields[8]) != 0)
            fprintf(stderr, "  expected: %s\n  actual:   %s\n", fields[8], actual);
    }
    fclose(file);
}

static MdkrOnlineStep join(
    MdkrOnlineLobby *lobby, uint64_t actor, unsigned seats,
    const MdkrOnlineCompatibilityV1 *compat) {
    MdkrOnlineCommand value = command(lobby, actor, 1u, MDKR_ONLINE_JOIN);
    value.value = seats;
    value.compatibility = *compat;
    return mdkr_online_lobby_dispatch(lobby, &value);
}

static MdkrOnlineStep select_value(
    MdkrOnlineLobby *lobby, uint64_t actor, uint64_t command_id,
    MdkrOnlineCommandType type, unsigned seat, unsigned selected) {
    MdkrOnlineCommand value = command(lobby, actor, command_id, type);
    value.target_endpoint_id = seat;
    value.value = selected;
    return mdkr_online_lobby_dispatch(lobby, &value);
}

static void test_service_fingerprint_vector(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;

    expect(mdkr_online_lobby_init(&lobby, 1u, 100u, &compat, 1u),
           "fingerprint fixture creates a lobby");
    value = command(&lobby, 100u, 1u, MDKR_ONLINE_SET_CHARACTER);
    value.target_endpoint_id = 0u;
    value.value = 0u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "fingerprint fixture command is accepted");
    expect(lobby.members[0].last_command_fingerprint ==
               UINT64_C(1764495763471733581),
           "non-join command fingerprint matches the service reducer vector");
}

static void test_lifecycle_and_votes(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    expect(mdkr_online_lobby_init(&lobby, 0x1234u, 10u, &compat, 1u),
           "leader creates a private lobby");
    expect(mdkr_online_lobby_valid(&lobby), "initial lobby validates");
    expect(join(&lobby, 20u, 1u, &compat).accepted,
           "compatible endpoint joins");

    expect(select_value(
               &lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 9u).accepted &&
           select_value(
               &lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 1u).accepted,
           "leader selects a unique character and hovercraft");
    expect(select_value(
               &lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 7u).accepted &&
           select_value(
               &lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 1u).accepted,
           "peer selects a unique character and hovercraft");

    value = command(&lobby, 10u, 3u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 0u;
    value.value = 5u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "leader seat votes");
    value = command(&lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 1u;
    value.value = 7u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "peer seat votes");

    value = command(&lobby, 10u, 4u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "leader becomes ready");
    value = command(&lobby, 20u, 5u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "peer becomes ready");

    value = command(&lobby, 20u, 6u, MDKR_ONLINE_BEGIN_LOADING);
    value.value = 0x06u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED,
           "only the leader can begin loading");
    value = command(&lobby, 10u, 5u, MDKR_ONLINE_BEGIN_LOADING);
    value.value = 0x06u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && lobby.phase == MDKR_ONLINE_LOADING &&
           lobby.match_epoch == 1u &&
           (lobby.selected_track == 5u || lobby.selected_track == 7u),
           "ready lobby selects a deterministic vote and enters loading");

    value = command(&lobby, 10u, 6u, MDKR_ONLINE_ACK_LOADED);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "leader acknowledges load");
    value = command(&lobby, 20u, 7u, MDKR_ONLINE_ACK_LOADED);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "peer acknowledges load");
    value = command(&lobby, 10u, 7u, MDKR_ONLINE_BEGIN_RACE);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.phase == MDKR_ONLINE_RACING,
           "leader starts only after every load acknowledgement");
    value = command(&lobby, 10u, 8u, MDKR_ONLINE_PUBLISH_RESULTS);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.phase == MDKR_ONLINE_RESULTS,
           "results are an explicit leader transition");
    value = command(&lobby, 10u, 9u, MDKR_ONLINE_REMATCH);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.phase == MDKR_ONLINE_LOBBY &&
           lobby.match_epoch == 1u && lobby.selected_track == MDKR_ONLINE_NO_VOTE &&
           lobby.selected_vehicle_mask == 0u &&
           !lobby.members[0].ready && !lobby.members[1].ready,
           "rematch preserves room/epoch history but clears round readiness");
}

static void test_atomic_compatibility_capacity_and_idempotency(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    expect(mdkr_online_lobby_init(&lobby, 9u, 100u, &compat, 2u),
           "two-seat leader lobby initializes");
    before = lobby;
    value = command(&lobby, 200u, 1u, MDKR_ONLINE_JOIN);
    value.value = 1u;
    value.compatibility = compat;
    value.compatibility.gameplay_digest[3] ^= 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INCOMPATIBLE &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "gameplay mismatch rejects atomically");

    expect(join(&lobby, 200u, 2u, &compat).accepted,
           "second two-seat endpoint fills room");
    before = lobby;
    step = join(&lobby, 300u, 1u, &compat);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_CAPACITY &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "seat overflow rejects atomically");

    value = command(&lobby, 100u, 1u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 0u;
    value.value = 5u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted, "new command accepted");
    before = lobby;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && step.duplicate &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "exact retry returns cached success without a second mutation");
    value.value = 6u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_COMMAND_CONFLICT &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "same id with different payload fails closed");
    value.command_id = 2u;
    value.expected_revision = lobby.revision - 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_STALE_REVISION,
           "compare-and-swap revision rejects stale concurrent UI state");
    value.command_id = 3u;
    value.expected_revision = lobby.revision;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "higher command id advances the actor high-water mark");
    before = lobby;
    value.command_id = 2u;
    value.expected_revision = lobby.revision - 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_STALE_REVISION &&
               memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "stale revision wins over an unseen lower command id");
    value.expected_revision = lobby.revision;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_STALE_COMMAND &&
               memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "lower command id is stale only at the current revision");
}

static void test_disconnect_and_leader_custody(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    mdkr_online_lobby_init(&lobby, 77u, 50u, &compat, 1u);
    join(&lobby, 20u, 1u, &compat);
    value = command(&lobby, 20u, 2u, MDKR_ONLINE_DISCONNECT);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.seat_count == 2u && !lobby.members[1].connected,
           "disconnect preserves canonical seat custody");
    before = lobby;
    value = command(&lobby, 20u, 3u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_DISCONNECTED &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "disconnected member cannot mutate lobby");
    value = command(&lobby, 20u, 3u, MDKR_ONLINE_RECONNECT);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.members[1].connected && !lobby.members[1].ready,
           "reconnect restores membership but requires fresh readiness");

    value = command(&lobby, 50u, 1u, MDKR_ONLINE_TRANSFER_LEADER);
    value.target_endpoint_id = 20u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && step.leader_changed &&
           lobby.leader_endpoint_id == 20u && lobby.leader_generation == 2u,
           "leader custody transfer is explicit and generation tracked");
    value = command(&lobby, 50u, 2u, MDKR_ONLINE_LEAVE);
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && lobby.member_count == 1u && lobby.seat_count == 1u,
           "nonleader leave releases only its own seats");
    before = lobby;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && step.duplicate &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "leave retry survives member removal through receipt window");
}

static void test_validator_positive_controls(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    mdkr_online_lobby_init(&lobby, 1u, 10u, &compat, 2u);
    expect(mdkr_online_lobby_valid(&lobby), "validator accepts real lobby");
    lobby.seats[1].local_index = 0u;
    expect(!mdkr_online_lobby_valid(&lobby),
           "validator rejects duplicate endpoint-local seat index");
}

static void test_loading_cancel_is_leader_owned_and_repeatable(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    mdkr_online_lobby_init(&lobby, 66u, 10u, &compat, 1u);
    join(&lobby, 20u, 1u, &compat);
    select_value(&lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 1u);
    select_value(&lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 0u);
    select_value(&lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 2u);
    select_value(&lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u);
    value = command(&lobby, 10u, 3u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 0u;
    value.value = 5u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 1u;
    value.value = 5u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 10u, 4u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 20u, 5u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 10u, 5u, MDKR_ONLINE_BEGIN_LOADING);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "cancel fixture reaches loading");

    value = command(&lobby, 20u, 6u, MDKR_ONLINE_CANCEL_LOADING);
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED,
           "nonleader cannot cancel the shared loading barrier");
    value = command(&lobby, 10u, 6u, MDKR_ONLINE_CANCEL_LOADING);
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && lobby.phase == MDKR_ONLINE_LOBBY &&
               !lobby.members[0].ready && !lobby.members[1].ready &&
               lobby.selected_track == MDKR_ONLINE_NO_VOTE,
           "leader cancel returns the intact room to a fresh round");
}

static void test_selection_ownership_conflicts_and_readiness(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    mdkr_online_lobby_init(&lobby, 88u, 10u, &compat, 2u);
    join(&lobby, 20u, 1u, &compat);
    before = lobby;
    value = command(&lobby, 10u, 1u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_NOT_READY &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "ready rejects incomplete per-seat selections atomically");

    expect(select_value(
               &lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 9u).accepted &&
           select_value(
               &lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 1u).accepted,
           "first local seat selection is accepted");
    before = lobby;
    step = select_value(
        &lobby, 10u, 3u, MDKR_ONLINE_SET_CHARACTER, 1u, 9u);
    expect(!step.accepted &&
           step.error == MDKR_ONLINE_ERROR_SELECTION_CONFLICT &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "duplicate character rejects atomically");
    expect(select_value(
               &lobby, 10u, 3u, MDKR_ONLINE_SET_CHARACTER, 1u, 8u).accepted &&
           select_value(
               &lobby, 10u, 4u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u).accepted &&
           select_value(
               &lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 2u, 7u).accepted &&
           select_value(
               &lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 2u, 2u).accepted,
           "remaining owned seats select unique characters and vehicles");

    before = lobby;
    step = select_value(
        &lobby, 10u, 5u, MDKR_ONLINE_SET_VEHICLE, 2u, 0u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "endpoint cannot mutate another endpoint's seat");

    value = command(&lobby, 10u, 5u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.members[0].ready,
           "complete owned selections enable readiness");
    expect(select_value(
               &lobby, 10u, 6u, MDKR_ONLINE_SET_VEHICLE, 0u, 2u).accepted &&
           !lobby.members[0].ready &&
           lobby.seats[0].selection_revision == 3u,
           "selection changes clear readiness and advance seat revision");

    lobby.members[0].ready = true;
    lobby.seats[0].vehicle_id = MDKR_ONLINE_NO_VEHICLE;
    expect(!mdkr_online_lobby_valid(&lobby),
           "validator rejects ready state with an incomplete selection");
}

int main(void) {
    test_shared_service_parity_trace();
    test_service_fingerprint_vector();
    test_lifecycle_and_votes();
    test_atomic_compatibility_capacity_and_idempotency();
    test_disconnect_and_leader_custody();
    test_validator_positive_controls();
    test_loading_cancel_is_leader_owned_and_repeatable();
    test_selection_ownership_conflicts_and_readiness();
    if (failures != 0) return 1;
    puts("online lobby core contract passed");
    return 0;
}
