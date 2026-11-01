#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace recomp::controls {
struct MouseAim {
    float x = 0, y = 0;
    bool released = false;

    static bool gameContext(unsigned scene, unsigned mode, bool event, unsigned eventState, bool hudHidden) {
        // Scripted combat keeps its event running while waiting for normal player input (state 6).
        return scene == 2 && (mode == 0 || mode == 4) && !hudHidden && (!event || eventState == 6);
    }

    void clear() { x = y = 0; }
    void motion(float dx, float dy) {
        if (!std::isfinite(dx) || !std::isfinite(dy)) return;
        x = std::clamp(x + dx, -4096.f, 4096.f);
        y = std::clamp(y - dy, -4096.f, 4096.f);
    }
    std::array<int, 2> take(float sensitivity) {
        float ax = x * sensitivity * 0.03f, ay = y * sensitivity * 0.03f;
        clear();
        const float length = std::hypot(ax, ay);
        if (!std::isfinite(length) || length == 0) return {};
        // Compensate for the original PADClamp dead zone, including slow mouse movements.
        const float magnitude = 15.f + 112.f * std::min(length, 1.f);
        return {int(std::lround(ax / length * magnitude)), int(std::lround(ay / length * magnitude))};
    }
    bool capture(bool gameplay, bool focused, bool menu, bool enabled) const {
        return gameplay && focused && !menu && enabled && !released;
    }
};
}  // namespace recomp::controls
