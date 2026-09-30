#ifndef MDKR64_PACK_INSTALL_JOB_H
#define MDKR64_PACK_INSTALL_JOB_H

#include "pack_installer.h"

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

// The launcher's one pack installation. A worker copies; the launcher thread
// publishes. service() runs once per launcher frame after that frame's input,
// so a Cancel click or Quit and a copy finishing in the same frame resolve in
// the player's favour: a cancelled copy is never published.
//
// A copy that is cancelled or refused, and staging an earlier crash left
// behind, are deleted on background threads so a large folder never freezes
// the launcher. finishForExit() waits for all of it.
class PackInstallJob {
public:
    PackInstallJob() = default;
    PackInstallJob(const PackInstallJob &) = delete;
    PackInstallJob &operator=(const PackInstallJob &) = delete;
    ~PackInstallJob();

    // True while a copy is running or waiting to be published.
    bool busy() const { return result_.valid(); }
    // Start copying `source` into `modsDirectory`. Ignored while busy.
    void start(const std::string &source, const std::string &modsDirectory);
    // A copy not yet published never will be.
    void cancel();
    // Publish a ready copy unless it was cancelled or `quitting`. Returns true
    // when a pack was published, so the caller can refresh its inventory.
    bool service(bool quitting);
    // Remove staging that an earlier crash or force-quit left in
    // `modsDirectory`, in the background.
    void reclaimAbandonedStaging(const std::string &modsDirectory);
    // Cancel, then wait for the copy and every background deletion. Never
    // publishes. Safe to call more than once.
    void finishForExit();

    const std::string &note() const { return note_; }
    void setNote(std::string note) { note_ = std::move(note); }

    // For tests: wait until the running copy is ready to publish, or until
    // background deletions have finished.
    void waitForCopy() const;
    void waitForBackground();

private:
    void discard(PreparedPackInstall &&prepared);
    void pruneBackground();

    std::shared_ptr<std::atomic<bool>> cancel_;
    std::future<PreparedPackInstall> result_;
    std::shared_ptr<std::atomic<bool>> stopBackground_ =
        std::make_shared<std::atomic<bool>>(false);
    std::vector<std::future<void>> background_;
    std::string note_;
};

#endif
