#include "port/touch_controls.h"

#include "port/input.h"

#include <lucent/touch.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

extern "C" void hw_handle_key(int sym, int down);
extern "C" void hw_touch_controls_changed(void);
extern "C" int pc_modern_touch(void);

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
  if (action == PI_INTERACT && !pc_modern_touch()) {
    state.action_contacts[action] = 0;
    pc_input_touch_action(action, 0);
    return;
  }
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

void disc(uint32_t *argb, int width, int height, float center_x, float center_y,
          float radius, uint32_t color, unsigned alpha) {
  const int cx = static_cast<int>(center_x * width);
  const int cy = static_cast<int>(center_y * height);
  const int r = std::max(8, static_cast<int>(radius * std::min(width, height)));
  const int min_x = std::max(0, cx - r), max_x = std::min(width - 1, cx + r);
  const int min_y = std::max(0, cy - r), max_y = std::min(height - 1, cy + r);
  for (int y = min_y; y <= max_y; ++y)
    for (int x = min_x; x <= max_x; ++x) {
      const int dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= r * r)
        blend(&argb[y * width + x], color, alpha);
    }
}

void line(uint32_t *argb, int width, int height, int x0, int y0, int x1, int y1,
          int thickness) {
  const int span_x = std::abs(x1 - x0), step_x = x0 < x1 ? 1 : -1;
  const int span_y = -std::abs(y1 - y0), step_y = y0 < y1 ? 1 : -1;
  int error = span_x + span_y;
  for (;;) {
    for (int offset_y = -thickness; offset_y <= thickness; ++offset_y)
      for (int offset_x = -thickness; offset_x <= thickness; ++offset_x) {
        const int x = x0 + offset_x, y = y0 + offset_y;
        if (x >= 0 && x < width && y >= 0 && y < height)
          blend(&argb[y * width + x], 0xFFFFFFFFU, 190U);
      }
    if (x0 == x1 && y0 == y1)
      break;
    const int doubled = 2 * error;
    if (doubled >= span_y) {
      error += span_y;
      x0 += step_x;
    }
    if (doubled <= span_x) {
      error += span_x;
      y0 += step_y;
    }
  }
}

void triangle(uint32_t *argb, int width, int height, int x0, int y0, int x1,
              int y1, int x2, int y2, uint32_t color, unsigned alpha) {
  const int min_x = std::max(0, std::min({x0, x1, x2}));
  const int max_x = std::min(width - 1, std::max({x0, x1, x2}));
  const int min_y = std::max(0, std::min({y0, y1, y2}));
  const int max_y = std::min(height - 1, std::max({y0, y1, y2}));
  const auto edge = [](int ax, int ay, int bx, int by, int px, int py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
  };
  for (int y = min_y; y <= max_y; ++y)
    for (int x = min_x; x <= max_x; ++x) {
      const int a = edge(x0, y0, x1, y1, x, y);
      const int b = edge(x1, y1, x2, y2, x, y);
      const int c = edge(x2, y2, x0, y0, x, y);
      if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
        blend(&argb[y * width + x], color, alpha);
    }
}

enum class Direction { up, down, left, right };

void direction_button(uint32_t *argb, int width, int height, float center_x,
                      float center_y, Direction direction) {
  constexpr float radius = 0.055F;
  const int cx = static_cast<int>(center_x * width);
  const int cy = static_cast<int>(center_y * height);
  const int span =
      std::max(11, static_cast<int>(radius * std::min(width, height) * 0.55F));
  disc(argb, width, height, center_x, center_y, radius, 0xFF101820U, 145U);
  circle(argb, width, height, center_x, center_y, radius);
  switch (direction) {
  case Direction::up:
    triangle(argb, width, height, cx, cy - span, cx - span, cy + span / 2,
             cx + span, cy + span / 2, 0xFFFFFFFFU, 230U);
    break;
  case Direction::down:
    triangle(argb, width, height, cx, cy + span, cx - span, cy - span / 2,
             cx + span, cy - span / 2, 0xFFFFFFFFU, 230U);
    break;
  case Direction::left:
    triangle(argb, width, height, cx - span, cy, cx + span / 2, cy - span,
             cx + span / 2, cy + span, 0xFFFFFFFFU, 230U);
    break;
  case Direction::right:
    triangle(argb, width, height, cx + span, cy, cx - span / 2, cy - span,
             cx - span / 2, cy + span, 0xFFFFFFFFU, 230U);
    break;
  }
}

void control_glyphs(uint32_t *argb, int width, int height) {
  const int unit = std::max(2, std::min(width, height) / 160);
  const int fire_x = static_cast<int>(0.89F * width);
  const int fire_y = static_cast<int>(0.85F * height);
  const int fire_span = std::max(9, std::min(width, height) / 22);
  line(argb, width, height, fire_x - fire_span, fire_y + fire_span,
       fire_x + fire_span, fire_y, unit);
  line(argb, width, height, fire_x + fire_span, fire_y, fire_x - fire_span,
       fire_y - fire_span, unit);
  line(argb, width, height, fire_x - fire_span, fire_y - fire_span,
       fire_x - fire_span, fire_y + fire_span, unit);

  if (pc_modern_touch()) {
    const int interact_x = static_cast<int>(0.71F * width);
    const int interact_y = static_cast<int>(0.77F * height);
    const int interact_span = std::max(8, std::min(width, height) / 25);
    line(argb, width, height, interact_x, interact_y - interact_span,
         interact_x + interact_span, interact_y, unit);
    line(argb, width, height, interact_x + interact_span, interact_y,
         interact_x, interact_y + interact_span, unit);
    line(argb, width, height, interact_x, interact_y + interact_span,
         interact_x - interact_span, interact_y, unit);
    line(argb, width, height, interact_x - interact_span, interact_y,
         interact_x, interact_y - interact_span, unit);
  }

  const int pause_x = static_cast<int>(0.94F * width);
  const int pause_y = static_cast<int>(0.08F * height);
  const int pause_span = std::max(6, std::min(width, height) / 35);
  line(argb, width, height, pause_x - pause_span / 2, pause_y - pause_span,
       pause_x - pause_span / 2, pause_y + pause_span, unit);
  line(argb, width, height, pause_x + pause_span / 2, pause_y - pause_span,
       pause_x + pause_span / 2, pause_y + pause_span, unit);
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
  direction_button(argb, width, height, 0.14F, 0.70F, Direction::up);
  direction_button(argb, width, height, 0.14F, 0.90F, Direction::down);
  direction_button(argb, width, height, 0.07F, 0.80F, Direction::left);
  direction_button(argb, width, height, 0.21F, 0.80F, Direction::right);
  circle(argb, width, height, 0.89F, 0.85F, 0.13F); // Fire
  if (pc_modern_touch())
    circle(argb, width, height, 0.71F, 0.77F, 0.10F); // Interact
  circle(argb, width, height, 0.94F, 0.08F, 0.06F);   // Pause
  control_glyphs(argb, width, height);
}
