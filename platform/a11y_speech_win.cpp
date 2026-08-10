/* a11y_speech_win.cpp — the Windows voice, through SAPI 5 (ISpVoice).
 *
 * VERIFICATION: COMPILED ONLY. Cross-compiled clean with x86_64-w64-mingw32-g++
 * under -Wall -Wextra -Werror, and never run, on any Windows machine, by
 * anyone. Nothing below is a claim that Windows speech works; it is a claim
 * that it builds. In particular the COM apartment reasoning below is reasoned,
 * not observed.
 *
 * WHY EVERY ISpVoice CALL HAPPENS ON THE WORKER THREAD. COM objects belong to
 * the apartment that created them, and this process's apartment state is not
 * ours to assume: SDL initialises OLE for drag-and-drop, so the UI thread may
 * already be an STA and CoInitializeEx(MTA) on it would return
 * RPC_E_CHANGED_MODE. An interface pointer created over there and called from
 * here would then need marshalling — a whole failure mode that simply does not
 * arise if one thread creates the voice, speaks through it and releases it.
 *
 * So _stop(), which the UI thread calls to barge in, does NOT touch COM. It
 * raises a flag; the worker is sitting in WaitUntilDone() in 50 ms slices and
 * purges the voice itself on the next slice. Barge-in is therefore up to 50 ms
 * late, which is a twentieth of the pause between two words and far cheaper
 * than a cross-apartment call.
 */
#include "a11y_speech.h"

#include "a11y_model.h"  /* MDKR_A11Y_TEXT_MAX: one utterance's worth of room */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objbase.h>
#include <sapi.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

/* mingw-w64 ships <sapi.h> but not the library that carries SAPI's GUID
 * constants, and sphelper.h (which defines them inline) is an MSVC-only SDK
 * header. Spelling the two we need out here is what keeps this file building
 * under both toolchains. Values are SAPI 5's published CLSID/IID. */
const CLSID kClsidSpVoice = {
    0x96749377, 0x3391, 0x11D2,
    {0x9E, 0xE3, 0x00, 0xC0, 0x4F, 0x79, 0x73, 0x96}};
const IID kIidISpVoice = {
    0x6C44DF74, 0x72B9, 0x4992,
    {0xA1, 0xEC, 0xEF, 0x99, 0x6E, 0x04, 0x22, 0xD4}};

bool  g_ready = false;          /* UI thread: a probe found SAPI */
int   g_ratePercent = MDKR_A11Y_SPEECH_RATE_NORMAL;
int   g_volumePercent = MDKR_A11Y_SPEECH_VOLUME_MAX;

/* Created by, and spoken through by, the worker thread. It outlives a
 * _shutdown() on purpose — see that function. */
ISpVoice *g_voice = nullptr;
/* Per thread, because an apartment is per thread. A second worker (the player
 * switched speech off and on again) must join the MTA itself rather than
 * inherit a flag the first one set. */
thread_local bool t_comReady = false;
LONG volatile g_stopRequested = 0;

void report(char *reason, size_t reason_size, const char *text) {
    if (reason != nullptr && reason_size > 0u) {
        snprintf(reason, reason_size, "%s", text);
    }
}

/* SAPI's rate is -10..10 and each step is roughly a multiplicative change, with
 * the ends landing near a third and near three times the normal pace. That maps
 * onto the schema's 50..300 percent as a logarithm rather than a straight line:
 * 100 percent is 0, 300 percent is +10, 50 percent is about -6. */
long sapiRate(int percent) {
    if (percent < MDKR_A11Y_SPEECH_RATE_MIN) percent = MDKR_A11Y_SPEECH_RATE_MIN;
    if (percent > MDKR_A11Y_SPEECH_RATE_MAX) percent = MDKR_A11Y_SPEECH_RATE_MAX;
    const double steps = 10.0 * log((double)percent / 100.0) / log(3.0);
    long rate = (long)(steps < 0.0 ? steps - 0.5 : steps + 0.5);
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    return rate;
}

/* Brings COM and the voice up on whichever thread first speaks — the worker.
 * Idempotent; a failure here leaves the backend silent rather than half-open. */
bool ensureVoice() {
    if (!t_comReady) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        /* RPC_E_CHANGED_MODE: somebody else already set this thread's
         * apartment. Not an error here — the voice still works; it just means
         * the apartment is not ours to close. */
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            return false;
        }
        t_comReady = true;
    }
    if (g_voice != nullptr) {
        /* The multi-threaded apartment is process-wide, so a voice created by
         * an earlier worker is usable by this one. */
        return true;
    }
    void *created = nullptr;
    const HRESULT hr = CoCreateInstance(kClsidSpVoice, nullptr, CLSCTX_ALL,
                                        kIidISpVoice, &created);
    if (FAILED(hr) || created == nullptr) {
        return false;
    }
    g_voice = static_cast<ISpVoice *>(created);
    return true;
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
    /* A probe, and only a probe: create a voice, prove SAPI is registered and
     * a voice token exists, then release it. The one the worker speaks through
     * is created over there, in an apartment this file controls. */
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(init);
    void *probe = nullptr;
    const HRESULT hr = CoCreateInstance(kClsidSpVoice, nullptr, CLSCTX_ALL,
                                        kIidISpVoice, &probe);
    if (SUCCEEDED(hr) && probe != nullptr) {
        static_cast<ISpVoice *>(probe)->Release();
    }
    if (ownsCom) {
        CoUninitialize();
    }
    if (FAILED(hr)) {
        report(reason, reason_size,
               "Windows Speech (SAPI) did not start. Check that a voice is "
               "installed under Settings, Time & language, Speech.");
        return false;
    }
    g_ready = true;
    report(reason, reason_size, "");
    return true;
}

void mdkr_a11y_speech_speak(const char *text) {
    if (!g_ready || text == nullptr || text[0] == '\0') {
        return;
    }
    if (!ensureVoice()) {
        return;
    }
    /* One utterance's worth of UTF-16. The model refuses anything longer than
     * MDKR_A11Y_TEXT_MAX, and no UTF-8 sequence expands past one UTF-16 unit
     * per input byte, so this cannot be short. */
    WCHAR wide[MDKR_A11Y_TEXT_MAX];
    const int written = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide,
                                            (int)(sizeof wide / sizeof wide[0]));
    if (written <= 0) {
        return;
    }

    InterlockedExchange(&g_stopRequested, 0);
    g_voice->SetRate(sapiRate(g_ratePercent));
    g_voice->SetVolume((USHORT)g_volumePercent);
    /* SPF_ASYNC so the wait below is ours to interrupt; SPF_IS_NOT_XML because
     * a settings label containing a '<' is text, not markup, and SAPI would
     * otherwise try to parse it. */
    if (FAILED(g_voice->Speak(wide, SPF_ASYNC | SPF_IS_NOT_XML, nullptr))) {
        return;
    }
    while (g_voice->WaitUntilDone(50) == S_FALSE) {
        if (InterlockedCompareExchange(&g_stopRequested, 0, 1) == 1) {
            g_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
            break;
        }
    }
}

void mdkr_a11y_speech_stop(void) {
    /* Deliberately COM-free: see the file header. The worker acts on it. */
    InterlockedExchange(&g_stopRequested, 1);
}

void mdkr_a11y_speech_shutdown(void) {
    /* WHY NOTHING IS RELEASED HERE. This runs on the UI thread, after the
     * service has already stopped the worker and joined it — so the voice is
     * silent and unused, but it belongs to the worker's apartment and both
     * Release() and CoUninitialize() are thread-bound calls that this thread
     * must not make on that thread's behalf. One COM object therefore lives
     * until the process ends, which is also what makes switching speech off
     * and on again cost nothing: the next worker reuses it. */
    g_ready = false;
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
    g_volumePercent = percent;
}
