#pragma once
// Native mouse turning (mouse_look.cpp). The game's C-stick is a turn *speed* with acceleration
// (measured: a 90 degree turn at full push takes 19 game frames), so a mouse fed through it lags
// behind the hand. Instead mouse x turns the player's angle (+0x88, u16, 0x10000 = 360 degrees) 1:1
// and at once, through the game's own set-rotation routine. Pure logic here
// (pc/tests/mouse_look_test.cpp).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace recomp {
struct Ctx;
}

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

// The aim pitch in degrees after `up` pixels of upward mouse motion. The game keeps it between 0
// (level) and 29.4 (measured, with the C-stick: up = 29.4, down = 0, resting about 11), and the stick
// is the other way round from a mouse, so mouse up lowers it.
constexpr float kMaxPitch = 29.4f;
inline float pitched(float pitch, float up, float sensitivity = 1.0f) {
    return std::clamp(pitch - up * sensitivity * kDegreesPerPixel, 0.0f, kMaxPitch);
}

// controls.cpp: vertical mouse motion while walking around (up positive, scaled by the sensitivity).
void addPitchPixels(float up);
// controls.cpp: horizontal mouse motion while walking around (already scaled by the sensitivity).
void addPixels(float pixels);
// Before the game's per-frame update of player `self` (fn_800B783C hook): turns the player this PC
// controls by the mouse motion collected since, at once (walking around only: scene 2, mode 0).
void beforeUpdate(Ctx& c, std::uint32_t self);

}  // namespace recomp::mouselook
