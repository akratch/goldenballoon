// ui_rom.cpp — the launcher's "Game ROM" panel: the user hands us a file.
//
// ============================================================================
//  There is NO automatic ROM discovery here, deliberately
// ============================================================================
//
// The first version of this panel scanned the working directory, $HOME,
// Downloads, Documents and Desktop for *.z64 and auto-selected the first match.
// Three things were wrong with that, all reported from live macOS validation:
//
//   1. It picked the WRONG ROM. "First match in scan order" is not "the one the
//      player wants" when several dumps are on disk, and the selection happened
//      before the player had seen a single frame of UI.
//   2. The in-app folder browser it came with could not reach anything. It
//      started at the working directory ("." from defaultScanDirs) and
//      parentOf(".") returns empty, so no ".." row was ever emitted and there
//      was no way to navigate out. From the .app bundle the working directory
//      is Contents/Resources, so the browser opened on an empty list with no
//      exit — the root cause of "the file browser doesn't work".
//   3. Worst of all: opendir() on Downloads/Documents/Desktop trips macOS TCC
//      and raises a system permission prompt before the player has asked for
//      anything. An unsigned fan-made emulator asking for your Documents folder
//      on first launch is alarming, and rightly so.
//
// So discovery is gone, root and branch — rom_scan.{h,cpp} is deleted, not
// disabled. This panel never enumerates a directory, which is what makes "no
// permission prompt can ever fire" a property of the code rather than a
// promise. Three manual, permission-free paths remain:
//
//   a) drag-and-drop onto the window (SDL_DROPFILE — user-initiated, no TCC),
//   b) a native open-panel (NSOpenPanel / GetOpenFileNameW; a user-selected
//      file is TCC-exempt), and
//   c) a typed path.
//
// The one path we DO read without being asked is the remembered ROM in the
// app's own preferences file, which the app owns and which needs no permission.
#include "ui_launcher.h"
#include "app_brand.h"
#include "app_config.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "file_dialog.h"
#include "ui_common.h"
#include "ui_phone_party.h"
#include "ui_settings.h"
#include "video_config.h"
#include "party/native_party_host.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::string g_pathInput;

// Set when the player asks to replace a ROM that is already working, so the
// acquisition controls stay behind an explicit "Change ROM" rather than
// cluttering the ready state.
bool g_changing = false;

// Non-fatal note shown under the acquisition controls (e.g. a cancelled panel).
std::string g_note;
std::string g_forgetError;

enum class ValidationPurpose { Selection, Remembered, Play };

struct ValidationRequest {
    uint64_t id = 0;
    ValidationPurpose purpose = ValidationPurpose::Selection;
    std::string path;
};

struct ValidationResult {
    ValidationPurpose purpose = ValidationPurpose::Selection;
    std::string path;
    RomInfo info{};
};

// One process-lifetime worker owns every full-ROM read. A newer request bumps
// the generation immediately, so the active 64 KiB read loop cancels at its
// next boundary and its stale result can never publish into launcher state.
class RomValidationWorker {
public:
    ~RomValidationWorker() {
        stopping_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_.fetch_add(1u, std::memory_order_acq_rel);
            pending_ = false;
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    void request(ValidationPurpose purpose, const std::string &path) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uint64_t id =
                latest_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
            request_ = ValidationRequest{id, purpose, path};
            pending_ = true;
            resultReady_ = false;
            progressBytes_.store(0u, std::memory_order_release);
            progressTotal_.store(DKR_ROM_SIZE_BYTES, std::memory_order_release);
            if (!thread_.joinable()) {
                thread_ = std::thread(&RomValidationWorker::run, this);
            }
        }
        wake_.notify_one();
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.fetch_add(1u, std::memory_order_acq_rel);
        pending_ = false;
        resultReady_ = false;
        progressBytes_.store(0u, std::memory_order_release);
        progressTotal_.store(0u, std::memory_order_release);
    }

    bool poll(ValidationResult &result, unsigned &completed, unsigned &total) {
        completed = progressBytes_.load(std::memory_order_acquire);
        total = progressTotal_.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!resultReady_) return false;
        result = result_;
        resultReady_ = false;
        return true;
    }

private:
    struct ProgressContext {
        RomValidationWorker *worker;
        uint64_t id;
    };

    static int progress(unsigned completed, unsigned total, void *opaque) {
        auto *context = static_cast<ProgressContext *>(opaque);
        std::lock_guard<std::mutex> lock(context->worker->mutex_);
        if (context->worker->stopping_.load(std::memory_order_acquire) ||
            context->worker->latest_.load(std::memory_order_acquire) !=
                context->id) {
            return 0;
        }
        context->worker->progressBytes_.store(completed, std::memory_order_release);
        context->worker->progressTotal_.store(total, std::memory_order_release);
        return 1;
    }

    void run() {
        for (;;) {
            ValidationRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_acquire) || pending_;
                });
                if (stopping_.load(std::memory_order_acquire)) return;
                request = request_;
                pending_ = false;
            }

            ProgressContext context{this, request.id};
            RomInfo info = mdkr_validate_rom_progress(
                request.path.c_str(), progress, &context);
            if (info.cancelled ||
                latest_.load(std::memory_order_acquire) != request.id) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stopping_.load(std::memory_order_acquire) &&
                    latest_.load(std::memory_order_acquire) == request.id) {
                    result_ = ValidationResult{request.purpose, request.path, info};
                    resultReady_ = true;
                }
            }
        }
    }

    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> latest_{0u};
    std::atomic<unsigned> progressBytes_{0u};
    std::atomic<unsigned> progressTotal_{0u};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    ValidationRequest request_;
    ValidationResult result_;
    bool pending_ = false;
    bool resultReady_ = false;
};

RomValidationWorker &validationWorker() {
    static RomValidationWorker worker;
    return worker;
}

bool romPathWithinLimit(const std::string &path) {
    return path.size() <= kLauncherRomPathMaxBytes;
}

int resizePathInput(ImGuiInputTextCallbackData *data) {
    if (data == nullptr) {
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        /* ImGui applies this to pasted input too, before asking the string to
         * grow. This makes the extended-path limit a real input bound rather
         * than an after-the-fact validation error. */
        return data->BufTextLen >= (int)kLauncherRomPathMaxBytes;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) return 0;
    auto *value = static_cast<std::string *>(data->UserData);
    value->resize(static_cast<size_t>(data->BufTextLen));
    data->Buf = value->data();
    return 0;
}

void drawDropZone(bool haveRom) {
    // The drop target is the whole window (SDL_DROPFILE is window-wide), so this
    // is an invitation, not a hit-box. Mirrors the web shell's drop-zone
    // language so the two front-ends read the same.
    const ImVec4 accent = AppTheme::accent();
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
    ImGui::BeginChild("##dropzone", ImVec2(0, ui::kDropZoneHeight()), true);
    {
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(haveRom ? "Drop a different ROM here"
                                       : "Drag your ROM file here");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ui::TextSubtle(
            "…or use the controls below. Accepts .z64, .v64, and .n64 files.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// The acquisition controls: native panel (where one exists), and a typed path.
void drawAcquisition(LauncherState &s) {
    if (filedialog::isAvailable()) {
        const bool browsePressed = ImGui::Button("Browse…", ui::kBtnWide());
        ui::SpeakFocusedItem("Browse", nullptr,
                             "Opens your system file picker to choose a ROM.");
        if (browsePressed) {
            RomPanel_chooseRom(s);
        }
        ImGui::SameLine();
        ui::TextSubtle("Opens your system's file picker.");
        ui::Gap(ui::kGapM);
    }

    ui::TextSubtle(filedialog::isAvailable()
                       ? "Or paste the full path to your ROM:"
                       : "Drag the file onto this window, or paste its full path:");
    if (g_pathInput.capacity() < 256u) g_pathInput.reserve(256u);
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float pathWidth = ui::kControlWidth(1.6f);
    const bool stackAction = rowWidth < pathWidth +
        ImGui::GetStyle().ItemSpacing.x + ui::kBtnSecondary().x;
    ImGui::SetNextItemWidth(pathWidth);
    const bool entered = ImGui::InputTextWithHint(
        "##rompath", "/path/to/your/game.z64", g_pathInput.data(),
        g_pathInput.capacity() + 1u,
        ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackResize |
            ImGuiInputTextFlags_CallbackCharFilter,
        resizePathInput, &g_pathInput);
    if (!stackAction) ImGui::SameLine();
    const bool pressed = ImGui::Button("Use This Path", ui::kBtnSecondary());
    if (entered || pressed) {
        if (g_pathInput.empty()) {
            g_note = "Type a path first, or use the file picker.";
        } else {
            g_note.clear();
            RomPanel_setRom(s, g_pathInput.c_str());
        }
    }

    if (!g_note.empty()) {
        ui::Gap(ui::kGapS);
        ui::TextSubtle("%s", g_note.c_str());
    }
}

}  // namespace

bool RomPanel_chooseRom(LauncherState &s) {
    if (!filedialog::isAvailable()) return false;
    std::string picked;
    if (!filedialog::openRom(picked)) return false;
    g_note.clear();
    RomPanel_setRom(s, picked.c_str());
    return true;
}

static void applySelectionResult(LauncherState &s,
                                 const std::string &candidatePath,
                                 const RomInfo &candidateInfo,
                                 bool remembered) {
    if (!candidateInfo.valid) {
        if (remembered) {
            s.romPath = candidatePath;
            s.romInfo = candidateInfo;
            if (s.romCandidatePath.empty() ||
                s.romCandidatePath == candidatePath) {
                s.romCandidatePath.clear();
                s.romCandidateInfo = RomInfo{};
                s.romCandidateVisible = false;
            }
            return;
        }
        s.romCandidatePath = candidatePath;
        s.romCandidateInfo = candidateInfo;
        s.romCandidateError[0] = '\0';
        s.romCandidateVisible = true;
        if (s.romInfo.valid) g_changing = true;
        return;
    }

    if (remembered) {
        s.romPath = candidatePath;
        s.romInfo = candidateInfo;
        if (s.romCandidatePath.empty() ||
            s.romCandidatePath == candidatePath) {
            s.romCandidatePath.clear();
            s.romCandidateInfo = RomInfo{};
            s.romCandidateError[0] = '\0';
            s.romCandidateVisible = false;
        }
        s.romPersistenceWarning[0] = '\0';
        g_changing = s.romCandidateVisible;
        return;
    }

    // A replacement becomes active only after its preference transaction
    // lands, so failed storage cannot displace a working ROM. With no playable
    // selection yet, the verified file may remain session-local instead.
    const AppConfig::PersistResult persist =
        AppConfig::setAndSave("rom_path", candidatePath);
    if (!AppConfig::persistResultApplied(persist)) {
        if (!s.romInfo.valid) {
            // There is no last-known-good selection to preserve. Blocking a
            // fully verified ROM solely because its path cannot be remembered
            // makes a transient preferences failure needlessly terminal. Keep
            // it session-local and say exactly what will be lost on restart.
            s.romPath = candidatePath;
            s.romInfo = candidateInfo;
            s.romCandidatePath.clear();
            s.romCandidateInfo = RomInfo{};
            s.romCandidateError[0] = '\0';
            s.romCandidateVisible = false;
            std::snprintf(
                s.romPersistenceWarning, sizeof(s.romPersistenceWarning),
                "Ready for this session, but saving the ROM path could not be "
                "confirmed. Keep the file available next launch. "
                "Check Diagnostics if preferences or saves also fail.");
            g_changing = false;
            return;
        }
        s.romCandidatePath = candidatePath;
        s.romCandidateInfo = candidateInfo;
        std::snprintf(
            s.romCandidateError, sizeof(s.romCandidateError),
            "This ROM is valid, but the preference file save could not be confirmed.%s",
            s.romInfo.valid
                ? " Your current ROM was kept."
                : " Restore write access to the app's data folder and choose it again.");
        s.romCandidateVisible = true;
        if (s.romInfo.valid) g_changing = true;
        return;
    }

    s.romPath = candidatePath;
    s.romInfo = candidateInfo;
    s.romCandidatePath.clear();
    s.romCandidateInfo = RomInfo{};
    s.romCandidateError[0] = '\0';
    s.romCandidateVisible = false;
    if (persist == AppConfig::PersistResult::DurabilityUnconfirmed) {
        std::snprintf(
            s.romPersistenceWarning, sizeof(s.romPersistenceWarning),
            "ROM path applied, but was not confirmed written to disk. You may "
            "need to choose it again after an unexpected shutdown.");
    } else {
        s.romPersistenceWarning[0] = '\0';
    }
    g_changing = false;
}

static void requestValidation(LauncherState &s, ValidationPurpose purpose,
                              const std::string &path);

static void clearCandidateFeedback(LauncherState &s) {
    s.romCandidatePath.clear();
    s.romCandidateInfo = RomInfo{};
    s.romCandidateError[0] = '\0';
    s.romCandidateVisible = false;
}

static void retryCandidatePersistence(LauncherState &s) {
    if (!s.romInfo.valid || !s.romCandidateVisible ||
        !s.romCandidateInfo.valid || s.romCandidatePath.empty()) {
        return;
    }

    // The candidate can live on removable or externally managed storage. It
    // may therefore have changed since the first full-image check failed to
    // persist its path. Re-enter the normal Selection transaction: only a new
    // validation result followed by a successful durable write may replace the
    // active last-known-good ROM.
    const std::string candidatePath = s.romCandidatePath;
    clearCandidateFeedback(s);
    requestValidation(s, ValidationPurpose::Selection, candidatePath);
}

static void requestValidation(LauncherState &s, ValidationPurpose purpose,
                              const std::string &path) {
    s.romValidationPath = path;
    s.romValidationBytes = 0u;
    s.romValidationTotal = DKR_ROM_SIZE_BYTES;
    s.romValidationPending = true;
    s.romPlayValidationPending = purpose == ValidationPurpose::Play;
    s.romPlayValidationPassed = false;
    validationWorker().request(purpose, path);
}

/* A cancellation is a user-visible decision, not merely a progress-bar change.
 * Invalidate the worker's generation before restoring the current selection so
 * a completed replacement/remembered result cannot publish after Cancel Change
 * or Forget Saved Path has returned the launcher to its previous state. */
static void cancelValidation(LauncherState &s, bool clearUnusableSelection) {
    const std::string checkingPath = s.romValidationPath;
    if (s.romValidationPending) {
        validationWorker().cancel();
    }
    if (clearUnusableSelection && !s.romInfo.valid &&
        s.romPath == checkingPath) {
        s.romPath.clear();
        s.romInfo = RomInfo{};
        g_pathInput.clear();
    } else if (s.romInfo.valid) {
        g_pathInput = s.romPath;
    }
    s.romValidationPending = false;
    s.romPlayValidationPending = false;
    s.romPlayValidationPassed = false;
    s.romValidationPath.clear();
    s.romValidationBytes = 0u;
    s.romValidationTotal = 0u;
}

void RomPanel_setRom(LauncherState &s, const char *path) {
    if (path == nullptr || path[0] == '\0') return;
    const std::string candidatePath(path);
    if (!romPathWithinLimit(candidatePath)) {
        g_note = "That path is longer than this platform can safely open.";
        return;
    }
    g_pathInput = candidatePath;
    g_note.clear();
    if (s.romInfo.valid) g_changing = true;
    // A new attempt supersedes the prior candidate verdict. Keeping the old
    // refusal beside a new progress card misattributes it to the file currently
    // being checked.
    clearCandidateFeedback(s);
    requestValidation(s, ValidationPurpose::Selection, candidatePath);
}

void RomPanel_serviceValidation(LauncherState &s) {
    if (!s.romValidationPending) return;
    ValidationResult result;
    unsigned completed = 0u;
    unsigned total = 0u;
    const bool ready = validationWorker().poll(result, completed, total);
    s.romValidationBytes = completed;
    s.romValidationTotal = total;
    if (!ready) return;

    s.romValidationPending = false;
    s.romPlayValidationPending = false;
    if (result.purpose == ValidationPurpose::Play) {
        if (result.info.valid && result.path == s.romPath) {
            s.romInfo = result.info;
            s.romPlayValidationPassed = true;
            s.bootErrorVisible = false;
        } else {
            s.romInfo = result.info;
            std::snprintf(
                s.bootError, sizeof(s.bootError),
                "The selected ROM changed or became unavailable after it was "
                "verified. %s Your saved selection was kept so you can "
                "reconnect the drive or choose another file.",
                result.info.message);
            s.bootErrorVisible = true;
            /* Service priority: this pass can run after the navigation controls
             * have already drawn, so a plain assignment here would erase a tab
             * the player pressed during the in-flight Play check. The recovery
             * card stays visible on whichever panel they chose. */
            Launcher_requestTab(s, 0, kLauncherTabService);
        }
        return;
    }
    const bool restoreRemembered =
        result.purpose == ValidationPurpose::Selection &&
        !result.info.valid && !s.romInfo.valid && !s.romPath.empty() &&
        s.romPath != result.path;
    applySelectionResult(s, result.path, result.info,
                         result.purpose == ValidationPurpose::Remembered);
    if (restoreRemembered) {
        requestValidation(s, ValidationPurpose::Remembered, s.romPath);
    }
}

void RomPanel_requestPlayValidation(LauncherState &s) {
    const AppUiRomPlayRequest request = AppUi_romPlayRequest(
        !s.romPath.empty() && s.romInfo.valid,
        s.romValidationPending, s.romPlayValidationPending);
    if (request == AppUiRomPlayRequest::Ignore) return;
    if (request == AppUiRomPlayRequest::SupersedeReplacementCheck) {
        // Choosing Play abandons only the unresolved replacement. The proven
        // active ROM remains intact and immediately enters its mandatory final
        // full-image check, so the visible gold action never becomes a no-op.
        cancelValidation(s, /*clearUnusableSelection=*/false);
        clearCandidateFeedback(s);
        g_changing = false;
        g_note.clear();
    }
    requestValidation(s, ValidationPurpose::Play, s.romPath);
}

void RomPanel_ensureInit(LauncherState &s) {
    if (s.romInitialized) return;
    s.romInitialized = true;

    /* main() loads preferences before the first launcher frame. Loading again
     * here would also clear the pending-write set, discarding a preference
     * another panel staged in the frames before this one first drew. */
    std::string remembered = AppConfig::get("rom_path", "");
    if (remembered.empty()) return;   // first run: the player will hand us one

    if (!romPathWithinLimit(remembered)) {
        s.romCandidatePath = remembered;
        std::snprintf(s.romCandidateError, sizeof(s.romCandidateError),
                      "The remembered ROM path is longer than this platform can safely open. "
                      "Choose the ROM again with a shorter path.");
        s.romCandidateVisible = true;
        return;
    }

    // Validation touches exactly one remembered file and runs off the UI
    // thread. No directory is enumerated.
    s.romPath = remembered;
    g_pathInput = s.romPath;
    requestValidation(s, ValidationPurpose::Remembered, s.romPath);
}

void RomPanel_draw(LauncherState &s, LauncherAction &out) {
    (void)out;
    RomPanel_ensureInit(s);
    RomPanel_serviceValidation(s);

    /* Panel 0 is the launcher's home. It carries the ROM flow as its FIRST-RUN
     * STATE rather than as a destination named after a file format, so the
     * heading names the state the player is actually in: choosing a game before
     * they have one, and ready to play once they do. */
    const bool headingReady = !s.romPath.empty() && s.romInfo.valid;
    ui::SectionHeader(
        headingReady ? "Ready to Play" : "Choose Your Game",
        headingReady
            ? "Press Play to start. Everything below is here if you want to "
              "change what you play or who plays with you."
            : MDKR_BRAND_NAME " needs a copy of the original game that you "
              "supply and legally own. No game data is included, and "
              "nothing on your disk is searched.");

    const bool haveRom = !s.romPath.empty();
    const bool ready   = haveRom && s.romInfo.valid;

    /* What pressing Play will actually do, named on the home screen so the
     * player never has to open Settings to find out. Reads the EFFECTIVE
     * values (staged edits if any, else persisted), so it updates live the
     * moment Settings change them. Presentation mode and frame rate are the
     * two choices that visibly change how the game looks in motion; both are
     * restart-scope, so the card carries the same "next launch" language the
     * settings rows use rather than implying a live change. */
    if (ready && !g_changing && !s.romValidationPending) {
        const char *mode = Settings_effectiveLabel(MDKR_VIDEO_MODE);
        // Frame rate is the merged Original/Smooth/Custom preset, so name it in
        // the vocabulary the player set it with rather than the underlying
        // Frame limit option label ("Match Display") they no longer choose.
        const char *rate = Settings_effectivePaceLabel();
        ui::Gap(ui::kGapS);
        // A purely informational summary. It is not a focusable control, so it
        // carries no self-voicing of its own -- both values are announced where
        // they are set, under Settings > Display -- and voicing a non-focusable
        // line here only ever produced dead code that could not fire.
        if (ui::CardBegin("##willlaunch", AppTheme::surface(), 0.0f)) {
            ui::TextSubtle("This launch");
            ImGui::TextUnformatted(mode);
            ImGui::SameLine();
            ui::TextSubtle("  \xE2\x80\xA2  ");
            ImGui::SameLine();
            ImGui::TextUnformatted(rate);
            ui::CardEnd();
        }
        ui::Gap(ui::kGapS);
    }

    const AppUiRomPanelVisibility visibility = AppUi_romPanelVisibility(
        haveRom, ready, s.romValidationPending, g_changing);

    if (s.romValidationPending) {
        if (ui::CardBegin("##romchecking", AppTheme::accent(), 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
            ImGui::PushFont(AppTheme::fonts().title);
            ImGui::TextUnformatted(
                s.romPlayValidationPending ? "Final ROM Check" : "Checking ROM…");
            ImGui::PopFont();
            ImGui::PopStyleColor();
            const float fraction = s.romValidationTotal == 0u ? 0.0f :
                static_cast<float>(s.romValidationBytes) /
                    static_cast<float>(s.romValidationTotal);
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
            ui::TextSubtleUnformattedWrapped(s.romValidationPath.c_str());
            const bool cancelCheckPressed =
                ImGui::Button("Cancel Check", ui::kBtnSecondary());
            ui::SpeakFocusedItem("Cancel Check", nullptr,
                                 "Stops checking this ROM.");
            if (cancelCheckPressed) {
                cancelValidation(s, /*clearUnusableSelection=*/true);
                g_changing = false;
                g_note = "ROM check cancelled.";
            }
        }
        ui::CardEnd();
        ui::Gap(ui::kGapM);
    }

    // ---- The identified ROM, stated plainly before Play -------------------
    // A wrong pick has to be VISIBLE, so the acceptance verdict leads the card
    // and the revision the validator recognised sits directly underneath.
    if (visibility.showVerdict) {
        const ImVec4 border = ready ? AppTheme::good() : AppTheme::bad();
        if (ui::CardBegin("##romcard", border, 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, border);
            ImGui::PushFont(AppTheme::fonts().title);
            ImGui::TextUnformatted(ready ? "Your Game"
                                         : "This ROM Cannot Be Used");
            ImGui::PopFont();
            ImGui::PopStyleColor();

            if (ready) {
                ImGui::TextUnformatted(s.romInfo.revision[0]
                                           ? s.romInfo.revision
                                           : "Supported Diddy Kong Racing ROM");
                ui::TextSubtle("%s format • 12 MB • %s",
                               s.romInfo.byte_order,
                               s.romInfo.integrity_verified
                                   ? "full-image integrity verified"
                                   : "developer-modified image");
            }

            ui::Gap(ui::kGapXS);
            if (!ready) {
                // The validator's own sentence: it names the revision it found
                // and why this build refuses it.
                ImGui::TextWrapped("%s", s.romInfo.message);
                ui::Gap(ui::kGapXS);
            }
            ui::TextSubtleUnformattedWrapped(s.romPath.c_str());
            if (ready && s.romPersistenceWarning[0] != '\0') {
                ui::Gap(ui::kGapS);
                ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
                ImGui::TextWrapped("%s", s.romPersistenceWarning);
                ImGui::PopStyleColor();
            }
        }
        ui::CardEnd();

        if (!ready) {
            ui::Gap(ui::kGapS);
            ui::TextSubtle("Supported: %s", mdkr_supported_rom_list());
            const bool forgetPressed =
                ImGui::Button("Forget Remembered ROM", ui::kBtnSecondary());
            ui::SpeakFocusedItem(
                "Forget Remembered ROM", nullptr,
                "Forgets the file path. The ROM itself and all saved progress "
                "are left alone. Asks you to confirm first.");
            if (forgetPressed) {
                g_forgetError.clear();
                ImGui::OpenPopup("Forget remembered ROM?");
            }
        }
        ui::Gap(ui::kGapM);
    }

    if (AppUi_romCandidateFeedbackVisible(
            s.romCandidateVisible, s.romValidationPending)) {
        const char *message = s.romCandidateError[0]
            ? s.romCandidateError : s.romCandidateInfo.message;
        if (ui::CardBegin("##romcandidate", AppTheme::bad(), 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::TextUnformatted(
                s.romCandidateInfo.valid
                    ? (ready ? "Replacement Was Not Saved" : "ROM Was Not Saved")
                    : (ready ? "Replacement ROM Cannot Be Used"
                             : "ROM Cannot Be Used"));
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", message);
            ui::TextSubtleUnformattedWrapped(s.romCandidatePath.c_str());
        }
        ui::CardEnd();
        if (ready && s.romCandidateInfo.valid &&
            s.romCandidateError[0] != '\0') {
            ui::Gap(ui::kGapS);
            if (ui::BrandPrimaryButton("Retry Save", ui::kBtnWide())) {
                retryCandidatePersistence(s);
            }
        }
        ui::Gap(ui::kGapM);
    }

    // ---- Changing or forgetting the remembered selection -----------------
    if (ready) {
        const char *changeLabel = g_changing ? "Cancel Change" : "Change ROM…";
        const bool changePressed =
            ImGui::Button(changeLabel, ui::kBtnSecondary());
        ui::SpeakFocusedItem(
            changeLabel, nullptr,
            g_changing ? "Keeps the game you already have."
                       : "Lets you pick a different ROM. The one you have "
                         "stays playable until a new one is checked.");
        if (changePressed) {
            if (g_changing) {
                cancelValidation(s, /*clearUnusableSelection=*/false);
                g_changing = false;
                s.romCandidateVisible = false;
                s.romCandidatePath.clear();
                s.romCandidateError[0] = '\0';
                g_pathInput = s.romPath;
            } else {
                g_changing = true;
            }
            g_note.clear();
        }
        if (!g_changing) {
            if (ImGui::GetContentRegionAvail().x > ui::kBtnSecondary().x +
                                                   ImGui::GetStyle().ItemSpacing.x) {
                ImGui::SameLine();
            }
            const bool forgetPressed =
                ImGui::Button("Forget Remembered ROM", ui::kBtnSecondary());
            ui::SpeakFocusedItem(
                "Forget Remembered ROM", nullptr,
                "Forgets the file path. The ROM itself and all saved progress "
                "are left alone. Asks you to confirm first.");
            if (forgetPressed) {
                g_forgetError.clear();
                ImGui::OpenPopup("Forget remembered ROM?");
            }
        }
        ui::Gap(ui::kGapL);
    }

    // ---- Acquisition ------------------------------------------------------
    // Shown whenever there is nothing usable yet, or the player asked to change.
    if (visibility.showAcquisition) {
        if (ready) {
            ImGui::Separator();
            ui::Gap(ui::kGapM);
        }
        drawDropZone(haveRom);
        ui::Gap(ui::kGapM);
        drawAcquisition(s);
    }

    bool keepPopupOpen = true;
    ui::ConfirmModalSize();
    if (ImGui::BeginPopupModal("Forget remembered ROM?", &keepPopupOpen,
                               ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped(
            "Golden Balloon will forget this file path. The ROM itself and all "
            "saved progress remain untouched.");
        if (!g_forgetError.empty()) {
            ui::Gap(ui::kGapS);
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::TextWrapped("%s", g_forgetError.c_str());
            ImGui::PopStyleColor();
        }
        ui::Gap(ui::kGapS);
        const AppUiButtonPairLayout actions = AppUi_fitButtonPair(
            ImGui::GetContentRegionAvail().x,
            ImGui::GetStyle().ItemSpacing.x,
            ui::kBtnSecondary().x, ui::kBtnSecondary().x,
            ui::kPairMinWidth());
        const ImVec2 cancelSize(actions.firstWidth, ui::kBtnSecondary().y);
        const ImVec2 forgetSize(actions.secondWidth, ui::kBtnSecondary().y);
        if (ImGui::Button("Cancel", cancelSize)) {
            g_forgetError.clear();
            ImGui::CloseCurrentPopup();
        }
        if (actions.sameLine) ImGui::SameLine();
        const ImVec4 destructive = AppTheme::bad();
        ImGui::PushStyleColor(ImGuiCol_Button, destructive);
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4((std::min)(1.0f, destructive.x * 1.12f),
                   (std::min)(1.0f, destructive.y * 1.12f),
                   (std::min)(1.0f, destructive.z * 1.12f), 1.0f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImVec4(destructive.x * 0.82f, destructive.y * 0.82f,
                   destructive.z * 0.82f, 1.0f));
        if (ImGui::Button("Forget Remembered ROM", forgetSize)) {
            const AppConfig::PersistResult persist =
                AppConfig::setAndSave("rom_path", "");
            if (AppConfig::persistResultApplied(persist)) {
                cancelValidation(s, /*clearUnusableSelection=*/false);
                s.romPath.clear();
                s.romInfo = RomInfo{};
                s.romCandidatePath.clear();
                s.romCandidateInfo = RomInfo{};
                s.romCandidateError[0] = '\0';
                s.romCandidateVisible = false;
                s.romPersistenceWarning[0] = '\0';
                g_pathInput.clear();
                g_changing = false;
                g_note = persist == AppConfig::PersistResult::DurabilityUnconfirmed
                    ? "ROM path forgotten for this session, but storage durability was not "
                      "confirmed. Your game file and saves were not changed."
                    : "ROM path forgotten. Your game file and saves were not changed.";
                ImGui::CloseCurrentPopup();
            } else {
                g_forgetError =
                    "The path could not be forgotten because the preference "
                    "save could not be confirmed. Nothing was changed.";
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }

    /* Phone controllers are a sitting-down-to-play decision, so they belong to
     * the ready state. Drawn unconditionally, this section offered a first-run
     * player a way to invite friends to a game they do not have yet, directly
     * underneath the drop zone they still had to use. */
    /* MDKR_APP_PARTY_TRACE=1 also relaxes the ROM gate for this draw: every
     * release lane is asset-free, so without this the packaged-build party
     * qualification could never execute one line of party UI and its gate
     * would pass on nothing. Trace-only surface; still non-interactive CI. */
    static const bool partyTraceRequested = [] {
        const char *value = std::getenv("MDKR_APP_PARTY_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    const bool partyAvailable = PhoneParty_availableInBuild(MDKR_PARTY_ORIGIN);
    /* A trace may relax only the ROM-ready gate. It cannot manufacture the
     * feature in an origin-less release: setting a test environment variable
     * on a packaged binary must still leave the player-facing surface absent. */
    if (s.phoneParty != nullptr && !g_changing &&
        partyAvailable && (ready || partyTraceRequested)) {
        PhoneParty_drawLauncher(*s.phoneParty, MDKR_PARTY_ORIGIN);
    }
}
