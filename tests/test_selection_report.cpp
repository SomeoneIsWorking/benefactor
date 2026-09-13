/* The picker report is the boundary between what Android staged and what the
 * title validates: only the documents the report names may become candidates,
 * and a report can never address a path outside its own staging directory. */
#include "platform/selection_report.h"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

using benefactor::platform::selection_report_paths;

namespace {

void reported_documents_become_candidate_paths() {
    const std::filesystem::path staging{"staged"};
    const auto paths = selection_report_paths(staging, {"Disk.1", "Disk.2", "Disk.3"});
    assert(paths.size() == 3);
    assert(paths[0] == staging / "Disk.1");
    assert(paths[1] == staging / "Disk.2");
    assert(paths[2] == staging / "Disk.3");
}

void a_single_archive_is_one_candidate() {
    const auto paths = selection_report_paths("staged", {"benefactor-disks.zip"});
    assert(paths.size() == 1);
    assert(paths[0].filename() == "benefactor-disks.zip");
}

void an_empty_report_yields_no_candidates() {
    assert(selection_report_paths("staged", {}).empty());
    assert(selection_report_paths("", {"Disk.1"}).empty());
}

void a_name_that_is_not_a_leaf_is_refused() {
    // A report naming a path outside its own directory contributes nothing at
    // all, rather than a partially accepted selection.
    assert(selection_report_paths("staged", {"../Disk.1"}).empty());
    assert(selection_report_paths("staged", {"nested/Disk.1"}).empty());
    assert(selection_report_paths("staged", {"nested\\Disk.1"}).empty());
    assert(selection_report_paths("staged", {""}).empty());
    assert(selection_report_paths("staged", {".."}).empty());
    assert(selection_report_paths("staged", {"Disk.1", "../Disk.2"}).empty());
}

} // namespace

int main() {
    reported_documents_become_candidate_paths();
    a_single_archive_is_one_candidate();
    an_empty_report_yields_no_candidates();
    a_name_that_is_not_a_leaf_is_refused();
    return 0;
}
