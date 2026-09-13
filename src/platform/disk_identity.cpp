#include "platform/disk_identity.h"

#include <lucent/content.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace benefactor::platform {

const char *const kDiskNames[3] = {"Disk.1", "Disk.2", "Disk.3"};

namespace {

constexpr std::array<std::uintmax_t, 3> kDiskSizes = {1003520, 1003520, 1003520};
constexpr std::array<const char *, 3> kDiskHashes = {
    "25416a6e390cbe94e4b2375c9513a2adf3411072fc5b6069ea34a0f3ff697916",
    "f3649c8db4adfce3c7da5e21cb018be098404771eceeec44741c2528e9071b73",
    "8dd262d02174a6706d5214b25f7bd9fc4bffe94761e16c209b880bc1dd8e7a42"};

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

} // namespace

bool validate_set(const std::array<std::filesystem::path, 3> &paths, std::string &error) {
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (!validate_disk(paths[index], index, error))
            return false;
    }
    return true;
}

} // namespace benefactor::platform
