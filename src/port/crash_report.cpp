#include "port/crash_report.h"

#include "port/crash_redact.h"
#include "port/install.h"
#include "port/update.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace port::crash {
namespace {

namespace fs = std::filesystem;

SessionProvider sProvider = nullptr;

constexpr size_t kLogHead = 6000, kLogMax = 150000;  // what is sent: the start of the log and the end of it
constexpr int kKeepReports = 20;
constexpr const char* kInboxConfig = "https://raw.githubusercontent.com/Recoi1/LmCoop/main/data/crash_inbox.json";

fs::path reportsDir() { return dataDir() / "crash_reports"; }
fs::path sessionFile() { return configDir() / "last_run_session.txt"; }

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

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

std::string randomId() {
    std::random_device rd;
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%08x%08x", rd(), rd());
    return buf;
}

// "key=value" lines into a map; repeated keys (mod=) into a list as well.
struct Facts {
    std::map<std::string, std::string> kv;
    std::vector<std::string> mods;
};
Facts parseFacts(const std::string& text) {
    Facts f;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "mod") f.mods.push_back(v);
        else f.kv[k] = v;
    }
    return f;
}

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// OS and graphics lines Aurora logs at startup.
void systemFromLog(const std::string& log, std::string& os, std::string& gpu) {
    std::set<std::string> gpus;
    std::string driver;
    std::istringstream in(log);
    for (std::string line; std::getline(in, line);) {
        if (line.find("aurora::system_info]") != std::string::npos) {
            const auto p = line.find("OS: ");
            if (p != std::string::npos && os.empty()) os = line.substr(p + 4);
            const auto g = line.find("Detected GPU: ");
            if (g != std::string::npos) {
                std::string name = line.substr(g + 14);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
                gpus.insert(name);
            }
        }
        const auto d = line.find("Driver: ");
        if (d != std::string::npos && driver.empty()) {
            driver = line.substr(d + 8);
            while (!driver.empty() && (driver.back() == ' ' || driver.back() == '\r')) driver.pop_back();
        }
    }
    for (const std::string& g : gpus) gpu += (gpu.empty() ? "" : "; ") + g;
    if (!driver.empty()) gpu += (gpu.empty() ? "" : " / ") + driver;
    while (!os.empty() && (os.back() == ' ' || os.back() == '\r')) os.pop_back();
    while (!gpu.empty() && (gpu.back() == ' ' || gpu.back() == '\r')) gpu.pop_back();
}

Scrub scrubFor(const Facts& session) {
    Scrub sc;
    auto add = [&](const std::string& needle, const char* with) {
        if (needle.size() >= 3) sc.literals.emplace_back(needle, with);
    };
    add(envOr("USERNAME"), "<user>");
    add(envOr("USER"), "<user>");
    add(envOr("COMPUTERNAME"), "<pc>");
    add(envOr("HOSTNAME"), "<pc>");
    if (const auto it = session.kv.find("name"); it != session.kv.end()) add(it->second, "<name>");
    add(plainPath(dataDir()).string(), "%DATA%");
    add(plainPath(configDir()).string(), "%DATA%");
    return sc;
}

std::string endpoint(std::string& error) {
    if (const char* e = std::getenv("LM_CRASH_ENDPOINT")) return e;  // testing: http://127.0.0.1:<port>/report
    std::string body, err;
    if (!update::fetchText(kInboxConfig, body, err, 8192)) {
        error = "could not look up the crash inbox (" + err + ")";
        return {};
    }
    try {
        const auto j = nlohmann::json::parse(body);
        return j.value("url", "");
    } catch (const std::exception&) {
        error = "the crash inbox settings are unreadable";
        return {};
    }
}

void prune() {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(reportsDir(), ec))
        if (e.is_directory(ec)) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());  // names start with the time: oldest first
    for (size_t i = 0; dirs.size() - i > size_t(kKeepReports); i++) fs::remove_all(dirs[i], ec);
}

}  // namespace

void setSessionProvider(SessionProvider p) { sProvider = p; }

void writeSession() {
    if (!sProvider) return;
    std::error_code ec;
    fs::create_directories(sessionFile().parent_path(), ec);
    std::ofstream(sessionFile(), std::ios::binary) << sProvider();
}

const char* buildId() {
#ifdef LM_GIT_SHA
    return LM_GIT_SHA;
#else
    return "unknown";
#endif
}

fs::path preserve(const std::string& reason, const std::string& version, const std::string& build) {
    std::error_code ec;
    const fs::path log = configDir() / "previous_run.log";
    // crash_trace_<pid>.txt: what the crash handler wrote itself (crash_handler.h), kept apart from the log
    std::vector<fs::path> traces;
    for (const auto& e : fs::directory_iterator(configDir(), ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("crash_trace_", 0) == 0) traces.push_back(e.path());
    }
    if (!fs::exists(log, ec) && traces.empty()) return {};
    const std::string id = randomId();
    const fs::path dir = reportsDir() / (stamp() + "-" + id);
    fs::create_directories(dir, ec);
    if (ec) return {};
    if (fs::exists(log, ec)) fs::copy_file(log, dir / "log.txt", fs::copy_options::overwrite_existing, ec);
    for (const auto& t : traces) {
        std::ofstream(dir / "log.txt", std::ios::binary | std::ios::app) << "\n=== crash trace file " << t.filename().string() << " ===\n" << readAll(t);
        fs::remove(t, ec);
    }
    if (fs::exists(sessionFile(), ec)) fs::copy_file(sessionFile(), dir / "session.txt", fs::copy_options::overwrite_existing, ec);
    std::ofstream(dir / "meta.txt", std::ios::binary) << "id=" << id << "\nreason=" << reason << "\nversion=" << version << "\nbuild=" << build << "\n";
    prune();
    return dir;
}

std::vector<Pending> pending() {
    std::vector<Pending> out;
    std::error_code ec;
    if (!fs::is_directory(reportsDir(), ec)) return out;
    for (const auto& e : fs::directory_iterator(reportsDir(), ec)) {
        if (!e.is_directory(ec) || fs::exists(e.path() / "sent.txt", ec) || !fs::exists(e.path() / "meta.txt", ec)) continue;
        const Facts meta = parseFacts(readAll(e.path() / "meta.txt"));
        const auto id = meta.kv.find("id");
        if (id != meta.kv.end()) out.push_back({e.path(), id->second});
    }
    std::sort(out.begin(), out.end(), [](const Pending& a, const Pending& b) { return a.dir > b.dir; });
    return out;
}

bool notSetUp() {
#ifndef _WIN32
    return true;
#else
    if (std::getenv("LM_CRASH_ENDPOINT")) return false;
    std::string body, err;
    if (!update::fetchText(kInboxConfig, body, err, 8192)) return false;  // offline: unknown, not "not set up"
    try {
        return nlohmann::json::parse(body).value("url", "").empty();
    } catch (const std::exception&) {
        return false;
    }
#endif
}

std::string unavailableReason() {
#ifndef _WIN32
    return "sending crash reports is only built for Windows so far";
#else
    if (std::getenv("LM_CRASH_ENDPOINT")) return {};
    std::string err;
    return endpoint(err).empty() ? (err.empty() ? "crash reports are not set up yet (no inbox address)" : err) : std::string();
#endif
}

Outcome send(const Pending& report, const std::string& description) {
    Outcome out;
    const Facts meta = parseFacts(readAll(report.dir / "meta.txt"));
    const Facts session = parseFacts(readAll(report.dir / "session.txt"));
    std::string log = readAll(report.dir / "log.txt");
    if (log.empty()) {
        out.error = "the saved log is empty";
        return out;
    }
    auto get = [](const Facts& f, const char* k) {
        const auto it = f.kv.find(k);
        return it == f.kv.end() ? std::string() : it->second;
    };

    std::string os, gpu;
    systemFromLog(log, os, gpu);
    if (log.size() > kLogMax) log = log.substr(0, kLogHead) + "\n[... middle of the log left out ...]\n" + log.substr(log.size() - (kLogMax - kLogHead));

    const Scrub sc = scrubFor(session);
    auto clean = [&](const std::string& s) { return redact(s, sc); };

    nlohmann::json j;
    j["schema"] = 1;
    j["id"] = report.id;
    j["version"] = clean(get(meta, "version"));
    j["build"] = clean(get(meta, "build"));
    j["os"] = clean(os);
    j["gpu"] = clean(gpu);
    const std::string mode = get(session, "mode"), net = get(session, "net");
    j["mode"] = mode == "host" || mode == "join" || mode == "off" ? mode : "unknown";
    j["net"] = net == "direct" || net == "internet" || net == "steam" ? net : "unknown";
    j["reason"] = clean(get(meta, "reason"));
    j["description"] = clean(description.substr(0, 2000));
    j["log"] = clean(log);
    nlohmann::json mods = nlohmann::json::array();
    for (const std::string& m : session.mods) {  // kind|name|version
        std::vector<std::string> p;
        std::stringstream ss(m);
        for (std::string part; std::getline(ss, part, '|');) p.push_back(part);
        if (p.size() >= 2) mods.push_back({{"kind", clean(p[0])}, {"name", clean(p[1])}, {"version", p.size() > 2 ? clean(p[2]) : ""}});
    }
    j["mods"] = mods;

    std::string err;
    const std::string url = endpoint(err);
    if (url.empty()) {
        out.error = err.empty() ? "crash reports are not set up yet (no inbox address)" : err;
        return out;
    }
    std::string response;
    int status = 0;
    if (!update::postJson(url, j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), response, status, err)) {
        out.error = "could not reach the crash inbox (" + err + ")";
        return out;
    }
    if (status == 429) {
        out.error = "too many reports from this connection; try again later";
        return out;
    }
    if (status != 200) {
        out.error = "the crash inbox answered HTTP " + std::to_string(status);
        return out;
    }
    try {
        const auto r = nlohmann::json::parse(response);
        out.number = r.value("number", 0);
        out.ok = r.value("ok", false) && out.number > 0;
    } catch (const std::exception&) {
    }
    if (!out.ok) {
        out.error = "the crash inbox sent an unexpected answer";
        return out;
    }
    std::ofstream(report.dir / "sent.txt") << "number=" << out.number << "\n";
    return out;
}

#ifdef _WIN32
namespace {
struct DescState {
    HWND edit = nullptr;
    std::wstring text;
    bool send = false, done = false;
};
DescState sDesc;

LRESULT CALLBACK descProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            if (LOWORD(w) == IDOK) {
                std::wstring t(2100, L'\0');
                t.resize(size_t(GetWindowTextW(sDesc.edit, t.data(), int(t.size()))));
                sDesc.text = t, sDesc.send = true;
            }
            sDesc.done = true;
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        sDesc.done = true;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace
#endif

bool askDescription(std::string& text) {
    text.clear();
#ifdef _WIN32
    if (std::getenv("LM_BACKGROUND") || std::getenv("LM_CRASH_AUTO")) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = descProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LmCoopCrashDescription";
    RegisterClassW(&wc);
    sDesc = DescState{};
    const int w = 520, hgt = 300;
    HWND win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, wc.lpszClassName, L"LmCoop crash report", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2, w, hgt, nullptr, nullptr, wc.hInstance, nullptr);
    if (!win) return true;  // no window: send without a description
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto child = [&](const wchar_t* cls, const wchar_t* label, DWORD style, int x, int y, int cw, int ch, int id) {
        HWND c = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, label, WS_CHILD | WS_VISIBLE | style, x, y, cw, ch, win,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    child(L"STATIC", L"What happened? (optional) The report holds the log, your Windows and graphics card, and your mods.\nPasswords, addresses and personal folders are removed.", SS_LEFT, 12, 10, w - 40, 40, 0);
    sDesc.edit = child(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 12, 56, w - 40, 140, 100);
    SendMessageW(sDesc.edit, EM_SETLIMITTEXT, 2000, 0);
    child(L"BUTTON", L"Send report", WS_TABSTOP | BS_DEFPUSHBUTTON, w - 40 - 230, 210, 110, 30, IDOK);
    child(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, w - 40 - 110, 210, 100, 30, IDCANCEL);
    ShowWindow(win, SW_SHOW);
    SetForegroundWindow(win);
    SetFocus(sDesc.edit);
    MSG msg;
    while (!sDesc.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (sDesc.send) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, sDesc.text.c_str(), int(sDesc.text.size()), nullptr, 0, nullptr, nullptr);
        text.resize(size_t(n));
        WideCharToMultiByte(CP_UTF8, 0, sDesc.text.c_str(), int(sDesc.text.size()), text.data(), n, nullptr, nullptr);
    }
    return sDesc.send;
#else
    return true;
#endif
}

}  // namespace port::crash
