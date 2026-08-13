#include "lobby_core.h"

#include <stddef.h>
#include <string.h>

static bool any_nonzero(const uint8_t *bytes, size_t size) {
    uint8_t value = 0u;
    size_t index;
    for (index = 0u; index < size; index++) value |= bytes[index];
    return value != 0u;
}

bool mdkr_online_compatibility_valid(const MdkrOnlineCompatibilityV1 *value) {
    return value != NULL &&
           value->protocol_version == MDKR_ONLINE_PROTOCOL_VERSION &&
           any_nonzero(value->build_id, sizeof(value->build_id)) &&
           any_nonzero(value->gameplay_digest, sizeof(value->gameplay_digest)) &&
           (value->rom_revision == 1u || value->rom_revision == 2u) &&
           (value->cadence_hz == 25u || value->cadence_hz == 30u);
}

static MdkrOnlineMember *member(MdkrOnlineLobby *lobby, uint64_t endpoint_id) {
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        if (lobby->members[index].occupied &&
            lobby->members[index].endpoint_id == endpoint_id) {
            return &lobby->members[index];
        }
    }
    return NULL;
}

static const MdkrOnlineMember *member_const(
    const MdkrOnlineLobby *lobby, uint64_t endpoint_id) {
    return member((MdkrOnlineLobby *)lobby, endpoint_id);
}

static unsigned free_member_index(const MdkrOnlineLobby *lobby) {
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        if (!lobby->members[index].occupied) return index;
    }
    return MDKR_ONLINE_MAX_ENDPOINTS;
}

static unsigned free_seats(const MdkrOnlineLobby *lobby) {
    return MDKR_ONLINE_MAX_SEATS - lobby->seat_count;
}

static bool add_member(
    MdkrOnlineLobby *lobby, uint64_t endpoint_id, unsigned seat_count,
    MdkrOnlineMember **output) {
    unsigned member_index;
    unsigned seat_index;
    unsigned assigned = 0u;
    MdkrOnlineMember *next;
    if (endpoint_id == 0u || seat_count == 0u ||
        seat_count > MDKR_ONLINE_MAX_SEATS_PER_ENDPOINT ||
        lobby->member_count >= MDKR_ONLINE_MAX_ENDPOINTS ||
        free_seats(lobby) < seat_count || member(lobby, endpoint_id) != NULL) {
        return false;
    }
    member_index = free_member_index(lobby);
    if (member_index >= MDKR_ONLINE_MAX_ENDPOINTS) return false;
    next = &lobby->members[member_index];
    memset(next, 0, sizeof(*next));
    next->endpoint_id = endpoint_id;
    next->seat_count = (uint8_t)seat_count;
    next->occupied = true;
    next->connected = true;
    for (seat_index = 0u;
         seat_index < MDKR_ONLINE_MAX_SEATS && assigned < seat_count;
         seat_index++) {
        MdkrOnlineSeat *seat = &lobby->seats[seat_index];
        if (seat->occupied) continue;
        seat->endpoint_id = endpoint_id;
        seat->local_index = (uint8_t)assigned++;
        seat->vote_track = MDKR_ONLINE_NO_VOTE;
        seat->character_id = MDKR_ONLINE_NO_CHARACTER;
        seat->vehicle_id = MDKR_ONLINE_NO_VEHICLE;
        seat->occupied = true;
    }
    lobby->member_count++;
    lobby->seat_count = (uint8_t)(lobby->seat_count + seat_count);
    if (output != NULL) *output = next;
    return assigned == seat_count;
}

bool mdkr_online_lobby_init(
    MdkrOnlineLobby *lobby, uint64_t room_id, uint64_t leader_endpoint_id,
    const MdkrOnlineCompatibilityV1 *compatibility, unsigned leader_seats) {
    if (lobby == NULL || room_id == 0u || leader_endpoint_id == 0u ||
        !mdkr_online_compatibility_valid(compatibility)) return false;
    memset(lobby, 0, sizeof(*lobby));
    lobby->protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    lobby->revision = 1u;
    lobby->leader_generation = 1u;
    lobby->room_id = room_id;
    lobby->leader_endpoint_id = leader_endpoint_id;
    lobby->phase = MDKR_ONLINE_LOBBY;
    lobby->compatibility = *compatibility;
    lobby->selected_track = MDKR_ONLINE_NO_VOTE;
    if (!add_member(lobby, leader_endpoint_id, leader_seats, NULL)) {
        memset(lobby, 0, sizeof(*lobby));
        return false;
    }
    return true;
}

bool mdkr_online_lobby_valid(const MdkrOnlineLobby *lobby) {
    unsigned members = 0u;
    unsigned seats = 0u;
    unsigned index;
    if (lobby == NULL || lobby->protocol_version != MDKR_ONLINE_PROTOCOL_VERSION ||
        lobby->revision == 0u || lobby->room_id == 0u ||
        lobby->leader_endpoint_id == 0u ||
        lobby->leader_generation == 0u ||
        lobby->phase < MDKR_ONLINE_LOBBY || lobby->phase > MDKR_ONLINE_CLOSED ||
        lobby->member_count == 0u || lobby->member_count > MDKR_ONLINE_MAX_ENDPOINTS ||
        lobby->seat_count == 0u || lobby->seat_count > MDKR_ONLINE_MAX_SEATS ||
        lobby->next_receipt >= MDKR_ONLINE_RECEIPT_WINDOW ||
        !mdkr_online_compatibility_valid(&lobby->compatibility) ||
        member_const(lobby, lobby->leader_endpoint_id) == NULL) return false;
    if ((lobby->phase == MDKR_ONLINE_LOBBY &&
         (lobby->selected_track != MDKR_ONLINE_NO_VOTE ||
          lobby->selected_vehicle_mask != 0u)) ||
        (lobby->phase >= MDKR_ONLINE_LOADING &&
         lobby->phase <= MDKR_ONLINE_RESULTS &&
         (lobby->selected_track == MDKR_ONLINE_NO_VOTE ||
          lobby->selected_vehicle_mask == 0u ||
          (lobby->selected_vehicle_mask &
           (uint8_t)~MDKR_ONLINE_PLAYER_VEHICLE_MASK) != 0u ||
          lobby->match_epoch == 0u))) return false;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        const MdkrOnlineMember *item = &lobby->members[index];
        unsigned owned = 0u;
        unsigned local_mask = 0u;
        bool selections_complete = true;
        unsigned seat_index;
        if (!item->occupied) continue;
        if (item->endpoint_id == 0u || item->seat_count == 0u ||
            item->seat_count > MDKR_ONLINE_MAX_SEATS_PER_ENDPOINT) return false;
        members++;
        for (seat_index = index + 1u;
             seat_index < MDKR_ONLINE_MAX_ENDPOINTS; seat_index++) {
            if (lobby->members[seat_index].occupied &&
                lobby->members[seat_index].endpoint_id == item->endpoint_id)
                return false;
        }
        if (lobby->phase == MDKR_ONLINE_LOBBY && item->loaded) return false;
        for (seat_index = 0u; seat_index < MDKR_ONLINE_MAX_SEATS; seat_index++) {
            const MdkrOnlineSeat *seat = &lobby->seats[seat_index];
            if (seat->occupied && seat->endpoint_id == item->endpoint_id) {
                if (seat->local_index >= item->seat_count) return false;
                if ((local_mask & (1u << seat->local_index)) != 0u) return false;
                local_mask |= 1u << seat->local_index;
                if (seat->character_id == MDKR_ONLINE_NO_CHARACTER ||
                    seat->vehicle_id == MDKR_ONLINE_NO_VEHICLE)
                    selections_complete = false;
                owned++;
            }
        }
        if (owned != item->seat_count || (item->ready && !selections_complete))
            return false;
    }
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        const MdkrOnlineSeat *seat = &lobby->seats[index];
        unsigned other;
        if (!seat->occupied) continue;
        if (seat->endpoint_id == 0u ||
            member_const(lobby, seat->endpoint_id) == NULL ||
            (seat->character_id != MDKR_ONLINE_NO_CHARACTER &&
             seat->character_id >= MDKR_ONLINE_CHARACTER_COUNT) ||
            (seat->vehicle_id != MDKR_ONLINE_NO_VEHICLE &&
             seat->vehicle_id >= MDKR_ONLINE_PLAYER_VEHICLE_COUNT) ||
            ((seat->character_id != MDKR_ONLINE_NO_CHARACTER ||
              seat->vehicle_id != MDKR_ONLINE_NO_VEHICLE) !=
             (seat->selection_revision != 0u))) return false;
        for (other = 0u; other < index; other++) {
            if (lobby->seats[other].occupied &&
                seat->character_id != MDKR_ONLINE_NO_CHARACTER &&
                lobby->seats[other].character_id == seat->character_id)
                return false;
        }
        if (lobby->phase >= MDKR_ONLINE_LOADING &&
            lobby->phase <= MDKR_ONLINE_RESULTS &&
            (seat->character_id == MDKR_ONLINE_NO_CHARACTER ||
             seat->vehicle_id == MDKR_ONLINE_NO_VEHICLE ||
             (lobby->selected_vehicle_mask &
              (uint8_t)(1u << seat->vehicle_id)) == 0u)) return false;
        seats++;
    }
    return members == lobby->member_count && seats == lobby->seat_count &&
           members > 0u && seats > 0u;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    unsigned index;
    for (index = 0u; index < 8u; index++) {
        hash ^= (uint8_t)(value >> (index * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t command_fingerprint(const MdkrOnlineCommand *command) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    hash = hash_u64(hash, (uint64_t)command->type);
    hash = hash_u64(hash, command->value);
    hash = hash_u64(hash, command->target_endpoint_id);
    hash = hash_u64(hash, command->compatibility.protocol_version);
    for (index = 0u; index < sizeof(command->compatibility.build_id); index++)
        hash = hash_u64(hash, command->compatibility.build_id[index]);
    for (index = 0u; index < sizeof(command->compatibility.gameplay_digest); index++)
        hash = hash_u64(hash, command->compatibility.gameplay_digest[index]);
    hash = hash_u64(hash, command->compatibility.rom_revision);
    return hash_u64(hash, command->compatibility.cadence_hz);
}

static const MdkrOnlineCommandReceipt *receipt_for(
    const MdkrOnlineLobby *lobby, uint64_t actor_endpoint_id,
    uint64_t command_id) {
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_RECEIPT_WINDOW; index++) {
        const MdkrOnlineCommandReceipt *receipt = &lobby->receipts[index];
        if (receipt->actor_endpoint_id == actor_endpoint_id &&
            receipt->command_id == command_id) return receipt;
    }
    return NULL;
}

static void retain_receipt(
    MdkrOnlineLobby *lobby, uint64_t actor_endpoint_id, uint64_t command_id,
    uint64_t fingerprint) {
    MdkrOnlineCommandReceipt *receipt = &lobby->receipts[lobby->next_receipt];
    receipt->actor_endpoint_id = actor_endpoint_id;
    receipt->command_id = command_id;
    receipt->fingerprint = fingerprint;
    lobby->next_receipt = (uint8_t)((lobby->next_receipt + 1u) %
                                    MDKR_ONLINE_RECEIPT_WINDOW);
}

static MdkrOnlineStep step_for(
    const MdkrOnlineLobby *lobby, bool accepted, bool duplicate,
    bool leader_changed, MdkrOnlineError error) {
    MdkrOnlineStep step;
    memset(&step, 0, sizeof(step));
    step.accepted = accepted;
    step.duplicate = duplicate;
    step.leader_changed = leader_changed;
    step.error = error;
    if (lobby != NULL) {
        step.revision = lobby->revision;
        step.match_epoch = lobby->match_epoch;
        step.leader_endpoint_id = lobby->leader_endpoint_id;
        step.selected_track = lobby->selected_track;
        step.selected_vehicle_mask = lobby->selected_vehicle_mask;
    }
    return step;
}

static bool compatible(
    const MdkrOnlineCompatibilityV1 *left,
    const MdkrOnlineCompatibilityV1 *right) {
    return left->protocol_version == right->protocol_version &&
           left->rom_revision == right->rom_revision &&
           left->cadence_hz == right->cadence_hz &&
           memcmp(left->build_id, right->build_id, sizeof(left->build_id)) == 0 &&
           memcmp(left->gameplay_digest, right->gameplay_digest,
                  sizeof(left->gameplay_digest)) == 0;
}

static bool all_ready(const MdkrOnlineLobby *lobby) {
    unsigned index;
    if (lobby->member_count < 2u) return false;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        const MdkrOnlineMember *item = &lobby->members[index];
        if (item->occupied && (!item->connected || !item->ready)) return false;
    }
    return true;
}

static bool member_selection_complete(
    const MdkrOnlineLobby *lobby, uint64_t endpoint_id) {
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        const MdkrOnlineSeat *seat = &lobby->seats[index];
        if (seat->occupied && seat->endpoint_id == endpoint_id &&
            (seat->character_id == MDKR_ONLINE_NO_CHARACTER ||
             seat->vehicle_id == MDKR_ONLINE_NO_VEHICLE)) return false;
    }
    return true;
}

static bool all_vehicles_legal(
    const MdkrOnlineLobby *lobby, uint8_t vehicle_mask) {
    unsigned index;
    if (vehicle_mask == 0u ||
        (vehicle_mask & (uint8_t)~MDKR_ONLINE_PLAYER_VEHICLE_MASK) != 0u)
        return false;
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        const MdkrOnlineSeat *seat = &lobby->seats[index];
        if (seat->occupied &&
            (seat->vehicle_id >= MDKR_ONLINE_PLAYER_VEHICLE_COUNT ||
             (vehicle_mask & (uint8_t)(1u << seat->vehicle_id)) == 0u))
            return false;
    }
    return true;
}

static bool all_loaded(const MdkrOnlineLobby *lobby) {
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        const MdkrOnlineMember *item = &lobby->members[index];
        if (item->occupied && (!item->connected || !item->loaded)) return false;
    }
    return true;
}

static uint16_t select_track(const MdkrOnlineLobby *lobby) {
    uint8_t counts[256] = {0};
    uint16_t candidates[MDKR_ONLINE_MAX_SEATS];
    unsigned candidate_count = 0u;
    unsigned best = 0u;
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        if (lobby->seats[index].occupied &&
            lobby->seats[index].vote_track != MDKR_ONLINE_NO_VOTE &&
            lobby->seats[index].vote_track <= 255u) {
            unsigned count = ++counts[lobby->seats[index].vote_track];
            if (count > best) best = count;
        }
    }
    if (best == 0u) return MDKR_ONLINE_NO_VOTE;
    for (index = 0u; index < 256u; index++) {
        if (counts[index] == best) candidates[candidate_count++] = (uint16_t)index;
    }
    return candidates[(unsigned)((lobby->room_id ^ (lobby->match_epoch + 1u)) %
                                 candidate_count)];
}

static void clear_round(MdkrOnlineLobby *lobby) {
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        lobby->members[index].ready = false;
        lobby->members[index].loaded = false;
    }
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++)
        lobby->seats[index].vote_track = MDKR_ONLINE_NO_VOTE;
    lobby->selected_track = MDKR_ONLINE_NO_VOTE;
    lobby->selected_vehicle_mask = 0u;
}

static bool remove_member(MdkrOnlineLobby *lobby, uint64_t endpoint_id) {
    MdkrOnlineMember *item = member(lobby, endpoint_id);
    unsigned index;
    if (item == NULL || lobby->member_count <= 1u) return false;
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        if (lobby->seats[index].occupied &&
            lobby->seats[index].endpoint_id == endpoint_id) {
            memset(&lobby->seats[index], 0, sizeof(lobby->seats[index]));
            lobby->seats[index].vote_track = MDKR_ONLINE_NO_VOTE;
            lobby->seat_count--;
        }
    }
    memset(item, 0, sizeof(*item));
    lobby->member_count--;
    return true;
}

static uint64_t lowest_connected_endpoint(const MdkrOnlineLobby *lobby) {
    uint64_t chosen = 0u;
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        const MdkrOnlineMember *item = &lobby->members[index];
        if (item->occupied && item->connected &&
            (chosen == 0u || item->endpoint_id < chosen)) chosen = item->endpoint_id;
    }
    return chosen;
}

MdkrOnlineStep mdkr_online_lobby_dispatch(
    MdkrOnlineLobby *lobby, const MdkrOnlineCommand *command) {
    MdkrOnlineLobby next;
    MdkrOnlineMember *actor;
    const MdkrOnlineCommandReceipt *receipt;
    uint64_t fingerprint;
    bool leader_changed = false;
    unsigned index;
    if (lobby == NULL || command == NULL ||
        command->protocol_version != MDKR_ONLINE_PROTOCOL_VERSION)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_PROTOCOL);
    if (!mdkr_online_lobby_valid(lobby) || command->actor_endpoint_id == 0u ||
        command->command_id == 0u)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_INVALID_STATE);
    actor = member(lobby, command->actor_endpoint_id);
    fingerprint = command_fingerprint(command);
    receipt = receipt_for(lobby, command->actor_endpoint_id, command->command_id);
    if (receipt != NULL) {
        if (receipt->fingerprint != fingerprint)
            return step_for(lobby, false, false, false,
                            MDKR_ONLINE_ERROR_COMMAND_CONFLICT);
        return step_for(lobby, true, true, false, MDKR_ONLINE_OK);
    }
    if (actor != NULL && command->command_id == actor->last_command_id) {
        if (fingerprint != actor->last_command_fingerprint)
            return step_for(lobby, false, false, false,
                            MDKR_ONLINE_ERROR_COMMAND_CONFLICT);
        return step_for(lobby, true, true, false, MDKR_ONLINE_OK);
    }
    if (command->expected_revision != lobby->revision)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_STALE_REVISION);
    if (actor != NULL && command->command_id < actor->last_command_id)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_STALE_COMMAND);
    if (lobby->phase == MDKR_ONLINE_CLOSED)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_INVALID_STATE);
    if (actor == NULL && command->type != MDKR_ONLINE_JOIN)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_NOT_FOUND);
    if (actor != NULL && !actor->connected &&
        command->type != MDKR_ONLINE_RECONNECT &&
        command->type != MDKR_ONLINE_LEAVE)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_DISCONNECTED);
    next = *lobby;
    actor = member(&next, command->actor_endpoint_id);

    switch (command->type) {
        case MDKR_ONLINE_JOIN:
            if (actor != NULL) return step_for(lobby, false, false, false,
                                               MDKR_ONLINE_ERROR_ALREADY_JOINED);
            if (next.phase != MDKR_ONLINE_LOBBY)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            if (!mdkr_online_compatibility_valid(&command->compatibility) ||
                !compatible(&next.compatibility, &command->compatibility))
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INCOMPATIBLE);
            if (!add_member(&next, command->actor_endpoint_id, command->value,
                            &actor))
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_CAPACITY);
            break;
        case MDKR_ONLINE_LEAVE:
            if (next.phase != MDKR_ONLINE_LOBBY && next.phase != MDKR_ONLINE_RESULTS)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            if (!remove_member(&next, command->actor_endpoint_id))
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            if (next.leader_endpoint_id == command->actor_endpoint_id) {
                next.leader_endpoint_id = lowest_connected_endpoint(&next);
                next.leader_generation++;
                leader_changed = true;
            }
            actor = NULL;
            break;
        case MDKR_ONLINE_DISCONNECT:
            actor->connected = false;
            actor->ready = false;
            actor->loaded = false;
            break;
        case MDKR_ONLINE_RECONNECT:
            actor->connected = true;
            actor->ready = false;
            actor->loaded = false;
            break;
        case MDKR_ONLINE_SET_READY:
            if (next.phase != MDKR_ONLINE_LOBBY || command->value > 1u)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            if (command->value != 0u && !member_selection_complete(
                    &next, command->actor_endpoint_id))
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_NOT_READY);
            actor->ready = command->value != 0u;
            break;
        case MDKR_ONLINE_SET_VOTE:
            if (next.phase != MDKR_ONLINE_LOBBY || command->value > 255u ||
                command->target_endpoint_id >= MDKR_ONLINE_MAX_SEATS)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            index = (unsigned)command->target_endpoint_id;
            if (!next.seats[index].occupied ||
                next.seats[index].endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            next.seats[index].vote_track = (uint16_t)command->value;
            actor->ready = false;
            break;
        case MDKR_ONLINE_SET_CHARACTER:
        case MDKR_ONLINE_SET_VEHICLE:
            if (next.phase != MDKR_ONLINE_LOBBY ||
                command->target_endpoint_id >= MDKR_ONLINE_MAX_SEATS)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            index = (unsigned)command->target_endpoint_id;
            if (!next.seats[index].occupied ||
                next.seats[index].endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            if (command->type == MDKR_ONLINE_SET_CHARACTER) {
                unsigned other;
                if (command->value >= MDKR_ONLINE_CHARACTER_COUNT)
                    return step_for(lobby, false, false, false,
                                    MDKR_ONLINE_ERROR_INVALID_STATE);
                for (other = 0u; other < MDKR_ONLINE_MAX_SEATS; other++) {
                    if (other != index && next.seats[other].occupied &&
                        next.seats[other].character_id == command->value)
                        return step_for(
                            lobby, false, false, false,
                            MDKR_ONLINE_ERROR_SELECTION_CONFLICT);
                }
                next.seats[index].character_id = (uint8_t)command->value;
            } else {
                if (command->value >= MDKR_ONLINE_PLAYER_VEHICLE_COUNT)
                    return step_for(lobby, false, false, false,
                                    MDKR_ONLINE_ERROR_INVALID_STATE);
                next.seats[index].vehicle_id = (uint8_t)command->value;
            }
            if (next.seats[index].selection_revision == UINT32_MAX)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            next.seats[index].selection_revision++;
            actor->ready = false;
            break;
        case MDKR_ONLINE_BEGIN_LOADING:
            if (next.leader_endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            if (next.phase != MDKR_ONLINE_LOBBY || !all_ready(&next))
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_NOT_READY);
            next.selected_track = select_track(&next);
            if (next.selected_track == MDKR_ONLINE_NO_VOTE)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_NOT_READY);
            if (!all_vehicles_legal(&next, (uint8_t)command->value))
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_ILLEGAL_VEHICLE);
            next.selected_vehicle_mask = (uint8_t)command->value;
            next.match_epoch++;
            next.phase = MDKR_ONLINE_LOADING;
            for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++)
                next.members[index].loaded = false;
            break;
        case MDKR_ONLINE_ACK_LOADED:
            if (next.phase != MDKR_ONLINE_LOADING)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            actor->loaded = true;
            break;
        case MDKR_ONLINE_BEGIN_RACE:
            if (next.leader_endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            if (next.phase != MDKR_ONLINE_LOADING || !all_loaded(&next))
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_NOT_READY);
            next.phase = MDKR_ONLINE_RACING;
            break;
        case MDKR_ONLINE_CANCEL_LOADING:
            if (next.leader_endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            if (next.phase != MDKR_ONLINE_LOADING)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            next.phase = MDKR_ONLINE_LOBBY;
            clear_round(&next);
            break;
        case MDKR_ONLINE_PUBLISH_RESULTS:
            if (next.leader_endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            if (next.phase != MDKR_ONLINE_RACING)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            next.phase = MDKR_ONLINE_RESULTS;
            break;
        case MDKR_ONLINE_REMATCH:
            if (next.leader_endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            if (next.phase != MDKR_ONLINE_RESULTS)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_INVALID_STATE);
            next.phase = MDKR_ONLINE_LOBBY;
            clear_round(&next);
            break;
        case MDKR_ONLINE_TRANSFER_LEADER: {
            MdkrOnlineMember *target;
            if (next.leader_endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            target = member(&next, command->target_endpoint_id);
            if (target == NULL || !target->connected)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_NOT_FOUND);
            if (next.leader_endpoint_id != target->endpoint_id) {
                next.leader_endpoint_id = target->endpoint_id;
                next.leader_generation++;
                leader_changed = true;
            }
            break;
        }
        case MDKR_ONLINE_CLOSE:
            if (next.leader_endpoint_id != command->actor_endpoint_id)
                return step_for(lobby, false, false, false,
                                MDKR_ONLINE_ERROR_UNAUTHORIZED);
            next.phase = MDKR_ONLINE_CLOSED;
            break;
        default:
            return step_for(lobby, false, false, false,
                            MDKR_ONLINE_ERROR_INVALID_STATE);
    }

    if (actor != NULL) {
        actor->last_command_id = command->command_id;
        actor->last_command_fingerprint = fingerprint;
    }
    retain_receipt(&next, command->actor_endpoint_id, command->command_id,
                   fingerprint);
    if (next.revision == UINT32_MAX)
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_INVALID_STATE);
    next.revision++;
    if (!mdkr_online_lobby_valid(&next))
        return step_for(lobby, false, false, false, MDKR_ONLINE_ERROR_INVALID_STATE);
    *lobby = next;
    return step_for(lobby, true, false, leader_changed, MDKR_ONLINE_OK);
}
