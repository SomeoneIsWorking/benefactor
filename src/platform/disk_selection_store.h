#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace benefactor::platform {

class DiskSelectionStore {
  public:
    using Paths = std::array<std::filesystem::path, 3>;

    explicit DiskSelectionStore(std::filesystem::path directory);

    [[nodiscard]] bool read(Paths &paths) const;
    [[nodiscard]] bool persist_direct(const Paths &paths, std::string &error) const;

    [[nodiscard]] bool prepare_import(std::string &error) const;
    [[nodiscard]] std::filesystem::path import_directory() const;
    void discard_import(std::string &error) const;
    [[nodiscard]] bool publish_import(const Paths &prepared_paths, Paths &installed_paths,
                                      std::string &error) const;

  private:
    [[nodiscard]] bool persist(const Paths &paths, std::string &error) const;
    [[nodiscard]] std::filesystem::path inactive_slot(std::string &error) const;

    std::filesystem::path directory_;
};

} // namespace benefactor::platform
