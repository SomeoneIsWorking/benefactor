#include "platform/disk_selection_store.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

using benefactor::platform::DiskSelectionStore;

namespace {

void write_file(const std::filesystem::path &path, const std::string &contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << contents;
    assert(output.good());
}

DiskSelectionStore::Paths paths_in(const std::filesystem::path &directory) {
    return {directory / "Disk.1", directory / "Disk.2", directory / "Disk.3"};
}

void write_disks(const DiskSelectionStore::Paths &paths, const std::string &contents) {
    for (const auto &path : paths)
        write_file(path, contents);
}

} // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    assert(root.filename() == "disk-selection-store");
    assert(root.parent_path().filename() == "verification");
    std::error_code status;
    std::filesystem::remove_all(root, status);
    assert(!status);
    std::filesystem::create_directories(root);

    const DiskSelectionStore store(root);
    std::string error;
    const auto original = paths_in(root / "disk-set-a");
    write_disks(original, "old");
    assert(store.persist_direct(original, error));
    DiskSelectionStore::Paths loaded;
    assert(store.read(loaded) && loaded == original);

    assert(store.prepare_import(error));
    auto prepared = paths_in(store.import_directory() / "nested");
    write_disks(prepared, "new");
    std::filesystem::create_directory(root / "disk-selection.txt.new");
    DiskSelectionStore::Paths installed;
    assert(!store.publish_import(prepared, installed, error));
    assert(!error.empty());
    assert(store.read(loaded) && loaded == original);
    assert(std::filesystem::exists(original[0]));
    assert(!std::filesystem::exists(root / "disk-set-b"));
    std::filesystem::remove(root / "disk-selection.txt.new");
    error.clear();

    assert(store.prepare_import(error));
    prepared = paths_in(store.import_directory() / "nested");
    write_disks(prepared, "new");
    assert(store.publish_import(prepared, installed, error));
    assert(installed == paths_in(root / "disk-set-b" / "nested"));
    assert(store.read(loaded) && loaded == installed);
    assert(std::filesystem::exists(original[0]));
    assert(std::filesystem::exists(installed[0]));
    assert(!std::filesystem::exists(store.import_directory()));

    error.clear();
    assert(store.prepare_import(error));
    prepared = paths_in(store.import_directory());
    write_disks(prepared, "unsafe");
    prepared[0] = original[0];
    assert(!store.publish_import(prepared, loaded, error));
    assert(store.read(loaded) && loaded == installed);
    assert(std::filesystem::exists(installed[0]));
    store.discard_import(error);

    error.clear();
    auto invalid_direct = original;
    invalid_direct[0] = std::filesystem::path("Disk.1\ninvalid");
    assert(!store.persist_direct(invalid_direct, error));
    assert(store.read(loaded) && loaded == installed);

    std::filesystem::remove_all(root, status);
    assert(!status);
    return 0;
}
