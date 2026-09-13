/* setup_flow.h — the title-owned setup flow over the shared setup-ui screen.
 *
 * Same policy on every platform: show the in-app setup screen, ask the host's
 * native picker for files when the player taps Choose files, stage what comes
 * back, validate it against the shipped disk identity, and publish it through
 * DiskSelectionStore's all-or-nothing promotion. The host supplies only the
 * picker; identity, storage layout, and publication stay here. */
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace benefactor::platform {

// Asks the host for a file selection. The platform implementation must invoke
// `deliver` exactly once, with the chosen paths (empty when the player
// cancelled). It may complete asynchronously and from another thread; the flow
// applies the result on its own thread.
using SetupDeliver = std::function<void(const std::vector<std::filesystem::path> &)>;
using SetupRequestSelection = std::function<void(SetupDeliver)>;

struct SetupFlowResult {
    bool ok = false;
    std::array<std::filesystem::path, 3> disks;
    std::string error;
};

/* Runs the browser-free setup screen until the player provides a validated
 * disk set (ok), dismisses it, or the picker fails. `staging_root` is the
 * app-private directory the screen stages chosen files under. */
SetupFlowResult run_setup_flow(const SetupRequestSelection &request_selection,
                               const std::filesystem::path &staging_root,
                               const std::filesystem::path &store_root);

/* Committed disks from a previous run, when they still validate. */
bool committed_disks(const std::filesystem::path &store_root,
                     std::array<std::filesystem::path, 3> &disks, std::string &error);

} // namespace benefactor::platform
