#ifndef MDKR_ROLLBACK_AUDIO_H
#define MDKR_ROLLBACK_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "rollback_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ROLLBACK_AUDIO_CAPACITY MDKR_ROLLBACK_EVENT_CAPACITY
#define MDKR_ROLLBACK_AUDIO_COMMAND_CAPACITY 2048u

typedef struct MdkrRollbackAudioVoice {
    void *handle;
    uint64_t generation;
} MdkrRollbackAudioVoice;

typedef struct MdkrRollbackAudioRequest MdkrRollbackAudioRequest;

typedef void (*MdkrRollbackAudioStartFn)(
    const MdkrRollbackAudioRequest *request,
    MdkrRollbackAudioVoice *voice, void *context);
typedef void (*MdkrRollbackAudioCancelFn)(
    const MdkrRollbackAudioRequest *request,
    MdkrRollbackAudioVoice voice, void *context);

typedef enum MdkrRollbackAudioMode {
    MDKR_ROLLBACK_AUDIO_TABLE = 0,
    MDKR_ROLLBACK_AUDIO_DIRECT,
    MDKR_ROLLBACK_AUDIO_SPATIAL
} MdkrRollbackAudioMode;

struct MdkrRollbackAudioRequest {
    MdkrRollbackEventId id;
    uint32_t sound_id;
    uint8_t mode;
    float x;
    float y;
    float z;
    void *handle_slot;
    MdkrRollbackAudioStartFn start;
    MdkrRollbackAudioCancelFn cancel;
    void *context;
};

typedef struct MdkrRollbackAudioEntry {
    MdkrRollbackAudioRequest request;
    MdkrRollbackAudioVoice voice;
    bool previewed;
} MdkrRollbackAudioEntry;

typedef struct MdkrRollbackAudioCommand MdkrRollbackAudioCommand;
typedef void (*MdkrRollbackAudioApplyFn)(
    const MdkrRollbackAudioCommand *command, void *context);

struct MdkrRollbackAudioCommand {
    void *resource;
    uint64_t generation;
    uint32_t operation;
    uint32_t value;
    void *handle_slot;
    MdkrRollbackAudioApplyFn apply;
    void *context;
};

typedef struct MdkrRollbackAudioStats {
    uint64_t started;
    uint64_t suppressed;
    uint64_t deferred;
    uint64_t cancelled;
    uint64_t confirmed;
    uint64_t rejected;
    uint64_t commands_deferred;
    uint64_t commands_coalesced;
    uint64_t commands_applied;
    uint64_t command_overflows;
} MdkrRollbackAudioStats;

typedef struct MdkrRollbackAudioAdapter {
    MdkrRollbackAudioEntry entries[MDKR_ROLLBACK_AUDIO_CAPACITY];
    unsigned count;
    MdkrRollbackAudioCommand commands[MDKR_ROLLBACK_AUDIO_COMMAND_CAPACITY];
    unsigned command_count;
    MdkrRollbackAudioStats stats;
} MdkrRollbackAudioAdapter;

void mdkr_rollback_audio_init(MdkrRollbackAudioAdapter *adapter);
bool mdkr_rollback_audio_request(
    MdkrRollbackAudioAdapter *adapter,
    const MdkrRollbackAudioRequest *request, bool resimulating);
bool mdkr_rollback_audio_preview(
    MdkrRollbackAudioAdapter *adapter, MdkrRollbackEventId id);
bool mdkr_rollback_audio_cancel(
    MdkrRollbackAudioAdapter *adapter, MdkrRollbackEventId id);
void mdkr_rollback_audio_confirm_through(
    MdkrRollbackAudioAdapter *adapter, uint32_t tick);
void mdkr_rollback_audio_forget_all(MdkrRollbackAudioAdapter *adapter);
bool mdkr_rollback_audio_defer_command(
    MdkrRollbackAudioAdapter *adapter,
    const MdkrRollbackAudioCommand *command);
void mdkr_rollback_audio_flush_commands(MdkrRollbackAudioAdapter *adapter);
void mdkr_rollback_audio_discard_commands(MdkrRollbackAudioAdapter *adapter);

#ifdef __cplusplus
}
#endif
#endif
