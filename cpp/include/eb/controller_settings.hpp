#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <string>

namespace eb {
// Bindings use SDL's positional layout after Nintendo input is normalized.
// Order follows the SNES JOY1 register, from B at bit 15 through R at bit 4.
struct ControllerSettings {
    std::array<int, 12> bindings{SDL_GAMEPAD_BUTTON_SOUTH,
                                 SDL_GAMEPAD_BUTTON_WEST,
                                 SDL_GAMEPAD_BUTTON_BACK,
                                 SDL_GAMEPAD_BUTTON_START,
                                 SDL_GAMEPAD_BUTTON_DPAD_UP,
                                 SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                 SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                 SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                                 SDL_GAMEPAD_BUTTON_EAST,
                                 SDL_GAMEPAD_BUTTON_NORTH,
                                 SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
                                 SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    int stick_deadzone = 12000;
    bool operator==(const ControllerSettings &) const = default;
};

inline constexpr std::array<const char *, 12> snes_button_names{
    "B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R"};

struct ControllerSnapshot {
    bool connected = false;
    bool nintendo_layout = false;
    std::string name;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> pressed{};
    int stick_x{}, stick_y{};
    std::uint16_t game_buttons{};
};

// Labels match physical Nintendo/SNES buttons or SDL's generic Xbox layout.
inline const char *controller_button_name(int button, bool nintendo_layout) {
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT)
        return "Unassigned";
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return nintendo_layout ? "B (bottom)" : "A (bottom)";
    case SDL_GAMEPAD_BUTTON_EAST:
        return nintendo_layout ? "A (right)" : "B (right)";
    case SDL_GAMEPAD_BUTTON_WEST:
        return nintendo_layout ? "Y (left)" : "X (left)";
    case SDL_GAMEPAD_BUTTON_NORTH:
        return nintendo_layout ? "X (top)" : "Y (top)";
    case SDL_GAMEPAD_BUTTON_BACK:
        return "Select / Back";
    case SDL_GAMEPAD_BUTTON_GUIDE:
        return "Home / Guide";
    case SDL_GAMEPAD_BUTTON_START:
        return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return "Left stick click";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return "Right stick click";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return "L / Left shoulder";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return "R / Right shoulder";
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return "D-pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return "D-pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return "D-pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return "D-pad Right";
    case SDL_GAMEPAD_BUTTON_MISC1:
        return "Capture / Misc";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
        return "Paddle 1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
        return "Paddle 2";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
        return "Paddle 3";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
        return "Paddle 4";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return "Touchpad";
    default:
        return "Unassigned";
    }
}
} // namespace eb
