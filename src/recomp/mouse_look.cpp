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

std::atomic<float> sPending{0.0f};

// The player this PC controls: player 1, or the online client's player 2.
u32 localActor() {
    if (const int lp = coop::localPlayer(); lp >= 0) return coop::cachedPlayer(lp);
    const u32 l = rd32(VA(0x803D48A0));
    return gu::inMem1(l) && rd32(l + 0x3C) == 0 ? l : 0;
}

// LM_TEST_TURN=<frame>:<pixels>;... (testing): mouse motion added once that guest frame is reached.
float scripted() {
    static const char* sSpec = std::getenv("LM_TEST_TURN");
    if (!sSpec) return 0;
    const u32 now = skip::guestFrames();
    static u32 sDone = 0;  // entries up to this frame were applied (game frames may skip numbers)
    float px = 0;
    for (const char* s = sSpec; s && *s; s = std::strchr(s, ';') ? std::strchr(s, ';') + 1 : nullptr) {
        unsigned f = 0;
        float v = 0;
        if (std::sscanf(s, "%u:%f", &f, &v) == 2 && f > sDone && f <= now) px += v;
    }
    sDone = now;
    return px;
}

}  // namespace

void addPixels(float pixels) {
    float cur = sPending.load();
    while (!sPending.compare_exchange_weak(cur, cur + pixels)) {
    }
}

void beforeUpdate(Ctx& c, u32 self) {
    if (!g_mem || self != localActor()) return;
    const float pixels = sPending.exchange(0.0f) + scripted();
    // Only while walking around (scene 2, mode 0); motion from anything else is dropped.
    if (pixels == 0 || rd32(VA(0x804D80A0)) != 2 || rd32(VA(0x804D80C4)) != 0) return;
    const std::uint16_t yaw = turned(rd16(self + 0x88), pixels, 1.0f);
    // The game's own "set rotation" (fn_800B1B3C): the movement object's angles, the facing
    // quaternion (+0xF38), the facing vector (+0xF48) and the model's copy (*(+0x7BC)) together.
    static const u32 sSetRotation = VA(0x800B1B3C);
    guestCall(c, sSetRotation, self, rd16(self + 0x84), yaw, rd16(self + 0x8C));
    wr16(self + 0x8A, yaw);
    // The mouse is not pad input, so without this the game's idle timer (+0x116C, counts frames of
    // standing still) would run out and Luigi's idle animation turns him on its own (measured).
    wrf32(self + 0x116C, 0.0f);
}

}  // namespace recomp::mouselook
