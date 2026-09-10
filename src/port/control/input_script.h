/* src/port/control/input_script.h — held, timed and stepped input.
 *
 * The debug channel could only HOLD a button: set it, and set it back later.
 * At fifty frames a second that makes a press an unrepeatable race against two
 * round trips, so "press fire for three frames" was not expressible and every
 * attempt to drive the game through a menu was a guess.
 *
 * This owns the three things a player (or a test) actually needs:
 *
 *   hold    a button stays set until changed        — what /input always did
 *   press   a button is set for exactly N frames    — released by the frame
 *                                                     loop, not by the caller
 *   step    the game runs N frames and then holds   — inspect, then continue
 *
 * The frame loop calls `benefactor::control::InputScript::instance().frame()`
 * once per presented frame; nothing here blocks a request. */
#ifndef BENEFACTOR_PORT_CONTROL_INPUT_SCRIPT_H
#define BENEFACTOR_PORT_CONTROL_INPUT_SCRIPT_H

#ifdef __cplusplus
#include <cstdint>

namespace benefactor::control {

/* The buttons the game reads, as one value so a press and a hold describe the
 * same thing. */
struct Buttons {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool fire = false;
    bool interact = false;
    bool drop = false;
    bool hop = false;

    bool any() const { return up || down || left || right || fire || interact || drop || hop; }
};

class InputScript {
  public:
    static InputScript &instance();

    /* Set the buttons and leave them set. */
    void hold(const Buttons &buttons);

    /* Set the buttons for `frames` presented frames, then release them. A
     * press while one is already running replaces it. */
    void press(const Buttons &buttons, int frames);

    /* Run `frames` frames, then stop presenting new ones until resumed. */
    void step(int frames);
    void pause();
    void resume();
    bool paused() const;

    /* Called once per presented frame by the display path. */
    void frame();

    /* How many frames the running press has left; 0 when none is running. */
    int press_frames_left() const;

  private:
    InputScript() = default;

    void apply(const Buttons &buttons);

    Buttons held_{};
    int press_left_ = 0;
    int step_left_ = -1; /* -1 = not stepping */
    bool paused_ = false;
};

} // namespace benefactor::control

extern "C" {
#endif

/* The C display path's view: called once per presented frame, and asked
 * whether the game is currently held still. */
void pc_control_frame(void);
int pc_control_paused(void);

#ifdef __cplusplus
}
#endif

#endif /* BENEFACTOR_PORT_CONTROL_INPUT_SCRIPT_H */
