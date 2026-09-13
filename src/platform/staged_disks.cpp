/* staged_disks.cpp — the staged-upload disk-set resolver.
 *
 * See staged_disks.h. The staged set is either the three exact disk files or
 * one bounded ZIP archive containing them; both resolve into the store's
 * all-or-nothing promotion so a rejected upload never displaces the current
 * installation. */
#include "platform/staged_disks.h"

#include "platform/disk_identity.h"

#include <lucent/zip.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace benefactor::platform {
namespace {

constexpr auto kDiskCount = 3U;

} // namespace

std::string validate_staged_disks(DiskSelectionStore *store,
                                  const std::vector<setup_ui::StagedFile> &files) {
    DiskSelectionStore::Paths found;
    bool have_zip = false;
    std::filesystem::path archive;
    for (const setup_ui::StagedFile &file : files) {
        const std::string name = file.spec.name;
        bool matched = false;
        for (std::size_t disk = 0; disk < kDiskCount; ++disk) {
            if (name == kDiskNames[disk]) {
                if (!found[disk].empty())
                    return "Each disk image must be provided exactly once";
                found[disk] = file.path;
                matched = true;
                break;
            }
        }
        if (matched)
            continue;
        if (file.is_archive) {
            if (have_zip)
                return "Provide one archive, not two";
            have_zip = true;
            archive = file.path;
            continue;
        }
        return "The chosen files include something that is not part of the disk set: " + name;
    }

    if (have_zip) {
        if (files.size() != 1)
            return "Provide either one ZIP or the three disk images, not both";
        std::string prepare_error;
        const auto pending = store->reserve_import(prepare_error);
        if (pending.empty())
            return prepare_error.empty() ? "The disk import area could not be prepared"
                                         : prepare_error;
        std::vector<std::filesystem::path> extracted;
        std::string failure;
        const lucent::zip::ExtractionLimits limits{.max_archive_bytes = 32ULL * 1024u * 1024u,
                                                   .max_extracted_bytes = 16ULL * 1024u * 1024u,
                                                   .max_entry_bytes = 4ULL * 1024u * 1024u,
                                                   .max_entries = 128u};
        if (!lucent::zip::extract_archive(archive, pending, extracted, failure, limits)) {
            store->discard_import(failure);
            return failure.empty() ? "The archive could not be read" : failure;
        }
        for (const auto &path : extracted) {
            const std::string name = path.filename().string();
            for (std::size_t disk = 0; disk < kDiskCount; ++disk) {
                if (name != kDiskNames[disk])
                    continue;
                if (!found[disk].empty()) {
                    store->discard_import(failure);
                    return "The archive contains more than one " + name;
                }
                found[disk] = path;
            }
        }
    }

    if (std::any_of(found.begin(), found.end(),
                    [](const std::filesystem::path &path) { return path.empty(); })) {
        std::string failure = "All three images are required: Disk.1, Disk.2, and Disk.3";
        if (have_zip)
            store->discard_import(failure);
        return failure;
    }
    std::string failure;
    if (!validate_set(found, failure)) {
        if (have_zip)
            store->discard_import(failure);
        return failure;
    }
    if (!have_zip) {
        // Direct picks are staged outside the store's managed slot, so move
        // them into the store's own import directory before publication: the
        // promotion contract accepts only paths it owns.
        const auto pending = store->create_import_directory(failure);
        if (pending.empty())
            return failure;
        DiskSelectionStore::Paths relocated;
        for (std::size_t index = 0; index < found.size(); ++index) {
            const auto target = pending / kDiskNames[index];
            std::error_code status;
            std::filesystem::copy_file(found[index], target,
                                       std::filesystem::copy_options::overwrite_existing, status);
            if (status) {
                failure = "The chosen image could not be stored: " + status.message();
                store->discard_import(failure);
                return failure;
            }
            relocated[index] = target;
        }
        found = std::move(relocated);
    }

    DiskSelectionStore::Paths installed;
    std::string publish_error;
    if (!store->publish_import(found, installed, publish_error))
        return publish_error;
    return {};
}

} // namespace benefactor::platform
