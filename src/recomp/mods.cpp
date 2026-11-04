#include "recomp/mods.h"

#include "port/update.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace recomp::mods {
namespace {

constexpr const char* kListUrl =
    "https://gamebanana.com/apiv11/Mod/Index?_nPerpage=50&_nPage=1&_aFilters%5BGeneric_Game%5D=7120&_sSort=Generic_MostLiked";
constexpr const char* kTestsUrl = "https://raw.githubusercontent.com/Recoi1/LmCoop/main/data/mod_compat.json";

std::mutex sM;
State sState;
std::atomic<bool> sRunning{false};

void work() {
    State next;
    std::string body, err;
    if (port::update::fetchText(kListUrl, body, err, 4u << 20)) {
        try {
            const auto j = nlohmann::json::parse(body);
            for (const auto& r : j.at("_aRecords")) {
                Entry e;
                e.id = r.value("_idRow", 0);
                e.name = r.value("_sName", "");
                e.url = r.value("_sProfileUrl", "");
                if (r.contains("_aSubmitter")) e.author = r["_aSubmitter"].value("_sName", "");
                if (r.contains("_aRootCategory")) e.category = r["_aRootCategory"].value("_sName", "");
                e.likes = r.contains("_nLikeCount") && r["_nLikeCount"].is_number() ? r["_nLikeCount"].get<int>() : 0;
                if (e.id && !e.name.empty()) next.mods.push_back(std::move(e));
            }
            next.loaded = true;
        } catch (const std::exception& x) {
            err = std::string("GameBanana sent something unexpected (") + x.what() + ")";
        }
    }
    if (!next.loaded) next.error = err.empty() ? "could not reach GameBanana" : err;

    // What the developers tested; optional, a failure just leaves everything "untested".
    body.clear();
    std::string tErr;
    if (next.loaded && port::update::fetchText(kTestsUrl, body, tErr, 1u << 20)) {
        try {
            const auto j = nlohmann::json::parse(body);
            const auto& t = j.at("mods");
            for (Entry& e : next.mods) {
                const auto it = t.find(std::to_string(e.id));
                if (it == t.end()) continue;
                e.status = it->value("status", "");
                e.note = it->value("note", "");
            }
            next.haveTests = true;
        } catch (const std::exception&) {
        }
    }
    {
        std::lock_guard lk(sM);
        sState = std::move(next);
    }
    sRunning = false;
}

}  // namespace

void refresh() {
    if (sRunning.exchange(true)) return;
    {
        std::lock_guard lk(sM);
        sState.busy = true;
        sState.error.clear();
    }
    std::thread([] {
        work();
        std::lock_guard lk(sM);
        sState.busy = false;
    }).detach();
}

State state() {
    std::lock_guard lk(sM);
    return sState;
}

void openPage(const std::string& url) {
    static const std::string prefix = "https://gamebanana.com/";
    if (url.rfind(prefix, 0) != 0) return;
    for (char c : url)  // nothing that could end a command line or a quoted string
        if (!(std::isalnum(static_cast<unsigned char>(c)) || std::strchr(":/._-?=&%", c))) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::system(("open '" + url + "' >/dev/null 2>&1 &").c_str());
#else
    std::system(("xdg-open '" + url + "' >/dev/null 2>&1 &").c_str());
#endif
}

}  // namespace recomp::mods
