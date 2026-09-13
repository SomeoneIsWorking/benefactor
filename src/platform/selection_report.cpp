/* selection_report.cpp — see selection_report.h. */
#include "platform/selection_report.h"

namespace benefactor::platform {
namespace {

bool is_leaf_name(const std::string &name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

} // namespace

std::vector<std::filesystem::path>
selection_report_paths(const std::filesystem::path &staging_directory,
                       const std::vector<std::string> &document_names) {
    std::vector<std::filesystem::path> paths;
    if (staging_directory.empty()) {
        return paths;
    }
    paths.reserve(document_names.size());
    for (const std::string &name : document_names) {
        if (!is_leaf_name(name)) {
            return {};
        }
        paths.push_back(staging_directory / name);
    }
    return paths;
}

} // namespace benefactor::platform
