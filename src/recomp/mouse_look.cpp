// Native mouse turning (mouse_look.h).
#include "recomp/mouse_look.h"

#include "recomp/coop.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/skip.h"
#include "recomp/version.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp::mouselook {
namespace {

std::atomic<float> sPending{0.0f}, sPendingPitch{0.0f};
float sPitch = 0;  // degrees the look-at is held at
bool sPitchOn = false;

// The player this PC controls: player 1, or the online client's player 2.
u32 localActor() {
    if (const int lp = coop::localPlayer(); lp >= 0) return coop::cachedPlayer(lp);
    const u32 l = rd32(VA(0x803D48A0));
    return gu::inMem1(l) && rd32(l + 0x3C) == 0 ? l : 0;
}

// LM_TEST_TURN / LM_TEST_PITCH=<frame>:<pixels>;... (testing): mouse motion added once that guest frame is reached.
float scripted(const char* name, u32& done) {
    const char* spec = std::getenv(name);
    if (!spec) return 0;
    const u32 now = skip::guestFrames();
    float px = 0;
    for (const char* t = spec; t && *t; t = std::strchr(t, ';') ? std::strchr(t, ';') + 1 : nullptr) {
        unsigned f = 0;
        float v = 0;
        if (std::sscanf(t, "%u:%f", &f, &v) == 2 && f > done && f <= now) px += v;
    }
    done = now;  // entries up to this frame were applied (game frames may skip numbers)
    return px;
}

}  // namespace

void addPixels(float pixels) {
    float cur = sPending.load();
    while (!sPending.compare_exchange_weak(cur, cur + pixels)) {
    }
}

void addPitchPixels(float up) {
    float cur = sPendingPitch.load();
    while (!sPendingPitch.compare_exchange_weak(cur, cur + up)) {
    }
}

void beforeUpdate(Ctx& c, u32 self) {
    if (!g_mem || self != localActor()) return;
    static u32 sTurnDone = 0, sPitchDone = 0;
    const float pixels = sPending.exchange(0.0f) + scripted("LM_TEST_TURN", sTurnDone);
    const float up = sPendingPitch.exchange(0.0f) + scripted("LM_TEST_PITCH", sPitchDone);
    // Only while walking around (scene 2, mode 0); motion from anything else is dropped.
    if (rd32(VA(0x804D80A0)) != 2 || rd32(VA(0x804D80C4)) != 0) return void(sPitchOn = false);
    // Each room has its own camera: a new room starts from the game's own pitch again.
    static u32 sRoom = ~0u;
    if (const u32 room = rd32(VA(0x803A3CA0) + 12) >> 24; room != sRoom) sRoom = room, sPitchOn = false;
    if (pixels == 0 && up == 0 && !sPitchOn) return;
    if (pixels != 0) {
        const std::uint16_t yaw = turned(rd16(self + 0x88), pixels, 1.0f);
        // The game's own "set rotation" (fn_800B1B3C): the movement object's angles, the facing
        // quaternion (+0xF38), the facing vector (+0xF48) and the model's copy (*(+0x7BC)) together.
        static const u32 sSetRotation = VA(0x800B1B3C);
        guestCall(c, sSetRotation, self, rd16(self + 0x84), yaw, rd16(self + 0x8C));
        wr16(self + 0x8A, yaw);
    }
    if (up != 0 || sPitchOn) {
        // The camera (object at +0x798: eye at +0x8C, look-at at +0x98) is what the game pitches with the
        // C-stick, easing for ~100 frames; F80 (+0xF80) is the direction from look-at to eye, rebuilt each
        // frame from it. The pitch is kept here and written into both every frame.
        const u32 cam = rd32(self + 0x798);
        if (gu::inMem1(cam)) {
            const float ex = rdf32(cam + 0x8C), ey = rdf32(cam + 0x90), ez = rdf32(cam + 0x94);
            const float tx = rdf32(cam + 0x98), ty = rdf32(cam + 0x9C), tz = rdf32(cam + 0xA0);
            const float run = std::hypot(tx - ex, tz - ez);
            if (!sPitchOn) sPitch = std::atan2(ty - ey, run) * (180.0f / 3.14159265359f), sPitchOn = true;
            sPitch = pitched(sPitch, up);
            const float r = sPitch * (3.14159265359f / 180.0f);
            wrf32(cam + 0x9C, ey + run * std::tan(r));
            wrf32(self + 0xF84, -std::sin(r));
            wrf32(self + 0xF88, std::copysign(std::cos(r), rdf32(self + 0xF88)));
        }
    }
    // The mouse is not pad input, so without this the game's idle timer (+0x116C, counts frames of
    // standing still) would run out and Luigi's idle animation turns him on its own (measured).
    if (pixels != 0 || up != 0) wrf32(self + 0x116C, 0.0f);
}

}  // namespace recomp::mouselook
