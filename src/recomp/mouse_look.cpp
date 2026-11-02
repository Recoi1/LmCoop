// Native mouse turning (mouse_look.h).
#include "recomp/mouse_look.h"

#include "recomp/coop.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
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

std::atomic<float> sPending{0.0f}, sPendingPitch{0.0f};
std::uint16_t sYaw = 0;  // the facing the mouse holds
bool sYawOn = false, sMouseSeen = false;
std::atomic<bool> sStickTurn{false}, sStickPitch{false};  // the C-stick is turning / tilting this frame (arrow keys, a gamepad)
float sAim = 0.5f;  // the aim blend the mouse holds (0 to 1, 0.5 neutral)
bool sAimOn = false;
float sCamPitch = 0;  // the camera pitch (radians) kept while the mouse aims
bool sCamPin = false;

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
    static const bool sTest = std::getenv("LM_TEST_INPUT_ONLY") != nullptr;  // tests ignore the physical mouse
    if (sTest) return;
    float cur = sPending.load();
    while (!sPending.compare_exchange_weak(cur, cur + pixels)) {
    }
}

void addPitchPixels(float up) {
    static const bool sTest = std::getenv("LM_TEST_INPUT_ONLY") != nullptr;
    if (sTest) return;
    float cur = sPendingPitch.load();
    while (!sPendingPitch.compare_exchange_weak(cur, cur + up)) {
    }
}

namespace {

// The game's own "set rotation" (fn_800B1B3C): the movement object's angles, the facing quaternion
// (+0xF38), the facing vector (+0xF48) and the model's copy (*(+0x7BC)) together.
void setYaw(Ctx& c, u32 self, std::uint16_t yaw) {
    static const u32 sSetRotation = VA(0x800B1B3C);
    guestCall(c, sSetRotation, self, rd16(self + 0x84), yaw, rd16(self + 0x8C));
    wr16(self + 0x8A, yaw);
}

// The aim blend (0 to 1, 0.5 neutral) the game copies into actor +0x8AC every frame in its movement
// routine: *(actor + 0x7EC) + 0x240. Writing the copy does nothing (measured); this is the source.
u32 aimSource(u32 self) {
    const u32 obj = rd32(self + 0x7EC);
    return gu::inMem1(obj) ? obj + 0x240 : 0;
}

// The game tilts its camera along with the aim; the mouse aims the beam and leaves the camera where it
// was. The camera (object at +0x798) looks from its eye (+0x98) at its look-at point (+0x8C): the eye's
// height over the look-at, for the distance on the ground, is the pitch (measured). Rewritten every
// frame (after the game's update, so what is drawn is this).
void pinCamera(u32 self) {
    const u32 cam = rd32(self + 0x798);
    if (!gu::inMem1(cam)) return;
    const float lx = rdf32(cam + 0x8C), ly = rdf32(cam + 0x90), lz = rdf32(cam + 0x94);
    const float ex = rdf32(cam + 0x98), ey = rdf32(cam + 0x9C), ez = rdf32(cam + 0xA0);
    const float run = std::hypot(ex - lx, ez - lz);
    if (!sCamPin) sCamPitch = std::atan2(ey - ly, run), sCamPin = true;
    wrf32(cam + 0x9C, ly + run * std::tan(sCamPitch));
}

// Walking around freely: scene 2, mode 0, and Luigi's own state +0x1180 is 5 (normal) or 1 (vacuum on).
// Hits, doors and other scripted moments are left to the game.
bool freeMovement(u32 self) {
    if (rd32(VA(0x804D80A0)) != 2 || rd32(VA(0x804D80C4)) != 0) return false;
    const u32 state = rd32(self + 0x1180);
    return state == 5 || state == 1;
}

}  // namespace

void beforeUpdate(Ctx& c, u32 self) {
    if (ver::pal() || !g_mem || self != localActor()) return;  // field offsets are the USA game's
    static u32 sTurnDone = 0, sPitchDone = 0;
    const float pixels = sPending.exchange(0.0f) + scripted("LM_TEST_TURN", sTurnDone);
    const float up = sPendingPitch.exchange(0.0f) + scripted("LM_TEST_PITCH", sPitchDone);
    if (pixels != 0 || up != 0) sMouseSeen = true;
    if (!freeMovement(self)) return void(sAimOn = sCamPin = sYawOn = false);  // motion from anything else is dropped
    const std::uint16_t now = rd16(self + 0x88);
    if (!sYawOn) {
        sYaw = now, sYawOn = true;
    } else if (pixels == 0 && std::abs(std::int16_t(now - sYaw)) > 0x3000) {
        sYaw = now;  // something else turned him far more than walking does (a script, a hit): follow it
    }
    if (sStickTurn && pixels == 0) sYaw = now;  // the C-stick turns him: the game's turn stands
    if (pixels != 0) sYaw = turned(sYaw, pixels, 1.0f);
    if (sMouseSeen && sYaw != now) setYaw(c, self, sYaw);
    if (sStickPitch && up == 0) sAimOn = sCamPin = false;  // the C-stick tilts it: the game's aim and camera stand; the mouse picks them up again
    if (up != 0 || sAimOn) {
        const u32 src = aimSource(self);
        if (!src) return;
        if (!sAimOn) sAim = rdf32(self + 0x8AC), sAimOn = true;  // where the game has it now (not the source: that is stale at rest)
        sAim = aimed(sAim, up);
        wrf32(src, sAim);
        pinCamera(self);
    }
    // The mouse is not pad input, so without this the game's idle timer (+0x116C, counts frames of
    // standing still) would run out and Luigi's idle animation turns him on its own (measured). After 60
    // idle frames the game also recenters the aim, so the timer stays at zero while the mouse holds one.
    if (pixels != 0 || up != 0 || sAimOn) wrf32(self + 0x116C, 0.0f);
}

void afterUpdate(Ctx& c, u32 self) {
    // The game turns him toward the way he walks; with a mouse the facing is the mouse's.
    if (ver::pal() || !g_mem || !sMouseSeen || !sYawOn || self != localActor() || !freeMovement(self)) return;
    if (sAimOn && !sStickPitch) {
        if (const u32 src = aimSource(self)) wrf32(src, sAim);
        pinCamera(self);
    }
    if (sStickTurn) return void(sYaw = rd16(self + 0x88));  // the C-stick is turning him: let it
    setYaw(c, self, sYaw);
}

void stick(PADStatus* st, bool keyboard) {
    static const bool sForce = std::getenv("LM_TEST_FPS") != nullptr;  // testing: any device
    sStickTurn = std::abs(int(st[0].substickX)) > 20;
    sStickPitch = std::abs(int(st[0].substickY)) > 20;
    if (ver::pal() || !g_mem || !sMouseSeen || !sYawOn || !(keyboard || sForce)) return;
    const u32 self = localActor();
    const u32 cam = self ? rd32(self + 0x798) : 0;
    if (!self || !gu::inMem1(cam) || !freeMovement(self)) return;
    const float facingRad = float(sYaw) * (6.28318530718f / 65536.0f);
    // The camera looks from its eye (+0x98) at the look-at point (+0x8C, near Luigi).
    const float cameraRad = std::atan2(rdf32(cam + 0x8C) - rdf32(cam + 0x98), rdf32(cam + 0x94) - rdf32(cam + 0xA0));
    int x = st[0].stickX, y = st[0].stickY;
    fpsStick(x, y, facingRad, cameraRad);
    st[0].stickX = s8(x), st[0].stickY = s8(y);
}

}  // namespace recomp::mouselook
