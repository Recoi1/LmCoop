#include "recomp/characters.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/rarc.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::characters {
namespace {
namespace fs = std::filesystem;

constexpr s32 kEntryBase = 0x50000000;  // virtual FST entries (HM 0x40.., rooms 0x48.., unused maps 0x4C..)

std::mutex sM;
std::string sChoice = "luigi";

// A pack is <dataDir>/characters/<id>; LM_CHARACTER_DIR=<folder> adds one (id "env") for testing.
fs::path packDir(const std::string& id) {
    if (id == "env") {
        const char* d = std::getenv("LM_CHARACTER_DIR");
        return d ? fs::path(d) : fs::path();
    }
    return port::dataDir() / "characters" / id;
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
bool havePack(const std::string& id) {
    std::error_code ec;
    return !packDir(id).empty() && fs::exists(packDir(id) / "pack.ini", ec);
}

rarc::Bytes readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return rarc::Bytes(std::istreambuf_iterator<char>(in), {});
}

// The playable Luigi lives in Game/game_usa.szp (data/model/luige.arc, luige/model/luige.mdl). The game decompresses
// that file through the host (hle_dvd.cpp), and the pack's model is written over the original's bytes right after,
// so the disc's own compressed file (and the game's heap use) stay exactly as they are. Same size only.
struct GameModel {
    std::string forPack;
    bool ok = false;
    size_t rawSize = 0, offset = 0;
    rarc::Bytes original, replacement;
} sGame;

void prepareGame(const std::string& id) {
    if (sGame.forPack == id) return;
    sGame = GameModel{};
    sGame.forPack = id;
    sGame.replacement = readFile(packDir(id) / "luige.mdl");
    rarc::Bytes raw = rarc::decompress(rarc::readDisc("/Game/game_usa.szp"));
    rarc::Archive outer;
    if (sGame.replacement.empty() || !outer.parse(raw)) return;
    rarc::Item* arc = outer.find("model/luige.arc");
    rarc::Archive inner;
    if (!arc || !inner.parse(rarc::decompress(arc->data))) return;
    const rarc::Item* old = inner.find("model/luige.mdl");
    if (!old || old->data.size() != sGame.replacement.size()) {
        log("[character] luige.mdl: the pack's model is %zu bytes, the game's is %zu: not used", sGame.replacement.size(), old ? old->data.size() : size_t(0));
        return;
    }
    const auto at = std::search(raw.begin(), raw.end(), old->data.begin(), old->data.end());
    if (at == raw.end()) return;
    sGame.rawSize = raw.size(), sGame.offset = size_t(at - raw.begin()), sGame.original = old->data, sGame.ok = true;
}

struct Served {
    const char* path;      // guest path
    const char* packFile;  // file in the pack
    rarc::Bytes bytes;
    bool built = false;
};
Served sServed[] = {
    {"/AudioRes/Banks/LuiSec0_0.aw", "LuiSec0_0.aw", {}, false},
    {"/AudioRes/Banks/LuiSec1_0.aw", "LuiSec1_0.aw", {}, false},
    {"/AudioRes/Banks/LuiSec2_0.aw", "LuiSec2_0.aw", {}, false},
    {"/AudioRes/JaiInit.aaf", "JaiInit.aaf", {}, false},
};
std::string sBuiltFor;  // the pack the served bytes belong to

bool build(Served& s, const std::string& id) {
    if (sBuiltFor != id) {
        for (Served& x : sServed) x.bytes.clear(), x.built = false;
        sBuiltFor = id;
    }
    if (s.built) return !s.bytes.empty();
    s.built = true;
    const fs::path dir = packDir(id);
    s.bytes = readFile(dir / s.packFile);
    if (s.bytes.empty()) log("[character] %s: not served (no %s in the pack, or the archive did not match)", s.path, s.packFile);
    else log("[character] %s served from the pack '%s' (%zu bytes)", s.path, id.c_str(), s.bytes.size());
    return !s.bytes.empty();
}

void reset() { sChoice = "luigi"; }
void load(const std::string& section, const std::string& key, const std::string& value) {
    if (section == "character" && key == "id") sChoice = value.substr(0, 40);
}
void save(std::ostream& out) { out << "[character]\nid=" << sChoice << '\n'; }
const bool sRegistered = [] {
    port::addSettingsExtension({&reset, &load, &save});
    return true;
}();

}  // namespace

std::vector<Info> available() {
    std::vector<Info> v{{"luigi", "Luigi"}};
    std::error_code ec;
    const fs::path root = port::dataDir() / "characters";
    if (fs::is_directory(root, ec))
        for (const auto& e : fs::directory_iterator(root, ec))
            if (e.is_directory(ec) && fs::exists(e.path() / "pack.ini", ec)) v.push_back({e.path().filename().string(), packName(e.path())});
    if (havePack("env")) v.push_back({"env", packName(packDir("env"))});
    return v;
}

std::string selected() {
    std::string id;
    {
        std::lock_guard lk(sM);
        id = sChoice;
    }
    if (const char* e = std::getenv("LM_CHARACTER")) id = e;
    return id == "luigi" || !havePack(id) ? "luigi" : id;
}

void select(const std::string& id) {
    std::lock_guard lk(sM);
    sChoice = id;
}

bool drawSelect(float scale) {
    bool changed = false;
    const std::string cur = selected();
    for (const Info& c : available()) {
        ImGui::PushID(c.id.c_str());
        const bool on = c.id == cur;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.75f, 1));
        if (ImGui::Button(c.name.c_str(), ImVec2(180 * scale, 34 * scale)) && !on) select(c.id), changed = true;
        if (on) ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();
    return changed;
}

void afterDecompress(u32 dest, u32 size) {
    const std::string id = selected();
    if (id == "luigi") return;
    std::lock_guard lk(sM);
    prepareGame(id);
    if (!sGame.ok || size != sGame.rawSize) return;
    u8* p = hostp(dest) + sGame.offset;
    if (std::memcmp(p, sGame.original.data(), sGame.original.size()) != 0) return;  // another file of the same size
    std::memcpy(p, sGame.replacement.data(), sGame.replacement.size());
    log("[character] the pack's luige.mdl is in place (%zu bytes at +%zu)", sGame.replacement.size(), sGame.offset);
}

s32 overlayEntry(const char* guestPath) {
    const std::string id = selected();
    if (id == "luigi") return -1;
    std::lock_guard lk(sM);
    for (size_t i = 0; i < std::size(sServed); i++)
        if (!std::strcmp(guestPath, sServed[i].path)) return build(sServed[i], id) ? kEntryBase + s32(i) : -1;
    return -1;
}
bool isOverlayEntry(s32 entry) { return entry >= kEntryBase && entry < kEntryBase + s32(std::size(sServed)); }
u32 overlayLength(s32 entry) {
    std::lock_guard lk(sM);
    return isOverlayEntry(entry) ? u32(sServed[entry - kEntryBase].bytes.size()) : 0;
}
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lk(sM);
    if (!isOverlayEntry(entry)) return -1;
    const rarc::Bytes& b = sServed[entry - kEntryBase].bytes;
    if (offset < 0 || length < 0 || size_t(offset) > b.size()) return -1;
    const size_t n = std::min(size_t(length), b.size() - size_t(offset));
    std::memcpy(dst, b.data() + offset, n);
    return s32(n);
}

}  // namespace recomp::characters
