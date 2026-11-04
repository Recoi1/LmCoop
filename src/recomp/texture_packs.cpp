#include "recomp/texture_packs.h"

#include "port/install.h"
#include "port/settings.h"
#include "port/update.h"

#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <unordered_map>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::texpacks {
namespace {
namespace fs = std::filesystem;

std::mutex sM;
std::set<std::string> sEnabled;
std::vector<aurora::texture::ReplacementGroup> sGroups;
InstallState sInstall;
std::atomic<bool> sCancel{false};
std::atomic<bool> sRunning{false};
std::atomic<std::uint64_t> sGot{0};
std::atomic<bool> sPendingApply{false};

fs::path root() { return port::dataDir() / "texture_packs"; }
bool safeId(const std::string& id) {
    if (id.empty() || id.size() > 40 || id[0] == '.') return false;
    for (char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}
std::string packName(const fs::path& dir) {
    std::ifstream in(dir / "pack.ini");
    for (std::string line; std::getline(in, line);)
        if (line.rfind("name=", 0) == 0) {
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
            return line.substr(5);
        }
    return dir.filename().string();
}
int countTextures(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (name.rfind("tex1_", 0) == 0 && (name.ends_with(".png") || name.ends_with(".dds"))) n++;
    }
    return n;
}

// Disc folders a loose-files mod has under its files/ tree.
bool discFolder(std::string n) {
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const char* k[] = {"game", "iwamoto", "model", "map", "event", "kawano", "nakamura", "ajioka", "audiores", "effect", "ending", "ukon"};
    for (const char* d : k)
        if (n == d) return true;
    return false;
}
// The folder named files (any case) at most three levels down whose children are disc folders; empty when none.
fs::path findFilesRoot(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 3 || !it->is_directory(ec)) continue;
        std::string n = it->path().filename().string();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (n != "files") continue;
        for (const auto& c : fs::directory_iterator(it->path(), ec))
            if (c.is_directory(ec) && discFolder(c.path().filename().string())) return it->path();
    }
    return {};
}
int countFiles(const fs::path& filesRoot) {
    int n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(filesRoot, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n++;
    return n;
}

// Loose game files of the enabled packs, by lower-case disc path (for example /iwamoto/map0/test_01.bin).
constexpr s32 kFileBase = 0x54000000;  // virtual FST entries (HM 0x40.., rooms 0x48.., unused maps 0x4C.., characters 0x50..)
struct LooseFile {
    fs::path path;
    std::uint32_t size = 0;
};
std::mutex sFilesM;
std::vector<LooseFile> sLoose;
std::unordered_map<std::string, s32> sLooseIndex;
fs::path sOpenPath;
std::ifstream sOpen;

void rescanFiles(const std::set<std::string>& ids) {
    std::vector<LooseFile> v;
    std::unordered_map<std::string, s32> idx;
    for (const std::string& id : ids) {
        std::error_code ec;
        const fs::path dir = root() / id;
        std::ifstream in(dir / "pack.ini");
        std::string rel;
        for (std::string line; std::getline(in, line);)
            if (line.rfind("files=", 0) == 0) rel = line.substr(6);
        rel.erase(std::remove(rel.begin(), rel.end(), '\r'), rel.end());
        if (rel.empty()) continue;
        const fs::path fr = dir / rel;
        for (auto it = fs::recursive_directory_iterator(fr, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string key = "/" + fs::relative(it->path(), fr, ec).generic_string();
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            const std::uint64_t sz = it->file_size(ec);
            if (sz == 0 || sz > 0x7FFFFFFFull) continue;
            const auto found = idx.find(key);
            if (found != idx.end()) v[size_t(found->second)] = {it->path(), std::uint32_t(sz)};  // a later pack wins
            else idx[key] = s32(v.size()), v.push_back({it->path(), std::uint32_t(sz)});
        }
    }
    std::lock_guard lk(sFilesM);
    sLoose = std::move(v);
    sLooseIndex = std::move(idx);
    sOpen.close();
    sOpenPath.clear();
    log("[textures] %zu loose game files from the enabled packs", sLoose.size());
}

void reset() { sEnabled.clear(); }
void load(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "textures" || key != "enabled") return;
    std::stringstream ss(value);
    for (std::string id; std::getline(ss, id, ',');)
        if (safeId(id)) sEnabled.insert(id);
}
void save(std::ostream& out) {
    out << "[textures]\nenabled=";
    bool first = true;
    for (const std::string& id : sEnabled) out << (first ? "" : ",") << id, first = false;
    out << '\n';
}
const bool sRegistered = [] {
    port::addSettingsExtension({&reset, &load, &save});
    return true;
}();

bool gbHost(const std::string& h) { return h == "gamebanana.com" || h.ends_with(".gamebanana.com"); }

void fail(const std::string& why) {
    std::lock_guard lk(sM);
    sInstall.stage = Stage::Failed, sInstall.message = why;
    log("[textures] install failed: %s", why.c_str());
}

void work(int modId, std::string name) {
    const std::string id = "gb" + std::to_string(modId);
    // 1. what to download
    std::string body, err;
    if (!port::update::fetchText("https://gamebanana.com/apiv11/Mod/" + std::to_string(modId) + "/ProfilePage", body, err, 2u << 20))
        return fail("could not reach GameBanana (" + err + ")");
    std::string url, file;
    std::uint64_t size = 0;
    log("[textures] mod %d: GameBanana answered %zu bytes", modId, body.size());
    try {
        const auto doc = nlohmann::json::parse(body);
        for (const auto& f : doc.at("_aFiles")) {
            const std::string n = f.value("_sFile", "");
            log("[textures]   file %s", n.c_str());
            const std::string ext = fs::path(n).extension().string();
            if (ext == ".zip" || ext == ".7z" || ext == ".rar") {
                url = f.value("_sDownloadUrl", ""), file = n, size = f.value("_nFilesize", std::uint64_t(0));
                break;
            }
        }
    } catch (const std::exception& x) {
        return fail(std::string("GameBanana sent something unexpected (") + x.what() + ")");
    }
    if (url.empty()) return fail("this mod has no .zip, .7z or .rar file to install");
    {
        std::lock_guard lk(sM);
        sInstall.stage = Stage::Downloading, sInstall.total = size;
    }

    // 2. download
    const fs::path tmp = root() / ".download";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path archive = tmp / ("pack" + fs::path(file).extension().string());
    sGot = 0;
    std::atomic<bool> done{false};
    std::thread tick([&] {  // the UI reads the byte count from the state
        while (!done) {
            {
                std::lock_guard lk(sM);
                sInstall.got = sGot;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });
    const bool ok = port::update::downloadFile(url, archive, gbHost, sGot, sCancel, err);
    done = true;
    tick.join();
    if (!ok) return fail(err == "cancelled" ? "cancelled" : "the download failed: " + err);

    // 3. unpack into the pack's folder, and check it holds textures
    {
        std::lock_guard lk(sM);
        sInstall.stage = Stage::Unpacking;
    }
    const fs::path dir = root() / id;
    fs::remove_all(dir, ec);
    err = port::update::extractArchive(archive, dir);
    fs::remove_all(tmp, ec);
    if (!err.empty()) {
        fs::remove_all(dir, ec);
        return fail(err);
    }
    const int n = countTextures(dir);
    const fs::path filesRoot = findFilesRoot(dir);
    const int nf = filesRoot.empty() ? 0 : countFiles(filesRoot);
    if (n == 0 && nf == 0) {
        fs::remove_all(dir, ec);
        return fail("this archive holds neither Dolphin-style textures (tex1_*.png / .dds) nor a files/ folder of game files: LmCoop cannot use it");
    }
    std::ofstream(dir / "pack.ini") << "name=" << name << "\ngamebanana=" << modId << "\ntextures=" << n << "\nfiles="
                                    << (filesRoot.empty() ? std::string() : fs::relative(filesRoot, dir, ec).generic_string()) << "\nfilecount=" << nf << "\n";
    {
        std::lock_guard lk(sM);
        sEnabled.insert(id);
        sInstall.stage = Stage::Done, sInstall.message = name + ": " + (n ? std::to_string(n) + " textures" : std::string()) + (n && nf ? ", " : "") + (nf ? std::to_string(nf) + " game files" : std::string()) + " installed and enabled";
    }
    log("[textures] installed %s (%d textures)", id.c_str(), n);
    sPendingApply = true;  // the UI thread registers it (pump)
}

}  // namespace

std::vector<Info> installed() {
    std::vector<Info> v;
    std::error_code ec;
    if (!fs::is_directory(root(), ec)) return v;
    std::lock_guard lk(sM);
    for (const auto& e : fs::directory_iterator(root(), ec)) {
        const std::string id = e.path().filename().string();
        if (!e.is_directory(ec) || !safeId(id)) continue;
        Info i;
        i.id = id, i.name = packName(e.path()), i.enabled = sEnabled.count(id) > 0;
        std::ifstream in(e.path() / "pack.ini");
        for (std::string line; std::getline(in, line);)
            if (line.rfind("textures=", 0) == 0) i.textures = std::atoi(line.c_str() + 9);
            else if (line.rfind("filecount=", 0) == 0) i.files = std::atoi(line.c_str() + 10);
        v.push_back(std::move(i));
    }
    return v;
}

void setEnabled(const std::string& id, bool on) {
    if (!safeId(id)) return;
    {
        std::lock_guard lk(sM);
        if (on) sEnabled.insert(id);
        else sEnabled.erase(id);
    }
    apply();
    port::saveSettings();
}

bool remove(const std::string& id) {
    if (!safeId(id)) return false;
    {
        std::lock_guard lk(sM);
        sEnabled.erase(id);
    }
    std::error_code ec;
    const bool gone = fs::remove_all(root() / id, ec) > 0 && !ec;
    apply();
    port::saveSettings();
    return gone;
}

void apply() {
    std::set<std::string> ids;
    {
        std::lock_guard lk(sM);
        ids = sEnabled;
    }
    rescanFiles(ids);
    for (auto& g : sGroups) aurora::texture::unregister_replacements(g);
    sGroups.clear();
    int priority = 0;
    for (const std::string& id : ids) {
        std::error_code ec;
        if (!fs::is_directory(root() / id, ec)) continue;
        aurora::texture::ReplacementOptions opt;
        opt.priority = priority++;
        sGroups.push_back(aurora::texture::load_replacement_directory(root() / id, opt));
        log("[textures] pack %s: %zu textures registered", id.c_str(), sGroups.back().registrations.size());
    }
    if (const char* d = std::getenv("LM_TEXTURE_PACK_DIR")) {  // test aid: any folder
        sGroups.push_back(aurora::texture::load_replacement_directory(fs::path(d), {}));
        log("[textures] LM_TEXTURE_PACK_DIR: %zu textures registered", sGroups.back().registrations.size());
    }
}

void pump() {
    if (!sPendingApply.exchange(false)) return;
    apply();
    port::saveSettings();
}

s32 fileOverlayEntry(const char* guestPath) {
    std::string key = guestPath;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::lock_guard lk(sFilesM);
    const auto it = sLooseIndex.find(key);
    if (it == sLooseIndex.end()) return -1;
    static std::set<s32> logged;
    if (logged.insert(it->second).second) log("[textures] serving %s from a mod", guestPath);
    return kFileBase + it->second;
}
bool isFileOverlay(s32 entry) {
    std::lock_guard lk(sFilesM);
    return entry >= kFileBase && entry < kFileBase + s32(sLoose.size());
}
u32 fileOverlayLength(s32 entry) {
    std::lock_guard lk(sFilesM);
    return entry >= kFileBase && entry < kFileBase + s32(sLoose.size()) ? sLoose[size_t(entry - kFileBase)].size : 0;
}
s32 fileOverlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lk(sFilesM);
    if (entry < kFileBase || entry >= kFileBase + s32(sLoose.size())) return -1;
    const LooseFile& f = sLoose[size_t(entry - kFileBase)];
    if (offset < 0 || length < 0 || std::uint32_t(offset) > f.size) return -1;
    if (sOpenPath != f.path) {
        sOpen.close();
        sOpen.clear();
        sOpen.open(f.path, std::ios::binary);
        sOpenPath = f.path;
    }
    const std::uint32_t n = std::min<std::uint32_t>(std::uint32_t(length), f.size - std::uint32_t(offset));
    sOpen.clear();
    sOpen.seekg(offset);
    sOpen.read(static_cast<char*>(dst), n);
    return sOpen ? s32(n) : -1;
}

bool installedFromMod(int gbModId) {
    std::error_code ec;
    return fs::is_directory(root() / ("gb" + std::to_string(gbModId)), ec);
}

void install(int gbModId, const std::string& name) {
    if (sRunning.exchange(true)) return;
    {
        std::lock_guard lk(sM);
        sInstall = InstallState{};
        sInstall.stage = Stage::Resolving, sInstall.modId = gbModId, sInstall.name = name;
    }
    sCancel = false;
    std::thread([gbModId, name] {
        work(gbModId, name);
        sRunning = false;
    }).detach();
}

InstallState installState() {
    std::lock_guard lk(sM);
    return sInstall;
}

void cancelInstall() { sCancel = true; }

}  // namespace recomp::texpacks
