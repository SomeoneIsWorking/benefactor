#include "port/touch_controls.h"

#include "port/input.h"

#include <lucent/touch.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

extern "C" void hw_handle_key(int sym, int down);
extern "C" void hw_touch_controls_changed(void);

namespace {

enum ZoneId : std::uint32_t {
  kUp = 1,
  kDown,
  kLeft,
  kRight,
  kFire,
  kInteract,
  kPause,
};

constexpr std::array<lucent::touch::Zone, 7> kZones{{
    {kUp, 0.07F, 0.63F, 0.21F, 0.77F, 0},
    {kDown, 0.07F, 0.83F, 0.21F, 0.97F, 0},
    {kLeft, 0.00F, 0.73F, 0.14F, 0.87F, 0},
    {kRight, 0.14F, 0.73F, 0.28F, 0.87F, 0},
    {kFire, 0.80F, 0.72F, 0.99F, 0.98F, 0},
    {kInteract, 0.63F, 0.67F, 0.79F, 0.86F, 0},
    {kPause, 0.88F, 0.02F, 0.99F, 0.14F, 0},
}};

struct State {
  lucent::touch::Router router;
  std::array<unsigned, PI_NUM> action_contacts{};
  bool controller_connected = false;
  bool initialized = false;
} state;

void initialize() {
  if (!state.initialized) {
    state.router.set_zones(kZones);
    state.initialized = true;
  }
}

int action_for_zone(std::uint32_t zone) {
  switch (zone) {
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
  default:
    return -1;
  }
}

void release_captured_contacts() {
  initialize();
  for (const auto &event : state.router.cancel()) {
    if (const int action = action_for_zone(event.zone_id); action >= 0)
      state.action_contacts[action] = 0;
  }
  state.action_contacts.fill(0);
  pc_input_touch_clear();
  hw_touch_controls_changed();
}

void apply_action_contact(const lucent::touch::Event &event) {
  const int action = action_for_zone(event.zone_id);
  if (action < 0)
    return;
  unsigned &contacts = state.action_contacts[action];
  if (event.phase == lucent::touch::Phase::began) {
    ++contacts;
  } else if ((event.phase == lucent::touch::Phase::ended ||
              event.phase == lucent::touch::Phase::canceled) &&
             contacts > 0) {
    --contacts;
  }
  pc_input_touch_action(action, contacts != 0);
}

void blend(uint32_t *pixel, uint32_t color, unsigned alpha) {
  const uint32_t old = *pixel;
  const unsigned inverse = 255U - alpha;
  const unsigned r =
      (((old >> 16) & 0xFFU) * inverse + ((color >> 16) & 0xFFU) * alpha) /
      255U;
  const unsigned g =
      (((old >> 8) & 0xFFU) * inverse + ((color >> 8) & 0xFFU) * alpha) / 255U;
  const unsigned b = ((old & 0xFFU) * inverse + (color & 0xFFU) * alpha) / 255U;
  *pixel = 0xFF000000U | (r << 16) | (g << 8) | b;
}

void circle(uint32_t *argb, int width, int height, float center_x,
            float center_y, float radius) {
  const int cx = static_cast<int>(center_x * width);
  const int cy = static_cast<int>(center_y * height);
  const int r = std::max(8, static_cast<int>(radius * std::min(width, height)));
  const int min_x = std::max(0, cx - r), max_x = std::min(width - 1, cx + r);
  const int min_y = std::max(0, cy - r), max_y = std::min(height - 1, cy + r);
  for (int y = min_y; y <= max_y; ++y)
    for (int x = min_x; x <= max_x; ++x) {
      const int dx = x - cx, dy = y - cy;
      const int distance2 = dx * dx + dy * dy;
      if (distance2 <= r * r && distance2 >= (r - 2) * (r - 2))
        blend(&argb[y * width + x], 0xFFFFFFFFU, 150U);
    }
}

} // namespace

extern "C" int touch_controls_handle_sdl_event(const SDL_Event *event) {
  if (!event || state.controller_connected)
    return 0;
  if (event->type != SDL_FINGERDOWN && event->type != SDL_FINGERMOTION &&
      event->type != SDL_FINGERUP)
    return 0;

  initialize();
  const auto phase = event->type == SDL_FINGERDOWN ? lucent::touch::Phase::began
                     : event->type == SDL_FINGERMOTION
                         ? lucent::touch::Phase::moved
                         : lucent::touch::Phase::ended;
  const lucent::touch::Contact contact{
      static_cast<std::int64_t>(event->tfinger.fingerId),
      {event->tfinger.x, event->tfinger.y},
      phase};
  const std::array contacts{contact};
  for (const auto &routed : state.router.route(contacts)) {
    if (routed.zone_id == kPause) {
      if (routed.phase == lucent::touch::Phase::began)
        hw_handle_key(SDLK_ESCAPE, 1);
      continue;
    }
    apply_action_contact(routed);
  }
  hw_touch_controls_changed();
  return 1;
}

extern "C" void touch_controls_set_controller_connected(int connected) {
  const bool next = connected != 0;
  if (next == state.controller_connected)
    return;
  state.controller_connected = next;
  if (next)
    release_captured_contacts();
}

extern "C" void touch_controls_draw(uint32_t *argb, int width, int height) {
  if (!argb || width <= 0 || height <= 0 || state.controller_connected)
    return;
  circle(argb, width, height, 0.14F, 0.80F, 0.13F); // D-pad envelope
  circle(argb, width, height, 0.89F, 0.85F, 0.13F); // Fire
  circle(argb, width, height, 0.71F, 0.77F, 0.10F); // Interact
  circle(argb, width, height, 0.94F, 0.08F, 0.06F); // Pause
}
