/* web_setup.cpp — the browser product's entry point.
 *
 * The browser shows the same setup screen as the other products: the shared
 * setup-ui document rendered into the canvas, with the flow, the disk identity,
 * and the publication in the title's own code. Only the file picker is the
 * platform's — a hidden <input type="file"> — and the picked files arrive in the
 * browser's filesystem, so they are handed to the flow by name exactly like
 * Android's SAF import hands over its staged documents.
 *
 * The screen cannot block: the browser's event loop has to keep running, so the
 * setup flow is stepped once per animation frame and the game starts on the
 * frame after the player accepts a set. */
#include "platform/selection_report.h"
#include "platform/setup_flow.h"

#include "platform/update_transport.h"
#include "port/port.h"

#include <emscripten/emscripten.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char *kDataRoot = "/benefactor-data";
constexpr const char *kSetupRoot = "/benefactor-data/setup";
constexpr const char *kStoreRoot = "/benefactor-data/disks";
constexpr const char *kImportRoot = "/benefactor-data/import";
constexpr const char *kFontPath = "/setup-font.ttf";

std::unique_ptr<benefactor::platform::SetupFlow> g_flow;
std::vector<std::string> g_pick_names;
bool g_game_started = false;
bool g_reported_ready = false;
std::array<std::string, 3> g_disk_paths;

/* The page owns visible status: it is the only surface that still has text when
 * the canvas cannot be drawn (no WebGL, a failed picker). */
void report_status(const char *status) {
    EM_ASM({ benefactorWebStatus(UTF8ToString($0)); }, status);
}

void start_game() {
    std::array<const char *, 3> disks{};
    for (std::size_t index = 0; index < g_disk_paths.size(); ++index) {
        disks[index] = g_disk_paths[index].c_str();
    }
    if (pc_init_from_disk(disks.data(), 3) < 0) {
        report_status("The game could not start from the validated disks.");
        return;
    }
    g_game_started = true;
    /* Once, in the background: the page fetches the latest release and the
     * product's own rule decides what its tag means. */
    platform_update_check_start();
}

/* Asked by the flow when the player presses Choose files. The picker reports
 * back through the benefactor_web_pick_* exports below; the delivery callback
 * the flow passes is its own deliver(), so nothing is kept here. */
void request_selection(benefactor::platform::SetupDeliver /*deliver*/) {
    EM_ASM({ benefactorWebPickFiles(UTF8ToString($0)); }, kImportRoot);
}

void web_frame() {
    if (!g_game_started) {
        if (!g_flow) {
            return;
        }
        if (g_flow->step()) {
            return;
        }
        if (g_flow->accepted()) {
            const auto &disks = g_flow->disks();
            for (std::size_t index = 0; index < g_disk_paths.size(); ++index) {
                g_disk_paths[index] = disks[index].string();
            }
            g_flow.reset();
            start_game();
            return;
        }
        report_status(g_flow->error().empty() ? "Setup was cancelled." : g_flow->error().c_str());
        g_flow.reset();
        emscripten_cancel_main_loop();
        return;
    }
    if (pc_step() != 0) {
        emscripten_cancel_main_loop();
    }
}

} // namespace

/* The picker's result: one call per document the page staged in the import
 * directory below. A started pick replaces any earlier selection. The page is
 * told where to write, and never names that path back: the import directory is
 * this product's own, and a page-supplied path would be a boundary the title
 * does not need to open. */
extern "C" EMSCRIPTEN_KEEPALIVE void benefactor_web_pick_begin() { g_pick_names.clear(); }

extern "C" EMSCRIPTEN_KEEPALIVE void benefactor_web_pick_add(const char *name) {
    if (name != nullptr) {
        g_pick_names.emplace_back(name);
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void benefactor_web_pick_end() {
    if (!g_flow) {
        return;
    }
    /* Only the documents the picker reports become candidates, and a name that
     * is not a leaf contributes nothing: the same rule the native platforms
     * hand their picker results through. */
    g_flow->deliver(benefactor::platform::selection_report_paths(kImportRoot, g_pick_names));
    g_pick_names.clear();
}

int main() {
    std::error_code status;
    std::filesystem::create_directories(kDataRoot, status);
    std::filesystem::create_directories(kImportRoot, status);

    benefactor::platform::SetupFlowOptions options;
    options.request_selection = request_selection;
    options.staging_root = kSetupRoot;
    options.store_root = kStoreRoot;
    options.font_path = kFontPath;
    g_flow = std::make_unique<benefactor::platform::SetupFlow>(options);
    if (!g_flow->open()) {
        report_status(g_flow->error().c_str());
        g_flow.reset();
        return 1;
    }
    if (!g_reported_ready) {
        g_reported_ready = true;
        report_status(
            "Select your original Disk.1, Disk.2, and Disk.3 files, or one ZIP containing them.");
    }
    emscripten_set_main_loop(web_frame, 0, 1);
    return 0;
}
