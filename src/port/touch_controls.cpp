/* touch_controls.cpp — Benefactor's on-screen controls.
 *
 * The shared touch-ui subsystem owns the geometry, the hit regions, the contact
 * claiming, and the drawing; this file owns the title's meanings: which controls
 * the player gets, which logical action each asserts, and when the overlay
 * steps aside. It is deliberately small — every line here is policy, not
 * mechanism. */
#include "port/touch_controls.h"

#include "port/config.h"
#include "port/input.h"
#include "port/overlay_ui.h"

#include "common/log.h"

#include "touch_ui/touch_ui.h"

#include <SDL3/SDL.h>

#include <memory>

extern "C" {
#include "engine/hw.h"
}

namespace {

/* Action bits are this title's own vocabulary; touch-ui only routes them. */
constexpr std::uint32_t kUp = 1U << 0;
constexpr std::uint32_t kDown = 1U << 1;
constexpr std::uint32_t kLeft = 1U << 2;
constexpr std::uint32_t kRight = 1U << 3;
constexpr std::uint32_t kFire = 1U << 4;
constexpr std::uint32_t kInteract = 1U << 5;
constexpr std::uint32_t kPause = 1U << 6;
constexpr std::uint32_t kTurbo = 1U << 7;
constexpr std::uint32_t kCamera = 1U << 8;

std::unique_ptr<touch_ui::Controls> g_controls;
/* The window's pixel size is not settled on the first frame — a rotating Android
 * window reports the pre-rotation surface — so the layout is reported whenever it
 * changes rather than once, which would describe a geometry nothing is drawn in. */
int g_reported_width = 0;
int g_reported_height = 0;

/* A pad in hand hides the overlay until the player touches the screen again:
 * presence of a controller is not a reason to erase controls, using it is. */
void note_controller_use(const SDL_Event &event) {
    const bool button = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    const bool stick =
        event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && SDL_abs(event.gaxis.value) >= 16000;
    if ((button || stick) && g_controls) {
        g_controls->note_controller_input();
    }
}

touch_ui::Config controls_config() {
    touch_ui::Config config;
    config.controls = {
        {1, touch_ui::Placement::dpad_up, "direction_up", false, kUp},
        {2, touch_ui::Placement::dpad_down, "direction_down", false, kDown},
        {3, touch_ui::Placement::dpad_left, "direction_left", false, kLeft},
        {4, touch_ui::Placement::dpad_right, "direction_right", false, kRight},
        {5, touch_ui::Placement::action_primary, "attack", true, kFire},
        {6, touch_ui::Placement::action_secondary, "use", true, kInteract},
        {7, touch_ui::Placement::top_right, "pause", true, kPause},
        /* Turbo is held like a pedal, in the third action slot beside the thumb;
         * the camera is a toggle, in the corner a thumb would cover. */
        {8, touch_ui::Placement::action_tertiary, "turbo", true, kTurbo},
        {9, touch_ui::Placement::top_left, "camera", true, kCamera},
    };
    return config;
}

/* The gameplay action a control stands for, or -1 for the pause button, which
 * is the pause menu's own key edge rather than a held logical action. */
int action_for(std::uint32_t control) {
    switch (control) {
    case kUp:
        return PI_UP;
    case kDown:
        return PI_DOWN;
    case kLeft:
        return PI_LEFT;
    case kRight:
        return PI_RIGHT;
    case kFire:
        return PI_FIRE;
    case kInteract:
        return PI_INTERACT;
    case kTurbo:
        return PI_FFWD; /* the same hold-to-fast-forward every other device uses */
    case kCamera:
        return PI_FREECAM; /* an edge on the binding: the toggle owns its state */
    default:
        return -1;
    }
}

/* The pause menu is a key-driven overlay: it reads navigation through its own
 * entry points, not through the gameplay action state, so a touch has to arrive
 * as the intent the keyboard and the pad deliver. */
int navigation_for(std::uint32_t control) {
    switch (control) {
    case kUp:
        return HW_NAV_UP;
    case kDown:
        return HW_NAV_DOWN;
    case kLeft:
        return HW_NAV_LEFT;
    case kRight:
        return HW_NAV_RIGHT;
    case kFire:
    case kInteract:
        return HW_NAV_SELECT;
    default:
        return -1;
    }
}

void apply_action(std::uint32_t actions, bool down) {
    benefactor_log_write(BENEFACTOR_LOG_INFO, "input", "touch actions 0x%X %s", actions,
                         down ? "pressed" : "released");
    const std::uint32_t control = actions;
    const int action = action_for(control);
    if (action < 0) {
        if (control == kPause && down) {
            hw_handle_key(SDLK_ESCAPE, 1);
        }
        return;
    }
    const int navigation = navigation_for(control);
    if (down && navigation >= 0 && hw_overlay_navigate(navigation, 1)) {
        return;
    }
    /* A release always clears the held action, even while an overlay is up: a
     * press that began before the menu opened would otherwise stay latched and
     * move the player the moment the menu closes. */
    pc_input_touch_action(action, down ? 1 : 0);
    hw_touch_controls_changed();
}

touch_ui::Controls &ensure_controls() {
    if (!g_controls) {
        g_controls = std::make_unique<touch_ui::Controls>(controls_config(), apply_action);
    }
    return *g_controls;
}

/* Window size in physical pixels, the safe area scaled into the same space, and
 * the display scale the controls size themselves from. */
touch_ui::Geometry window_geometry(SDL_Window *window) {
    touch_ui::Geometry geometry;
    int pixel_width = 0;
    int pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height)) {
        return geometry;
    }
    geometry.output_width = pixel_width;
    geometry.output_height = pixel_height;
    geometry.display_scale = SDL_GetWindowDisplayScale(window);
    SDL_Rect safe{0, 0, pixel_width, pixel_height};
    SDL_Rect window_safe{};
    int window_width = 0;
    int window_height = 0;
    if (SDL_GetWindowSafeArea(window, &window_safe) &&
        SDL_GetWindowSize(window, &window_width, &window_height) && window_width > 0 &&
        window_height > 0) {
        const float scale_x = static_cast<float>(pixel_width) / static_cast<float>(window_width);
        const float scale_y = static_cast<float>(pixel_height) / static_cast<float>(window_height);
        safe.x = static_cast<int>(static_cast<float>(window_safe.x) * scale_x);
        safe.y = static_cast<int>(static_cast<float>(window_safe.y) * scale_y);
        safe.w = static_cast<int>(static_cast<float>(window_safe.w) * scale_x);
        safe.h = static_cast<int>(static_cast<float>(window_safe.h) * scale_y);
    }
    geometry.safe =
        touch_ui::Rect{static_cast<float>(safe.x), static_cast<float>(safe.y),
                       static_cast<float>(safe.x + safe.w), static_cast<float>(safe.y + safe.h)};
    return geometry;
}

} // namespace

extern "C" int touch_controls_handle_sdl_event(const SDL_Event *event) {
    if (!event) {
        return 0;
    }
    note_controller_use(*event);
    if (event->type != SDL_EVENT_FINGER_DOWN && event->type != SDL_EVENT_FINGER_MOTION &&
        event->type != SDL_EVENT_FINGER_UP && event->type != SDL_EVENT_FINGER_CANCELED) {
        return 0;
    }
    /* Normalized finger coordinates and what they consumed: the one measurement
     * that says whether a touch landed on a control or on the game. */
    const char *phase = event->type == SDL_EVENT_FINGER_DOWN     ? "down"
                        : event->type == SDL_EVENT_FINGER_MOTION ? "move"
                        : event->type == SDL_EVENT_FINGER_UP     ? "up"
                                                                 : "cancel";
    const int consumed = ensure_controls().handle_event(*event) ? 1 : 0;
    /* A press and a release are the events a device run has to account for; a
     * drag is a stream, so it stays behind the debug level. */
    const BenefactorLogLevel level =
        event->type == SDL_EVENT_FINGER_MOTION ? BENEFACTOR_LOG_DEBUG : BENEFACTOR_LOG_INFO;
    benefactor_log_write(level, "input", "finger %s id=%" SDL_PRIu64 " at (%.3f, %.3f) handled=%d",
                         phase, static_cast<std::uint64_t>(event->tfinger.fingerID),
                         static_cast<double>(event->tfinger.x),
                         static_cast<double>(event->tfinger.y), consumed);
    return consumed;
}

extern "C" void touch_controls_present(SDL_Renderer *renderer, SDL_Window *window) {
    if (!renderer || !window) {
        return;
    }
    touch_ui::Controls &controls = ensure_controls();
    /* Which controls the current screen has a use for.
     *
     * The interact action exists only under modern touch controls; the classic
     * scheme reaches those objects with fire, exactly as the pad does.
     *
     * While the pause menu is open the menu consumes the D-pad, the action
     * control, and the pause control, and nothing else. The gameplay-only
     * controls are withdrawn rather than left covering the panel: on a narrow
     * screen the turbo and camera discs sit over its rows, and a menu whose
     * values cannot be read is worse than a missing button. */
    std::uint32_t unavailable = pc_modern_touch() ? 0U : kInteract;
    if (pc_pause_active()) {
        unavailable |= kTurbo | kCamera | kInteract;
    }
    controls.set_unavailable_actions(unavailable);
    const touch_ui::Geometry geometry = window_geometry(window);
    if (geometry.output_width <= 0 || geometry.output_height <= 0) {
        return;
    }
    controls.set_geometry(geometry);
    controls.present(renderer);
    if (geometry.output_width != g_reported_width || geometry.output_height != g_reported_height) {
        g_reported_width = geometry.output_width;
        g_reported_height = geometry.output_height;
        benefactor_log_write(BENEFACTOR_LOG_INFO, "input",
                             "touch controls at %dx%d (scale %.2f pt, %zu controls, unit %.0f)",
                             geometry.output_width, geometry.output_height, geometry.display_scale,
                             controls.layout().visuals.size(),
                             static_cast<double>(controls.layout().unit));
        /* Where each control actually is, so a device run can be told apart
         * from the layout it claims. */
        for (const touch_ui::Visual &visual : controls.layout().visuals) {
            benefactor_log_write(
                BENEFACTOR_LOG_INFO, "input", "  control %u (%s) x %.0f..%.0f y %.0f..%.0f",
                visual.id, visual.icon.c_str(), static_cast<double>(visual.bounds.left),
                static_cast<double>(visual.bounds.right), static_cast<double>(visual.bounds.top),
                static_cast<double>(visual.bounds.bottom));
        }
    }
}
