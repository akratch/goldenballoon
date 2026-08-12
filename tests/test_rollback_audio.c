/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "platform/rollback/rollback_audio.h"

typedef struct Fixture {
    unsigned starts;
    unsigned cancels;
    uintptr_t next_handle;
    uint64_t next_generation;
    unsigned commands;
    uint32_t last_value;
} Fixture;

static void start_voice(
    const MdkrRollbackAudioRequest *request,
    MdkrRollbackAudioVoice *voice, void *context) {
    Fixture *fixture = (Fixture *)context;
    (void)request;
    fixture->starts++;
    voice->handle = (void *)++fixture->next_handle;
    voice->generation = ++fixture->next_generation;
}

static void cancel_voice(
    const MdkrRollbackAudioRequest *request,
    MdkrRollbackAudioVoice voice, void *context) {
    Fixture *fixture = (Fixture *)context;
    assert(request->handle_slot == (void *)(uintptr_t)0x1234u);
    assert(voice.handle != NULL);
    assert(voice.generation != 0u);
    fixture->cancels++;
}

static MdkrRollbackAudioRequest request(
    Fixture *fixture, uint32_t tick, uint32_t emitter, uint16_t ordinal) {
    MdkrRollbackAudioRequest value = {0};
    value.id = (MdkrRollbackEventId){tick, emitter, ordinal, 1u};
    value.sound_id = 42u;
    value.mode = MDKR_ROLLBACK_AUDIO_SPATIAL;
    value.x = 1.0f;
    value.y = 2.0f;
    value.z = 3.0f;
    value.handle_slot = (void *)(uintptr_t)0x1234u;
    value.start = start_voice;
    value.cancel = cancel_voice;
    value.context = fixture;
    return value;
}

static void apply_command(
    const MdkrRollbackAudioCommand *command, void *context) {
    Fixture *fixture = (Fixture *)context;
    fixture->commands++;
    fixture->last_value = command->value;
}

int main(void) {
    MdkrRollbackAudioAdapter adapter;
    Fixture fixture = {0};
    MdkrRollbackAudioRequest predicted = request(&fixture, 10u, 100u, 0u);
    MdkrRollbackAudioRequest corrected = request(&fixture, 11u, 101u, 0u);
    MdkrRollbackAudioRequest invalid;
    MdkrRollbackAudioCommand command = {
        (void *)(uintptr_t)0x99u, 7u, 3u, 10u, NULL,
        apply_command, &fixture};

    mdkr_rollback_audio_init(&adapter);
    assert(mdkr_rollback_audio_request(&adapter, &predicted, false));
    assert(fixture.starts == 1u);
    assert(adapter.count == 1u);

    predicted.x = 4.0f;
    assert(mdkr_rollback_audio_request(&adapter, &predicted, true));
    assert(fixture.starts == 1u);
    assert(adapter.stats.suppressed == 1u);

    assert(mdkr_rollback_audio_request(&adapter, &corrected, true));
    assert(fixture.starts == 1u);
    assert(adapter.stats.deferred == 1u);
    assert(mdkr_rollback_audio_preview(&adapter, corrected.id));
    assert(fixture.starts == 2u);
    assert(mdkr_rollback_audio_cancel(&adapter, predicted.id));
    assert(fixture.cancels == 1u);

    mdkr_rollback_audio_confirm_through(&adapter, 11u);
    assert(adapter.count == 0u);
    assert(adapter.stats.confirmed == 1u);

    invalid = request(&fixture, 12u, 102u, 0u);
    invalid.start = NULL;
    assert(!mdkr_rollback_audio_request(&adapter, &invalid, false));
    assert(adapter.stats.rejected == 1u);

    assert(mdkr_rollback_audio_defer_command(&adapter, &command));
    command.value = 20u;
    assert(mdkr_rollback_audio_defer_command(&adapter, &command));
    assert(adapter.command_count == 1u);
    assert(adapter.stats.commands_coalesced == 1u);
    mdkr_rollback_audio_flush_commands(&adapter);
    assert(fixture.commands == 1u);
    assert(fixture.last_value == 20u);
    assert(adapter.command_count == 0u);

    puts("test_rollback_audio: PASS");
    return 0;
}
