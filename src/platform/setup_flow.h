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
#include <memory>
#include <string>
#include <vector>

namespace benefactor::platform {

// Asks the host for a file selection. The platform implementation must invoke
// `deliver` exactly once, with the chosen paths (empty when the player
// cancelled). It may complete asynchronously and from another thread; the flow
// applies the result on its own thread.
using SetupDeliver = std::function<void(const std::vector<std::filesystem::path> &)>;
using SetupRequestSelection = std::function<void(SetupDeliver)>;

/* Everything the flow needs from the host: its picker, where staged files and
 * the published set live, and the font the screen draws with. An empty
 * `font_path` selects the first usable system font; a host with no system font
 * (the browser) ships one and names it. */
struct SetupFlowOptions {
    SetupRequestSelection request_selection;
    std::filesystem::path staging_root;
    std::filesystem::path store_root;
    std::filesystem::path font_path;
};

struct SetupFlowResult {
    bool ok = false;
    std::array<std::filesystem::path, 3> disks;
    std::string error;
};

/* The setup screen as a state machine: one step() per host frame. A host with
 * its own loop that must keep running (the browser's) drives it that way; a
 * host that can block (SDL desktop, Android) uses run_setup_flow() below. The
 * policy is identical because there is one implementation of it. */
class SetupFlow {
  public:
    explicit SetupFlow(SetupFlowOptions options);
    ~SetupFlow();
    SetupFlow(const SetupFlow &) = delete;
    SetupFlow &operator=(const SetupFlow &) = delete;

    /* Create the screen. False means it could not be shown; error() says why. */
    bool open();

    /* Run one iteration. Returns true while the screen is still up; false once
     * the player accepted a set, dismissed the screen, or it failed. */
    bool step();

    [[nodiscard]] bool accepted() const;
    [[nodiscard]] const std::array<std::filesystem::path, 3> &disks() const;
    [[nodiscard]] const std::string &error() const;

    /* Hand the platform picker's result to the flow (any thread; the paths are
     * applied by step()). */
    void deliver(const std::vector<std::filesystem::path> &paths);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Blocking wrapper for a host whose loop the screen itself owns. */
SetupFlowResult run_setup_flow(const SetupRequestSelection &request_selection,
                               const std::filesystem::path &staging_root,
                               const std::filesystem::path &store_root);

/* Committed disks from a previous run, when they still validate. */
bool committed_disks(const std::filesystem::path &store_root,
                     std::array<std::filesystem::path, 3> &disks, std::string &error);

} // namespace benefactor::platform
