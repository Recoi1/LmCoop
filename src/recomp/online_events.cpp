// The host chooses cinematic event starts. Each PC runs the native script/camera locally;
// dialogue may be read independently, while choices and leaving the scene wait for both PCs.
#include "recomp/online_events.h"
#include "recomp/online.h"
#include "recomp/coop.h"
#include "recomp/guest_util.h"
#include "recomp/rarc.h"
#include "recomp/skip.h"
#include <dolphin/pad.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace recomp::online::events {
namespace {
constexpr u32 kTriggers = 0x804D8370, kCount = 0x804D8374, kCurrent = 0x804D8378;
constexpr u32 kScript = 0x803D2D40;
SceneEvent sEvent{};
EventProgress sProgress{};
u32 sSerial = 0, sTrigger = 0, sScriptBase = 0;
bool sTeleported = false, sDeferredScene = false, sOriginalTimed = false;
std::atomic<bool> sBusy{false};
using Clock = std::chrono::steady_clock;
Clock::time_point sSent;

// LM_ONLINE_NOEVENTS=1 (test aid): no shared cutscenes, each PC plays its own events
bool eventsOn() {
    static const bool off = std::getenv("LM_ONLINE_NOEVENTS") != nullptr;
    return !off && wanted();
}

bool cinematic(u16 number) {
    static std::array<int, 110> cache{};
    if (number >= cache.size()) return false;
    if (!cache[number]) {
        char path[48], member[40];
        std::snprintf(path, sizeof(path), "/Event/event%02u.szp", number);
        std::snprintf(member, sizeof(member), "text/event%02u.txt", number);
        rarc::Archive archive;
        cache[number] = -1;
        if (archive.parse(rarc::decompress(rarc::readDisc(path)))) {
            if (const auto* file = archive.find(member)) {
                const std::string_view script(reinterpret_cast<const char*>(file->data.data()), file->data.size());
                for (const char* tag : {"<MENUOFF>", "<LUIGISTOP>", "<WARP>"})
                    if (script.find(tag) != script.npos) cache[number] = 1;
            }
        }
    }
    return cache[number] > 0;
}
bool cinematicTrigger(u32 trigger) { return (rd32(trigger) & 0x300) || cinematic(rd16(trigger + 0x38)); }
int indexOf(u32 trigger) {
    const u32 base = rd32(kTriggers), count = rd16(kCount);
    if (!gu::inMem1(base) || count > 2048 || trigger < base || (trigger - base) % 0x58) return -1;
    const u32 i = (trigger - base) / 0x58;
    return i < count ? int(i) : -1;
}
const EventProgress* peer() {
    const auto* p = remote();
    return p && p->event.serial == sProgress.serial ? &p->event : nullptr;
}
bool active() { return sProgress.serial && sProgress.phase != EventPhase::Finished; }
void teleport(Ctx& c) {
    for (int i = 0; i < 2; i++) {
        if (const u32 p = coop::player(c, i)) {
            for (int k = 0; k < 3; k++) gu::wrf(p + 0x44 + k * 4, sEvent.game.pos[k]);
            if (i == 1) {  // beside the host's player, not inside it
                gu::wrf(p + 0x44, sEvent.game.pos[0] + std::cos(sEvent.game.rotY) * 70.0f);
                gu::wrf(p + 0x4C, sEvent.game.pos[2] - std::sin(sEvent.game.rotY) * 70.0f);
            }
            gu::wrf(p + 0xF48, std::sin(sEvent.game.rotY));
            gu::wrf(p + 0xF50, std::cos(sEvent.game.rotY));
        }
    }
    std::memcpy(hostp(0x803D3399), sEvent.game.flags, sizeof(sEvent.game.flags));
    log("[online-event] both players moved to map %u room %u for event %u serial %u",
        sEvent.game.map, sEvent.game.room, sEvent.number, sEvent.serial);
}
}  // namespace

void reset() {
    sEvent = {}, sProgress = {};
    sTrigger = sScriptBase = 0;
    sTeleported = sDeferredScene = false;
    sBusy = false;
}
bool busy() { return sBusy.load(); }
u32 eventWorld() { return sProgress.serial ? sEvent.game.world : 0; }
bool holdPlayers() {
    if (!eventsOn() || !active() || (guest() && !sTeleported)) return false;
    const u32 p = coop::cachedPlayer(0);
    return sProgress.phase <= EventPhase::Prepared || sProgress.phase == EventPhase::Ending ||
           sDeferredScene || rd32(0x804D8088) != 0 || (p && (rd32(p + 0x40) & 0x10));
}
bool watching() {
    return busy() && holdPlayers();
}
void mapSetup() {
    sDeferredScene = false;
    sTrigger = sScriptBase = 0, sTeleported = false;
    sBusy = false;
    if (!guest()) sEvent = {}, sProgress = {};
}
void fill(EventProgress& progress) { progress = sProgress; }
const GameState* hostGame() { return !guest() && active() ? &sEvent.game : nullptr; }
void receive(const SceneEvent& event) {
    // Not before this PC has joined the host's game: the host repeats the packet while the scene runs.
    if (!guest() || world() != event.game.world || (sProgress.serial && int32_t(event.serial - sProgress.serial) <= 0)) return;
    sEvent = event;
    sProgress = {}, sProgress.serial = event.serial;
    sTrigger = sScriptBase = 0, sTeleported = false;
    sBusy = false;
    log("[online-event] received event %u serial %u map %u room %u", event.number, event.serial, event.game.map, event.game.room);
}

bool condition(Ctx& c, u32 trigger, bool eligible) {
    static u32 trace = 0;
    if (guest() && trigger == sTrigger && trace != sProgress.serial) {
        trace = sProgress.serial;
        log("[online-event] guest trigger check serial %u active %u moved %u phase %u cinematic %u index %d",
            trace, active(), sTeleported, unsigned(sProgress.phase), cinematic(rd16(trigger + 0x38)), indexOf(trigger));
    }
    if (!eventsOn() || indexOf(trigger) < 0 || !cinematicTrigger(trigger)) return eligible;
    if (guest()) return active() && sTeleported && trigger == sTrigger && sProgress.phase == EventPhase::None;
    if (active()) {
        if (trigger != sTrigger) return false;
        const auto* p = peer();
        return !connected() || (p && p->phase >= EventPhase::Prepared);
    }
    if (!eligible || gu::call(c, GU_SYM("fn_8002BBC4"), {trigger, 0}) != 1) return false;
    GameState game{};
    if (!captureGame(c, game)) return eligible;
    sEvent = {game, ++sSerial, rd32(trigger + 0x54), u16(indexOf(trigger)), u16(rd16(trigger + 0x38))};
    sProgress = {}, sProgress.serial = sSerial, sProgress.phase = EventPhase::Prepared;
    sTrigger = trigger, sScriptBase = 0;
    sOriginalTimed = (rd32(trigger) & 0x80) != 0;
    wr32(trigger, rd32(trigger) & ~0x80u);  // suspend the optional time window while its partner loads
    sSent = {};
    teleport(c);
    log("[online-event] host prepared event %u serial %u map %u room %u", sEvent.number, sSerial, game.map, game.room);
    return !connected();
}

void frame(Ctx& c) {
    if (!eventsOn()) return;
    if (!guest()) {
        const auto now = Clock::now();
        if (active() && connected() && now - sSent > std::chrono::milliseconds(150)) sendEvent(sEvent), sSent = now;
        if (active() && rd32(0x804D80A0) != rd32(0x803A3AE4)) {
            sDeferredScene = true;
            wr32(0x804D80A0, 2);
        }
        if (sDeferredScene && (!connected() || (peer() && peer()->phase == EventPhase::Finished))) {
            wr32(0x804D80A0, 0);
            sDeferredScene = false;
            sProgress.phase = EventPhase::Finished;
            log("[online-event] host finished event %u serial %u (both ready for map change)", sEvent.number, sEvent.serial);
        }
        return;
    }
    if (active() && world() == sEvent.game.world && !joiningMap() && rd32(0x803A3AE0) == 2 && rd32(0x804D80A0) == 2) {
        if (!sTrigger) {
            const u32 base = rd32(kTriggers), count = rd16(kCount);
            if (gu::inMem1(base) && count <= 2048) {
                for (u32 i = 0; i < count; i++) {
                    const u32 t = base + 0x58 * i;
                    if (rd16(t + 0x38) == sEvent.number && (i == sEvent.trigger || rd32(t + 0x54) == sEvent.parameter)) {
                        sTrigger = t;
                        if (i == sEvent.trigger) break;
                    }
                }
            }
        }
        if (sTrigger && !sTeleported) {
            teleport(c);
            wr32(sTrigger, rd32(sTrigger) & ~0xC9u);
            wr32(sTrigger + 0x54, sEvent.parameter);
            sTeleported = true;
            sBusy = true;
        }
    }
    // A guest's WARP is completed by the host's next map snapshot, after both watched the scene.
    if ((active() || sDeferredScene) && rd32(0x804D80A0) != rd32(0x803A3AE4)) sDeferredScene = true;
    if (sDeferredScene) wr32(0x804D80A0, 2);
}

void pad(PADStatus* pads, const PADStatus& mine) {
    if (!watching()) return;
    pads[0] = {};
    pads[0].err = pads[1].err = PAD_ERR_NONE;
    // Let both people read dialogue. Choices come from the host through the script getter.
    if (rd32(kScript + 0x1C) != 7) pads[0].button = mine.button & (PAD_BUTTON_A | PAD_BUTTON_B);
    pads[1] = {};
    pads[1].err = PAD_ERR_NONE;
}

HLE(fn_8002B7BC) {
    const u32 trigger = c.r[3];
    if (guest() && trigger == sTrigger && sProgress.phase == EventPhase::Prepared &&
        (!peer() || peer()->phase < EventPhase::Playing)) return;
    callOriginal(c, "fn_8002B7BC");
}
HLE(fn_80065DDC) {
    callOriginal(c, "fn_80065DDC");
    if (!eventsOn() || rd32(kCurrent) != sTrigger || !active()) return;
    sScriptBase = rd32(kScript + 0xC);
    sProgress.phase = guest() ? EventPhase::Prepared : EventPhase::Playing;
    log("[online-event] %s event %u serial %u (script ready)", guest() ? "guest prepared" : "host playing", sEvent.number, sEvent.serial);
}
HLE(fn_80065F7C) {
    if (guest() && active() && sProgress.phase == EventPhase::Prepared) sProgress.phase = EventPhase::Playing;
    callOriginal(c, "fn_80065F7C");
}
HLE(fn_8002BA54) {
    if (!eventsOn() || c.r[3] != sTrigger || !active()) return callOriginal(c, "fn_8002BA54");
    if (!guest()) {
        sProgress.phase = EventPhase::Ending;
        if (connected() && (!peer() || peer()->phase != EventPhase::Finished)) return;
    }
    callOriginal(c, "fn_8002BA54");
    if (sOriginalTimed && !guest()) wr32(sTrigger, rd32(sTrigger) | 0x80u);
    sProgress.phase = EventPhase::Finished;
    sBusy = false;
    log("[online-event] %s finished event %u serial %u", guest() ? "guest" : "host", sEvent.number, sEvent.serial);
}
HLE(fn_800441C4) {
    callOriginal(c, "fn_800441C4");
    if (!eventsOn() || !active() || !sScriptBase || rd32(kScript + 0x1C) != 7) return;
    const u32 offset = rd32(kScript + 0xC) - sScriptBase;
    if (offset > 0x100000) return;
    if (guest()) {
        c.r[3] = ~0u;
        if (const auto* p = peer(); p && p->choice > sProgress.choice && p->choiceOffset == offset) {
            sProgress.choice = p->choice, sProgress.choiceOffset = offset, sProgress.selection = p->selection;
            c.r[3] = p->selection;
            log("[online-event] guest accepted choice %u = %u", p->choice, p->selection);
        }
    } else {
        if (sProgress.choiceOffset != offset && s32(c.r[3]) >= 0 && c.r[3] <= 31) {
            ++sProgress.choice, sProgress.choiceOffset = offset, sProgress.selection = u8(c.r[3]);
        }
        if (sProgress.choiceOffset == offset) {
            c.r[3] = !connected() || (peer() && peer()->choice == sProgress.choice) ? sProgress.selection : ~0u;
        }
    }
}
}  // namespace recomp::online::events
