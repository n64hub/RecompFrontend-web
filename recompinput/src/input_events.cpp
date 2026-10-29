#include "recompinput/recompinput.h"
#include "recompinput/input_binding.h"
#include "recompinput/input_events.h"
#include "recompinput/profiles.h"
#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"

#ifdef __EMSCRIPTEN__
#include <atomic>
#include <emscripten/html5.h>
#endif

static struct {
    std::list<std::filesystem::path> files_dropped;
} DropState;

namespace recompinput {

void queue_if_enabled(SDL_Event* event) {
    if (!recompinput::all_input_disabled() && !binding::should_skip_events()) {
        recompui::queue_event(*event);
    }
}

// Controllers plugged in while in single player mode will create profiles after switching to multiplayer.
static std::unordered_map<uint64_t, ControllerGUID> deferred_controller_profiles;

static int get_or_create_controller_profile_index(ControllerGUID guid) {
    std::string default_profile_key = profiles::get_string_from_controller_guid(guid);
    int profile_index = profiles::get_input_profile_by_key(default_profile_key);
    if (profile_index < 0) {
        profile_index = profiles::add_input_profile(default_profile_key, "Controller", InputDevice::Controller, false);
        profiles::reset_profile_bindings(profile_index, InputDevice::Controller);
    }
    return profile_index;
}

void purge_deferred_controller_profiles() {
    for (auto &guid_pair : deferred_controller_profiles) {
        int profile_index = get_or_create_controller_profile_index(guid_pair.second);
        profiles::add_controller(guid_pair.second, profile_index);
    }
    deferred_controller_profiles.clear();
}

#ifdef __EMSCRIPTEN__
// On a page the cursor lock below is the browser's pointer lock, which SDL asks for when relative mode goes on and the
// browser grants at a click or key. The browser also ends it by itself: at Esc, which then never reaches the page, and
// when the page loses focus. Emscripten's SDL keeps relative mode on through that, and sends the cursor's position on
// the canvas as motion from the last locked one, so a game looking with the mouse turned at once by however far that
// was, and kept turning as the cursor moved; and Esc opened the menu only when pressed a second time. Here motion
// counts only while the browser holds the lock, and a lock the browser ended while it was wanted opens the menu as Esc
// does natively. Closing the menu turns relative mode on again, and SDL asks for the lock at once if the player's
// click or key there still allows it, else at their next one (browsers don't count Esc as one).
static std::atomic<bool> web_pointer_locked = false;
static SDL_EventFilter web_next_filter = nullptr;
static void* web_next_filter_userdata = nullptr;

// Called on the page's thread at once, before the motion SDL's handlers then send on the thread they run on.
static EM_BOOL web_pointerlock_change(int, const EmscriptenPointerlockChangeEvent* event, void*) {
    web_pointer_locked = event->isActive;
    return EM_FALSE;
}

// Run as SDL pushes an event, before any event watch sees it (a game's own mouse look included).
static int web_mouse_filter(void*, SDL_Event* event) {
    if (event->type == SDL_MOUSEMOTION && SDL_GetRelativeMouseMode() && !web_pointer_locked) {
        return 0;
    }
    return web_next_filter != nullptr ? web_next_filter(web_next_filter_userdata, event) : 1;
}

static void web_watch_pointer_lock() {
    static bool watching = false;
    static bool had_lock = false;
    if (!watching) {
        watching = true;
        emscripten_set_pointerlockchange_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_FALSE,
            web_pointerlock_change, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        // RT64 chains the filter it finds when it installs its own, and this one chains whatever came before it.
        if (!SDL_GetEventFilter(&web_next_filter, &web_next_filter_userdata)) {
            web_next_filter = nullptr;
            web_next_filter_userdata = nullptr;
        }
        SDL_SetEventFilter(web_mouse_filter, nullptr);
    }

    // Relative mode is still on only if the lock ended without this side turning it off (a menu opened another way).
    bool has_lock = web_pointer_locked;
    if (had_lock && !has_lock && SDL_GetRelativeMouseMode() && ultramodern::is_game_started()) {
        SDL_Event escape{};
        escape.type = SDL_KEYDOWN;
        escape.key.state = SDL_PRESSED;
        escape.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        escape.key.keysym.sym = SDLK_ESCAPE;
        recompui::queue_event(escape);
    }
    had_lock = has_lock;
}
#endif

bool sdl_event_filter(void* userdata, SDL_Event* event) {
    switch (event->type) {
    case SDL_EventType::SDL_KEYDOWN:
    {
        SDL_KeyboardEvent* keyevent = &event->key;

        // Skip repeated events when not in the menu
        if (!recompui::is_context_capturing_input() &&
            event->key.repeat) {
            break;
        }

        if ((keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_RETURN && (keyevent->keysym.mod & SDL_Keymod::KMOD_ALT)) ||
            keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_F11
            ) {
            recompui::config::graphics::toggle_fullscreen();
        }
        if (binding::is_binding()) {
            if (keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_ESCAPE) {
                binding::stop_scanning();
            }
            else if (binding::get_scanning_device() == InputDevice::Keyboard) {
                binding::set_scanned_input({ InputType::Keyboard, keyevent->keysym.scancode });
            }
        }
        else {
            if (!should_override_keystate(keyevent->keysym.scancode, static_cast<SDL_Keymod>(keyevent->keysym.mod))) {
                queue_if_enabled(event);
            }
        }
    }
    break;
    case SDL_EventType::SDL_CONTROLLERDEVICEADDED:
    {
        SDL_ControllerDeviceEvent* controller_event = &event->cdevice;
        SDL_GameController* controller = SDL_GameControllerOpen(controller_event->which);
        printf("Controller added: %d\n", controller_event->which);
        if (controller != nullptr) {
            printf("  Instance ID: %d\n", SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)));
            printf("  Path: %s\n", SDL_JoystickPath(SDL_GameControllerGetJoystick(controller)));
            recompinput::add_controller_state(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)), controller);

            if (SDL_GameControllerHasSensor(controller, SDL_SensorType::SDL_SENSOR_GYRO) && SDL_GameControllerHasSensor(controller, SDL_SensorType::SDL_SENSOR_ACCEL)) {
                SDL_GameControllerSetSensorEnabled(controller, SDL_SensorType::SDL_SENSOR_GYRO, SDL_TRUE);
                SDL_GameControllerSetSensorEnabled(controller, SDL_SensorType::SDL_SENSOR_ACCEL, SDL_TRUE);
            }
        }
        
        ControllerGUID guid = profiles::get_guid_from_sdl_controller(controller);
        if (profiles::get_controller_by_guid(guid) < 0) {
            if (players::is_single_player_mode()) {
                deferred_controller_profiles[guid.hash] = guid;
            } else {
                int profile_index = get_or_create_controller_profile_index(guid);
                profiles::add_controller(guid, profile_index);
            }
        }
#ifdef RECOMPINPUT_SINGLE_PLAYER_UNTIL_ASSIGNED
        // A game that allows more players makes a controller's profile at once, as the controls page's player cards list
        // them, though it plays as single-player mode until players are assigned (players.cpp).
        if (players::get_max_number_of_players() > 1) {
            purge_deferred_controller_profiles();
        }
#endif
    }
    break;
    case SDL_EventType::SDL_CONTROLLERDEVICEREMOVED:
    {
        SDL_ControllerDeviceEvent* controller_event = &event->cdevice;
        printf("Controller removed: %d\n", controller_event->which);
        recompinput::remove_controller_state(controller_event->which);
    }
    break;
    case SDL_EventType::SDL_QUIT: {
        if (!ultramodern::is_game_started()) {
            ultramodern::quit();
            return true;
        }

        recompui::open_quit_game_prompt();
        recompui::activate_mouse();
        break;
    }
    case SDL_EventType::SDL_MOUSEWHEEL:
    {
        SDL_MouseWheelEvent* wheel_event = &event->wheel;
        recompinput::add_mouse_wheel_delta(wheel_event->y * (wheel_event->direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1));
    }
    queue_if_enabled(event);
    break;
    case SDL_EventType::SDL_CONTROLLERBUTTONDOWN:
        if (binding::is_binding() && binding::is_controller_being_bound(event->cbutton.which)) {
            // TODO: Needs the controller profile index.
            auto menuToggleBinding0 = profiles::get_input_binding(0, GameInput::TOGGLE_MENU, 0);
            auto menuToggleBinding1 = profiles::get_input_binding(0, GameInput::TOGGLE_MENU, 1);
            // note - magic number: 0 is InputType::None
            if ((menuToggleBinding0.input_type != InputType::None && event->cbutton.button == menuToggleBinding0.input_id) ||
                (menuToggleBinding1.input_type != InputType::None && event->cbutton.button == menuToggleBinding1.input_id)) {
                binding::stop_scanning();
            }
            else if (binding::get_scanning_device() == InputDevice::Controller) {
                SDL_ControllerButtonEvent* button_event = &event->cbutton;
                GameInput scanning_game_input = binding::get_scanning_game_input();
                if ((scanning_game_input == GameInput::TOGGLE_MENU ||
                    scanning_game_input == GameInput::ACCEPT_MENU ||
                    scanning_game_input == GameInput::APPLY_MENU) && (
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_UP ||
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                    break;
                }

                binding::set_scanned_input({ InputType::ControllerDigital, button_event->button });
            }
        }
        else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_CONTROLLERAXISMOTION:
        if (binding::is_controller_being_bound(event->caxis.which)) {
            GameInput scanning_game_input = binding::get_scanning_game_input();
            if (scanning_game_input == GameInput::TOGGLE_MENU ||
                scanning_game_input == GameInput::ACCEPT_MENU ||
                scanning_game_input == GameInput::APPLY_MENU) {
                break;
            }

            SDL_ControllerAxisEvent* axis_event = &event->caxis;
            float axis_value = axis_event->value * (1/32768.0f);
            if (axis_value > recompinput::axis_digital_threshold) {
                SDL_Event set_stick_return_event;
                set_stick_return_event.type = SDL_USEREVENT;
                set_stick_return_event.user.code = axis_event->axis;
                set_stick_return_event.user.data1 = nullptr;
                set_stick_return_event.user.data2 = nullptr;
                recompui::queue_event(set_stick_return_event);

                binding::set_scanned_input({ InputType::ControllerAnalog, axis_event->axis + 1 });
            }
            else if (axis_value < -recompinput::axis_digital_threshold) {
                SDL_Event set_stick_return_event;
                set_stick_return_event.type = SDL_USEREVENT;
                set_stick_return_event.user.code = axis_event->axis;
                set_stick_return_event.user.data1 = nullptr;
                set_stick_return_event.user.data2 = nullptr;
                recompui::queue_event(set_stick_return_event);

                binding::set_scanned_input({ InputType::ControllerAnalog, -axis_event->axis - 1 });
            }
        }
        else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_CONTROLLERSENSORUPDATE:
        if (event->csensor.sensor == SDL_SensorType::SDL_SENSOR_ACCEL) {
            // Convert acceleration to g's.
            float x = event->csensor.data[0] / SDL_STANDARD_GRAVITY;
            float y = event->csensor.data[1] / SDL_STANDARD_GRAVITY;
            float z = event->csensor.data[2] / SDL_STANDARD_GRAVITY;
            ControllerState& state = recompinput::get_controller_state(event->csensor.which);
            state.latest_accelerometer[0] = x;
            state.latest_accelerometer[1] = y;
            state.latest_accelerometer[2] = z;
        }
        else if (event->csensor.sensor == SDL_SensorType::SDL_SENSOR_GYRO) {
            // constexpr float gyro_threshold = 0.05f;
            // Convert rotational velocity to degrees per second.
            constexpr float rad_to_deg = 180.0f / M_PI;
            float x = event->csensor.data[0] * rad_to_deg;
            float y = event->csensor.data[1] * rad_to_deg;
            float z = event->csensor.data[2] * rad_to_deg;
            ControllerState& state = recompinput::get_controller_state(event->csensor.which);
            uint64_t cur_timestamp = event->csensor.timestamp;
            uint32_t delta_ms = cur_timestamp - state.prev_gyro_timestamp;
            state.motion.ProcessMotion(x, y, z, state.latest_accelerometer[0], state.latest_accelerometer[1], state.latest_accelerometer[2], delta_ms * 0.001f);
            state.prev_gyro_timestamp = cur_timestamp;

            float rot_x = 0.0f;
            float rot_y = 0.0f;
            state.motion.GetPlayerSpaceGyro(rot_x, rot_y);
            recompinput::add_rotation_deltas(event->csensor.which, rot_x, rot_y);
        }
        break;
    case SDL_EventType::SDL_MOUSEMOTION:
        if (!recompinput::game_input_disabled()) {
            SDL_MouseMotionEvent* motion_event = &event->motion;
            recompinput::add_mouse_deltas(motion_event->xrel, motion_event->yrel);
        }
        queue_if_enabled(event);
        break;
    case SDL_EventType::SDL_DROPBEGIN:
        DropState.files_dropped.clear();
        break;
    case SDL_EventType::SDL_DROPFILE:
        DropState.files_dropped.emplace_back(std::filesystem::path(std::u8string_view((const char8_t*)(event->drop.file))));
        SDL_free(event->drop.file);
        break;
    case SDL_EventType::SDL_DROPCOMPLETE:
        recompui::drop_files(DropState.files_dropped);
        break;
    case SDL_EventType::SDL_CONTROLLERBUTTONUP:
        // Always queue button up events to avoid missing them during binding.
        recompui::queue_event(*event);
        break;
    default:
        queue_if_enabled(event);
        break;
    }
    playerassignment::process_sdl_event(event);
    return false;
}

void handle_events() {
    SDL_Event cur_event;
    static bool started = false;
    static bool exited = false;
#ifdef __EMSCRIPTEN__
    web_watch_pointer_lock();
#endif
    while (SDL_PollEvent(&cur_event) && !exited) {
        exited = sdl_event_filter(nullptr, &cur_event);

        bool has_mouse_sensitivity = recompui::config::general::has_mouse_sensitivity_option();

        // Lock the cursor if all three conditions are true: mouse aiming is enabled, game input is not disabled, and the game has been started. 
        bool cursor_locked = (has_mouse_sensitivity && recompui::config::general::get_mouse_sensitivity() != 0) && !recompinput::game_input_disabled() && ultramodern::is_game_started();

        // Hide the cursor based on its enable state, but override visibility to false if the cursor is locked.
        bool cursor_visible = recompui::get_cursor_visible();
        if (cursor_locked) {
            cursor_visible = false;
        }

        SDL_ShowCursor(cursor_visible ? SDL_ENABLE : SDL_DISABLE);
        SDL_SetRelativeMouseMode(cursor_locked ? SDL_TRUE : SDL_FALSE);
    }

    binding::stop_skipping_events();

    if (!started && ultramodern::is_game_started()) {
        started = true;
        recompui::process_game_started();
    }
}

}
