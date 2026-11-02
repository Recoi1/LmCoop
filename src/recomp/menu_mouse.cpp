// Mouse in the game's menus (menu_mouse.h). So far the file select (scene 3): the menu object
// [lbl_804D8404] holds the selected item at +1 (0-2 File 1-3, 3 Options), found by comparing
// RAM snapshots (LM_RAM_SNAP) with the cursor on each item.
#include "recomp/menu_mouse.h"

#include "recomp/guest_util.h"
#include "recomp/runtime.h"
#include "recomp/skip.h"
#include "recomp/version.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp::menumouse {
namespace {

#define kCurScene VA(0x804D80A0)
#define kFileSelectMenu VA(0x804D8404)  // lbl_804D8404: the file select's menu object
constexpr u32 kSceneFileSelect = 3;

Steer sSteer;
u32 sCalls = 0;

struct Pointer {
    bool valid = false, left = false, right = false;  // left/right: pressed (held)
    Point game{};                                       // in the 640x480 picture
    bool inside = false;
};

// LM_TEST_MOUSE=<frame>:<x>,<y>[,L|R];... (testing): a pointer in picture coordinates from that
// guest frame on, clicking on that frame only; the real mouse is then ignored.
bool scripted(Pointer& p) {
    static const char* sSpec = std::getenv("LM_TEST_MOUSE");
    if (!sSpec) return false;
    const u32 now = skip::guestFrames();
    for (const char* s = sSpec; s && *s; s = std::strchr(s, ';') ? std::strchr(s, ';') + 1 : nullptr) {
        unsigned f = 0;
        float x = 0, y = 0;
        char b = 0;
        const int n = std::sscanf(s, "%u:%f,%f,%c", &f, &x, &y, &b);
        if (n < 3 || f > now) continue;
        p = {true, n == 4 && b == 'L' && f == now, n == 4 && b == 'R' && f == now, {x, y}, true};
    }
    return true;
}

Pointer real() {
    Pointer p;
    SDL_Window* w = aurora_get_window();
    if (!w || !(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)) return p;
    float mx = 0, my = 0;
    const SDL_MouseButtonFlags b = SDL_GetMouseState(&mx, &my);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    p.valid = true;
    p.left = b & SDL_BUTTON_LMASK;
    p.right = b & SDL_BUTTON_RMASK;
    p.inside = toGame(mx, my, float(ww), float(wh), p.game);
    return p;
}

}  // namespace

void pad(PADStatus* st) {
    sCalls++;
    if (rd32(kCurScene) != kSceneFileSelect) {
        sSteer = Steer{};
        return;
    }
    const u32 menu = rd32(kFileSelectMenu);
    if (!gu::inMem1(menu)) return;
    const int current = rd8(menu + 1);
    if (current > 3) return;
    Pointer p;
    static const bool sTestInput = std::getenv("LM_TEST_INPUT_ONLY") != nullptr;  // tests: never the real mouse
    if (!scripted(p) && !sTestInput) p = real();
    if (!p.valid) return;
    const int hovered = p.inside ? fileSelectItem(p.game) : -1;
    static Point sLast{-1, -1};
    static bool sLastL = false, sLastR = false;
    if (p.game.x != sLast.x || p.game.y != sLast.y) sSteer.mouseMoved(hovered), sLast = p.game;
    const u16 add = sSteer.buttons(current, p.left && !sLastL, p.right && !sLastR, sCalls, hovered);
    sLastL = p.left, sLastR = p.right;
    if (add) {
        st[0].button |= add;
        st[0].err = PAD_ERR_NONE;
    }
}

}  // namespace recomp::menumouse
