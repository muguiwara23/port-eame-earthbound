#pragma once

// Port/eame: an on-screen SNES controller for touch screens (iPhone, iPad),
// the same look as Port/eame's Yu-Gi-Oh! Forbidden Memories port: dark glass
// buttons with a light rim and thin white symbols, drawn at the screen's own
// resolution. A D-pad and Y/X/B/A at the sides of the picture, L and R pills in
// the top corners, MENU under L (it opens the settings panel), SELECT and
// START under the D-pad and the face buttons; all kept clear of the notch.
//
// It shows after the screen is first touched and hides when a key or a
// gamepad button is pressed. Fingers are in drawable pixels. It feeds JOY1
// bits beside the keyboard and gamepad, and never touches game state.

#include <cstdint>
#include <memory>

namespace eb {
class TouchControls {
  public:
    enum Button { Up, Down, Left, Right, A, B, X, Y, L, R, Select, Start, Menu, Count };
    enum Finger { FingerDown, FingerMove, FingerUp };

    TouchControls();
    ~TouchControls();
    TouchControls(const TouchControls &) = delete;
    TouchControls &operator=(const TouchControls &) = delete;

    // The drawable's size, the picture's left and right edges in it, the
    // pixels per point and the safe-area insets (pixels from the left, right
    // and bottom edges clear of cutouts and rounded corners).
    void layout(int width, int height, int picture_left, int picture_right, float density, int inset_left,
                int inset_right, int inset_bottom);
    // A finger, in drawable pixels. A finger that lands on a control stays the
    // controls' until it lifts. Returns whether the controls took it.
    bool finger(Finger kind, std::uint64_t id, float x, float y);
    // Whether a point is on a control (for the mouse events SDL makes of touches).
    bool covers(float x, float y) const;
    // A key or a gamepad button was pressed: the controls hide.
    void other_input();
    // While a panel is up the controls step aside and let go of everything.
    void block(bool blocked);
    bool shown() const;
    // JOY1 bits held now, or pressed since the last call and let go already.
    std::uint16_t buttons();
    // MENU was tapped since the last call.
    bool take_menu();
    // Draws over the frame (OpenGL ES; the caller's viewport is replaced and
    // restored).
    void draw();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eb
