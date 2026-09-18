/* setup_flow.cpp — see setup_flow.h. */
#include "platform/setup_flow.h"

#include "platform/disk_identity.h"
#include "platform/disk_selection_store.h"
#include "platform/staged_disks.h"

#include "common/log.h"

#include "setup_ui/setup_ui.h"

#include <memory>
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
    config.accepted_message = "Disk set accepted.";
    return config;
}

} // namespace

struct SetupFlow::Impl {
    SetupFlowOptions options;
    DiskSelectionStore store;
    setup_ui::Session session;
    setup_ui::View view;
    std::array<std::filesystem::path, 3> disks;
    std::string error;
    std::mutex pending_mutex;
    std::vector<std::filesystem::path> pending;
    bool picker_active = false;
    bool opened = false;
    bool finished = false;
    bool accepted = false;

    explicit Impl(SetupFlowOptions flow_options)
        : options(std::move(flow_options)), store(options.store_root),
          /* The validator captures the store this flow publishes through, so
           * the accepted set is committed by the same owner that judged it. */
          session(setup_config(), session_options(options.staging_root),
                  [this](const std::vector<setup_ui::StagedFile> &files) {
                      return validate_staged_disks(&store, files);
                  }),
          view(session, view_options()) {
    }

    static setup_ui::SessionOptions session_options(const std::filesystem::path &staging) {
        setup_ui::SessionOptions settings;
        settings.staging_root = staging;
        settings.max_file_bytes = kMaxDiskBytes;
        return settings;
    }

    setup_ui::ViewOptions view_options() const {
        setup_ui::ViewOptions screen;
        screen.window_title = "Benefactor setup";
        screen.font_path = options.font_path.string();
        return screen;
    }

    /* Publish the accepted set and stop the screen. */
    void finish_accepted() {
        std::string read_error;
        if (!committed_disks(options.store_root, disks, read_error)) {
            error = read_error;
            finished = true;
            return;
        }
        accepted = true;
        finished = true;
    }
};

SetupFlow::SetupFlow(SetupFlowOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
}

SetupFlow::~SetupFlow() {
    impl_->view.close();
}

bool SetupFlow::open() {
    if (!impl_->options.request_selection) {
        impl_->error = "the platform does not provide a file picker";
        impl_->finished = true;
        return false;
    }
    // A process killed with the screen open (Android force-stop, a crash) never
    // runs the session destructor, so its staging directory is pruned here.
    setup_ui::Session::discard_stale_staging(impl_->options.staging_root);
    if (!impl_->view.open()) {
        impl_->error = impl_->view.last_error();
        impl_->finished = true;
        return false;
    }
    impl_->opened = true;
    return true;
}

bool SetupFlow::step() {
    if (impl_->finished) {
        return false;
    }
    for (const setup_ui::Request &request : impl_->view.poll()) {
        switch (request.kind) {
        case setup_ui::RequestKind::Browse: {
            std::lock_guard lock(impl_->pending_mutex);
            if (impl_->picker_active) {
                break;
            }
            impl_->picker_active = true;
            benefactor_log_write(BENEFACTOR_LOG_INFO, "setup",
                                 "asking the platform for the disk images");
            // The picker answers through this flow's own deliver(), which is
            // thread-safe: a platform picker may complete on another thread.
            const SetupDeliver answer = [this](const std::vector<std::filesystem::path> &paths) {
                SetupFlow::deliver(paths);
            };
            impl_->options.request_selection(answer);
            break;
        }
        case setup_ui::RequestKind::Start:
            if (impl_->session.status() == setup_ui::Status::Accepted) {
                impl_->view.finish();
            }
            impl_->session.validate_if_ready();
            break;
        case setup_ui::RequestKind::Cancel:
            impl_->error = "setup was dismissed before a disk set was provided";
            impl_->view.finish();
            break;
        }
    }

    std::vector<std::filesystem::path> chosen;
    {
        std::lock_guard lock(impl_->pending_mutex);
        if (!impl_->pending.empty() || impl_->picker_active) {
            chosen.swap(impl_->pending);
            impl_->picker_active = false;
        }
    }
    if (!chosen.empty()) {
        std::string selection_error;
        const std::size_t added = impl_->session.add_selected(chosen, selection_error);
        if (added == 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "setup", "%s", selection_error.c_str());
        } else if (!selection_error.empty()) {
            /* An incomplete set is the normal next step, not a failure. */
            benefactor_log_write(BENEFACTOR_LOG_INFO, "setup", "%s", selection_error.c_str());
        }
        if (!selection_error.empty()) {
            impl_->error = selection_error;
        }
    }

    impl_->session.validate_if_ready();
    if (impl_->session.status() == setup_ui::Status::Accepted) {
        // Keep the accepted screen visible for one more frame so the player
        // sees the result before the game starts.
        impl_->view.frame();
        impl_->view.finish();
        impl_->finish_accepted();
        return false;
    }
    impl_->view.frame();
    if (!impl_->view.running()) {
        impl_->finished = true;
        if (impl_->error.empty()) {
            impl_->error = "no disk set was provided";
        }
    }
    return !impl_->finished;
}

bool SetupFlow::accepted() const {
    return impl_->accepted;
}

const std::array<std::filesystem::path, 3> &SetupFlow::disks() const {
    return impl_->disks;
}

const std::string &SetupFlow::error() const {
    return impl_->error;
}

void SetupFlow::deliver(const std::vector<std::filesystem::path> &paths) {
    std::lock_guard lock(impl_->pending_mutex);
    impl_->pending = paths;
}

SetupFlowResult run_setup_flow(const SetupRequestSelection &request_selection,
                               const std::filesystem::path &staging_root,
                               const std::filesystem::path &store_root) {
    SetupFlowResult result;
    SetupFlowOptions options;
    options.request_selection = request_selection;
    options.staging_root = staging_root;
    options.store_root = store_root;
    SetupFlow flow(std::move(options));
    if (!flow.open()) {
        result.error = flow.error();
        return result;
    }
    while (flow.step()) {
    }
    if (flow.accepted()) {
        result.ok = true;
        result.disks = flow.disks();
        return result;
    }
    result.error = flow.error();
    return result;
}

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

} // namespace benefactor::platform
