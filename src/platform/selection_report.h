#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace benefactor::platform {

/* A file picker reports what the player chose as the private directory it
 * copied the selection into plus the display names of the documents it staged.
 * Nothing about the title's disk set is decided here: this turns that report
 * into the candidate paths the staged-set resolver validates, and refuses a
 * name that is not a plain leaf so a report can never reach outside the
 * directory the platform owns. */
[[nodiscard]] std::vector<std::filesystem::path>
selection_report_paths(const std::filesystem::path &staging_directory,
                       const std::vector<std::string> &document_names);

} // namespace benefactor::platform
