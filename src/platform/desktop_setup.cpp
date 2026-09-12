#include "platform/desktop_setup.h"

#include <SDL3/SDL.h>
#include <lucent/content.h>
#include <lucent/platform.h>
#include <lucent/zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr std::array<const char *, 3> kDiskNames = {"Disk.1", "Disk.2", "Disk.3"};
constexpr std::array<std::uintmax_t, 3> kDiskSizes = {1003520, 1003520, 1003520};
constexpr std::array<const char *, 3> kDiskHashes = {
    "25416a6e390cbe94e4b2375c9513a2adf3411072fc5b6069ea34a0f3ff697916",
    "f3649c8db4adfce3c7da5e21cb018be098404771eceeec44741c2528e9071b73",
    "8dd262d02174a6706d5214b25f7bd9fc4bffe94761e16c209b880bc1dd8e7a42"};
constexpr const char *kApplicationName = "benefactor";
constexpr const char *kSelectionFile = "disk-selection.txt";

struct SelectionResult {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    std::vector<std::filesystem::path> paths;
    std::string error;
};

std::optional<std::filesystem::path> user_data_directory() {
    const auto directory = lucent::platform::user_data_directory(kApplicationName);
    if (!directory)
        return std::nullopt;
    std::string error;
    if (!lucent::platform::ensure_user_data_directory(*directory, error))
        return std::nullopt;
    return directory;
}

bool validate_disk(const std::filesystem::path &path, std::size_t index, std::string &error) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status) || status) {
        error = std::string{kDiskNames[index]} + " is not a regular file";
        return false;
    }
    if (std::filesystem::file_size(path, status) != kDiskSizes[index] || status) {
        error = std::string{kDiskNames[index]} + " has the wrong size";
        return false;
    }
    std::string hash_error;
    const auto digest = lucent::content::sha256_file(path, hash_error);
    if (!digest || lucent::content::sha256_hex(*digest) != kDiskHashes[index]) {
        error = digest
                    ? std::string{kDiskNames[index]} + " does not match the supported disk identity"
                    : hash_error;
        return false;
    }
    return true;
}

bool validate_set(const std::array<std::filesystem::path, 3> &paths, std::string &error) {
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (!validate_disk(paths[index], index, error))
            return false;
    }
    return true;
}

bool read_persisted(const std::filesystem::path &directory,
                    std::array<std::filesystem::path, 3> &paths) {
    std::ifstream input(directory / kSelectionFile);
    if (!input)
        return false;
    std::array<std::string, 3> lines;
    for (std::string &line : lines) {
        if (!std::getline(input, line) || line.empty())
            return false;
    }
    for (std::size_t index = 0; index < paths.size(); ++index)
        paths[index] = std::filesystem::path(lines[index]);
    std::string error;
    return validate_set(paths, error);
}

bool persist_selection(const std::filesystem::path &directory,
                       const std::array<std::filesystem::path, 3> &paths) {
    const auto temporary = directory / (std::string{kSelectionFile} + ".new");
    std::ofstream output(temporary, std::ios::trunc);
    if (!output)
        return false;
    for (const auto &path : paths)
        output << path.string() << '\n';
    output.close();
    if (!output)
        return false;
    std::error_code status;
    std::filesystem::rename(temporary, directory / kSelectionFile, status);
    if (!status)
        return true;
    std::error_code cleanup_status;
    std::filesystem::remove(temporary, cleanup_status);
    return false;
}

void SDLCALL dialog_callback(void *userdata, const char *const *filelist, int) {
    auto &result = *static_cast<SelectionResult *>(userdata);
    std::lock_guard lock(result.mutex);
    if (filelist == nullptr) {
        result.error = SDL_GetError();
    } else if (filelist[0] == nullptr) {
        result.error = "No disk files were selected";
    } else {
        for (const char *const *path = filelist; *path != nullptr; ++path)
            result.paths.emplace_back(*path);
    }
    result.complete = true;
    result.condition.notify_one();
}

bool choose_files(std::vector<std::filesystem::path> &paths, std::string &error) {
    constexpr SDL_DialogFileFilter filter = {"Benefactor disk images or ZIP archives", "*"};
    SelectionResult result;
    SDL_ShowOpenFileDialog(dialog_callback, &result, nullptr, &filter, 1, nullptr, true);
    while (true) {
        {
            std::unique_lock lock(result.mutex);
            if (result.complete) {
                paths = std::move(result.paths);
                error = std::move(result.error);
                return error.empty();
            }
        }
        SDL_PumpEvents();
        SDL_Delay(10);
    }
}

bool is_zip_archive(const std::filesystem::path &path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".zip";
}

bool resolve_direct_files(const std::vector<std::filesystem::path> &selected,
                          std::array<std::filesystem::path, 3> &paths, std::string &error) {
    if (selected.size() != paths.size()) {
        error = "Select exactly Disk.1, Disk.2, and Disk.3, or one ZIP archive";
        return false;
    }
    for (const auto &path : selected) {
        const auto name = path.filename().string();
        for (std::size_t index = 0; index < kDiskNames.size(); ++index) {
            if (name == kDiskNames[index]) {
                if (!paths[index].empty()) {
                    error = "Each disk file must be selected exactly once";
                    return false;
                }
                paths[index] = path;
                break;
            }
        }
    }
    for (const auto &path : paths) {
        if (path.empty()) {
            error = "Select exactly Disk.1, Disk.2, and Disk.3";
            return false;
        }
    }
    return validate_set(paths, error);
}

bool resolve_zip(const std::filesystem::path &archive, const std::filesystem::path &directory,
                 std::array<std::filesystem::path, 3> &paths, std::string &error) {
    const auto pending = directory / "disk-import";
    std::error_code status;
    std::filesystem::remove_all(pending, status);
    if (status) {
        error = "Could not clear the previous disk import";
        return false;
    }
    std::vector<std::filesystem::path> files;
    if (!lucent::zip::extract_archive(archive, pending, files, error))
        return false;
    for (const auto &file : files) {
        const auto name = file.filename().string();
        for (std::size_t index = 0; index < kDiskNames.size(); ++index) {
            if (name != kDiskNames[index])
                continue;
            if (!paths[index].empty()) {
                error = "The ZIP contains duplicate " + name + " files";
                std::filesystem::remove_all(pending, status);
                return false;
            }
            paths[index] = file;
        }
    }
    if (!validate_set(paths, error)) {
        std::filesystem::remove_all(pending, status);
        return false;
    }
    const auto installed = directory / "disk-set";
    std::filesystem::remove_all(installed, status);
    if (status) {
        error = "Could not replace the previous disk set";
        std::filesystem::remove_all(pending, status);
        return false;
    }
    std::filesystem::rename(pending, installed, status);
    if (status) {
        error = "Could not publish the validated disk set";
        std::filesystem::remove_all(pending, status);
        return false;
    }
    for (std::size_t index = 0; index < paths.size(); ++index)
        paths[index] = installed / kDiskNames[index];
    return true;
}

bool show_setup_message() {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Browse"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
    const SDL_MessageBoxData data = {SDL_MESSAGEBOX_INFORMATION,
                                     nullptr,
                                     "Benefactor setup",
                                     "Choose your original Disk.1, Disk.2, and Disk.3 files, "
                                     "or one ZIP containing them.",
                                     2,
                                     buttons,
                                     nullptr};
    int button = 0;
    if (!SDL_ShowMessageBox(&data, &button))
        return false;
    return button == 1;
}

void publish_paths(const std::array<std::filesystem::path, 3> &paths, const char **disks) {
    static std::array<std::string, 3> stable_paths;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        stable_paths[index] = paths[index].string();
        disks[index] = stable_paths[index].c_str();
    }
}

} // namespace

extern "C" int desktop_setup_disks(const char **disks, int capacity) {
    if (disks == nullptr || capacity < 3 || !SDL_Init(SDL_INIT_VIDEO))
        return 0;
    const auto directory = user_data_directory();
    if (!directory)
        return 0;
    std::array<std::filesystem::path, 3> paths;
    if (read_persisted(*directory, paths)) {
        publish_paths(paths, disks);
        return 1;
    }

    if (!show_setup_message())
        return 0;
    std::vector<std::filesystem::path> selected;
    std::string error;
    if (!choose_files(selected, error))
        return 0;
    if (selected.size() == 1 && is_zip_archive(selected[0])) {
        if (!resolve_zip(selected[0], *directory, paths, error))
            return 0;
    } else if (!resolve_direct_files(selected, paths, error)) {
        return 0;
    }
    if (!persist_selection(*directory, paths))
        return 0;
    publish_paths(paths, disks);
    return 1;
}
