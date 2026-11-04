// Start-up recovery and the run log (recovery.h; docs/SETUP.md "When the game does not start").
#include "port/recovery.h"

#include "port/crash_report.h"
#include "port/install.h"
#include "port/settings.h"
#include "port/update.h"

#include <SDL3/SDL_messagebox.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <string>
#include <vector>

namespace port::recovery {
namespace {

namespace fs = std::filesystem;

// The run that is starting (written at once) and the one before it (read at start).
struct RunState {
    std::string state;  // starting | running | exited | crashed
    std::string phase;  // startup | play
    std::string reason;
    std::string version;
    std::string build;  // the git commit of the build that wrote this
    int failedStarts = 0;  // failed starts in a row before this run
    int crashesInRow = 0;  // crashed runs in a row before this run
};

std::mutex sHangMutex;  // noteHang (watchdog thread) vs the main thread's state changes
RunState sCur;
bool sStarted = false;
char sFatal[600] = {};
// Prepared at start so that the crash path only formats into fixed buffers.
char sStatePath[1024] = {};
std::FILE* sLog = nullptr;

fs::path statePath() { return configDir() / "last_run_state.ini"; }

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string oneLine(std::string s) {
    for (char& ch : s)
        if (ch == '\n' || ch == '\r') ch = ' ';
    if (s.size() > 500) s.resize(500);
    return s;
}

RunState readState() {
    RunState r;
    std::ifstream in(statePath());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "state") r.state = value;
        else if (key == "phase") r.phase = value;
        else if (key == "reason") r.reason = value;
        else if (key == "version") r.version = value;
        else if (key == "build") r.build = value;
        else if (key == "failed_starts") r.failedStarts = std::atoi(value.c_str());
        else if (key == "crashes_in_row") r.crashesInRow = std::atoi(value.c_str());
    }
    return r;
}

// Fixed-buffer write, also used on the crash path.
void writeState(const RunState& r) {
    if (!sStatePath[0]) return;
    char buf[1024];
    const int n = std::snprintf(buf, sizeof(buf),
                                "; How the last run of Luigi's Mansion Recompiled ended (start-up recovery, docs/SETUP.md)\n"
                                "state=%s\nphase=%s\nreason=%s\nversion=%s\nbuild=%s\nfailed_starts=%d\ncrashes_in_row=%d\n",
                                r.state.c_str(), r.phase.c_str(), r.reason.c_str(), r.version.c_str(), r.build.c_str(),
                                r.failedStarts, r.crashesInRow);
    if (std::FILE* f = std::fopen(sStatePath, "wb")) {
        std::fwrite(buf, 1, n > 0 ? std::min<size_t>(size_t(n), sizeof(buf) - 1) : 0, f);
        std::fclose(f);
    }
}

void say(const std::string& text) {
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
    logLine(text.c_str(), text.size());
}

// Moves `from` to `to` (rename, or copy + remove across volumes). True if it is gone from `from`.
bool moveAside(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) return false;
    fs::remove_all(from, ec);
    return !ec;
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

void resetSettings() {
    const fs::path ini = settingsPath();
    std::error_code ec;
    if (fs::exists(ini, ec)) {
        const fs::path bak = ini.string() + ".bak-" + stamp();
        fs::copy_file(ini, bak, fs::copy_options::overwrite_existing, ec);
        say("[recovery] settings.ini saved as " + u8(bak.filename()));
    }
    resetSettingsToDefaults();
    say("[recovery] settings reset to the defaults");
}

void rebuildGameData() {
    std::error_code ec;
    for (const fs::path& p : {dataDir() / "game", hiddenMansionOverlayDir()}) {
        if (fs::exists(p, ec)) {
            fs::remove_all(p, ec);
            say("[recovery] removed " + u8(p) + (ec ? " (failed: " + ec.message() + ")" : ""));
        }
    }
    // Shader and pipeline caches (rebuilt at the next start; the "Preparing shaders" screen).
    for (const char* name : {"pipeline_cache.db", "pipeline_cache.db-wal", "pipeline_cache.db-shm",
                             "pipeline_cache.journal", "dawn_cache.db", "dawn_cache.db-wal", "dawn_cache.db-shm"}) {
        const fs::path p = cacheDir() / name;
        if (fs::exists(p, ec)) fs::remove(p, ec);
    }
    say("[recovery] game data and shader caches will be rebuilt");
}

std::vector<fs::path> cardFolders() {
    std::vector<fs::path> out;
    const fs::path user = std::getenv("LM_CARD_DIR") ? envPath("LM_CARD_DIR") : dataDir();
    std::error_code ec;
    for (const char* region : {"USA", "EUR", "JAP"})
        for (const char* slot : {"Card A", "Card B"})
            if (fs::exists(user / region / slot, ec)) out.push_back(user / region / slot);
    return out;
}

void putSavesAside() {
    const std::string tag = " (put aside " + stamp() + ")";
    for (const fs::path& dir : cardFolders()) {
        const fs::path to = dir.string() + tag;
        say("[recovery] memory card " + u8(dir) + (moveAside(dir, to) ? " moved to " + u8(to.filename()) : " could not be moved"));
    }
}

int askOnce(const RunState& prev, bool canRebuild, const std::string& sentNote);

// Sends every saved, unsent crash report (the newest carries the description). Returns the text to show.
std::string sendReports(const std::string& description) {
    const auto reports = crash::pending();
    if (reports.empty()) return "There is no saved crash report to send.";
    int sent = 0;
    std::string numbers, firstError;
    for (size_t i = 0; i < reports.size(); i++) {
        const crash::Outcome o = crash::send(reports[i], i == 0 ? description : std::string());
        if (o.ok) sent++, numbers += (numbers.empty() ? "#" : ", #") + std::to_string(o.number);
        else if (firstError.empty()) firstError = o.error;
    }
    if (size_t(sent) == reports.size()) return sent == 1 ? "Report sent. Report number: " + numbers : std::to_string(sent) + " reports sent. Report numbers: " + numbers;
    if (sent > 0) return std::to_string(sent) + " of " + std::to_string(reports.size()) + " reports sent (" + numbers + "). The rest could not be sent: " + firstError + ". They are saved and can be sent from this screen the next time.";
    return "Could not send the report: " + firstError + ".\n\nIt is saved on this PC and can be sent from this screen the next time LmCoop crashes.";
}

int ask(const RunState& prev, bool canRebuild) {
    // Test aid LM_CRASH_AUTO=send: sends the saved reports without any window (LM_CRASH_DESC = the description).
    if (const char* a = std::getenv("LM_CRASH_AUTO"); a && !std::strcmp(a, "send") && prev.state == "crashed") {
        const char* d = std::getenv("LM_CRASH_DESC");
        say("[crash] " + sendReports(d ? d : ""));
        return 0;
    }
    std::string sentNote;  // after a send: what the prompt shows at the top
    for (;;) {
        const int c = askOnce(prev, canRebuild, sentNote);
        if (c != 4) return c;
        const std::string why = crash::unavailableReason();
        if (!why.empty()) {
            sentNote = "Could not send a crash report: " + why + ".";
            continue;
        }
        std::string description;
        if (!crash::askDescription(description)) continue;  // cancelled: back to the prompt
        sentNote = sendReports(description);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "LmCoop crash report", sentNote.c_str(), nullptr);
    }
}

int askOnce(const RunState& prev, bool canRebuild, const std::string& sentNote) {
    // Test aid LM_RECOVERY=<0 start normally | 1 reset settings | 2 rebuild game data | 3 saves aside>:
    // answers the prompt (automated runs never show it).
    if (const char* e = std::getenv("LM_RECOVERY")) return std::atoi(e);
    if (std::getenv("LM_BACKGROUND")) return 0;
    const bool canSend = prev.state == "crashed" && !crash::pending().empty() && !crash::notSetUp();
    std::string text;
    if (!sentNote.empty()) text = sentNote + "\n\n";
    if (canSend) {
        text += "LmCoop crashed last time. Send a crash report to Recoi1?\n\n";
        if (!prev.reason.empty()) text += "Error: " + prev.reason + "\n\n";
        text += "The report has the crash log, your Windows and graphics card, whether you were hosting or joining, "
                "your mods and an optional note. Passwords, addresses, personal folders and saves are never sent.\n\n"
                "If the game will not start, Reset settings";
        text += canRebuild ? ", Rebuild data" : "";
        text += " or Put saves aside may help. Nothing is deleted: your settings and saves are kept as backups.";
    } else {
        text += prev.state != "crashed" ? "The last starts of LmCoop did not finish"
                : prev.phase == "startup" ? "The last start of LmCoop stopped with an error"
                                          : "LmCoop stopped with an error last time";
        if (!prev.reason.empty()) text += ":\n\n" + prev.reason;
        text += "\n\nUsually starting normally is fine. If the game will not start or the error keeps coming back:\n\n"
                "Reset settings: all options back to the defaults (settings.ini is kept as a .bak copy).\n";
        if (canRebuild) text += "Rebuild data: recompile the game from your disc and rebuild the shader caches (a minute or two).\n";
        text += "Put saves aside: start without your saves (they are moved aside, not deleted).\n\n"
                "Details for a bug report: last_run.log and previous_run.log in " + u8(configDir());
    }
    std::vector<SDL_MessageBoxButtonData> buttons;
    if (canSend) buttons.push_back({0, 4, "Send report"});
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Start normally"});
    buttons.push_back({0, 1, "Reset settings"});
    if (canRebuild) buttons.push_back({0, 2, "Rebuild data"});
    buttons.push_back({0, 3, "Put saves aside"});
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    box.title = "LmCoop";
    box.message = text.c_str();
    box.numbuttons = int(buttons.size());
    box.buttons = buttons.data();
    int choice = 0;
    if (!SDL_ShowMessageBox(&box, &choice)) return 0;
    return choice < 0 ? 0 : choice;
}

}  // namespace

void onStart(bool canRebuild) {
    const std::string sp = u8(statePath());
    std::snprintf(sStatePath, sizeof(sStatePath), "%s", sp.c_str());
    const RunState prev = readState();
    const bool prevStartupCrash = prev.state == "crashed" && prev.phase == "startup";
    const bool prevFailedStart = prev.state == "starting" || prevStartupCrash;
    sCur = {};
    sCur.state = "starting";
    sCur.phase = "startup";
    sCur.version = update::buildVersion();
    sCur.build = crash::buildId();
    sCur.failedStarts = prevFailedStart ? prev.failedStarts + 1 : 0;
    sCur.crashesInRow = prev.state == "crashed" ? prev.crashesInRow + 1 : 0;
    if (!prev.state.empty() && prev.state != "exited")
        say("[recovery] the last run (" + (prev.version.empty() ? std::string("?") : prev.version) + ") ended " +
            (prev.state == "crashed" ? "with a crash" : prev.state == "running" ? "without a clean exit (closed or killed)"
                                                                                 : "during start-up") +
            (prev.phase.empty() ? "" : " in " + prev.phase) + (prev.reason.empty() ? "" : ": " + prev.reason) +
            "; failed starts in a row " + std::to_string(sCur.failedStarts) + ", crashes in a row " +
            std::to_string(sCur.crashesInRow));
    // Offered after any crash (start-up or play: issue #17's player could not start again after a
    // fatal error in play) and after two starts in a row that never got going (killed or hung).
    const bool offer = prev.state == "crashed" || sCur.failedStarts >= 2 || std::getenv("LM_RECOVERY") != nullptr;
    // The crashed run's log is previous_run.log now; keep it as its own report before a later start replaces it.
    if (prev.state == "crashed") crash::preserve(prev.reason, prev.version, prev.build);
    crash::writeSession();  // this run's facts (after the previous run's were kept)
    writeState(sCur);  // before the prompt: a start that dies in it still counts
    sStarted = true;
    if (!offer) return;
    switch (ask(prev, canRebuild)) {
    case 1:
        resetSettings();
        break;
    case 2:
        if (canRebuild) rebuildGameData();
        break;
    case 3:
        putSavesAside();
        break;
    default:
        say("[recovery] starting normally");
        break;
    }
}

void markRunning() {
    {
        std::lock_guard lock{sHangMutex};
        if (!sStarted || sCur.state != "starting") return;
        sCur.state = "running";
        sCur.phase = "play";
        sCur.failedStarts = 0;
        writeState(sCur);
    }
    crash::writeSession();  // the session mode (hosting or joining) is known by now
}

void onExit() {
    std::lock_guard lock{sHangMutex};
    if (!sStarted) return;
    sCur.state = "exited";
    sCur.failedStarts = 0;
    sCur.crashesInRow = 0;
    sCur.reason.clear();
    writeState(sCur);
    if (sLog) std::fflush(sLog);
}

void noteFatal(const char* message) {
    std::snprintf(sFatal, sizeof(sFatal), "%s", message ? message : "");
    for (char* p = sFatal; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
}

void noteHang(const char* what) {
    std::lock_guard lock{sHangMutex};
    if (!sStarted || sCur.state != "starting") return;
    sCur.reason = what ? oneLine(std::string("the game stopped responding: ") + what) : std::string();
    writeState(sCur);
}

void onCrash(const char* what) {
    if (!sStarted || sCur.state == "crashed") return;
    sCur.state = "crashed";
    sCur.reason = oneLine(sFatal[0] ? std::string(sFatal) : std::string(what ? what : "crash"));
    writeState(sCur);
    if (sLog) std::fflush(sLog);
}

#ifdef _WIN32
namespace {
bool sNoStdio = false;     // no console and nothing redirected (setupConsole)
bool sStdioInLog = false;  // stdout/stderr reopened onto last_run.log (openRunLog)

bool validHandle(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}

void reopenOnConsole() {
    std::FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}
}  // namespace
#endif

void setupConsole(int argc, char** argv) {
#ifdef _WIN32
    bool want = false, none = false;  // LM_CONSOLE=0 (test aid): as if started by a double-click
    if (const char* e = std::getenv("LM_CONSOLE")) want = *e == '1', none = *e == '0';
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--console") == 0) want = true;
    // A console program (lmrecomp) already has its console; a redirected stream stays as it is.
    if (GetConsoleWindow() || validHandle(STD_OUTPUT_HANDLE) || validHandle(STD_ERROR_HANDLE)) return;
    if (none) {
        sNoStdio = true;
        return;
    }
    if (want && AllocConsole()) {
        reopenOnConsole();
        return;
    }
    // Started from cmd/PowerShell: print into that terminal (its prompt does not wait for us).
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        reopenOnConsole();
        return;
    }
    sNoStdio = true;
#else
    (void)argc;
    (void)argv;
#endif
}

bool stdioInLog() {
#ifdef _WIN32
    return sStdioInLog;
#else
    return false;
#endif
}

void openRunLog() {
#ifdef _WIN32
    if (sLog) return;
    std::error_code ec;
    const fs::path dir = configDir();
    const fs::path path = dir / "last_run.log";
    if (fs::exists(path, ec)) {
        fs::rename(path, dir / "previous_run.log", ec);
        // A rename fails while another process still holds the file (the old version after an update restart, or a
        // crashed run that is still closing). Copy it instead, so the crashed run's log is not lost to the truncation below.
        if (ec) fs::copy_file(path, dir / "previous_run.log", fs::copy_options::overwrite_existing, ec);
    }
    sLog = _wfopen(path.wstring().c_str(), L"wb");
    if (!sLog) return;
    const std::string head = "Luigi's Mansion Recompiled " + std::string(update::buildVersion()) + ", started " + stamp();
    logLine(head.c_str(), head.size());
    if (sNoStdio) {
        // Everything appends (the three streams then never overwrite each other's lines);
        // unbuffered, so a crash loses nothing.
        // In a windowed program stdout/stderr have no descriptor (fileno -2), so _dup2 under them
        // does nothing: stdout is reopened onto the log, which then denies other writers, so the
        // run log is stdout itself and stderr is bound (via NUL) and moved onto its descriptor.
        std::fclose(sLog);
        sLog = nullptr;
        std::FILE* f = nullptr;
        if (_wfreopen_s(&f, path.wstring().c_str(), L"ab", stdout) != 0) {
            sLog = _wfopen(path.wstring().c_str(), L"ab");
            return;
        }
        if (freopen_s(&f, "NUL", "wb", stderr) == 0) _dup2(_fileno(stdout), _fileno(stderr));
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        sLog = stdout;
        sStdioInLog = true;
    }
#endif
}

std::FILE* runLog() { return sLog; }

void logLine(const char* text, std::size_t len) {
    if (!sLog || stdioInLog()) return;
    std::fwrite(text, 1, len, sLog);
    if (len == 0 || text[len - 1] != '\n') std::fputc('\n', sLog);
    std::fflush(sLog);
}

}  // namespace port::recovery
