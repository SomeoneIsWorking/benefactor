/* touch_controls.cpp — Benefactor's on-screen controls.
 *
 * The shared touch-ui subsystem owns the geometry, the hit regions, the contact
 * claiming, and the drawing; this file owns the title's meanings: which controls
 * the player gets, which logical action each asserts, and when the overlay
 * steps aside. It is deliberately small — every line here is policy, not
 * mechanism. */
#include "port/touch_controls.h"

#include "port/input.h"

#include "common/log.h"

#include "touch_ui/touch_ui.h"

#include <SDL3/SDL.h>

#include <memory>

extern "C" void hw_handle_key(int sym, int down);
extern "C" void hw_touch_controls_changed(void);
extern "C" int pc_modern_touch(void);

namespace {

/* Action bits are this title's own vocabulary; touch-ui only routes them. */
constexpr std::uint32_t kUp = 1U << 0;
constexpr std::uint32_t kDown = 1U << 1;
constexpr std::uint32_t kLeft = 1U << 2;
constexpr std::uint32_t kRight = 1U << 3;
constexpr std::uint32_t kFire = 1U << 4;
constexpr std::uint32_t kInteract = 1U << 5;
constexpr std::uint32_t kPause = 1U << 6;

std::unique_ptr<touch_ui::Controls> g_controls;
bool g_geometry_reported = false;

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
    };
    return config;
}

void apply_action(std::uint32_t action, bool down) {
    switch (action) {
    case kUp:
        pc_input_touch_action(PI_UP, down);
        break;
    case kDown:
        pc_input_touch_action(PI_DOWN, down);
        break;
    case kLeft:
        pc_input_touch_action(PI_LEFT, down);
        break;
    case kRight:
        pc_input_touch_action(PI_RIGHT, down);
        break;
    case kFire:
        pc_input_touch_action(PI_FIRE, down);
        break;
    case kInteract:
        pc_input_touch_action(PI_INTERACT, down);
        break;
    case kPause:
        /* Pause is the pause menu's own key edge, not a held logical action. */
        if (down) {
            hw_handle_key(SDLK_ESCAPE, 1);
        }
        return;
    default:
        return;
    }
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
    geometry.safe = touch_ui::Rect{static_cast<float>(safe.x), static_cast<float>(safe.y),
                                   static_cast<float>(safe.x + safe.w),
                                   static_cast<float>(safe.y + safe.h)};
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
    return ensure_controls().handle_event(*event) ? 1 : 0;
}

extern "C" void touch_controls_present(SDL_Renderer *renderer, SDL_Window *window) {
    if (!renderer || !window) {
        return;
    }
    touch_ui::Controls &controls = ensure_controls();
    // The interact action exists only under modern touch controls; the classic
    // scheme reaches those objects with fire, exactly as the pad does.
    controls.set_unavailable_actions(pc_modern_touch() ? 0U : kInteract);
    const touch_ui::Geometry geometry = window_geometry(window);
    if (geometry.output_width <= 0 || geometry.output_height <= 0) {
        return;
    }
    controls.set_geometry(geometry);
    controls.present(renderer);
    if (!g_geometry_reported) {
        g_geometry_reported = true;
        benefactor_log_write(BENEFACTOR_LOG_INFO, "input",
                             "touch controls at %dx%d (scale %.2f pt, %zu controls)",
                             geometry.output_width, geometry.output_height,
                             geometry.display_scale, controls.layout().visuals.size());
    }
}
