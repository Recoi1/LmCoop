#pragma once
// Native mouse turning (mouse_look.cpp). The game's C-stick is a turn *speed* with acceleration
// (measured: holding it right spins Luigi faster and faster, about 3.5 to 10 degrees a frame), so
// a mouse fed through it as stick deflection only nudged him while it moved. Here mouse x moves a
// target angle 1:1 and the C-stick steers the player's angle (+0x820, s16, 0x10000 = 360 degrees,
// copied at +0x88/+0x8A; facing vector +0xF48/+0xF50 = sin, cos) onto it. Pure logic here
// (pc/tests/mouse_look_test.cpp).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

struct PADStatus;

namespace recomp::mouselook {

constexpr float kDegreesPerPixel = 0.15f;  // at sensitivity 1: 2400 pixels turn Luigi once around

// The angle after `pixels` of horizontal mouse motion (right = the way C-stick right turns: the
// angle shrinks, measured).
inline std::uint16_t turned(std::uint16_t yaw, float pixels, float sensitivity) {
    const float units = pixels * sensitivity * kDegreesPerPixel * (65536.0f / 360.0f);
    return std::uint16_t(std::int32_t(yaw) - std::int32_t(std::lround(units)));
}

inline void facing(std::uint16_t yaw, float& fx, float& fz) {
    const float a = float(yaw) * (6.28318530718f / 65536.0f);
    fx = std::sin(a), fz = std::cos(a);
}

// Mouse turning through the game's own turning: the mouse moves a target angle 1:1 (turned), and
// each pad read the C-stick pushes the player's angle toward it, softer as it gets close, so he
// ends up facing exactly where the mouse says, with the game's animation. (Writing the angle
// directly is undone by the game within a frame.) Lets go after resting on target for a while
// or when the target cannot be reached, so the keyboard's C-stick keys still turn him.
struct Follow {
    std::uint16_t target = 0;
    bool active = false;
    int restFrames = 0, stuckFrames = 0;

    int stick(std::uint16_t current, float pixels, float sensitivity = 1.0f) {
        if (pixels != 0) {
            if (!active) target = current, active = true;
            target = turned(target, pixels, sensitivity);
            restFrames = stuckFrames = 0;
        }
        if (!active) return 0;
        const int err = std::int16_t(std::uint16_t(target - current));
        if (std::abs(err) <= kClose) {
            if (pixels == 0 && ++restFrames > 10) active = false;
            return 0;
        }
        if (pixels == 0 && ++stuckFrames > 30) {
            active = false;
            return 0;
        }
        // Full push from 45 degrees away; never less than the game's dead zone (PADClamp, ~15).
        const int mag = std::min(127, 16 + std::abs(err) * 111 / 0x2000);
        return err > 0 ? -mag : mag;  // C-stick right shrinks the angle
    }

    static constexpr int kClose = 0x80;  // 0.7 degrees: there
};

// controls.cpp: horizontal mouse motion while walking around (already scaled by the sensitivity).
void addPixels(float pixels);
// PADRead (hle_pad.cpp), after the input script and filters: while the mouse is steering, sets
// port 1's C-stick x (only while walking around: scene 2, mode 0).
void pad(PADStatus* st);

}  // namespace recomp::mouselook
