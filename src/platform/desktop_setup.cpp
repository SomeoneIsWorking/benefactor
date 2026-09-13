/* desktop_setup.cpp — desktop entry into the shared setup flow.
 *
 * The window, picker, and persistence are all owned elsewhere: the setup-ui
 * screen draws the in-app UI, SDL's native file chooser selects the images,
 * and setup_flow applies the shipped disk identity before publishing. */
#include "platform/desktop_setup.h"

#include "platform/setup_flow.h"

#include <SDL3/SDL.h>
#include <lucent/platform.h>

#include <array>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char *kApplicationName = "benefactor";

std::optional<std::filesystem::path> user_data_directory() {
    auto directory = lucent::platform::user_data_directory(kApplicationName);
    if (!directory)
        return std::nullopt;
    std::string error;
    if (!lucent::platform::ensure_user_data_directory(*directory, error))
        return std::nullopt;
    return directory;
}

struct DialogResult {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    std::vector<std::filesystem::path> paths;
};

void SDLCALL dialog_callback(void *userdata, const char *const *filelist, int) {
    auto &result = *static_cast<DialogResult *>(userdata);
    std::lock_guard lock(result.mutex);
    if (filelist != nullptr) {
        for (const char *const *path = filelist; *path != nullptr; ++path)
            result.paths.emplace_back(*path);
    }
    result.complete = true;
    result.condition.notify_one();
}

/* SDL delivers the chooser result on the thread that pumps events, which is
 * this one; the wait below is therefore a pump loop, not a blocking join. */
void request_selection(benefactor::platform::SetupDeliver deliver) {
    auto result = std::make_shared<DialogResult>();
    constexpr SDL_DialogFileFilter filter = {"Disk images or one ZIP archive", "*"};
    SDL_ShowOpenFileDialog(dialog_callback, result.get(), nullptr, &filter, 1, nullptr, true);
    while (true) {
        {
            std::unique_lock lock(result->mutex);
            if (result->complete)
                break;
        }
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    deliver(result->paths);
}

} // namespace

extern "C" int desktop_setup_disks(const char **disks, int capacity) {
    if (disks == nullptr || capacity < 3 || !SDL_Init(SDL_INIT_VIDEO))
        return 0;
    const auto directory = user_data_directory();
    if (!directory)
        return 0;

    const auto store_root = *directory;
    std::array<std::filesystem::path, 3> committed;
    std::string error;
    if (benefactor::platform::committed_disks(store_root, committed, error)) {
        static std::array<std::string, 3> stable;
        for (std::size_t index = 0; index < committed.size(); ++index) {
            stable[index] = committed[index].string();
            disks[index] = stable[index].c_str();
        }
        return 1;
    }

    auto flow =
        benefactor::platform::run_setup_flow(request_selection, *directory / "setup", store_root);
    if (!flow.ok)
        return 0;
    static std::array<std::string, 3> stable;
    for (std::size_t index = 0; index < flow.disks.size(); ++index) {
        stable[index] = flow.disks[index].string();
        disks[index] = stable[index].c_str();
    }
    return 1;
}
