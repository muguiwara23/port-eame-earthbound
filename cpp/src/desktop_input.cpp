#include "desktop_input.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace eb {
DesktopInput::DesktopInput() {
    // Port/eame: SDL3 reports gamepad buttons by position (south, east, ...)
    // and its database already has the hid-nintendo SNES mappings SDL2
    // lacked, so neither the label hint nor added mappings are needed.
    int count = 0;
    if (SDL_JoystickID *pads = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i)
            open_controller(pads[i]);
        SDL_free(pads);
    }
}
DesktopInput::~DesktopInput() {
    if (controller_)
        SDL_CloseGamepad(controller_);
}
void DesktopInput::process_device_event(const SDL_Event &event) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED)
        open_controller(event.gdevice.which);
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED && controller_ &&
        SDL_GetGamepadID(controller_) == event.gdevice.which) {
        SDL_CloseGamepad(controller_);
        controller_ = nullptr;
        // Select the next available pad, including one already connected.
        int count = 0;
        if (SDL_JoystickID *pads = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i)
                open_controller(pads[i]);
            SDL_free(pads);
        }
    }
}
void DesktopInput::configure(const ControllerSettings &settings) {
    settings_ = settings;
    settings_.stick_deadzone = std::clamp(settings_.stick_deadzone, 0, 32766);
    for (auto &binding : settings_.bindings)
        if (binding < -1 || binding >= SDL_GAMEPAD_BUTTON_COUNT)
            binding = -1;
}
ControllerSnapshot DesktopInput::controller_snapshot() const { return sample_controller(true); }
ControllerSnapshot DesktopInput::sample_controller(bool include_identity) const {
    ControllerSnapshot snapshot;
    if (!controller_ || !SDL_GamepadConnected(controller_))
        return snapshot;
    snapshot.connected = true;
    if (include_identity) {
        if (const char *name = SDL_GetGamepadName(controller_))
            snapshot.name = name;
        snapshot.nintendo_layout = SDL_GetGamepadVendor(controller_) == 0x057e;
    }
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button)
        snapshot.pressed[button] =
            SDL_GetGamepadButton(controller_, static_cast<SDL_GamepadButton>(button));
    for (unsigned index = 0; index < settings_.bindings.size(); ++index) {
        const int source = settings_.bindings[index];
        if (source >= 0 && source < SDL_GAMEPAD_BUTTON_COUNT && snapshot.pressed[source])
            snapshot.game_buttons |= std::uint16_t(1u << (15 - index));
    }
    snapshot.stick_x = SDL_GetGamepadAxis(controller_, SDL_GAMEPAD_AXIS_LEFTX);
    snapshot.stick_y = SDL_GetGamepadAxis(controller_, SDL_GAMEPAD_AXIS_LEFTY);
    if (snapshot.stick_x < -settings_.stick_deadzone)
        snapshot.game_buttons |= 1u << 9;
    if (snapshot.stick_x > settings_.stick_deadzone)
        snapshot.game_buttons |= 1u << 8;
    if (snapshot.stick_y < -settings_.stick_deadzone)
        snapshot.game_buttons |= 1u << 11;
    if (snapshot.stick_y > settings_.stick_deadzone)
        snapshot.game_buttons |= 1u << 10;
    return snapshot;
}
std::uint16_t DesktopInput::buttons() const {
    std::uint16_t buttons = 0;
    const auto *keys = SDL_GetKeyboardState(nullptr);
    // Bit positions match the SNES JOY1 register layout, not SDL key values.
    const std::array<std::pair<SDL_Scancode, unsigned>, 12> key_map{{{SDL_SCANCODE_Z, 15},
                                                                     {SDL_SCANCODE_A, 14},
                                                                     {SDL_SCANCODE_RSHIFT, 13},
                                                                     {SDL_SCANCODE_RETURN, 12},
                                                                     {SDL_SCANCODE_UP, 11},
                                                                     {SDL_SCANCODE_DOWN, 10},
                                                                     {SDL_SCANCODE_LEFT, 9},
                                                                     {SDL_SCANCODE_RIGHT, 8},
                                                                     {SDL_SCANCODE_X, 7},
                                                                     {SDL_SCANCODE_S, 6},
                                                                     {SDL_SCANCODE_Q, 5},
                                                                     {SDL_SCANCODE_W, 4}}};
    for (const auto &[key, bit] : key_map)
        if (keys[key])
            buttons |= std::uint16_t(1u << bit);
    buttons |= sample_controller(false).game_buttons;

    return buttons;
}
void DesktopInput::open_controller(SDL_JoystickID id) {
    if (!controller_ && SDL_IsGamepad(id))
        controller_ = SDL_OpenGamepad(id);
}

} // namespace eb
