#include <algorithm>
#include <mutex>
#include <span>
#include <atomic>

#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "recompinput/input_binding.h"
#include "recompinput/players.h"
#include "recompui/config.h"

using namespace recompinput;

template <typename T>
using InputArray = std::array<T, max_num_players_supported>;

static struct {
    const Uint8* keys = nullptr;
    SDL_Keymod keymod = SDL_Keymod::KMOD_NONE;
    int numkeys = 0;
    std::atomic_int32_t mouse_wheel_pos = 0;
    std::mutex controllers_mutex;
    std::vector<SDL_GameController*> detected_controllers{};
    std::unordered_map<SDL_JoystickID, ControllerState> controller_states;

    std::array<float, 2> mouse_delta{};
    std::mutex pending_input_mutex;
    std::array<float, 2> pending_mouse_delta{};

    InputArray<float> cur_rumble{};
    InputArray<bool> rumble_active{};
} InputState;

#ifdef RECOMPINPUT_PADS_IN_ORDER
// RECOMPINPUT_PADS_IN_ORDER, for a game of several players: until players are assigned, each controller plays a port of
// its own, in the order they were connected (poll_inputs), where single-player mode gives every controller to every
// port, so a second pad moved player 1 too and a menu that each player works with a pad of their own (a character
// select) had every cursor move at once. Its buttons, sticks, gyro and rumble are that port's alone; the keyboard plays
// port 1 (profiles.cpp). Assigned players play as before. The caller holds controllers_mutex.
static SDL_GameController* controller_for_port(int controller_num) {
    if (controller_num < 0 || (size_t)controller_num >= InputState.detected_controllers.size()) {
        return nullptr;
    }
    return InputState.detected_controllers[controller_num];
}
#endif

// The controllers SDL's events have opened, not detected_controllers, which only poll_inputs fills: a game asks which
// ports have a controller (osContInit) as it boots, before its first read of them, and the count there was 0, so a
// game that counts its players once at boot offered one whatever was plugged in.
size_t recompinput::get_detected_controller_count() {
    std::lock_guard lock{ InputState.controllers_mutex };
    size_t count = 0;
    for (const auto& [id, state] : InputState.controller_states) {
        (void)id;
        if (state.controller != nullptr) {
            count++;
        }
    }
    return count;
}

void recompinput::poll_inputs() {
    InputState.keys = SDL_GetKeyboardState(&InputState.numkeys);
    InputState.keymod = SDL_GetModState();
    static bool first_poll = true;

    {
        std::lock_guard lock{ InputState.controllers_mutex };
        InputState.detected_controllers.clear();

        static std::vector<size_t> free_controllers;
        free_controllers.clear();

        for (const auto& [id, state] : InputState.controller_states) {
            (void)id; // Avoid unused variable warning.
            SDL_GameController* controller = state.controller;
            if (controller != nullptr) {
                free_controllers.emplace_back(InputState.detected_controllers.size());
                InputState.detected_controllers.push_back(controller);
            }
        }
#ifdef RECOMPINPUT_PADS_IN_ORDER
        // In the order they were connected: SDL's joystick instance ids rise with each connection.
        std::sort(InputState.detected_controllers.begin(), InputState.detected_controllers.end(),
            [](SDL_GameController* a, SDL_GameController* b) {
                return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(a)) <
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(b));
            });
#endif
    }

    // Read the deltas while resetting them to zero.
    {
        std::lock_guard lock{ InputState.pending_input_mutex };

        for (auto &state_pair : InputState.controller_states) {
            ControllerState &state = state_pair.second;
            state.rotation_delta = state.pending_rotation_delta;
            state.pending_rotation_delta = { 0.0f, 0.0f };
        }

        InputState.mouse_delta = InputState.pending_mouse_delta;
        InputState.pending_mouse_delta = { 0.0f, 0.0f };
    }
}

#ifdef __EMSCRIPTEN__
#include <chrono>
#include <cstring>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

// Emscripten's SDL2 has no rumble (SDL_JoystickRumble is unsupported there), so on a page it goes to the browser's
// Gamepad API, on the main thread where that is: vibrationActuator.playEffect("dual-rumble") on the gamepad behind
// the joystick, its weak (high frequency) motor as SDL_JoystickRumble's is driven.
EM_JS(void, recompinput_web_rumble, (int index, int strength), {
    const pad = navigator.getGamepads ? navigator.getGamepads()[index] : null;
    const actuator = pad && pad.vibrationActuator;
    if (actuator && strength > 0) {
        actuator.playEffect('dual-rumble', { duration: 5000, strongMagnitude: 0, weakMagnitude: strength / 65535 })
            .catch(() => {});
    } else if (actuator) {
        actuator.reset().catch(() => {});
    }
});

// SDL's own, which names a joystick on Emscripten: SDL_CreateJoystickName(0, 0, NULL, id) of the browser gamepad's id.
extern "C" char *SDL_CreateJoystickName(Uint16 vendor, Uint16 product, const char *vendor_name, const char *product_name);

// The browser's index of the gamepad behind a joystick, or -1. SDL keeps it to itself, and its name for the joystick
// isn't the gamepad's id: the id comes through Emscripten's EmscriptenGamepadEvent cut to 63 bytes, which Chrome's ids
// for other than XInput pads run past ("DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)"),
// and SDL renames it (a maker's name shortened, "HORI CO.,LTD" to "HORI", or cut off the front when it shows again
// later). So each gamepad's id as Emscripten gives it is named the way SDL named the joystick, and the gamepad is the
// k-th of that name in the browser's order, for the k-th joystick of the name in the order SDL saw them connect
// (instance id).
static int web_gamepad_index(SDL_Joystick* joystick) {
    const char *name = SDL_JoystickName(joystick);
    SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    if (name == nullptr) {
        return -1;
    }
    if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) {
        return -1;
    }
    int rank = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        const char *other = SDL_JoystickNameForIndex(i);
        rank += SDL_JoystickGetDeviceInstanceID(i) < id && other != nullptr && strcmp(other, name) == 0;
    }
    EmscriptenGamepadEvent pad;
    for (int i = 0; i < emscripten_get_num_gamepads(); i++) {
        if (emscripten_get_gamepad_status(i, &pad) != EMSCRIPTEN_RESULT_SUCCESS) {
            continue;
        }
        char *pad_name = SDL_CreateJoystickName(0, 0, nullptr, pad.id);
        bool same = pad_name != nullptr && strcmp(pad_name, name) == 0;
        SDL_free(pad_name);
        if (same && rank-- == 0) {
            return i;
        }
    }
    return -1;
}

// Called every retrace for every pad. Only a change of strength goes to the page, and, since a browser ends an effect
// after 5 seconds at most, a rumble still on once a second. The gamepad is found as the joystick is first rumbled (a
// reset, as it is opened) and kept: a browser's index for a gamepad stays while it is connected.
static int web_rumble(SDL_Joystick* joystick, uint16_t strength) {
    struct Played {
        int index = -1;
        uint16_t strength = 0;
        std::chrono::steady_clock::time_point at;
    };
    static std::unordered_map<SDL_JoystickID, Played> played;
    SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    auto now = std::chrono::steady_clock::now();
    auto [it, added] = played.try_emplace(id);
    Played &p = it->second;
    if (!added && strength == p.strength && (strength == 0 || now - p.at < std::chrono::seconds(1))) {
        return 0;
    }
    p.strength = strength;
    p.at = now;
    if (p.index < 0) {
        p.index = web_gamepad_index(joystick);
    }
    if (p.index >= 0) {
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VII, recompinput_web_rumble, p.index, int(strength));
    }
    return 0;
}
#endif

void recompinput::set_rumble(int controller_num, bool on) {
    InputState.rumble_active[controller_num] = on;
}

static float smoothstep(float from, float to, float amount) {
    amount = (amount * amount) * (3.0f - 2.0f * amount);
    return std::lerp(from, to, amount);
}

// Update rumble to attempt to mimic the way n64 rumble ramps up and falls off
void recompinput::update_rumble() {
    auto do_rumble = [](SDL_GameController* controller, uint16_t rumble_strength, uint32_t duration) {
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
        SDL_JoystickID joystick_id = SDL_JoystickInstanceID(joystick);
        ControllerState &state = InputState.controller_states[joystick_id];

        // Skip setting rumble for controllers that failed to rumble previously.
        if (state.rumble_failed) {
           return;
        }
#ifdef __EMSCRIPTEN__
        int err = web_rumble(joystick, rumble_strength);
#else
        int err = SDL_JoystickRumble(joystick, 0, rumble_strength, duration);
#endif
        if (err != 0) {
            state.rumble_failed = true;
        }
    };

    // Skip rumble processing if the game doesn't have a rumble strength option. 
    if (!recompui::config::general::has_rumble_strength_option()) {
        return;
    }
    size_t rumbles_to_run = recompinput::players::is_single_player_mode() ? 1 : recompinput::players::get_number_of_assigned_players();
#ifdef RECOMPINPUT_PADS_IN_ORDER
    if (recompinput::players::is_single_player_mode()) {
        rumbles_to_run = recompinput::get_detected_controller_count();
    }
#endif
    if (rumbles_to_run > InputState.cur_rumble.size()) {
        rumbles_to_run = InputState.cur_rumble.size();
    }
    for (size_t i = 0; i < rumbles_to_run; i++) {
        // Note: values are not accurate! just approximations based on feel
        if (InputState.rumble_active[i]) {
            InputState.cur_rumble[i] += 0.17f;
            if (InputState.cur_rumble[i] > 1) InputState.cur_rumble[i] = 1;
        }
        else {
            InputState.cur_rumble[i] *= 0.92f;
            InputState.cur_rumble[i] -= 0.01f;
            if (InputState.cur_rumble[i] < 0) InputState.cur_rumble[i] = 0;
        }
        float smooth_rumble = smoothstep(0, 1, InputState.cur_rumble[i]);

        uint16_t rumble_strength = smooth_rumble * (recompui::config::general::get_rumble_strength() * 0xFFFF / 100);
        uint32_t duration = 1000000; // Dummy duration value that lasts long enough to matter as the game will reset rumble on its own.
        {
            std::lock_guard lock{ InputState.controllers_mutex };
            if (recompinput::players::is_single_player_mode()) {
#ifdef RECOMPINPUT_PADS_IN_ORDER
                if (SDL_GameController* controller = controller_for_port((int)i)) {
                    do_rumble(controller, rumble_strength, duration);
                }
#else
                for (const auto &controller : InputState.detected_controllers) {
                    do_rumble(controller, rumble_strength, duration);
                }
#endif
            }
            else {
                auto &player = recompinput::players::get_player(i);
                if (player.controller != nullptr) {
                    do_rumble(player.controller, rumble_strength, duration);
                }
            }
        }
    }
}

bool controller_button_state(int controller_num, int32_t input_id) {
    if (input_id >= 0 && input_id < SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_MAX) {
        SDL_GameControllerButton button = (SDL_GameControllerButton)input_id;
        bool ret = false;
        {
            std::lock_guard lock{ InputState.controllers_mutex };
            if (recompinput::players::is_single_player_mode()) {
#ifdef RECOMPINPUT_PADS_IN_ORDER
                if (SDL_GameController* controller = controller_for_port(controller_num)) {
                    ret |= SDL_GameControllerGetButton(controller, button);
                }
#else
                for (const auto &controller : InputState.detected_controllers) {
                    ret |= SDL_GameControllerGetButton(controller, button);
                }
#endif
            }
            else {
                auto &player = recompinput::players::get_player(controller_num);
                if (player.controller != nullptr) {
                    ret |= SDL_GameControllerGetButton(player.controller, button);
                }
            }
        }

        return ret;
    }
    return false;
}

static std::atomic_bool right_analog_suppressed = false;

float controller_axis_state(int controller_num, int32_t input_id, bool allow_suppression) {
    if (abs(input_id) - 1 < SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_MAX) {
        SDL_GameControllerAxis axis = (SDL_GameControllerAxis)(abs(input_id) - 1);
        bool negative_range = input_id < 0;
        float ret = 0.0f;

        {
            auto gather_axis_state = [&](SDL_GameController* controller) {
                float cur_val = SDL_GameControllerGetAxis(controller, axis) * (1 / 32768.0f);
                if (negative_range) {
                    cur_val = -cur_val;
                }

                // Check if this input is a right analog axis and suppress it accordingly.
                if (allow_suppression && right_analog_suppressed.load() &&
                    (axis == SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY)) {
                    cur_val = 0;
                }

                return std::clamp(cur_val, 0.0f, 1.0f);
            };

            std::lock_guard lock{ InputState.controllers_mutex };
            if (recompinput::players::is_single_player_mode()) {
#ifdef RECOMPINPUT_PADS_IN_ORDER
                if (SDL_GameController* controller = controller_for_port(controller_num)) {
                    ret = gather_axis_state(controller);
                }
#else
                for (SDL_GameController *controller : InputState.detected_controllers) {
                    float controller_state = gather_axis_state(controller);
                    if (fabsf(controller_state) > fabsf(ret)) {
                        ret = controller_state;
                    }
                }
#endif
            }
            else {
                auto &player = recompinput::players::get_player(controller_num);
                if (player.controller != nullptr) {
                    ret = gather_axis_state(player.controller);
                }
            }
        }

        return std::clamp(ret, 0.0f, 1.0f);
    }
    return false;
}

bool recompinput::should_override_keystate(SDL_Scancode key, SDL_Keymod mod) {
    // Override Enter when Alt is held.
    if (key == SDL_Scancode::SDL_SCANCODE_RETURN) {
        if (mod & SDL_Keymod::KMOD_ALT) {
            return true;
        }
    }

    return false;
}

float recompinput::get_input_analog(int controller_num, const InputField& field) {
    switch (field.input_type) {
    case InputType::Keyboard:
        if (InputState.keys && field.input_id >= 0 && field.input_id < InputState.numkeys) {
            if (should_override_keystate(static_cast<SDL_Scancode>(field.input_id), InputState.keymod)) {
                return 0.0f;
            }
            return InputState.keys[field.input_id] ? 1.0f : 0.0f;
        }
        return 0.0f;
    case InputType::ControllerDigital:
        return controller_button_state(controller_num, field.input_id) ? 1.0f : 0.0f;
    case InputType::ControllerAnalog:
        return controller_axis_state(controller_num, field.input_id, true);
    case InputType::Mouse:
        // TODO mouse support
        return 0.0f;
    case InputType::None:
        return false;
    }
}

float recompinput::get_input_analog(int controller_num, const std::span<const InputField> fields) {
    float ret = 0.0f;
    for (const auto& field : fields) {
        ret += get_input_analog(controller_num, field);
    }
    return std::clamp(ret, 0.0f, 1.0f);
}

bool recompinput::get_input_digital(int controller_num, const InputField& field) {
    switch (field.input_type) {
    case InputType::Keyboard:
        if (InputState.keys && field.input_id >= 0 && field.input_id < InputState.numkeys) {
            if (should_override_keystate(static_cast<SDL_Scancode>(field.input_id), InputState.keymod)) {
                return false;
            }
            return InputState.keys[field.input_id] != 0;
        }
        return false;
    case InputType::ControllerDigital:
        return controller_button_state(controller_num, field.input_id);
    case InputType::ControllerAnalog:
        // TODO adjustable threshold
        return controller_axis_state(controller_num, field.input_id, true) >= recompinput::axis_digital_threshold;
    case InputType::Mouse:
        // TODO mouse support
        return false;
    case InputType::None:
        return false;
    }
}

bool recompinput::get_input_digital(int controller_num, const std::span<const InputField> fields) {
    bool ret = 0;
    for (const auto& field : fields) {
        ret |= get_input_digital(controller_num, field);
    }
    return ret;
}

void recompinput::get_gyro_deltas(int controller_num, float* x, float* y) {
    float sensitivity = (float)recompui::config::general::get_gyro_sensitivity() / 100.0f;
    std::array<float, 2> cur_rotation_delta = { 0.0f, 0.0f };

    if (players::is_single_player_mode()) {
        {
            std::lock_guard lock{ InputState.controllers_mutex };
#ifdef RECOMPINPUT_PADS_IN_ORDER
            if (SDL_GameController* controller = controller_for_port(controller_num)) {
                cur_rotation_delta = InputState.controller_states[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))].rotation_delta;
            }
#else
            for (const auto &state_pair : InputState.controller_states) {
                const ControllerState &state = state_pair.second;
                cur_rotation_delta[0] += state.rotation_delta[0];
                cur_rotation_delta[1] += state.rotation_delta[1];
            }
#endif
        }

    } else {
        auto &player = players::get_player(controller_num);
        if (player.controller == nullptr) {
            *x = 0.0f;
            *y = 0.0f;
            return;
        }
        SDL_JoystickID joystick_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(player.controller));
        ControllerState &state = get_controller_state(joystick_id);
        cur_rotation_delta = state.rotation_delta;
    }

    *x = cur_rotation_delta[0] * sensitivity;
    *y = cur_rotation_delta[1] * sensitivity;
}

void recompinput::get_mouse_deltas(float* x, float* y) {
    std::array<float, 2> cur_mouse_delta = InputState.mouse_delta;
    float sensitivity = (float)recompui::config::general::get_mouse_sensitivity() / 100.0f;
    *x = cur_mouse_delta[0] * sensitivity;
    *y = cur_mouse_delta[1] * sensitivity;
}

void recompinput::apply_joystick_deadzone(float x_in, float y_in, float* x_out, float* y_out) {
    float joystick_deadzone = (float)recompui::config::general::get_joystick_deadzone() / 100.0f;

    if (fabsf(x_in) < joystick_deadzone) {
        x_in = 0.0f;
    }
    else {
        if (x_in > 0.0f) {
            x_in -= joystick_deadzone;
        }
        else {
            x_in += joystick_deadzone;
        }

        x_in /= (1.0f - joystick_deadzone);
    }

    if (fabsf(y_in) < joystick_deadzone) {
        y_in = 0.0f;
    }
    else {
        if (y_in > 0.0f) {
            y_in -= joystick_deadzone;
        }
        else {
            y_in += joystick_deadzone;
        }

        y_in /= (1.0f - joystick_deadzone);
    }

    *x_out = x_in;
    *y_out = y_in;
}

void recompinput::get_right_analog(int controller_num, float* x, float* y) {
    float x_val =
        controller_axis_state(controller_num, (SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX + 1), false) -
        controller_axis_state(controller_num, -(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX + 1), false);
    float y_val =
        controller_axis_state(controller_num, (SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY + 1), false) -
        controller_axis_state(controller_num, -(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY + 1), false);
    apply_joystick_deadzone(x_val, y_val, x, y);
}

void recompinput::set_right_analog_suppressed(bool suppressed) {
    right_analog_suppressed.store(suppressed);
}

// Disable input if any menu that blocks input is open.
bool recompinput::game_input_disabled() {
    return recompui::is_context_capturing_input();
}

// Disable all input if an input is being polled or currently binding.
bool recompinput::all_input_disabled() {
    return
        recompinput::playerassignment::is_blocking_input() ||
        recompinput::binding::is_binding();
}

ControllerState &recompinput::get_controller_state(SDL_JoystickID joystick_id) {
    std::lock_guard lock{ InputState.controllers_mutex };
    return InputState.controller_states[joystick_id];
}

SDL_GameController *recompinput::get_controller_from_joystick_id(SDL_JoystickID joystick_id) {
    std::lock_guard lock{ InputState.controllers_mutex };
    auto it = InputState.controller_states.find(joystick_id);
    if (it != InputState.controller_states.end()) {
        return it->second.controller;
    }
    return nullptr;
}

void recompinput::add_controller_state(SDL_JoystickID joystick_id, SDL_GameController* controller) {
    std::lock_guard lock{ InputState.controllers_mutex };
    ControllerState& state = InputState.controller_states[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))];
    state.controller = controller;
}

void recompinput::remove_controller_state(SDL_JoystickID joystick_id) {
    std::lock_guard lock{ InputState.controllers_mutex };
    InputState.controller_states.erase(joystick_id);
}

void recompinput::add_rotation_deltas(SDL_JoystickID joystick_id, float rot_x, float rot_y) {
    std::lock_guard lock{ InputState.pending_input_mutex };
    ControllerState &state = InputState.controller_states[joystick_id];
    state.pending_rotation_delta[0] += rot_x;
    state.pending_rotation_delta[1] += rot_y;
}

void recompinput::add_mouse_deltas(float delta_x, float delta_y) {
    std::lock_guard lock{ InputState.pending_input_mutex };
    InputState.pending_mouse_delta[0] += delta_x;
    InputState.pending_mouse_delta[1] += delta_y;
}
void recompinput::add_mouse_wheel_delta(int32_t delta) {
    InputState.mouse_wheel_pos.fetch_add(delta);
}
