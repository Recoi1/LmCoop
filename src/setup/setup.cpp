// First-run setup screen and pipeline (docs/SETUP.md).
#include "setup/setup.h"

#include "port/crash_handler.h"
#include "port/dol.h"
#include "recomp/precache.h"
#include "recomp/ui_scale.h"
#include "setup/disc.h"
#include "setup/hm_overlay.h"
#include "setup/logo.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/menu.h"
#include "recomp/online.h"
#include "setup/module_builder.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace setup {
namespace {

using port::GameVersion;

constexpr const char* kOverlayFormat = "hidden-mansion-overlay 3";  // 2: + usa/ (reverse), 3: lab menu choice (event08)

struct Slot {
    std::string path;     // UTF-8
    bool valid = false;
    GameVersion version = GameVersion::USA;
    std::string message;  // validation result
};

// State shared with the worker thread.
struct Job {
    std::mutex m;
    std::string stage;
    float fraction = 0.0f;
    bool done = false, ok = false;
    std::string error;
    std::atomic<bool> cancel{false};
    std::thread thread;
    ~Job() {
        cancel = true;
        if (thread.joinable()) thread.join();
    }
};

// Results of the file dialogs (SDL may call back on another thread).
std::mutex sPickM;
std::optional<std::string> sPicked[2];
std::string sDialogError;  // the file dialog could not open (Linux: no xdg-desktop-portal/zenity)

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(port::longPath(p), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string overlayKey(const Disc& us, const Disc& pal) {
    const std::string text = std::string(kOverlayFormat) + "\n" + us.dolSha1() + "\n" + pal.dolSha1();
    const auto d = port::dol::sha1(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return hex(d.data(), d.size());
}

bool overlayUpToDate(const Disc& us, const Disc& pal) {
    return readText(port::hiddenMansionOverlayDir() / "overlay.key") == overlayKey(us, pal);
}

void validate(Slot& s, GameVersion expected) {
    s.valid = false;
    s.message.clear();
    if (s.path.empty()) return;
    GameVersion v{};
    std::string err;
    if (!probeDisc(s.path, v, err)) {
        s.message = err;
        return;
    }
    if (v != expected) {
        s.message = v == GameVersion::PAL ? "This is the European disc; LmCoop needs the USA disc (GLME01)."
                                          : "This is the USA disc; put it in the other slot.";
        return;
    }
    s.valid = true;
    s.message = v == GameVersion::PAL ? "Luigi's Mansion (Europe, GLMP01)" : "Luigi's Mansion (USA, GLME01)";
}

struct Plan {
    std::vector<GameVersion> modules;  // modules to (re)build
    bool overlay = false;               // PAL Hidden Mansion overlay to (re)build
    bool empty() const { return modules.empty() && !overlay; }
};

Plan makePlan(const Slot slots[2]) {
    Plan p;
    std::unique_ptr<Disc> discs[2];
    for (int i = 0; i < 2; i++) {
        if (!slots[i].valid) continue;
        discs[i] = std::make_unique<Disc>();
        std::string err;
        if (!discs[i]->open(slots[i].path, err)) {
            discs[i].reset();
            continue;
        }
        if (!moduleUpToDate(*discs[i])) p.modules.push_back(discs[i]->version());
    }
    if (discs[0] && discs[1] && !overlayUpToDate(*discs[0], *discs[1])) p.overlay = true;
    return p;
}

void startJob(Job& job, const Slot slots[2], const Plan& plan) {
    const std::string paths[2] = {slots[0].valid ? slots[0].path : "", slots[1].valid ? slots[1].path : ""};
    job.thread = std::thread([&job, paths, plan] {
        auto set = [&](const std::string& stage, float f) {
            std::lock_guard lk(job.m);
            job.stage = stage;
            job.fraction = f;
        };
        auto fail = [&](const std::string& e) {
            std::lock_guard lk(job.m);
            job.error = e;
            job.done = true;
        };
        const float steps = float(plan.modules.size() + (plan.overlay ? 1 : 0));
        float step = 0.0f;
        for (GameVersion v : plan.modules) {
            const int i = v == GameVersion::PAL ? 1 : 0;
            const char* name = v == GameVersion::PAL ? "Europe" : "USA";
            set(std::string("Reading the ") + name + " disc", step / steps);
            Disc disc;
            std::string err;
            if (!disc.open(paths[i], err)) return fail(err);
            Progress pr;
            pr.cancel = &job.cancel;
            pr.report = [&](const std::string& s, float f) { set(std::string(name) + ": " + s, (step + f) / steps); };
            if (!buildModule(disc, pr, err)) return fail(err);
            step += 1.0f;
        }
        if (plan.overlay) {
            set("Building the European Hidden Mansion data", step / steps);
            Disc us, pal;
            std::string err;
            if (!us.open(paths[0], err) || !pal.open(paths[1], err)) return fail(err);
            const auto dir = port::hiddenMansionOverlayDir();
            if (!buildHiddenMansionOverlay(us, pal, dir, err)) return fail(err);
            std::ofstream(port::longPath(dir / "overlay.key"), std::ios::binary) << overlayKey(us, pal);
        }
        std::lock_guard lk(job.m);
        job.stage = "Done";
        job.fraction = 1.0f;
        job.ok = true;
        job.done = true;
    });
}

void SDLCALL onPicked(void* userdata, const char* const* files, int) {
    const int slot = int(reinterpret_cast<intptr_t>(userdata));
    std::lock_guard lk(sPickM);
    if (!files) {  // error (cancel is an empty list)
        const char* e = SDL_GetError();
        sDialogError = std::string("The file dialog could not be opened") + (e && *e ? std::string(": ") + e : "") +
                       ". Type or paste the path of the disc image instead.";
        return;
    }
    if (!files[0]) return;  // cancelled
    sPicked[slot] = files[0];
}

void browse(int slot, const std::string& current) {
    static const SDL_DialogFileFilter filters[] = {
        {"GameCube disc images", "iso;gcm;rvz;gcz;ciso;wia;wbfs;nfs;tgc"},
        {"All files", "*"},
    };
    const size_t slash = current.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string() : current.substr(0, slash + 1);
    SDL_ShowOpenFileDialog(&onPicked, reinterpret_cast<void*>(intptr_t(slot)), aurora_get_window(), filters, 2,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

enum class Screen { Pick, Working, Shaders, Failed, Start, Multiplayer, Settings, Updates };

}  // namespace

bool run(const Options& opts, LaunchInfo& launch) {
    recomp::ui::install(recomp::ui::Context::Setup);
    port::update::startupCheck();
    port::loadInstall();
    auto& inst = port::install();
    Slot slots[2];
    slots[0].path = !opts.usDisc.empty() ? opts.usDisc : inst.usDisc;
    slots[1].path = !opts.palDisc.empty() ? opts.palDisc : inst.palDisc;
    validate(slots[0], GameVersion::USA);
    validate(slots[1], GameVersion::PAL);
    GameVersion base = inst.base;

    auto chooseBase = [&] {
        if (!slots[1].valid) base = GameVersion::USA;
        else if (!slots[0].valid) base = GameVersion::PAL;
    };
    chooseBase();

    // Nothing to ask: the configured discs are all readable and the base is available.
    bool showUi = opts.forceUi || opts.autoStart || !(slots[0].valid || slots[1].valid) ||
                  (!slots[0].path.empty() && !slots[0].valid) || (!slots[1].path.empty() && !slots[1].valid);
    Screen screen = Screen::Pick;
    Screen settingsReturn = Screen::Pick, updatesReturn = Screen::Pick;
    std::unique_ptr<Job> job;
    Plan plan;
    bool started = false;

    auto begin = [&] {
        inst.usDisc = slots[0].valid ? slots[0].path : "";
        inst.palDisc = slots[1].valid ? slots[1].path : "";
        inst.base = base;
        port::saveInstall();
        plan = makePlan(slots);
        job = std::make_unique<Job>();
        if (!plan.empty()) startJob(*job, slots, plan);
        else job->done = job->ok = true;
        screen = Screen::Working;
        started = true;
    };
    if (!showUi) begin();
    else if (opts.autoStart && (slots[0].valid || slots[1].valid)) begin();

    port::setPhase("setup");
    bool finished = false;
    int framesDrawn = 0;
    // LmCoop start menu, every launch: Singleplayer (online off for this session) or Multiplayer.
    // LM_START_CHOICE / --setup-auto pick without waiting (automated runs).
    auto choose = [&](int mode) {
        recomp::online::setSessionMode(mode);
        recomp::log(mode == 0 ? "[start] singleplayer" : mode == 1 ? "[start] multiplayer host" : "[start] multiplayer join");
        finished = true;
    };
    auto toStart = [&] {
        const std::string& c = opts.startChoice;
        if (c == "single" || (c.empty() && opts.autoStart)) return choose(0);
        if (c == "host") return choose(1);
        if (c == "join") return choose(2);
        screen = Screen::Start;
        if (c == "settings") settingsReturn = screen, screen = Screen::Settings;
        if (c == "updates") updatesReturn = screen, screen = Screen::Updates;
        recomp::log("[start] menu");
    };
    if (screen == Screen::Pick && showUi && !(slots[0].valid || slots[1].valid)) recomp::log("[setup] waiting for a disc");
    if (!started && opts.startChoice == "settings") screen = Screen::Settings;
    if (!started && opts.startChoice == "updates") screen = Screen::Updates;
    std::vector<std::string> dropped;  // the background precache is queued by the first frame (Aurora patch 0013)
    while (!finished) {
        port::heartbeat();
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_EXIT) return false;
            if (ev->type == AURORA_PAUSED) port::setPhase(nullptr);  // waits for the window: not a hang
            if (ev->type == AURORA_UNPAUSED) port::setPhase("setup");
            // Files dropped on the window (LmCoop screen): only while waiting for a disc.
            if (ev->type == AURORA_SDL_EVENT && ev->sdl.type == SDL_EVENT_DROP_FILE && ev->sdl.drop.data &&
                screen == Screen::Pick)
                dropped.emplace_back(ev->sdl.drop.data);
        }
        // The first valid USA disc of a drop wins; otherwise the first file's problem is shown.
        if (!dropped.empty()) {
            Slot first;
            for (const std::string& path : dropped) {
                Slot s;
                s.path = path;
                validate(s, GameVersion::USA);
                if (s.valid) {
                    first = s;
                    break;
                }
                if (first.path.empty()) first = s;
            }
            dropped.clear();
            slots[0] = first;
            chooseBase();
            if (slots[0].valid) begin();
        }
        {
            std::lock_guard lk(sPickM);
            for (int i = 0; i < 2; i++) {
                if (sPicked[i]) {
                    slots[i].path = *sPicked[i];
                    sPicked[i].reset();
                    validate(slots[i], i ? GameVersion::PAL : GameVersion::USA);
                    if (slots[i].valid && !slots[1 - i].valid) base = slots[i].version;
                    chooseBase();
                    if (i == 0 && slots[0].valid && screen == Screen::Pick) begin();  // LmCoop: no extra click
                }
            }
        }
        if (job && screen == Screen::Working && framesDrawn > 0) {
            std::lock_guard lk(job->m);
            if (job->done) {
                if (!job->ok) screen = Screen::Failed;
                // Setup was visible (first run, disc change, rebuild after an update): also finish
                // compiling the shaders here, so the first game session runs as smoothly as later
                // ones. Later launches compile whatever is missing in the background (precache.h).
                else if ((showUi || !plan.empty()) && !recomp::precache::progress().finished()) screen = Screen::Shaders;
                else toStart();
            }
        }
        if (screen == Screen::Shaders && recomp::precache::progress().finished()) toStart();
        if (finished) break;
        if (!aurora_begin_frame()) continue;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::Begin("##setup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse);
        // Menu size (ui_scale.cpp; setup's Auto: 600 lines = 100%), font rasterized at that size.
        const float scale = recomp::ui::scale();
        // LmCoop look: black, the logo centered in the upper part, one line of text under it.
        const bool panel = screen == Screen::Settings || screen == Screen::Updates;
        if (!panel) {
            ImGui::Dummy(ImVec2(0, ImGui::GetWindowHeight() * 0.12f));
            drawLogo();
            ImGui::Dummy(ImVec2(0, 24 * scale));
        }
        const ImVec4 red(1.0f, 0.45f, 0.4f, 1), grey(0.6f, 0.6f, 0.6f, 1);
        auto centeredButton = [&](const char* label) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(label).x) * 0.5f - 4 * scale);
            return recomp::menu::SmallButton(label);
        };
        if (screen == Screen::Pick) {
            if (slots[0].path.empty()) {
                centeredText("No disc detected, drag & drop Luigi's Mansion (USA) onto this screen...");
            } else if (!slots[0].valid) {
                centeredText(slots[0].message.c_str(), red);
                centeredText("Drag & drop Luigi's Mansion (USA) onto this screen...");
            } else {
                centeredText(slots[0].message.c_str(), ImVec4(0.4f, 0.9f, 0.4f, 1));
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (centeredButton("Continue")) begin();
            }
            {
                std::lock_guard lk(sPickM);
                if (!sDialogError.empty()) centeredText(sDialogError.c_str(), red);
            }
            if (const auto& loc = port::dataLocation(); !loc.portable && !loc.problem.empty())
                centeredText(loc.problem.c_str(), ImVec4(1.0f, 0.75f, 0.4f, 1));
            ImGui::Dummy(ImVec2(0, 16 * scale));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, grey);
            if (centeredButton("Browse...")) browse(0, slots[0].path);
            if (centeredButton("Settings")) settingsReturn = screen, screen = Screen::Settings;
            if (centeredButton("Quit")) {
                ImGui::PopStyleColor(2);
                ImGui::End();
                ImGui::PopStyleColor(2);
                aurora_end_frame();
                return false;
            }
            ImGui::PopStyleColor(2);
        } else if (screen == Screen::Start) {
            const ImVec2 big(ImGui::GetWindowWidth() * 0.3f, std::min(44 * scale, ImGui::GetWindowHeight() * 0.065f));
            auto bigButton = [&](const char* label) {
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - big.x) * 0.5f);
                const bool hit = ImGui::Button(label, big);
                ImGui::Dummy(ImVec2(0, 6 * scale));
                return hit;
            };
            if (framesDrawn == 0 || ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (bigButton("Singleplayer")) choose(0);
            if (bigButton("Multiplayer")) screen = Screen::Multiplayer;
            if (bigButton("Settings")) {
                settingsReturn = Screen::Start;
                screen = Screen::Settings;
                recomp::log("[start] settings");
            }
            if (bigButton("Quit")) {
                ImGui::End();
                ImGui::PopStyleColor(2);
                aurora_end_frame();
                return false;
            }
        } else if (screen == Screen::Multiplayer) {
            const float w = ImGui::GetWindowWidth() * 0.5f;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
            ImGui::BeginGroup();
            ImGui::PushItemWidth(w * 0.5f);
            centeredText("Host: your friends join you, you play as Luigi. Join: you play as Mario on a friend's game.", grey);
            ImGui::Dummy(ImVec2(0, 6 * scale));
            recomp::online::drawHostJoin(false);
            ImGui::PopItemWidth();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, 12 * scale));
            const ImVec2 half(ImGui::GetWindowWidth() * 0.14f, 40 * scale);
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - half.x - 6 * scale);
            ImGui::BeginDisabled(!recomp::online::readyToStart());
            if (ImGui::Button("Start", half)) {
                port::saveSettings();  // the host/join choice and address are remembered
                choose(recomp::online::configuredMode());
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 12 * scale);
            if (ImGui::Button("Back", half)) screen = Screen::Start;
        } else if (panel) {
            ImGui::Dummy(ImVec2(0, 12 * scale));
            centeredText(screen == Screen::Settings ? "Settings" : "Updates");
            ImGui::Dummy(ImVec2(0, 12 * scale));
            const float width = ImGui::GetWindowWidth() * 0.9f;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            ImGui::BeginChild("startup-panel", ImVec2(width, std::max(80.0f, ImGui::GetWindowHeight() - 160 * scale)));
            if (screen == Screen::Settings) recomp::menu::drawStartupSettings();
            else recomp::menu::drawStartupUpdates();
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0, 10 * scale));
            if (centeredButton("Back")) screen = screen == Screen::Settings ? settingsReturn : updatesReturn;
        } else {
            std::string stage, error;
            float f = 0.0f;
            {
                std::lock_guard lk(job->m);
                stage = job->stage;
                f = job->fraction;
                error = job->error;
            }
            const float barW = ImGui::GetWindowWidth() * 0.5f;
            auto bar = [&](float fraction) {
                ImGui::Dummy(ImVec2(0, 8 * scale));
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barW) * 0.5f);
                ImGui::ProgressBar(fraction, ImVec2(barW, 6 * scale), "");
            };
            if (screen == Screen::Shaders) {
                const auto pr = recomp::precache::progress();
                char line[96];
                std::snprintf(line, sizeof(line), "Compiling shaders for your graphics card... %u / %u", pr.done, pr.total);
                centeredText(line);
                bar(pr.fraction());
                // (No "play now": the game waits for its shaders anyway, precache.h holdGame.)
            } else if (screen == Screen::Working) {
                centeredText(("Building LmCoop from your disc... " + stage).c_str());
                bar(f);
                ImGui::Dummy(ImVec2(0, 16 * scale));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, grey);
                if (centeredButton("Cancel")) {
                    job.reset();  // joins the worker after it notices
                    screen = Screen::Pick;
                }
                ImGui::PopStyleColor(2);
            } else {
                centeredText(("Setup failed: " + error).c_str(), red);
                centeredText("Drag & drop the disc again, or go back.");
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (centeredButton("Back")) {
                    job.reset();
                    screen = Screen::Pick;
                }
            }
        }
        // Keep version and update status visible before gameplay, including first-run setup.
        const std::string version = std::string("v") + port::update::buildVersion();
        const ImVec2 verSize = ImGui::CalcTextSize(version.c_str());
        const float right = ImGui::GetWindowWidth() - 16 * scale;
        const float bottom = ImGui::GetWindowHeight() - 12 * scale;
        ImGui::SetCursorPos(ImVec2(right - verSize.x, bottom - verSize.y));
        ImGui::TextDisabled("%s", version.c_str());
        const auto update = port::update::state();
        using Phase = port::update::Phase;
        std::string status = "Check for updates";
        switch (update.phase) {
        case Phase::Checking: status = "Checking for updates..."; break;
        case Phase::UpToDate: status = "Up to date"; break;
        case Phase::CheckFailed: status = "Couldn't check for updates"; break;
        case Phase::Available: status = "Update available: " + update.info.tag; break;
        case Phase::Downloading: status = "Downloading update..."; break;
        case Phase::Verifying: status = "Verifying update..."; break;
        case Phase::Extracting: status = "Preparing update..."; break;
        case Phase::Ready: status = "Update ready to install"; break;
        case Phase::Installing: status = "Installing update..."; break;
        case Phase::Failed: status = "Update failed - view details"; break;
        default: break;
        }
        const float statusHeight = ImGui::GetFrameHeight();
        ImGui::SetCursorPos(ImVec2(std::max(8 * scale, right - ImGui::CalcTextSize(status.c_str()).x - 8 * scale),
                                  bottom - verSize.y - statusHeight - 6 * scale));
        ImGui::BeginDisabled(screen == Screen::Working || screen == Screen::Shaders || screen == Screen::Failed);
        if (ImGui::SmallButton(status.c_str()) && screen != Screen::Updates) {
            updatesReturn = screen;
            screen = Screen::Updates;
            recomp::log("[start] updates");
        }
        ImGui::EndDisabled();
        ImGui::End();
        ImGui::PopStyleColor(2);
        aurora_end_frame();
        framesDrawn++;
        // While the game is prepared and the shaders compile (from the first frame on): frames below
        // the refresh rate, as in the game's shader hold (issue #33; precache.h holdFramePause).
        if (screen != Screen::Pick) recomp::precache::holdFramePause(0.0);
    }
    job.reset();

    // Ready: report what is installed and what to launch.
    inst.usReady = slots[0].valid;
    inst.palReady = slots[1].valid;
    inst.hmOverlayReady = slots[0].valid && slots[1].valid &&
                          std::filesystem::exists(port::longPath(port::hiddenMansionOverlayDir() / "manifest.txt")) &&
                          std::filesystem::exists(port::longPath(port::hiddenMansionOverlayDir() / "usa" / "manifest.txt"));
    inst.running = base;
    launch.version = base;
    launch.disc = slots[base == GameVersion::PAL ? 1 : 0].path;
    launch.module = modulePath(base);
    return true;
}

}  // namespace setup
