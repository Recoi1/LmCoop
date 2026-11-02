#pragma once
// Mouse in the game's own menus (menu_mouse.cpp): the game has no pointer, so the port finds the
// item under the mouse and presses the game's D-pad until that item is selected (its highlight and
// sound play as usual), then A on a left click; a right click is B. Pure logic here (tested in
// pc/tests/menu_mouse_test.cpp).
#include <cstdint>

struct PADStatus;

namespace recomp::menumouse {

// PADRead (hle_pad.cpp): adds the mouse's presses to port 1 while a supported menu is up.
void pad(PADStatus* st);

constexpr std::uint16_t kLeft = 0x0001, kRight = 0x0002, kA = 0x0100, kB = 0x0200;  // PAD_BUTTON_*

struct Point {
    float x, y;
};

// Window pixel -> the game's 640x480 picture, which menus draw centered at 4:3 (bars at the sides
// on a wider window, at the top and bottom on a taller one). False outside the picture.
inline bool toGame(float mx, float my, float winW, float winH, Point& out) {
    if (winW <= 0 || winH <= 0) return false;
    const float scale = winW * 3 < winH * 4 ? winW / 640.0f : winH / 480.0f;
    const float x0 = (winW - 640.0f * scale) * 0.5f, y0 = (winH - 480.0f * scale) * 0.5f;
    out = {(mx - x0) / scale, (my - y0) / scale};
    return out.x >= 0 && out.x <= 640 && out.y >= 0 && out.y <= 480;
}

// File select (scene 3): File 1-3 are 0-2, Options 3; -1 when the pointer is on none.
// Boxes (picture plus its label) measured from the screen; where they overlap, the nearer centre.
inline int fileSelectItem(Point p) {
    struct Box { float x0, y0, x1, y1; };
    static constexpr Box kBoxes[4] = {{30, 95, 215, 270}, {139, 160, 310, 312}, {243, 218, 413, 372}, {467, 273, 602, 405}};
    int best = -1;
    float bestD = 0;
    for (int i = 0; i < 4; i++) {
        const Box& b = kBoxes[i];
        if (p.x < b.x0 || p.x > b.x1 || p.y < b.y0 || p.y > b.y1) continue;
        const float dx = p.x - (b.x0 + b.x1) * 0.5f, dy = p.y - (b.y0 + b.y1) * 0.5f, d = dx * dx + dy * dy;
        if (best < 0 || d < bestD) best = i, bestD = d;
    }
    return best;
}

// Which buttons to add this frame. The pointer only steers after it moved (or was clicked), so it
// never fights the keyboard or a controller; D-pad presses are pulses (2 frames on, 6 off) because
// the menu reacts to new presses, A and B are held 3 frames.
struct Steer {
    int target = -1;
    bool pendingA = false;
    std::uint32_t holdUntil = 0;
    std::uint16_t held = 0;

    void mouseMoved(int hovered) {
        if (hovered >= 0) target = hovered;
    }
    std::uint16_t buttons(int current, bool click, bool rightClick, std::uint32_t frame, int hovered = -1) {
        if (held && frame < holdUntil) return held;
        held = 0;
        if (rightClick) return hold(kB, frame);
        if (click && hovered >= 0) target = hovered, pendingA = true;
        if (target < 0) return 0;
        if (current == target) {
            target = -1;
            if (!pendingA) return 0;
            pendingA = false;
            return hold(kA, frame);
        }
        return frame % 8 < 2 ? (target > current ? kRight : kLeft) : 0;
    }

private:
    std::uint16_t hold(std::uint16_t b, std::uint32_t frame) {
        held = b, holdUntil = frame + 3;
        return b;
    }
};

}  // namespace recomp::menumouse
