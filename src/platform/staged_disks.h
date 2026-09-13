/* staged_disks.h — the staged-upload disk-set resolver for the setup host.
 *
 * Both the Android browser flow and the desktop browser flow hand staged
 * uploads to setup_ui; this owner turns a staged set (three exact files or
 * one bounded ZIP) into a validated, published disk set through the title's
 * DiskSelectionStore. One implementation, two consumers. */
#pragma once

#include "platform/disk_selection_store.h"

#include <string>
#include <vector>

#include <setup_ui/setup_ui.h>

namespace benefactor::platform {

/* setup-ui validator over a DiskSelectionStore: accepts the three exact disk
 * images or one ZIP containing them, and publishes the accepted set through
 * the store's all-or-nothing promotion. Returns an empty string on success, or
 * the reason the set was refused. */
std::string validate_staged_disks(DiskSelectionStore *store,
                                  const std::vector<setup_ui::StagedFile> &files);

} // namespace benefactor::platform
