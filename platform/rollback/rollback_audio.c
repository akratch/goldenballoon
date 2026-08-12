#include "rollback_audio.h"

#include <string.h>

static bool same_id(MdkrRollbackEventId left, MdkrRollbackEventId right) {
    return left.tick == right.tick && left.emitter == right.emitter &&
        left.ordinal == right.ordinal && left.kind == right.kind;
}

static MdkrRollbackAudioEntry *find_entry(
    MdkrRollbackAudioAdapter *adapter, MdkrRollbackEventId id,
    unsigned *index_out) {
    unsigned index;
    if (adapter == NULL) return NULL;
    for (index = 0u; index < adapter->count; index++) {
        if (!same_id(adapter->entries[index].request.id, id)) continue;
        if (index_out != NULL) *index_out = index;
        return &adapter->entries[index];
    }
    return NULL;
}

static bool same_binding(
    const MdkrRollbackAudioRequest *left,
    const MdkrRollbackAudioRequest *right) {
    return left->sound_id == right->sound_id && left->mode == right->mode &&
        left->handle_slot == right->handle_slot && left->start == right->start &&
        left->cancel == right->cancel && left->context == right->context;
}

static void start_entry(
    MdkrRollbackAudioAdapter *adapter, MdkrRollbackAudioEntry *entry) {
    entry->previewed = true;
    entry->voice = (MdkrRollbackAudioVoice){0};
    entry->request.start(
        &entry->request, &entry->voice, entry->request.context);
    adapter->stats.started++;
}

static void remove_entry(MdkrRollbackAudioAdapter *adapter, unsigned index) {
    if (index + 1u < adapter->count) {
        memmove(
            &adapter->entries[index], &adapter->entries[index + 1u],
            (adapter->count - index - 1u) * sizeof(adapter->entries[0]));
    }
    adapter->count--;
}

void mdkr_rollback_audio_init(MdkrRollbackAudioAdapter *adapter) {
    if (adapter != NULL) memset(adapter, 0, sizeof(*adapter));
}

bool mdkr_rollback_audio_request(
    MdkrRollbackAudioAdapter *adapter,
    const MdkrRollbackAudioRequest *request, bool resimulating) {
    MdkrRollbackAudioEntry *entry;
    if (adapter == NULL || request == NULL || request->id.kind == 0u ||
        request->start == NULL || request->cancel == NULL ||
        request->mode > MDKR_ROLLBACK_AUDIO_SPATIAL) {
        if (adapter != NULL) adapter->stats.rejected++;
        return false;
    }
    entry = find_entry(adapter, request->id, NULL);
    if (entry != NULL) {
        if (!same_binding(&entry->request, request)) {
            adapter->stats.rejected++;
            return false;
        }
        /* Coordinates are presentation payload. Identity includes their stable
         * hash, so updating them here cannot silently mutate one event into a
         * different effect. */
        entry->request.x = request->x;
        entry->request.y = request->y;
        entry->request.z = request->z;
        if (resimulating) {
            adapter->stats.suppressed++;
            return true;
        }
        adapter->stats.rejected++;
        return false;
    }
    if (adapter->count >= MDKR_ROLLBACK_AUDIO_CAPACITY) {
        adapter->stats.rejected++;
        return false;
    }
    entry = &adapter->entries[adapter->count++];
    memset(entry, 0, sizeof(*entry));
    entry->request = *request;
    if (resimulating) {
        adapter->stats.deferred++;
    } else {
        start_entry(adapter, entry);
    }
    return true;
}

bool mdkr_rollback_audio_preview(
    MdkrRollbackAudioAdapter *adapter, MdkrRollbackEventId id) {
    MdkrRollbackAudioEntry *entry = find_entry(adapter, id, NULL);
    if (entry == NULL) return false;
    if (!entry->previewed) start_entry(adapter, entry);
    return true;
}

bool mdkr_rollback_audio_cancel(
    MdkrRollbackAudioAdapter *adapter, MdkrRollbackEventId id) {
    unsigned index;
    MdkrRollbackAudioEntry *entry = find_entry(adapter, id, &index);
    if (entry == NULL) return false;
    if (entry->previewed && entry->voice.handle != NULL &&
        entry->voice.generation != 0u) {
        entry->request.cancel(
            &entry->request, entry->voice, entry->request.context);
        adapter->stats.cancelled++;
    }
    remove_entry(adapter, index);
    return true;
}

void mdkr_rollback_audio_confirm_through(
    MdkrRollbackAudioAdapter *adapter, uint32_t tick) {
    unsigned index = 0u;
    if (adapter == NULL) return;
    while (index < adapter->count) {
        if (adapter->entries[index].request.id.tick <= tick) {
            remove_entry(adapter, index);
            adapter->stats.confirmed++;
        } else {
            index++;
        }
    }
}

void mdkr_rollback_audio_forget_all(MdkrRollbackAudioAdapter *adapter) {
    if (adapter != NULL) {
        adapter->count = 0u;
        adapter->command_count = 0u;
    }
}

bool mdkr_rollback_audio_defer_command(
    MdkrRollbackAudioAdapter *adapter,
    const MdkrRollbackAudioCommand *command) {
    unsigned index;
    if (adapter == NULL || command == NULL || command->resource == NULL ||
        command->generation == 0u || command->operation == 0u ||
        command->apply == NULL) {
        if (adapter != NULL) adapter->stats.rejected++;
        return false;
    }
    for (index = 0u; index < adapter->command_count; index++) {
        MdkrRollbackAudioCommand *existing = &adapter->commands[index];
        if (existing->resource == command->resource &&
            existing->generation == command->generation &&
            existing->operation == command->operation &&
            existing->handle_slot == command->handle_slot &&
            existing->apply == command->apply &&
            existing->context == command->context) {
            existing->value = command->value;
            adapter->stats.commands_coalesced++;
            return true;
        }
    }
    if (adapter->command_count >= MDKR_ROLLBACK_AUDIO_COMMAND_CAPACITY) {
        adapter->stats.command_overflows++;
        return false;
    }
    adapter->commands[adapter->command_count++] = *command;
    adapter->stats.commands_deferred++;
    return true;
}

void mdkr_rollback_audio_flush_commands(MdkrRollbackAudioAdapter *adapter) {
    unsigned index;
    if (adapter == NULL) return;
    for (index = 0u; index < adapter->command_count; index++) {
        MdkrRollbackAudioCommand *command = &adapter->commands[index];
        command->apply(command, command->context);
        adapter->stats.commands_applied++;
    }
    adapter->command_count = 0u;
}

void mdkr_rollback_audio_discard_commands(MdkrRollbackAudioAdapter *adapter) {
    if (adapter != NULL) adapter->command_count = 0u;
}
