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

struct PADStatus;

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

// The flashlight and vacuum aim: a blend value at actor +0x8AC, 0 to 1 with 0.5 neutral, that the
// C-stick eases (down raises it: 0.5 to 1.0 in about 15 frames, the beam going from slightly down to
// 70 degrees up; up lowers it). The pose, the beam and the nozzle follow it (measured). Mouse up aims up,
// 0.0009 a pixel at sensitivity 1 (about 550 pixels from level to straight up).
constexpr float kAimPerPixel = 0.0009f;
inline float aimed(float aim, float up, float sensitivity = 1.0f) {
    return std::clamp(aim + up * sensitivity * kAimPerPixel, 0.0f, 1.0f);
}

// W/A/S/D relative to where the player faces: the game moves Luigi relative to its camera, so the
// stick (x right, y forward) is turned by the facing minus the camera's direction (radians, both
// (sin, cos) angles; turning right is a smaller angle, measured). In place, clamped to +-127.
inline void fpsStick(int& x, int& y, float facingRad, float cameraRad) {
    const float m = std::hypot(float(x), float(y));
    if (m == 0) return;
    const float a = std::atan2(float(x), float(y)) - (facingRad - cameraRad);
    x = std::clamp(int(std::lround(m * std::sin(a))), -127, 127);
    y = std::clamp(int(std::lround(m * std::cos(a))), -127, 127);
}

// controls.cpp: vertical mouse motion while walking around (up positive, scaled by the sensitivity).
void addPitchPixels(float up);
// controls.cpp: horizontal mouse motion while walking around (already scaled by the sensitivity).
void addPixels(float pixels);
// Before the game's per-frame update of player `self` (fn_800B783C hook): turns the player this PC
// controls by the mouse motion collected since, at once (walking around only: scene 2, mode 0).
void beforeUpdate(Ctx& c, std::uint32_t self);
// After that update: while the mouse is in use the facing stays the mouse's, not the walking direction.
void afterUpdate(Ctx& c, std::uint32_t self);
// PADRead (hle_pad.cpp), after the input script and filters: with keyboard and mouse, W/A/S/D move
// relative to the facing (fpsStick). Only after the mouse has been used.
void stick(PADStatus* st, bool keyboard);
// Online: the other player's actor on this PC takes that player's facing (angle as in actor +0x88) and aim
// (0 to 1) instead of working them out from the 30 Hz pad alone (mouse turning and mouse aim are not pad
// input). Applied around the actor's update; actor 0 stops it.
void setRemote(std::uint32_t actor, std::uint16_t yaw, float aim);

}  // namespace recomp::mouselook
