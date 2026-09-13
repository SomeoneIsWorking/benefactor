/* setup_flow.cpp — see setup_flow.h. */
#include "platform/setup_flow.h"

#include "platform/disk_identity.h"
#include "platform/disk_selection_store.h"
#include "platform/staged_disks.h"

#include "common/log.h"

#include "setup_ui/setup_ui.h"

#include <mutex>
#include <utility>

namespace benefactor::platform {
namespace {

constexpr std::size_t kDiskCount = 3;
constexpr std::size_t kMaxDiskBytes = 64ULL * 1024ULL * 1024ULL;

setup_ui::Config setup_config() {
    setup_ui::Config config;
    config.title = "Benefactor setup";
    config.message = "Benefactor needs your original Disk.1, Disk.2, and Disk.3 images.";
    config.hint = "or one ZIP archive containing all three";
    config.footer = "Your disks are copied into this device's private storage and never uploaded.";
    for (std::size_t index = 0; index < kDiskCount; ++index) {
        config.files.push_back(
            setup_ui::FileSpec{std::string{"disk"} + std::to_string(index + 1), kDiskNames[index],
                               std::string{"Original disk "} + std::to_string(index + 1) + " (" +
                                   kDiskNames[index] + ")"});
    }
    config.accepts_archive = true;
    return config;
}

} // namespace

bool committed_disks(const std::filesystem::path &store_root,
                     std::array<std::filesystem::path, 3> &disks, std::string &error) {
    DiskSelectionStore store(store_root);
    DiskSelectionStore::Paths installed;
    if (!store.read(installed)) {
        return false;
    }
    if (!validate_set(installed, error)) {
        return false;
    }
    disks = installed;
    return true;
}

SetupFlowResult run_setup_flow(const SetupRequestSelection &request_selection,
                               const std::filesystem::path &staging_root,
                               const std::filesystem::path &store_root) {
    SetupFlowResult result;
    if (!request_selection) {
        result.error = "the platform does not provide a file picker";
        return result;
    }

    // A process killed with the screen open (Android force-stop, a crash) never
    // runs the session destructor, so its staging directory is pruned here.
    setup_ui::Session::discard_stale_staging(staging_root);

    DiskSelectionStore store(store_root);
    setup_ui::SessionOptions session_options;
    session_options.staging_root = staging_root;
    session_options.max_file_bytes = kMaxDiskBytes;
    setup_ui::Session session(setup_config(), session_options,
                              [&store](const std::vector<setup_ui::StagedFile> &files) {
                                  return validate_staged_disks(&store, files);
                              });

    setup_ui::ViewOptions view_options;
    view_options.window_title = "Benefactor setup";
    setup_ui::View view(session, view_options);
    if (!view.open()) {
        result.error = view.last_error();
        return result;
    }

    // The picker may finish on another thread (Android delivers its Activity
    // result outside the SDL thread); selections are queued and applied by the
    // loop below, which owns the session.
    std::mutex pending_mutex;
    std::vector<std::filesystem::path> pending;
    bool picker_active = false;
    const SetupDeliver deliver = [&](const std::vector<std::filesystem::path> &paths) {
        std::lock_guard lock(pending_mutex);
        pending = paths;
    };

    std::string last_error;
    while (view.running()) {
        for (const setup_ui::Request &request : view.poll()) {
            switch (request.kind) {
            case setup_ui::RequestKind::Browse: {
                std::lock_guard lock(pending_mutex);
                if (picker_active) {
                    break;
                }
                picker_active = true;
                benefactor_log_write(BENEFACTOR_LOG_INFO, "setup",
                                     "asking the platform for the disk images");
                request_selection(deliver);
                break;
            }
            case setup_ui::RequestKind::Start:
                if (session.status() == setup_ui::Status::Accepted) {
                    view.finish();
                }
                session.validate_if_ready();
                break;
            case setup_ui::RequestKind::Cancel:
                result.error = "setup was dismissed before a disk set was provided";
                view.finish();
                break;
            }
        }

        std::vector<std::filesystem::path> chosen;
        {
            std::lock_guard lock(pending_mutex);
            if (!pending.empty() || picker_active) {
                chosen.swap(pending);
                picker_active = false;
            }
        }
        if (!chosen.empty()) {
            std::string selection_error;
            const std::size_t added = session.add_selected(chosen, selection_error);
            if (added == 0) {
                benefactor_log_write(BENEFACTOR_LOG_ERROR, "setup", "%s", selection_error.c_str());
            } else if (!selection_error.empty()) {
                /* An incomplete set is the normal next step, not a failure. */
                benefactor_log_write(BENEFACTOR_LOG_INFO, "setup", "%s", selection_error.c_str());
            }
            if (!selection_error.empty()) {
                last_error = selection_error;
            }
        }

        session.validate_if_ready();
        if (session.status() == setup_ui::Status::Accepted) {
            // Keep the accepted screen visible for one more frame so the
            // player sees the result before the game starts.
            view.frame();
            view.finish();
            break;
        }
        view.frame();
    }
    view.close();

    if (session.status() == setup_ui::Status::Accepted) {
        std::string read_error;
        if (!committed_disks(store_root, result.disks, read_error)) {
            result.error = read_error;
            return result;
        }
        result.ok = true;
        return result;
    }
    if (result.error.empty()) {
        result.error = last_error.empty() ? "no disk set was provided" : last_error;
    }
    return result;
}

} // namespace benefactor::platform
