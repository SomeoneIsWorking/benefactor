#include "platform/disk_selection_store.h"

#include <array>
#include <fstream>
#include <system_error>
#include <utility>

namespace benefactor::platform {
namespace {

constexpr const char *kSelectionFile = "disk-selection.txt";
constexpr const char *kImportDirectory = "disk-import";
constexpr const char *kSlots[] = {"disk-set-a", "disk-set-b"};

bool inside(const std::filesystem::path &path, const std::filesystem::path &directory) {
    const auto relative = path.lexically_relative(directory);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

void discard_path(const std::filesystem::path &path, std::string &error) {
    std::error_code status;
    std::filesystem::remove_all(path, status);
    if (status)
        error += (error.empty() ? "" : "; ") + std::string{"could not clear "} + path.string();
}

} // namespace

DiskSelectionStore::DiskSelectionStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

bool DiskSelectionStore::read(Paths &paths) const {
    std::ifstream input(directory_ / kSelectionFile);
    if (!input)
        return false;
    Paths found;
    for (auto &path : found) {
        std::string line;
        if (!std::getline(input, line) || line.empty())
            return false;
        path = std::filesystem::path(line);
    }
    std::string extra;
    if (std::getline(input, extra))
        return false;
    paths = std::move(found);
    return true;
}

bool DiskSelectionStore::persist(const Paths &paths, std::string &error) const {
    const auto temporary = directory_ / (std::string{kSelectionFile} + ".new");
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        error = "Could not stage the disk selection";
        return false;
    }
    for (const auto &path : paths) {
        const auto line = path.string();
        if (line.empty() || line.find('\n') != std::string::npos ||
            line.find('\r') != std::string::npos) {
            error = "Disk paths must not contain line breaks";
            output.close();
            discard_path(temporary, error);
            return false;
        }
        output << line << '\n';
    }
    output.close();
    if (!output) {
        error = "Could not finish the disk selection";
        discard_path(temporary, error);
        return false;
    }
    std::error_code status;
    std::filesystem::rename(temporary, directory_ / kSelectionFile, status);
    if (status) {
        error = "Could not publish the disk selection";
        discard_path(temporary, error);
        return false;
    }
    return true;
}

bool DiskSelectionStore::persist_direct(const Paths &paths, std::string &error) const {
    return persist(paths, error);
}

std::filesystem::path DiskSelectionStore::import_directory() const {
    return directory_ / kImportDirectory;
}

void DiskSelectionStore::discard_import(std::string &error) const {
    discard_path(import_directory(), error);
    discard_path(directory_ / "disk-import.lucent-stage", error);
}

bool DiskSelectionStore::prepare_import(std::string &error) const {
    std::error_code status;
    if (!std::filesystem::is_directory(directory_, status) || status) {
        error = "The application data directory is unavailable";
        return false;
    }
    discard_import(error);
    return error.empty();
}

std::filesystem::path DiskSelectionStore::inactive_slot(std::string &error) const {
    Paths previous;
    bool uses_a = false;
    bool uses_b = false;
    if (read(previous)) {
        for (const auto &path : previous) {
            uses_a |= inside(path, directory_ / kSlots[0]);
            uses_b |= inside(path, directory_ / kSlots[1]);
        }
    }
    if (uses_a && uses_b) {
        error = "The persisted disks span both managed install slots";
        return {};
    }
    return directory_ / kSlots[uses_a ? 1 : 0];
}

bool DiskSelectionStore::publish_import(const Paths &prepared_paths, Paths &installed_paths,
                                        std::string &error) const {
    const auto prepared = import_directory();
    std::array<std::filesystem::path, 3> relative_paths;
    for (std::size_t index = 0; index < prepared_paths.size(); ++index) {
        std::error_code status;
        if (!inside(prepared_paths[index], prepared) ||
            !std::filesystem::is_regular_file(prepared_paths[index], status) || status) {
            error = "An extracted disk is missing or outside the prepared import";
            return false;
        }
        relative_paths[index] = prepared_paths[index].lexically_relative(prepared);
    }
    const auto target = inactive_slot(error);
    if (!error.empty())
        return false;
    discard_path(target, error);
    if (!error.empty())
        return false;

    std::error_code status;
    std::filesystem::rename(prepared, target, status);
    if (status) {
        error = "Could not publish the validated disk import";
        return false;
    }
    Paths candidate;
    for (std::size_t index = 0; index < candidate.size(); ++index)
        candidate[index] = target / relative_paths[index];
    if (!persist(candidate, error)) {
        discard_path(target, error);
        return false;
    }
    installed_paths = std::move(candidate);
    return true;
}

} // namespace benefactor::platform
