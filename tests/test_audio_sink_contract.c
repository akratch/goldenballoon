#include "audio_queue_controller.h"

#include <SDL.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_RATE 22050u
#define CHANNELS 2u
#define FRAME_BYTES (CHANNELS * sizeof(int16_t))
#define FRAME_SIZE 736u
#define MAX_SAMPLES 2048u

static int parse_duration(int argc, char **argv, uint32_t *duration_ms,
                          const char **required_driver) {
    int index;
    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--duration-ms") == 0 && index + 1 < argc) {
            char *end = NULL;
            unsigned long value;
            errno = 0;
            value = strtoul(argv[++index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0' ||
                value < 250ul || value > 60000ul) {
                fprintf(stderr, "invalid --duration-ms (250..60000)\n");
                return 0;
            }
            *duration_ms = (uint32_t)value;
        } else if (strcmp(argv[index], "--require-driver") == 0 &&
                   index + 1 < argc) {
            *required_driver = argv[++index];
        } else {
            fprintf(stderr, "usage: %s [--duration-ms N] "
                            "[--require-driver NAME]\n", argv[0]);
            return 0;
        }
    }
    return 1;
}

static int fail(SDL_AudioDeviceID device, const char *message) {
    fprintf(stderr, "audio sink contract: FAIL — %s", message);
    if (SDL_GetError()[0] != '\0') {
        fprintf(stderr, ": %s", SDL_GetError());
    }
    fputc('\n', stderr);
    if (device != 0u) {
        SDL_CloseAudioDevice(device);
    }
    SDL_Quit();
    return 1;
}

int main(int argc, char **argv) {
    MdkrAudioQueueController controller;
    SDL_AudioSpec want;
    SDL_AudioSpec have;
    SDL_AudioDeviceID device = 0u;
    const char *driver;
    const char *required_driver = NULL;
    uint8_t silence[MAX_SAMPLES * FRAME_BYTES];
    uint64_t deadline;
    uint64_t queue_calls = 0u;
    uint64_t queue_failures = 0u;
    uint64_t drained_observations = 0u;
    uint32_t duration_ms = 750u;
    uint32_t previous_post_queued = 0u;
    uint32_t max_queued = 0u;
    int have_previous_post = 0;

    if (!parse_duration(argc, argv, &duration_ms, &required_driver)) {
        return 2;
    }
    SDL_ClearError();
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        return fail(0u, "SDL audio/timer initialization failed");
    }
    driver = SDL_GetCurrentAudioDriver();
    if (driver == NULL) {
        return fail(0u, "SDL selected no audio driver");
    }
    if (required_driver != NULL && strcmp(driver, required_driver) != 0) {
        fprintf(stderr, "audio sink contract: FAIL — required driver '%s', "
                        "got '%s'\n", required_driver, driver);
        SDL_Quit();
        return 1;
    }

    SDL_zero(want);
    SDL_zero(have);
    want.freq = (int)OUTPUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples = 1024u;
    want.callback = NULL;
    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (device == 0u) {
        return fail(0u, "SDL queue-mode device open failed");
    }
    if (have.freq != (int)OUTPUT_RATE || have.format != AUDIO_S16SYS ||
        have.channels != CHANNELS || have.samples == 0u) {
        fprintf(stderr, "audio sink contract: FAIL — obtained %d Hz format "
                        "0x%x %u ch %u samples\n", have.freq,
                        (unsigned)have.format, (unsigned)have.channels,
                        (unsigned)have.samples);
        SDL_CloseAudioDevice(device);
        SDL_Quit();
        return 1;
    }

    memset(silence, 0, sizeof(silence));
    mdkr_audio_queue_controller_init(&controller);
    deadline = SDL_GetTicks64() + duration_ms;
    SDL_PauseAudioDevice(device, 0);

    while (SDL_GetTicks64() < deadline) {
        uint32_t queued_bytes = SDL_GetQueuedAudioSize(device);
        uint32_t queued_frames = queued_bytes / FRAME_BYTES;
        uint32_t samples;

        if (have_previous_post && queued_frames < previous_post_queued) {
            drained_observations++;
        }
        samples = mdkr_audio_queue_controller_choose(
            &controller, true, (uint32_t)SDL_GetTicks64(), 1000u,
            OUTPUT_RATE, FRAME_SIZE, MAX_SAMPLES, queued_frames);
        if (samples == 0u || samples > MAX_SAMPLES) {
            return fail(device, "controller returned an invalid block");
        }
        if (SDL_QueueAudio(device, silence, samples * FRAME_BYTES) != 0) {
            queue_failures++;
        }
        queue_calls++;
        queued_frames = SDL_GetQueuedAudioSize(device) / FRAME_BYTES;
        previous_post_queued = queued_frames;
        have_previous_post = 1;
        if (queued_frames > max_queued) {
            max_queued = queued_frames;
        }
        SDL_Delay(8u);
    }

    SDL_PauseAudioDevice(device, 1);
    {
        uint32_t paused_before = SDL_GetQueuedAudioSize(device);
        SDL_Delay(40u);
        if (SDL_GetQueuedAudioSize(device) != paused_before) {
            return fail(device, "paused queue continued draining");
        }
    }
    SDL_ClearQueuedAudio(device);
    if (SDL_GetQueuedAudioSize(device) != 0u) {
        return fail(device, "clear did not empty the queue");
    }
    if (queue_calls < 10u || queue_failures != 0u ||
        drained_observations == 0u || max_queued > FRAME_SIZE * 3u ||
        controller.stats.stall_guards != 0u) {
        fprintf(stderr, "audio sink contract: FAIL — calls=%llu failures=%llu "
                        "drains=%llu maxqueued=%u stalls=%u\n",
                (unsigned long long)queue_calls,
                (unsigned long long)queue_failures,
                (unsigned long long)drained_observations,
                (unsigned)max_queued,
                (unsigned)controller.stats.stall_guards);
        SDL_CloseAudioDevice(device);
        SDL_Quit();
        return 1;
    }

    printf("audio sink contract: PASS — driver=%s dummy=%s duration_ms=%u "
           "format=%dHz/s16/%uch devicebuf=%u calls=%llu drains=%llu "
           "empty-observations=%u maxqueued=%u maxproduced=%u\n",
           driver, strcmp(driver, "dummy") == 0 ? "yes" : "no",
           (unsigned)duration_ms, have.freq, (unsigned)have.channels,
           (unsigned)have.samples, (unsigned long long)queue_calls,
           (unsigned long long)drained_observations,
           (unsigned)controller.stats.empty_queue_observations,
           (unsigned)max_queued,
           (unsigned)controller.stats.max_produced_frames);
    SDL_CloseAudioDevice(device);
    SDL_Quit();
    return 0;
}
