// Native mouse turning (mouse_look.h).
#include "recomp/mouse_look.h"

#include "recomp/coop.h"
#include "recomp/guest_util.h"
#include "recomp/runtime.h"
#include "recomp/skip.h"
#include "recomp/version.h"

#include <dolphin/pad.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp::mouselook {
namespace {

Follow sFollow;
std::atomic<float> sPending{0.0f};
constexpr int kNone = 1000;  // not steering: the pad's own C-stick stays

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

int stickX(float pixels) {
    // Only while walking around (scene 2, mode 0); anything else ends the steering.
    const u32 p = g_mem && rd32(VA(0x804D80A0)) == 2 && rd32(VA(0x804D80C4)) == 0 ? localActor() : 0;
    if (!p) {
        sFollow = Follow{};
        return kNone;
    }
    const int x = sFollow.stick(rd16(p + 0x820), pixels);
    return sFollow.active || x ? x : kNone;
}

}  // namespace

void addPixels(float pixels) {
    float cur = sPending.load();
    while (!sPending.compare_exchange_weak(cur, cur + pixels)) {
    }
}

void pad(PADStatus* st) {
    if (const int x = stickX(sPending.exchange(0.0f) + scripted()); x != kNone) {
        st[0].substickX = s8(x);
        st[0].err = PAD_ERR_NONE;
    }
}

}  // namespace recomp::mouselook
