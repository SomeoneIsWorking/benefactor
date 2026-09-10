#include "port/control/input_script.h"

#include "common/log.h"

extern "C" {
#include "engine/hw.h"
}

namespace benefactor::control {

InputScript &InputScript::instance() {
    static InputScript only;
    return only;
}

void InputScript::apply(const Buttons &buttons) {
    hw_set_joystick(buttons.up, buttons.down, buttons.left, buttons.right, buttons.fire);
    hw_set_mouse_lmb(buttons.fire);
    hw_set_interact(buttons.interact);
    hw_set_drop(buttons.drop);
    hw_set_hop(buttons.hop);
}

void InputScript::hold(const Buttons &buttons) {
    held_ = buttons;
    press_left_ = 0;
    apply(held_);
}

void InputScript::press(const Buttons &buttons, int frames) {
    if (frames < 1)
        frames = 1;
    press_left_ = frames;
    apply(buttons);
    benefactor_log_write(BENEFACTOR_LOG_INFO, "control", "press for %d frame(s)", frames);
}

void InputScript::step(int frames) {
    if (frames < 1)
        frames = 1;
    step_left_ = frames;
    paused_ = false;
}

void InputScript::pause() {
    paused_ = true;
    step_left_ = -1;
}

void InputScript::resume() {
    paused_ = false;
    step_left_ = -1;
}

bool InputScript::paused() const { return paused_; }

int InputScript::press_frames_left() const { return press_left_; }

void InputScript::frame() {
    /* A press is released by the frame loop, so its length is in frames the
     * game actually saw — not in however long two HTTP round trips took. */
    if (press_left_ > 0 && --press_left_ == 0)
        apply(held_);

    if (step_left_ > 0 && --step_left_ == 0) {
        step_left_ = -1;
        paused_ = true;
        benefactor_log_write(BENEFACTOR_LOG_INFO, "control", "stepped to frame %d; holding",
                             hw_get_frame_num());
    }
}

} // namespace benefactor::control

extern "C" {

void pc_control_frame(void) { benefactor::control::InputScript::instance().frame(); }

int pc_control_paused(void) {
    return benefactor::control::InputScript::instance().paused() ? 1 : 0;
}
}
