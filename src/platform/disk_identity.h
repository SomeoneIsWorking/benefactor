/* disk_identity.h — the shipped disk-set identity owner.
 *
 * One authoritative statement of the three disk names, sizes, and digests,
 * plus validation over an ordered path set. desktop_setup (native file
 * dialog) and the Android/browser setup host both consume this; neither
 * re-derives the identity. */
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>

namespace benefactor::platform {

extern const char *const kDiskNames[3];

/* Validate three paths against the shipped Disk.1-3 identity, in order.
 * Writes a human-readable reason into `error` on failure. Returns true when
 * the set matches. */
bool validate_set(const std::array<std::filesystem::path, 3> &paths, std::string &error);

} // namespace benefactor::platform
