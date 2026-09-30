#include "pack_install_job.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <utility>

PackInstallJob::~PackInstallJob() { finishForExit(); }

void PackInstallJob::start(const std::string &source, const std::string &modsDirectory) {
    if (result_.valid()) return;
    pruneBackground();
    try {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        cancel_ = cancel;
        result_ = std::async(std::launch::async, [source, modsDirectory, cancel] {
            return PreparedPackInstall::copyPack(std::filesystem::u8path(source),
                                                 std::filesystem::u8path(modsDirectory),
                                                 [cancel] { return cancel->load(); });
        });
        note_ = "Copying pack… You can continue using the launcher.";
    } catch (const std::exception &error) {
        note_ = std::string("Could not start pack installation: ") + error.what();
    }
}

void PackInstallJob::cancel() {
    if (cancel_) cancel_->store(true);
}

bool PackInstallJob::service(bool quitting) {
    pruneBackground();
    if (quitting) cancel();
    if (!result_.valid() ||
        result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    try {
        // Validation and publication stay on the launcher thread, after this
        // frame's input. While the engine runs, the finished copy waits here
        // as hidden staging.
        auto prepared = result_.get();
        if (cancel_->load()) {
            note_ = "Pack installation cancelled. Nothing was installed.";
            discard(std::move(prepared));
            return false;
        }
        const auto cancel = cancel_;
        const auto installed = prepared.finalize([cancel] { return cancel && cancel->load(); });
        note_ = installed.message;
        if (!installed.installed) discard(std::move(prepared));
        return installed.installed;
    } catch (const std::exception &error) {
        note_ = std::string("Could not install pack: ") + error.what();
        return false;
    }
}

void PackInstallJob::discard(PreparedPackInstall &&prepared) {
    try {
        background_.push_back(std::async(std::launch::async,
            [owned = std::move(prepared)]() mutable {
                // Deleting a large staged folder here keeps it off the
                // launcher thread.
                owned = PreparedPackInstall{};
            }));
    } catch (const std::exception &) {
        // No thread to spare: the staged copy is deleted as the caller's
        // scope ends instead.
    }
}

void PackInstallJob::reclaimAbandonedStaging(const std::string &modsDirectory) {
    pruneBackground();
    const auto stop = stopBackground_;
    try {
        background_.push_back(std::async(std::launch::async, [modsDirectory, stop] {
            (void)PreparedPackInstall::reclaimAbandonedStaging(
                std::filesystem::u8path(modsDirectory), [stop] { return stop->load(); });
        }));
    } catch (const std::exception &) {
        // Not reclaimed this time; the next launcher start tries again.
    }
}

void PackInstallJob::pruneBackground() {
    for (auto task = background_.begin(); task != background_.end();) {
        if (task->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++task;
            continue;
        }
        try {
            task->get();
        } catch (const std::exception &) {
            // A failed deletion leaves staging for the next sweep.
        }
        task = background_.erase(task);
    }
}

void PackInstallJob::finishForExit() {
    cancel();
    stopBackground_->store(true);
    if (result_.valid()) {
        try {
            // Never publish a finished-but-unconsumed copy as a side effect of
            // leaving: its staging is deleted as it goes out of scope here.
            auto discarded = result_.get();
        } catch (const std::exception &) {
            // Worker-local staging is owned by RAII, including on exceptions.
        }
    }
    for (auto &task : background_) {
        try {
            if (task.valid()) task.get();
        } catch (const std::exception &) {
        }
    }
    background_.clear();
    stopBackground_->store(false);
}

void PackInstallJob::waitForCopy() const {
    if (result_.valid()) result_.wait();
}

void PackInstallJob::waitForBackground() {
    for (auto &task : background_) {
        if (task.valid()) task.wait();
    }
    pruneBackground();
}
