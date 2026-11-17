// Port/eame: the on-screen SNES controller (touch_controls.hpp).
#include "touch_controls.hpp"
#include "gl_es.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace eb {
namespace {
using Button = TouchControls::Button;

// JOY1 bits, as DesktopInput's keyboard map sets them (A 7, X 6, L 5, R 4,
// B 15, Y 14, Select 13, Start 12, the D-pad 11-8), with one change for
// thumbs: EarthBound's quick Talk/Check is the SNES L button and its A opens
// the command menu, while Y does nothing in the game. So the on-screen A
// sends L (talk or check at once; L also confirms in menus) and the on-screen
// Y sends A (the command menu). L itself still sends L.
constexpr std::uint16_t snes_a = 1u << 7, snes_l = 1u << 5;
constexpr std::array<std::uint16_t, TouchControls::Count> joy1{
    1u << 11, 1u << 10, 1u << 9, 1u << 8, // Up Down Left Right
    snes_l, 1u << 15, 1u << 6, snes_a,    // A (talk/check) B X Y (menu)
    1u << 5, 1u << 4, 1u << 13, 1u << 12, // L R Select Start
    0};                                   // Menu: take_menu

struct Rect {
    float x{}, y{}, w{}, h{};
};

// --- pictures ---------------------------------------------------------------
// Each control is drawn once for its size into premultiplied RGBA: signed
// distances (negative inside) give anti-aliased edges at any resolution.

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float coverage(float d) { return clampf(0.5f - d, 0.f, 1.f); }

float rounded_box(float px, float py, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::fabs(px - cx) - hw + r, qy = std::fabs(py - cy) - hh + r;
    const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

float segment(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float t = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-9f), 0.f, 1.f);
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

float smooth_min(float a, float b, float k) {
    const float h = clampf(0.5f + 0.5f * (b - a) / k, 0.f, 1.f);
    return b + (a - b) * h - k * h * (1.f - h);
}

struct Canvas {
    int w{}, h{};
    std::vector<float> rgba; // premultiplied
    Canvas(int width, int height) : w(width), h(height), rgba(std::size_t(width) * height * 4, 0.f) {}
    // Porter-Duff "over" of a white-to-black grey `level` at alpha `a`.
    void over(int i, int j, float level, float a) {
        if (a <= 0.f || i < 0 || j < 0 || i >= w || j >= h)
            return;
        float *p = &rgba[(std::size_t(j) * w + i) * 4];
        const float keep = 1.f - a;
        p[0] = level * a + p[0] * keep;
        p[1] = level * a + p[1] * keep;
        p[2] = level * a + p[2] * keep;
        p[3] = a + p[3] * keep;
    }
};

using Shape = float (*)(const void *, float, float);

// Dark glass: a soft shadow below it, a dark tint (so it reads over a bright
// picture), a white veil lighter at the top, a gloss and a rim of light.
// `lit` (0-1 per pixel) brightens a held control.
template <class Lit>
void glass(Canvas &c, Shape shape, const void *data, float unit, Lit lit) {
    const float rim = std::max(1.5f, unit * 0.022f), blur = unit * 0.06f, drop = unit * 0.035f;
    for (int j = 0; j < c.h; ++j) {
        const float ny = float(j) / float(std::max(1, c.h - 1));
        for (int i = 0; i < c.w; ++i) {
            const float x = i + 0.5f, y = j + 0.5f, d = shape(data, x, y), inside = coverage(d);
            const float shadow = shape(data, x, y - drop);
            if (shadow > -blur && inside < 1.f)
                c.over(i, j, 0.f, 0.30f * (shadow < 0 ? 1.f : std::exp(-shadow / blur)) * (1.f - inside));
            if (inside <= 0.f)
                continue;
            const float light = clampf(lit(x, y), 0.f, 1.f);
            c.over(i, j, 0.06f, 0.30f * inside);
            c.over(i, j, 1.f, (0.13f + 0.10f * (1.f - ny) + 0.22f * light) * inside);
            if (ny < 0.45f)
                c.over(i, j, 1.f, 0.10f * (1.f - ny / 0.45f) * inside);
            const float band = coverage(std::fabs(d + rim * 0.5f) - rim * 0.5f);
            c.over(i, j, 1.f, band * (0.28f + 0.42f * (1.f - ny) + 0.25f * light));
        }
    }
}

// Thin white strokes: line segments with round ends.
struct Lines {
    std::vector<std::array<float, 4>> s;
    void add(float ax, float ay, float bx, float by) { s.push_back({ax, ay, bx, by}); }
    float distance(float x, float y) const {
        float best = 1e9f;
        for (const auto &l : s)
            best = std::min(best, segment(x, y, l[0], l[1], l[2], l[3]));
        return best;
    }
};
void stroke(Canvas &c, const Lines &lines, float width, float alpha) {
    if (lines.s.empty())
        return;
    for (int j = 0; j < c.h; ++j)
        for (int i = 0; i < c.w; ++i) {
            const float cover = coverage(lines.distance(i + 0.5f, j + 0.5f) - width * 0.5f);
            if (cover > 0.f)
                c.over(i, j, 1.f, cover * alpha);
        }
}

// A small stroke font for the labels: each letter is polylines in a box one
// unit tall, `width` units wide (y down).
struct Glyph {
    float width;
    std::vector<std::vector<std::array<float, 2>>> lines;
};
const Glyph *glyph(char c) {
    static const std::array<std::pair<char, Glyph>, 13> font{{
        {'A', {0.60f, {{{0, 1}, {0.30f, 0}, {0.60f, 1}}, {{0.11f, 0.64f}, {0.49f, 0.64f}}}}},
        {'B', {0.56f, {{{0, 0.48f}, {0.40f, 0.48f}, {0.52f, 0.37f}, {0.52f, 0.11f}, {0.40f, 0}, {0, 0}, {0, 1},
                        {0.43f, 1}, {0.56f, 0.88f}, {0.56f, 0.60f}, {0.43f, 0.48f}}}}},
        {'X', {0.58f, {{{0, 0}, {0.58f, 1}}, {{0.58f, 0}, {0, 1}}}}},
        {'Y', {0.58f, {{{0, 0}, {0.29f, 0.5f}, {0.58f, 0}}, {{0.29f, 0.5f}, {0.29f, 1}}}}},
        {'L', {0.46f, {{{0, 0}, {0, 1}, {0.46f, 1}}}}},
        {'R', {0.54f, {{{0, 1}, {0, 0}, {0.40f, 0}, {0.52f, 0.12f}, {0.52f, 0.36f}, {0.40f, 0.48f}, {0, 0.48f}},
                       {{0.26f, 0.48f}, {0.54f, 1}}}}},
        {'S', {0.54f, {{{0.52f, 0.12f}, {0.41f, 0}, {0.13f, 0}, {0.01f, 0.12f}, {0.01f, 0.36f}, {0.12f, 0.47f},
                        {0.43f, 0.53f}, {0.54f, 0.64f}, {0.54f, 0.88f}, {0.42f, 1}, {0.12f, 1}, {0, 0.88f}}}}},
        {'E', {0.48f, {{{0.48f, 0}, {0, 0}, {0, 1}, {0.48f, 1}}, {{0, 0.5f}, {0.40f, 0.5f}}}}},
        {'C', {0.54f, {{{0.54f, 0.13f}, {0.42f, 0}, {0.14f, 0}, {0, 0.14f}, {0, 0.86f}, {0.14f, 1}, {0.42f, 1},
                        {0.54f, 0.87f}}}}},
        {'T', {0.56f, {{{0, 0}, {0.56f, 0}}, {{0.28f, 0}, {0.28f, 1}}}}},
        {'M', {0.66f, {{{0, 1}, {0, 0}, {0.33f, 0.62f}, {0.66f, 0}, {0.66f, 1}}}}},
        {'N', {0.56f, {{{0, 1}, {0, 0}, {0.56f, 1}, {0.56f, 0}}}}},
        {'U', {0.56f, {{{0, 0}, {0, 0.80f}, {0.17f, 1}, {0.39f, 1}, {0.56f, 0.80f}, {0.56f, 0}}}}},
    }};
    for (const auto &[letter, g] : font)
        if (letter == c)
            return &g;
    return nullptr;
}
constexpr float letter_gap = 0.22f;
float text_width(const std::string &text) {
    float w = 0;
    for (char c : text)
        if (const auto *g = glyph(c))
            w += g->width + letter_gap;
    return text.empty() ? 0 : w - letter_gap;
}
// Text `size` pixels tall with its box's left edge at x and middle at y.
void add_text(Lines &out, const std::string &text, float x, float y, float size) {
    const float top = y - size * 0.5f;
    for (char c : text) {
        const auto *g = glyph(c);
        if (!g)
            continue;
        for (const auto &line : g->lines)
            for (std::size_t k = 1; k < line.size(); ++k)
                out.add(x + line[k - 1][0] * size, top + line[k - 1][1] * size, x + line[k][0] * size,
                        top + line[k][1] * size);
        x += (g->width + letter_gap) * size;
    }
}

struct BoxShape {
    float cx, cy, hw, hh, r;
};
float box_shape(const void *d, float x, float y) {
    const auto &b = *static_cast<const BoxShape *>(d);
    return rounded_box(x, y, b.cx, b.cy, b.hw, b.hh, b.r);
}
struct CircleShape {
    float cx, cy, r;
};
float circle_shape(const void *d, float x, float y) {
    const auto &c = *static_cast<const CircleShape *>(d);
    return std::hypot(x - c.cx, y - c.cy) - c.r;
}
struct CrossShape {
    float cx, cy, arm, half, r, k;
};
float cross_shape(const void *d, float x, float y) {
    const auto &c = *static_cast<const CrossShape *>(d);
    return smooth_min(rounded_box(x, y, c.cx, c.cy, c.arm, c.half, c.r),
                      rounded_box(x, y, c.cx, c.cy, c.half, c.arm, c.r), c.k);
}

struct Sprite {
    Rect place; // in drawable pixels, shadow included
    GLuint texture = 0;
};

Sprite upload(const Canvas &c, float x, float y) {
    std::vector<std::uint8_t> bytes(c.rgba.size());
    for (std::size_t i = 0; i < c.rgba.size(); ++i)
        bytes[i] = std::uint8_t(std::lround(clampf(c.rgba[i], 0.f, 1.f) * 255.f));
    Sprite s;
    s.place = {x, y, float(c.w), float(c.h)};
    glGenTextures(1, &s.texture);
    glBindTexture(GL_TEXTURE_2D, s.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c.w, c.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return s;
}

constexpr int fingers_max = 10;
enum class Zone { None, Dpad, Face, Button };
struct FingerState {
    bool used = false;
    std::uint64_t id = 0;
    Zone zone = Zone::None;
    int button = 0;
    std::uint16_t bits = 0;
    float x = 0, y = 0;
};
} // namespace

struct TouchControls::Impl {
    int width = 0, height = 0;
    float unit = 0, dpad_x = 0, dpad_y = 0, face_x = 0, face_y = 0;
    std::array<Rect, Count> rects{};
    bool touched = false, blocked = false;
    std::array<FingerState, fingers_max> fingers{};
    std::uint16_t held = 0, tapped = 0;
    // Which on-screen buttons a finger holds (for drawing: the on-screen A
    // and L send the same SNES bit).
    unsigned shown_held = 0;
    bool menu_down = false, menu_tapped = false;
    // Pictures: [button][held]; the D-pad is one picture per held arrows.
    std::array<std::array<Sprite, 2>, Count> sprites{};
    std::array<Sprite, 16> dpad{};
    GLuint program = 0;
    GLint alpha_test = -1;

    ~Impl() { forget(); if (program) glDeleteProgram(program); }

    void forget() {
        for (auto &pair : sprites)
            for (auto &s : pair)
                if (s.texture) glDeleteTextures(1, &s.texture), s.texture = 0;
        for (auto &s : dpad)
            if (s.texture) glDeleteTextures(1, &s.texture), s.texture = 0;
    }

    bool shown() const { return width > 0 && touched && !blocked; }

    void place(Button b, float x, float y, float w, float h) { rects[b] = {x, y, w, h}; }

    bool in_button(int b, float x, float y) const {
        Rect r = rects[b];
        const float grow = unit * 0.15f;
        if (r.h < unit) {
            r.y -= (unit - r.h) / 2;
            r.h = unit;
        }
        return x >= r.x - grow && y >= r.y - grow && x < r.x + r.w + grow && y < r.y + r.h + grow;
    }

    Zone zone_at(float x, float y, int &button) const {
        const float reach = unit * 2;
        if (std::hypot(x - dpad_x, y - dpad_y) <= reach) return Zone::Dpad;
        if (std::hypot(x - face_x, y - face_y) <= reach) return Zone::Face;
        for (int b = L; b < Count; ++b)
            if (in_button(b, x, y)) {
                button = b;
                return Zone::Button;
            }
        return Zone::None;
    }

    // The face button a finger presses: the nearest, unless it slid well away.
    int face_button(const FingerState &f) const {
        if (std::hypot(f.x - face_x, f.y - face_y) > unit * 3) return -1;
        int nearest = A;
        float best = 1e30f;
        for (int b : {A, B, X, Y}) {
            const auto &r = rects[b];
            const float d = std::hypot(f.x - (r.x + r.w / 2), f.y - (r.y + r.h / 2));
            if (d < best) best = d, nearest = b;
        }
        return nearest;
    }

    // The D-pad goes by the angle from its middle, so a diagonal presses both
    // arrows; the face buttons by the nearest one.
    std::uint16_t press_at(const FingerState &f) const {
        switch (f.zone) {
        case Zone::Dpad: {
            const float dx = f.x - dpad_x, dy = f.y - dpad_y, length = std::hypot(dx, dy);
            if (length < unit * 0.3f) return 0;
            std::uint16_t out = 0;
            const float c = dx / length, s = dy / length;
            if (c > 0.383f) out |= joy1[Right];
            if (c < -0.383f) out |= joy1[Left];
            if (s > 0.383f) out |= joy1[Down];
            if (s < -0.383f) out |= joy1[Up];
            return out;
        }
        case Zone::Face: {
            const int nearest = face_button(f);
            return nearest < 0 ? 0 : joy1[nearest];
        }
        case Zone::Button:
            return in_button(f.button, f.x, f.y) ? joy1[f.button] : 0;
        default:
            return 0;
        }
    }

    void refresh() {
        std::uint16_t now = 0;
        bool menu = false;
        shown_held = 0;
        for (const auto &f : fingers) {
            if (!f.used) continue;
            now |= f.bits;
            if (f.zone == Zone::Face) {
                if (const int b = face_button(f); b >= 0) shown_held |= 1u << b;
            } else if (f.zone == Zone::Button && in_button(f.button, f.x, f.y)) {
                shown_held |= 1u << f.button;
            }
            if (f.zone == Zone::Button && f.button == Menu) menu |= in_button(Menu, f.x, f.y);
        }
        tapped |= now & ~held;
        held = now;
        menu_down = menu;
    }

    void release_all() {
        fingers = {};
        refresh();
    }

    // --- pictures --------------------------------------------------------
    void make_face(Sprite &out, int b, bool down) {
        const Rect r = rects[b];
        const int pad = int(unit / 8);
        Canvas c(int(r.w) + 2 * pad, int(r.h) + 2 * pad);
        CircleShape shape{c.w * 0.5f, c.h * 0.5f, std::min(r.w, r.h) * 0.46f};
        glass(c, circle_shape, &shape, unit, [&](float, float) { return down ? 1.f : 0.f; });
        Lines letter;
        const std::string text(1, b == A ? 'A' : b == B ? 'B' : b == X ? 'X' : 'Y');
        const float size = shape.r * 0.62f;
        add_text(letter, text, shape.cx - text_width(text) * size / 2, shape.cy, size);
        stroke(c, letter, std::max(1.5f, shape.r * 0.085f), down ? 1.f : 0.9f);
        out = upload(c, r.x - pad, r.y - pad);
    }

    void make_pill(Sprite &out, int b, bool down) {
        static const std::array<const char *, Count> labels{"", "", "", "", "", "", "", "", "L", "R", "SELECT", "START", "MENU"};
        const Rect r = rects[b];
        const int pad = int(unit / 8);
        Canvas c(int(r.w) + 2 * pad, int(r.h) + 2 * pad);
        BoxShape shape{c.w * 0.5f, c.h * 0.5f, r.w * 0.5f, r.h * 0.5f, r.h * 0.5f};
        glass(c, box_shape, &shape, unit, [&](float, float) { return down ? 1.f : 0.f; });
        const std::string text = labels[b];
        const float size = r.h * (text.size() > 1 ? 0.36f : 0.44f), play = b == Start ? size * 0.75f : 0.f;
        const float total = text_width(text) * size + (play > 0 ? play + size * 0.35f : 0.f);
        const float left = shape.cx - total / 2;
        Lines label;
        add_text(label, text, left, shape.cy, size);
        stroke(c, label, std::max(1.5f, size * 0.13f), down ? 1.f : 0.9f);
        if (play > 0) { // START's play triangle
            const float px = left + text_width(text) * size + size * 0.35f, py = shape.cy;
            for (int j = 0; j < c.h; ++j)
                for (int i = 0; i < c.w; ++i) {
                    const float fx = i + 0.5f - px, fy = j + 0.5f - py;
                    const float e1 = -fx, e2 = fx * 0.5f + fy * 0.866f - play * 0.433f,
                                e3 = fx * 0.5f - fy * 0.866f - play * 0.433f;
                    c.over(i, j, 1.f, coverage(std::max({e1, e2, e3})) * (down ? 1.f : 0.9f));
                }
        }
        out = upload(c, r.x - pad, r.y - pad);
    }

    void make_dpad(Sprite &out, unsigned arrows) {
        const int pad = int(unit / 8);
        const Rect left = rects[Left], up = rects[Up];
        Canvas c(int(unit * 3) + 2 * pad, int(unit * 3) + 2 * pad);
        const float ox = left.x - pad, oy = up.y - pad, s = unit;
        CrossShape shape{dpad_x - ox, dpad_y - oy, s * 1.30f, s * 0.42f, s * 0.20f, s * 0.14f};
        const float edge = s * 0.45f, soft = s * 0.12f;
        glass(c, cross_shape, &shape, unit, [&](float x, float y) {
            const float dx = x - shape.cx, dy = y - shape.cy;
            float l = 0;
            if (arrows & 1) l += clampf((-dy - edge) / soft, 0, 1);
            if (arrows & 2) l += clampf((dy - edge) / soft, 0, 1);
            if (arrows & 4) l += clampf((-dx - edge) / soft, 0, 1);
            if (arrows & 8) l += clampf((dx - edge) / soft, 0, 1);
            return l;
        });
        Lines chevrons;
        const float k = s * 0.13f;
        auto chevron = [&](float cx, float cy, float dx, float dy) {
            const float tx = cx + dx * k * 0.5f, ty = cy + dy * k * 0.5f, bx = cx - dx * k * 0.5f,
                        by = cy - dy * k * 0.5f, px = -dy * k, py = dx * k;
            chevrons.add(bx + px, by + py, tx, ty);
            chevrons.add(bx - px, by - py, tx, ty);
        };
        chevron(shape.cx, shape.cy - s, 0, -1);
        chevron(shape.cx, shape.cy + s, 0, 1);
        chevron(shape.cx - s, shape.cy, -1, 0);
        chevron(shape.cx + s, shape.cy, 1, 0);
        stroke(c, chevrons, s * 0.05f, 0.9f);
        out = upload(c, ox, oy);
    }

    void draw_sprite(const Sprite &s) {
        if (!s.texture) return;
        const float x0 = s.place.x / width * 2 - 1, x1 = (s.place.x + s.place.w) / width * 2 - 1;
        const float y0 = 1 - (s.place.y + s.place.h) / height * 2, y1 = 1 - s.place.y / height * 2;
        const float positions[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
        glBindTexture(GL_TEXTURE_2D, s.texture);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(gles::position_attribute);
        glEnableVertexAttribArray(gles::texcoord_attribute);
        glVertexAttribPointer(gles::position_attribute, 2, GL_FLOAT, GL_FALSE, 0, positions);
        glVertexAttribPointer(gles::texcoord_attribute, 2, GL_FLOAT, GL_FALSE, 0, gles::uv_top_down);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(gles::position_attribute);
        glDisableVertexAttribArray(gles::texcoord_attribute);
    }
};

TouchControls::TouchControls() : impl_(std::make_unique<Impl>()) {}
TouchControls::~TouchControls() = default;

void TouchControls::layout(int width, int height, int picture_left, int picture_right, float density,
                           int inset_left, int inset_right, int inset_bottom) {
    auto &d = *impl_;
    const float shorter = float(std::min(width, height));
    float s = shorter * 0.13f;
    if (density > 0) s = std::clamp(s, 48 * density, std::min(80 * density, shorter * 0.16f));
    float m = s * 0.35f;
    const float left = std::max(m, inset_left * 2.f / 3), right = std::max(m, inset_right * 2.f / 3);
    // On a phone the D-pad and the face buttons keep to the bars beside the
    // picture (each about 2.9 buttons wide as drawn), down to Apple's 44 pt
    // target; where the picture fills the screen they go over it.
    const float bars = std::min(picture_left - left, width - picture_right - right);
    const float fit = bars / 2.9f;
    if (fit < s && fit >= 44 * std::max(density, 1.f)) {
        s = fit;
        m = s * 0.35f;
    }
    const std::array<Rect, Count> before = d.rects;
    const float before_unit = d.unit;
    d.width = width;
    d.height = height;
    d.unit = s;
    const float pill_w = s * 1.2f, pill_h = s * 0.55f, gap = s / 6, y0 = m / 2;
    d.dpad_x = left + s * 1.3f;
    d.face_x = width - right - s * 1.4f;
    d.place(L, left, y0, pill_w, pill_h);
    d.place(R, width - right - pill_w, y0, pill_w, pill_h);
    const float menu_y = y0 + pill_h + gap;
    d.place(Menu, left, menu_y, pill_w * 1.25f, pill_h * 0.9f);
    d.dpad_y = d.face_y = std::max(height * 0.56f, menu_y + pill_h + gap + s * 1.5f);
    d.place(Up, d.dpad_x - s / 2, d.dpad_y - s * 1.5f, s, s);
    d.place(Down, d.dpad_x - s / 2, d.dpad_y + s / 2, s, s);
    d.place(Left, d.dpad_x - s * 1.5f, d.dpad_y - s / 2, s, s);
    d.place(Right, d.dpad_x + s / 2, d.dpad_y - s / 2, s, s);
    // The SNES diamond: X top, A right, B bottom, Y left.
    d.place(X, d.face_x - s / 2, d.face_y - s * 1.5f, s, s);
    d.place(B, d.face_x - s / 2, d.face_y + s / 2, s, s);
    d.place(Y, d.face_x - s * 1.5f, d.face_y - s / 2, s, s);
    d.place(A, d.face_x + s / 2, d.face_y - s / 2, s, s);
    const float bottom = height - std::max(m / 2, inset_bottom / 2.f);
    d.place(Select, d.dpad_x - s * 0.75f, bottom - pill_h, s * 1.5f, pill_h);
    d.place(Start, d.face_x - s * 0.75f, bottom - pill_h, s * 1.5f, pill_h);
    bool same = before_unit == d.unit;
    for (int b = 0; b < Count && same; ++b)
        same = std::memcmp(&before[b], &d.rects[b], sizeof(Rect)) == 0;
    if (!same) d.forget();
}

bool TouchControls::finger(Finger kind, std::uint64_t id, float x, float y) {
    auto &d = *impl_;
    if (!d.width || d.blocked) return false;
    FingerState *f = nullptr;
    for (auto &each : d.fingers)
        if (each.used && each.id == id) f = &each;
    if (kind == FingerDown) {
        const bool was_shown = d.shown();
        d.touched = true;
        int button = 0;
        const Zone zone = d.zone_at(x, y, button);
        if (!was_shown || zone == Zone::None) return was_shown && zone != Zone::None;
        if (!f)
            for (auto &each : d.fingers)
                if (!each.used) { f = &each; break; }
        if (!f) return true;
        *f = {true, id, zone, button, 0, x, y};
        if (zone == Zone::Button && button == Menu) d.menu_tapped = true;
        f->bits = d.press_at(*f);
        d.refresh();
        return true;
    }
    if (!f) return false;
    f->x = x;
    f->y = y;
    if (kind == FingerUp) {
        *f = {};
    } else {
        f->bits = d.press_at(*f);
    }
    d.refresh();
    return true;
}

bool TouchControls::covers(float x, float y) const {
    int button = 0;
    return impl_->shown() && impl_->zone_at(x, y, button) != Zone::None;
}

void TouchControls::other_input() {
    if (!impl_->touched) return;
    impl_->touched = false;
    impl_->release_all();
}

void TouchControls::block(bool blocked) {
    if (blocked == impl_->blocked) return;
    impl_->blocked = blocked;
    if (blocked) impl_->release_all();
}

bool TouchControls::shown() const { return impl_->shown(); }

std::uint16_t TouchControls::buttons() {
    auto &d = *impl_;
    const std::uint16_t out = d.held | d.tapped;
    d.tapped = 0;
    return out;
}

bool TouchControls::take_menu() { return std::exchange(impl_->menu_tapped, false); }

void TouchControls::draw() {
    auto &d = *impl_;
    if (!d.shown()) return;
    if (!d.program) {
        d.program = gles::program(gles::plain_fragment, "Touch controls");
        d.alpha_test = glGetUniformLocation(d.program, "AlphaTest");
    }
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    glViewport(0, 0, d.width, d.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied pictures
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(d.program);
    glUniform1i(d.alpha_test, 0);
    const unsigned arrows = ((d.held & joy1[Up]) ? 1u : 0u) | ((d.held & joy1[Down]) ? 2u : 0u) |
                            ((d.held & joy1[Left]) ? 4u : 0u) | ((d.held & joy1[Right]) ? 8u : 0u);
    if (!d.dpad[arrows].texture) d.make_dpad(d.dpad[arrows], arrows);
    d.draw_sprite(d.dpad[arrows]);
    for (int b = A; b < Count; ++b) {
        const bool down = (d.shown_held >> b) & 1u;
        auto &sprite = d.sprites[b][down];
        if (!sprite.texture) {
            if (b <= Y) d.make_face(sprite, b, down);
            else d.make_pill(sprite, b, down);
        }
        d.draw_sprite(sprite);
    }
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}
} // namespace eb
