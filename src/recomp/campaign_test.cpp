// Opt-in, read-only observation and virtual-controller bridge for campaign playtests.
#include "recomp/campaign_test.h"
#include <aurora/aurora.h>
#include "recomp/coop.h"
#include "recomp/online_events.h"
#include "recomp/frame.h"
#include "recomp/skip.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include <dolphin/pad.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace recomp::campaign {
namespace {
const std::filesystem::path& dir() {
    static const std::filesystem::path p = [] {
        const char* v = std::getenv("LM_CAMPAIGN_DIR");
        if (!v || !*v) v = std::getenv("LM_TEST_REPORT_DIR");
        return v && *v ? std::filesystem::path(v) : std::filesystem::path{};
    }();
    return p;
}
std::atomic<u32> until{0};
std::atomic<unsigned long long> input{0};
bool capturing = false;
unsigned id = 0;
bool active = false;
void finish() {
    std::ofstream(dir() / "done.txt") << id << '\n';
    active = false;
}
}

bool hold() {
    if (dir().empty() || !std::getenv("LM_CAMPAIGN_DIR")) return false;
    using Clock = std::chrono::steady_clock;
    static auto poll = Clock::time_point{};
    const auto now = Clock::now();
    if (now - poll > std::chrono::milliseconds(20)) {
        poll = now;
        unsigned next = 0, buttons = 0;
        int frames = 0, x = 0, y = 0, cx = 0, cy = 0;
        std::ifstream file(dir() / "command.txt");
        if (file >> next >> frames >> buttons >> x >> y >> cx >> cy; file && next > id && !active) {
            if (frames < 1 || frames > 1800 || (buttons & ~0xF1FFFu) ||
                ((buttons & 0xA0000u) == 0xA0000u) ||
                std::abs(x) > 127 || std::abs(y) > 127 || std::abs(cx) > 127 || std::abs(cy) > 127) {
                log("[campaign] invalid controller command %u", next);
            } else {
                id = next;
                const auto byte = [](int v) { return static_cast<unsigned long long>(u8(v)); };
                input = (buttons & 0x1FFFu) | (byte(x) << 16) | (byte(y) << 24) | (byte(cx) << 32) | (byte(cy) << 40) | ((buttons & 0x10000u) ? 1ull << 63 : 0) | ((buttons & 0x20000u) ? 1ull << 62 : 0) | ((buttons & 0x40000u) ? 1ull << 61 : 0) | ((buttons & 0x80000u) ? 1ull << 60 : 0);
                capturing = false;
                std::error_code ec;
                std::filesystem::remove(dir() / "latest.ppm", ec);
                until = frame::guestFrames() + u32(frames);
                active = true;
                log("[campaign] command %u: %d frames, pad %04X %d,%d %d,%d", id, frames, buttons, x, y, cx, cy);
            }
        }
    }
    if (active && frame::guestFrames() >= until.load()) {
        if (!(input.load() & (1ull << 61))) {
            finish();
        } else if (!capturing) {
            const auto path = (dir() / "latest.ppm").string();
            aurora_request_screenshot(path.c_str());
            capturing = true;
        } else {
            std::ifstream shot(dir() / "latest.ppm", std::ios::binary);
            std::string magic;
            unsigned w=0, h=0, max=0;
            if (shot >> magic >> w >> h >> max; shot && magic == "P6" && max == 255 && w && h && w <= 16384 && h <= 16384) {
                shot.get();
                const auto header = shot.tellg();
                std::error_code ec;
                const auto bytes = std::filesystem::file_size(dir() / "latest.ppm", ec);
                if (!ec && bytes >= static_cast<unsigned long long>(header) + 3ull*w*h) finish();
            }
        }
    }
    skip::setTestFastForward(active && frame::guestFrames() < until.load() && (input.load() >> 63));
    return false;
}

void pad(PADStatus* pads) {
    if (dir().empty() || !std::getenv("LM_CAMPAIGN_DIR")) return;
    // Explicit test steps can exercise real SDL keyboard/mouse bindings instead of a virtual pad.
    if (frame::guestFrames() < until.load() && (input.load() & (1ull << 62))) return;
    for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
        pads[i] = {};
        pads[i].err = i == 0 ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
    }
    const auto p = input.load();
    // A deliberate hold survives observations, so ghost capture does not release R between steps.
    if (frame::guestFrames() >= until.load() && !(p & (1ull << 60))) return;
    pads[0].button = u16(p);
    pads[0].stickX = s8(p >> 16);
    pads[0].stickY = s8(p >> 24);
    pads[0].substickX = s8(p >> 32);
    pads[0].substickY = s8(p >> 40);
    pads[0].triggerLeft = (u16(p) & PAD_TRIGGER_L) ? 255 : 0;
    pads[0].triggerRight = (u16(p) & PAD_TRIGGER_R) ? 255 : 0;
}

// The 2-player mode's player that view 0's record (lbl_803A3CA0 +0, what camera 0 follows) is
// nearest to; -1 outside the 2-player mode.
int viewPlayer() {
    int best = -1;
    float bestD = 1e30f;
    for (int i = 0; i < 2; i++) {
        const u32 p = coop::cachedPlayer(i);
        if (!p) continue;
        float d = 0;
        for (int k = 0; k < 3; k++) {
            const float e = rdf32(VA(0x803A3CA0) + 4 * k) - rdf32(p + 0x44 + 4 * k);
            d += e * e;
        }
        if (d < bestD) bestD = d, best = i;
    }
    return best;
}

void snapshot() {
    if (dir().empty()) return;
    const u32 f = frame::guestFrames();
    // LM_RAM_SNAP=<frame>,<frame>,...: guest MEM1 (24 MB, offset = address - 0x80000000) to
    // ram_<frame>.bin in the report folder, e.g. to find a menu's state by comparing snapshots.
    if (const char* v = std::getenv("LM_RAM_SNAP")) {
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const unsigned long want = std::strtoul(p, &end, 10);
            if (end == p) break;
            if (want == f) std::ofstream(dir() / ("ram_" + std::to_string(f) + ".bin"), std::ios::binary)
                               .write(reinterpret_cast<const char*>(g_mem), kMem1Size);
            p = *end == ',' ? end + 1 : end;
        }
    }
    const bool reporting = std::getenv("LM_TEST_REPORT_DIR") != nullptr;
    if (reporting && f % 15) return;
    const u32 luigi = rd32(VA(0x803D48A0));
    const bool valid = luigi >= 0x80000000 && luigi + 0x1400 < 0x81800000 && rd32(luigi + 0x3C) == 0;
    std::ostringstream s;
    s << "{\"frame\":" << f << ",\"host_memory\":" << reinterpret_cast<unsigned long long>(g_mem)
      << ",\"scene\":" << rd32(VA(0x804D80A0)) << ",\"map\":" << rd32(VA(0x804D80C8))
      << ",\"room\":" << (rd32(VA(0x803A3CA0) + 12) >> 24)
      << ",\"event\":" << rd32(VA(0x803D2D40) + 12) << ",\"transition\":" << rd32(VA(0x804D8088))
      << ",\"event_number\":" << rd16(VA(0x803D2D40))
      << ",\"luigi\":" << (valid ? luigi : 0) << ",\"position\":[";
    for (int i = 0; i < 3; ++i) s << (i ? "," : "") << (valid ? rdf32(luigi + 0x44 + 4*i) : 0);
    s << "],\"hp\":" << (valid ? s16(rd16(luigi + 0xFC)) : -1)
      << ",\"flags\":" << (valid ? rd32(luigi + 0x40) : 0)
      << ",\"equipment\":" << (valid ? rd16(luigi + 0x792) : 0)
      << ",\"archive\":" << (valid ? (rd32(luigi + 0xB4) & 0xFF) : -1)
      << ",\"mode\":" << rd32(VA(0x804D80C4))
      << ",\"views\":" << rd32(VA(0x804CFF90)) << ",\"view_player\":" << viewPlayer();
    if (coop::cachedPlayer(1)) {  // 2-player mode: both players' positions and actor flags
        s << ",\"players\":[";
        for (int i = 0; i < 2; i++) {
            const u32 p = coop::cachedPlayer(i);
            s << (i ? ",[" : "[");
            for (int k = 0; k < 3; k++) s << (k ? "," : "") << (p ? rdf32(p + 0x44 + 4 * k) : 0);
            s << "]";
        }
        s << "],\"player_flags\":[" << rd32(coop::cachedPlayer(0) + 0x40) << "," << rd32(coop::cachedPlayer(1) + 0x40) << "]"
          << ",\"player_rooms\":[" << (rd32(coop::cachedPlayer(0) + 0xB4) & 255) << "," << (rd32(coop::cachedPlayer(1) + 0xB4) & 255) << "]"
          << ",\"player_aims\":[" << rdf32(coop::cachedPlayer(0) + 0x8AC) << "," << rdf32(coop::cachedPlayer(1) + 0x8AC) << "]"
          << ",\"player_yaws\":[" << rd16(coop::cachedPlayer(0) + 0x88) << "," << rd16(coop::cachedPlayer(1) + 0x88) << "]";
    }
    online::EventProgress event;
    online::events::fill(event);
    s << ",\"online_event\":" << event.serial << ",\"online_event_phase\":" << unsigned(event.phase)
      << ",\"online_choice\":" << event.choice;
    s << ",\"function_total\":" << (g_module ? g_module->funcCount : 0) << "}\n";
    std::ofstream(dir() / "status.json") << s.str();
    if (reporting) {
        std::ofstream(dir() / "observations.jsonl", std::ios::app) << s.str();
        flushTestCoverage();
    }
}
}
