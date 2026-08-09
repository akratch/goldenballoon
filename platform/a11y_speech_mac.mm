/* a11y_speech_mac.mm — the macOS voice, through AVSpeechSynthesizer.
 *
 * VERIFICATION: RUN, on macOS (Darwin 25.5), through this exact file. What was
 * observed, precisely: AVSpeechSynthesizer reported didStart, then
 * willSpeakRange word by word across the whole sentence, then didFinish 5.3 s
 * later — the progressive callbacks a synthesiser only emits while it is
 * producing audio. Driven through a11y_speech_worker.c from a settings config,
 * the same line took 5.34 s at 100 percent, 2.60 s at 200 and 1.81 s at 300,
 * and a barge-in cut it at 0.5 s. What was NOT done is a human putting an ear
 * to the speaker, so "audible" here rests on those callbacks and those
 * durations rather than on hearing.
 *
 * WHICH THREAD TOUCHES THE SYNTHESISER. All of it, the main one. The worker
 * calls _speak() and _stop() may be called from the UI thread, but every
 * message to AVSpeechSynthesizer is funnelled onto the main queue: the class
 * carries no thread-safety guarantee, and a crash in a player's accessibility
 * voice is not a defect worth risking to save a dispatch. That costs nothing in
 * responsiveness because synthesis itself is asynchronous — speakUtterance:
 * returns immediately and the speech service does the work elsewhere.
 *
 * WHAT MAKES _speak() BLOCK, THEN. The delegate: it flips a flag when the
 * sentence ends or is cancelled, and _speak() waits on that flag. A deadline
 * derived from the sentence's own length backs it up, so a speech service that
 * dies mid-utterance costs one late line rather than a wedged worker thread.
 */
#include "a11y_speech.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The delegate exists only to answer "is it still talking?". An atomic property
 * because the answer is written on the main queue and read on the worker. */
@interface MdkrSpeechSink : NSObject <AVSpeechSynthesizerDelegate>
@property(atomic, assign) BOOL busy;
@end

@implementation MdkrSpeechSink
- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer
    didFinishSpeechUtterance:(AVSpeechUtterance *)utterance {
    (void)synthesizer;
    (void)utterance;
    self.busy = NO;
}
- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer
    didCancelSpeechUtterance:(AVSpeechUtterance *)utterance {
    (void)synthesizer;
    (void)utterance;
    self.busy = NO;
}
@end

namespace {

AVSpeechSynthesizer *g_synth = nil;
MdkrSpeechSink      *g_sink = nil;
AVSpeechSynthesisVoice *g_voice = nil;
bool  g_ready = false;
int   g_ratePercent = MDKR_A11Y_SPEECH_RATE_NORMAL;
float g_volume = 1.0f;

void runOnMain(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        /* async, never sync: _stop() can be called from the main thread while
         * the worker is inside _speak(), and a sync hop either way is how that
         * pair becomes a deadlock. */
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

double monotonicSeconds() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Only a backstop against a speech service that never reports completion. The
 * delegate ends the wait in every healthy case, so the cost of being generous
 * here is zero and the cost of being tight is real: measured on this machine, a
 * 69-character line takes 5.8 s warm and about 8.2 s the first time a login
 * session synthesises anything, because the voice assets load on that call. A
 * two-second allowance timed out on exactly that cold start.
 *
 * ~13 characters a second at the normal rate — measured, not guessed — scaled
 * by the rate the player chose, over a fixed allowance sized for the cold
 * start. A player who barges in never waits for this: _stop() releases the
 * wait immediately. */
double deadlineSeconds(const char *text) {
    const double characters = (double)strlen(text);
    double ratio = (double)g_ratePercent / 100.0;
    if (ratio < 0.1) ratio = 0.1;
    return 8.0 + (characters / 13.0) / ratio;
}

/* The schema's rate is a percentage of normal speaking speed, which is what a
 * player understands. AVSpeechUtterance.rate is a 0..1 dial that is NOT that,
 * and treating it as one is wrong in a way that shows: measured on this machine
 * with the default en-US voice, the same sentence takes
 *
 *     rate 0.10  8.98 s      rate 0.50  5.35 s  (the platform default)
 *     rate 0.20  7.42 s      rate 0.60  3.40 s
 *     rate 0.30  6.89 s      rate 0.70  2.61 s
 *     rate 0.40  5.88 s      rate 0.85  1.79 s
 *                            rate 1.00  1.43 s
 *
 * so the entire lower half of the dial spans 0.6x to 1.0x speed while the upper
 * half spans 1.0x to 3.7x. Scaling the default by the player's percentage — the
 * obvious mapping — turns "50 percent" into 0.93x: a setting that reads as half
 * speed and is inaudibly different from normal.
 *
 * This interpolates the measured curve instead, so the percentage means what it
 * says wherever the platform can deliver it. It cannot below about 0.6x: that
 * is the floor of the dial, and 50 percent lands on it rather than pretending.
 *
 * Calibrated against the default voice; other voices differ somewhat, which
 * makes this an honest approximation rather than an exact one. */
float utteranceRate(int percent) {
    /* speed relative to the platform default -> the dial value that gives it */
    static const struct { float factor; float rate; } k_curve[] = {
        {0.60f, 0.10f}, {0.72f, 0.20f}, {0.78f, 0.30f}, {0.91f, 0.40f},
        {1.00f, 0.50f}, {1.57f, 0.60f}, {2.05f, 0.70f}, {2.99f, 0.85f},
        {3.74f, 1.00f}
    };
    const size_t count = sizeof k_curve / sizeof k_curve[0];
    const float wanted = (float)percent / 100.0f;
    size_t index;

    if (wanted <= k_curve[0].factor) {
        return k_curve[0].rate;
    }
    for (index = 1u; index < count; index++) {
        if (wanted <= k_curve[index].factor) {
            const float span = k_curve[index].factor - k_curve[index - 1u].factor;
            const float along =
                span > 0.0f ? (wanted - k_curve[index - 1u].factor) / span : 0.0f;
            return k_curve[index - 1u].rate +
                   along * (k_curve[index].rate - k_curve[index - 1u].rate);
        }
    }
    return k_curve[count - 1u].rate;
}

void report(char *reason, size_t reason_size, const char *text) {
    if (reason != NULL && reason_size > 0u) {
        snprintf(reason, reason_size, "%s", text);
    }
}

}  // namespace

bool mdkr_a11y_speech_init(char *reason, size_t reason_size) {
    if (!mdkr_a11y_speech_audio_permitted()) {
        report(reason, reason_size,
               "audio is switched off for this run (MDKR_AUDIO=0)");
        return false;
    }
    if (g_ready) {
        return true;
    }
    @autoreleasepool {
        /* A Mac with every voice removed is a real, if rare, state. Say so
         * rather than opening a synthesiser that will never make a sound. */
        if ([AVSpeechSynthesisVoice speechVoices].count == 0u) {
            report(reason, reason_size,
                   "macOS has no speech voices installed. Add one in System "
                   "Settings, under Accessibility, then Spoken Content.");
            return false;
        }
        AVSpeechSynthesizer *synth = [[AVSpeechSynthesizer alloc] init];
        if (synth == nil) {
            report(reason, reason_size,
                   "macOS would not start its speech synthesiser");
            return false;
        }
        MdkrSpeechSink *sink = [[MdkrSpeechSink alloc] init];
        synth.delegate = sink;
        /* The player's own language, so the voice pronounces the menu the way
         * the menu is written. nil is a legal answer and means "system
         * default", which is exactly the right fallback. */
        AVSpeechSynthesisVoice *voice = [AVSpeechSynthesisVoice
            voiceWithLanguage:[AVSpeechSynthesisVoice currentLanguageCode]];
        g_synth = synth;
        g_sink = sink;
        g_voice = [voice retain];
        g_ready = true;
    }
    report(reason, reason_size, "");
    return true;
}

void mdkr_a11y_speech_speak(const char *text) {
    if (!g_ready || text == NULL || text[0] == '\0') {
        return;
    }
    /* Claimed here, on the calling thread, BEFORE the utterance is handed over:
     * setting it inside the didStart delegate callback would leave a window in
     * which the wait below sees "not busy" and returns before a word is said. */
    g_sink.busy = YES;

    NSString *spoken = [[NSString alloc] initWithUTF8String:text];
    if (spoken == nil) {
        g_sink.busy = NO;
        return;
    }
    const int   percent = g_ratePercent;
    const float volume = g_volume;
    AVSpeechSynthesisVoice *voice = g_voice;
    runOnMain(^{
        @autoreleasepool {
            AVSpeechUtterance *utterance =
                [[AVSpeechUtterance alloc] initWithString:spoken];
            utterance.rate = utteranceRate(percent);
            utterance.volume = volume;
            if (voice != nil) {
                utterance.voice = voice;
            }
            [g_synth speakUtterance:utterance];
            [utterance release];  /* the synthesiser owns it now */
        }
        [spoken release];
    });

    const double deadline = monotonicSeconds() + deadlineSeconds(text);
    while (g_sink.busy && monotonicSeconds() < deadline) {
        usleep(10000);  /* 10 ms: a tenth of the shortest utterance */
    }
    if (g_sink.busy) {
        /* The deadline won, so the synthesiser may still be mid-sentence.
         * Returning without silencing it would let the NEXT utterance start on
         * top of this one — two voices at once, which is worse than the late
         * line the backstop was there to prevent. A return from _speak() has to
         * mean the engine is free. */
        mdkr_a11y_speech_stop();
    }
    g_sink.busy = NO;
}

void mdkr_a11y_speech_stop(void) {
    if (!g_ready) {
        return;
    }
    /* Released before the queue is purged, not after: the worker must be let
     * out of its wait even if the main queue is a frame behind in servicing
     * the block below. */
    g_sink.busy = NO;
    runOnMain(^{
        [g_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    });
}

void mdkr_a11y_speech_shutdown(void) {
    if (!g_ready) {
        return;
    }
    g_ready = false;
    g_sink.busy = NO;
    /* Synchronously enough: the worker has already been joined by the service
     * before this runs, so nothing else is talking to the synthesiser. */
    AVSpeechSynthesizer *synth = g_synth;
    MdkrSpeechSink *sink = g_sink;
    AVSpeechSynthesisVoice *voice = g_voice;
    g_synth = nil;
    g_sink = nil;
    g_voice = nil;
    [synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    synth.delegate = nil;
    [synth release];
    [sink release];
    [voice release];
}

bool mdkr_a11y_speech_available(void) {
    return g_ready;
}

void mdkr_a11y_speech_set_rate(int percent) {
    if (percent < MDKR_A11Y_SPEECH_RATE_MIN) percent = MDKR_A11Y_SPEECH_RATE_MIN;
    if (percent > MDKR_A11Y_SPEECH_RATE_MAX) percent = MDKR_A11Y_SPEECH_RATE_MAX;
    g_ratePercent = percent;
}

void mdkr_a11y_speech_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > MDKR_A11Y_SPEECH_VOLUME_MAX) {
        percent = MDKR_A11Y_SPEECH_VOLUME_MAX;
    }
    g_volume = (float)percent / 100.0f;
}
