// Online co-op session: docs/UNUSED_CONTENT.md "2-player mode", design in the C:\LM spec
// (2026-11-01-online-coop-design.md). The host binds the address the user chose (a Radmin VPN
// 26.x.x.x one, or 127.0.0.1 for two copies on one PC) and accepts one client that knows the
// password and runs the same game (GLME01). Both sides then exchange their player's state
// (online_protocol.h) at 30 Hz; 5 s without a packet ends the session.
#include "recomp/online.h"
#include "recomp/online_config.h"
#include "recomp/online_events.h"
#include "recomp/online_transport.h"

#include "port/dol.h"
#include "port/install.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/coop.h"
#include "recomp/guest_util.h"
#include "recomp/menu.h"
#include "recomp/version.h"
#ifndef LM_STATIC_MODULE
#include "setup/logo.h"
#endif

#include <dolphin/pad.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <string>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::online {
namespace {

using Clock = std::chrono::steady_clock;
constexpr double kTimeout = 5.0, kHelloEvery = 0.5;
constexpr int kStateEvery = 2;  // guest frames (60 Hz) per state packet: 30 Hz
// Both PCs must run the US game (GLME01 revision 0): its main.dol SHA-1 goes in the hello.
const u8* gameSha1() {
    return port::dol::version() == port::dol::Version::GLME01 ? port::dol::versionSha1(port::dol::Version::GLME01) : nullptr;
}

// ---- Settings ([online]); LM_ONLINE, LM_ONLINE_ADDR, LM_ONLINE_PORT, LM_ONLINE_PASSWORD, LM_ONLINE_NAME override ----
std::mutex sCfgM;
Config sCfg;

void resetConfig() {
    std::lock_guard lk(sCfgM);
    sCfg = Config{};
}
// One line per value: a newline inside a value would inject settings keys.
std::string oneLine(std::string s, size_t max) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return ch == '\n' || ch == '\r'; }), s.end());
    return s.substr(0, max);
}
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "online") return;
    std::lock_guard lk(sCfgM);
    if (key == "mode") sCfg.mode = value == "host" ? Mode::Host : value == "join" ? Mode::Join : Mode::Off;
    else if (key == "net") sCfg.internet = value == "internet";
    else if (key == "steam") sCfg.steam = value == "1";
    else if (key == "address") sCfg.address = value;
    else if (key == "port") sCfg.port = std::atoi(value.c_str());
    else if (key == "password") sCfg.password = value;
    else if (key == "name") sCfg.name = value.substr(0, kNameSize - 1);
    else if (key == "server") sCfg.signalUrl = oneLine(value, 200);
    else if (key == "ice") sCfg.iceServers = oneLine(value, 400);
    else if (key == "lobby_name") sCfg.lobbyName = oneLine(value, 24);
    else if (key == "public") sCfg.publicLobby = value == "1";
    else if (key == "code") sCfg.inviteCode = oneLine(value, 12);
    else if (key == "lobby_id") sCfg.lobbyId = oneLine(value, 40);
}
void saveConfig(std::ostream& out) {
    std::lock_guard lk(sCfgM);
    out << "[online]\nmode=" << (sCfg.mode == Mode::Host ? "host" : sCfg.mode == Mode::Join ? "join" : "off")
        << "\nnet=" << (sCfg.internet ? "internet" : "direct")
        << "\nsteam=" << (sCfg.steam ? 1 : 0) << "\naddress=" << sCfg.address << "\nport=" << sCfg.port << "\npassword=" << sCfg.password
        << "\nname=" << sCfg.name << "\nserver=" << sCfg.signalUrl << "\nice=" << sCfg.iceServers
        << "\nlobby_name=" << sCfg.lobbyName << "\npublic=" << (sCfg.publicLobby ? 1 : 0)
        << "\ncode=" << sCfg.inviteCode << "\nlobby_id=" << sCfg.lobbyId << '\n';
}
const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

std::atomic<int> sSessionMode{-1};

Config config() {
    Config c;
    {
        std::lock_guard lk(sCfgM);
        c = sCfg;
    }
    if (const int m = sSessionMode.load(); m >= 0) c.mode = Mode(m);
    if (const char* v = std::getenv("LM_ONLINE"))
        c.mode = !std::strcmp(v, "host") ? Mode::Host : !std::strcmp(v, "join") ? Mode::Join : Mode::Off;
    if (const char* v = std::getenv("LM_ONLINE_ADDR")) c.address = v;
    if (const char* v = std::getenv("LM_ONLINE_PORT")) c.port = std::atoi(v);
    if (const char* v = std::getenv("LM_ONLINE_PASSWORD")) c.password = v;
    if (const char* v = std::getenv("LM_ONLINE_NAME")) c.name = std::string(v).substr(0, kNameSize - 1);
    // Internet test aids: LM_ONLINE_NET=internet, LM_ONLINE_SERVER=ws://..., LM_ONLINE_CODE, LM_ONLINE_LOBBY (name),
    // LM_ONLINE_PUBLIC=1, LM_ONLINE_ICE.
    if (const char* v = std::getenv("LM_ONLINE_NET"))
        c.internet = !std::strcmp(v, "internet") || !std::strcmp(v, "steam"), c.steam = !std::strcmp(v, "steam");
    if (const char* v = std::getenv("LM_ONLINE_SERVER")) c.signalUrl = v;
    if (const char* v = std::getenv("LM_ONLINE_CODE")) c.inviteCode = v;
    if (const char* v = std::getenv("LM_ONLINE_LOBBY")) c.lobbyName = oneLine(v, 24);
    if (const char* v = std::getenv("LM_ONLINE_PUBLIC")) c.publicLobby = !std::strcmp(v, "1");
    if (const char* v = std::getenv("LM_ONLINE_ICE")) c.iceServers = v;
    return c;
}

// ---- Session (guest thread; status read by the menu) ------------------------------------
using Peer = PeerId;
std::mutex sStatusM;
std::string sStatus = "off";
std::string sInvite;  // internet host: the code to share (under sStatusM)
std::string sLastNetText;  // the transport's last progress line shown in the status
std::atomic<int> sLocal{-1};
std::atomic<bool> sConnected{false};

std::unique_ptr<Transport> sNet;  // null while off or after a final failure
Config sActive;  // the config the transport was opened with
bool sRejected = false;
Peer sPeer;
bool sHavePeer = false;
Clock::time_point sLastRecv, sLastHello;
u32 sSeq = 0, sFrames = 0;
u32 sLastStateSeq = 0;
PlayerState sLocalState{}, sRemote{};
bool sHaveLocal = false, sHaveRemote = false;
u32 sRemoteAge = 0;
u32 sWorld = 0, sJoinedWorld = 0;
bool sOpeningDone = false;
u32 sOpeningTarget = 2;
bool sNeedGame = false, sHaveGame = false, sLoadRequested = false;
GameState sPendingGame{}, sLoadingGame{};
u32 sSpawnTool = 0;
int sSpawnEntry = -1;
Clock::time_point sLastGame;
enum class JoinStage { Off, Connecting, Waiting, Loading, Playing };
std::atomic<JoinStage> sJoinStage{JoinStage::Off};

constexpr u32 kSceneControl = 0x803A3AE0, kSceneId = 0x804D80A0;
constexpr u32 kNextMap = 0x804D80A4, kMap = 0x804D80C8;
constexpr u32 kEvent = 0x803D2D40, kFlags = 0x803D3399;
// Phase 2 is safe to leave; the title's MENUOFF flag stays set even when its load is finished.
bool settled() {
    return rd32(kSceneControl) == 2 && rd32(0x804D80A8) == 0 &&
           rd32(kSceneId) == rd32(kSceneControl + 4);
}

void setStatus(const std::string& s) {
    std::lock_guard lk(sStatusM);
    if (s == sStatus) return;
    sStatus = s;
    log("[online] %s", s.c_str());
}

void closeSocket() {
    sNet.reset();
    sLastNetText.clear();
    {
        std::lock_guard lk(sStatusM);
        sInvite.clear();
    }
    sHavePeer = sHaveRemote = sRejected = false;
    sHaveLocal = sHaveGame = sNeedGame = sLoadRequested = false;
    sSpawnTool = 0, sSpawnEntry = -1;
    sJoinedWorld = 0;
    sLocalState = {};
    events::reset();
    sConnected = false;
}

std::array<u8, 32> passwordHash(const std::string& pw) {
    const std::string salted = "lmco-v1:" + pw;
    const auto h = port::dol::sha1(reinterpret_cast<const u8*>(salted.data()), salted.size());
    std::array<u8, 32> out{};
    std::memcpy(out.data(), h.data(), h.size());
    return out;
}

NetConfig netConfig(const Config& c) {
    NetConfig n;
    n.internet = c.internet, n.steam = c.steam && c.internet, n.host = c.mode == Mode::Host, n.address = c.address, n.port = c.port;
    n.signalUrl = c.signalUrl, n.iceServers = c.iceServers, n.lobbyName = c.lobbyName;
    n.publicLobby = c.publicLobby, n.hasPassword = !c.password.empty();
    n.inviteCode = c.inviteCode, n.lobbyId = c.lobbyId;
    n.build = port::update::buildVersion();  // the server only pairs identical builds
    return n;
}

std::mutex sAdoptM;
std::unique_ptr<Transport> sAdopted;  // opened by the lobby before the game booted

bool openSocket(const Config& c) {
    {
        std::lock_guard lk(sAdoptM);
        if (sAdopted) sNet = std::move(sAdopted);
    }
    if (!sNet) sNet = !c.internet ? makeUdpTransport(netConfig(c)) : c.steam ? makeSteamTransport(netConfig(c)) : makeRtcTransport(netConfig(c));
    if (const NetStatus s = sNet->status(); s.failed) {
        setStatus(s.text);
        sNet.reset();
        return false;
    }
    if (c.mode == Mode::Join) sPeer = sNet->initialPeer(), sHavePeer = sPeer != 0;
    sLastRecv = sLastHello = Clock::now() - std::chrono::seconds(1);
    sLastNetText = sNet->status().text;
    setStatus(sLastNetText);
    return true;
}

// Control, game and event messages must arrive; only the 30 Hz player state may be lost.
void send(Kind kind, const u8* body, size_t n, const Peer& to) {
    u8 buf[kMaxPacket];
    const size_t len = encode(Header{kMagic, kind, sSeq++}, body, n, buf, sizeof(buf));
    if (len && sNet) sNet->send(to, buf, len, kind != Kind::State);
}

void sendHello() {
    Hello h{};
    h.version = kProtocolVersion;
    if (const u8* sha = gameSha1()) std::memcpy(h.dolSha1, sha, 20);
    const auto pw = passwordHash(sActive.password);
    std::memcpy(h.passwordHash, pw.data(), 32);
    u8 body[kHelloWireSize];
    send(Kind::Hello, body, encodeHello(h, body, sizeof(body)), sPeer);
}

// Host: a Hello from a new address. Returns the reason it is refused, "" to accept.
std::string checkHello(const u8* body, size_t n) {
    Hello h{};
    if (!decodeHello(body, n, h)) return "bad hello";
    if (h.version != kProtocolVersion) return "version";
    if (const u8* sha = gameSha1(); !sha || std::memcmp(h.dolSha1, sha, 20))
        return "game (both need the USA disc, GLME01)";
    const auto pw = passwordHash(sActive.password);
    if (std::memcmp(h.passwordHash, pw.data(), 32)) return "password";
    return "";
}

void receive() {
    u8 buf[kMaxPacket + 1];
    for (int i = 0; i < 64; i++) {  // bounded per frame
        PeerId from = 0;
        const size_t n = sNet->receive(from, buf, sizeof(buf));
        if (n == 0) return;
        Header h{};
        const u8* body = nullptr;
        size_t bn = 0;
        if (!decode(buf, n, h, body, bn)) continue;
        const Peer p = from;
        if (sActive.mode == Mode::Host) {
            if (h.kind == Kind::Hello && (!sHavePeer || p == sPeer)) {
                if (const std::string why = checkHello(body, bn); !why.empty()) {
                    send(Kind::Reject, reinterpret_cast<const u8*>(why.data()), why.size(), p);
                    static u32 sRefused = 0;  // junk must not flood the log: the first 5, then every 1000th
                    if (++sRefused <= 5 || sRefused % 1000 == 0) log("[online] refused a join (%u): %s", sRefused, why.c_str());
                    continue;
                }
                if (!sHavePeer) sHaveRemote = false;
                sPeer = p, sHavePeer = true, sLastRecv = Clock::now(), sConnected = true;
                send(Kind::Welcome, nullptr, 0, p);
                continue;
            }
            if (!sHavePeer || !(p == sPeer)) continue;  // only the accepted client
        } else {
            if (!(p == sPeer)) continue;
            if (h.kind == Kind::Reject) {
                sRejected = true;
                setStatus("rejected: " + std::string(reinterpret_cast<const char*>(body), std::min<size_t>(bn, 64)));
                return;
            }
        }
        sLastRecv = Clock::now();
        if (h.kind == Kind::Welcome || h.kind == Kind::Hello) sConnected = true;
        if (h.kind == Kind::State && sConnected && (!sHaveRemote || int32_t(h.seq - sLastStateSeq) > 0) &&
            decodeState(body, bn, sRemote)) sHaveRemote = true, sRemoteAge = 0, sLastStateSeq = h.seq;
        if (h.kind == Kind::GameRequest && !bn && sConnected && sActive.mode == Mode::Host) sNeedGame = true;
        if (h.kind == Kind::Game && sConnected && sActive.mode == Mode::Join && !sLoadRequested) {
            GameState game{};
            if (decodeGame(body, bn, game) && game.world != sJoinedWorld &&
                (!sHaveRemote || !sRemote.world || game.world == sRemote.world)) {
                if (!sHaveGame) log("[online] received host game: world %u map %u room %u", game.world, game.map, game.room);
                sPendingGame = game, sHaveGame = true;
            }
        }
        if (h.kind == Kind::Event && sConnected && sActive.mode == Mode::Join) {
            SceneEvent event{};
            if (decodeEvent(body, bn, event)) events::receive(event);
        }
        if (h.kind == Kind::State && sHaveRemote) {
            static std::string sLastName;
            if (sLastName != sRemote.name) sLastName = sRemote.name, log("[online] partner: %s", sRemote.name);
        }
    }
}

}  // namespace

bool wanted() { return config().mode != Mode::Off; }
int localIndex() { return sLocal.load(); }
bool connected() { return sConnected.load(); }
bool guest() { return localIndex() == 1; }
u32 world() { return guest() ? sJoinedWorld : sWorld; }
bool loading() {
    const auto stage = sJoinStage.load();
    return stage != JoinStage::Off && stage != JoinStage::Playing && !(connected() && events::busy());
}
bool joiningMap() { return guest() && sLoadRequested; }
const PlayerState* remote() {
    return sHaveRemote && sConnected && sHaveLocal && sRemote.world && sRemote.world == sLocalState.world ? &sRemote : nullptr;
}
u32 remoteAge() { return sRemoteAge; }
void setLocal(const PlayerState& s) {
    const PadState keep = sLocalState.pad;  // the pad comes from pad()
    sLocalState = s, sLocalState.pad = keep, sHaveLocal = true;
    sLocalState.world = guest() ? sJoinedWorld : sWorld;
    events::fill(sLocalState.event);
    std::snprintf(sLocalState.name, kNameSize, "%s", sActive.name.c_str());
}

const char* remoteName() { return remote() ? sRemote.name : ""; }

namespace {
std::atomic<u32> sTag{0};  // name tag: bit 31 shown, x in bits 0-15, y in bits 16-30 (640x480 picture)
std::mutex sTagNameM;
std::string sTagName;
}  // namespace

void setNameTag(bool shown, int x, int y) {
    if (shown && remote()) {
        std::lock_guard lk(sTagNameM);
        sTagName = sRemote.name;
    }
    sTag = shown ? 0x80000000u | (u32(y & 0x7FFF) << 16) | u32(x & 0xFFFF) : 0;
}

void drawNameTag() {
    const u32 t = sTag.load();
    if (!(t & 0x80000000u)) return;
    std::string name;
    {
        std::lock_guard lk(sTagNameM);
        name = sTagName;
    }
    if (name.empty()) return;
    // The 3D view is drawn Hor+ (wider, same height), so a point of the game's 640x480 picture
    // lands where the centered 4:3 picture would put it.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float scale = vp->Size.x * 3 < vp->Size.y * 4 ? vp->Size.x / 640.0f : vp->Size.y / 480.0f;
    const float gx = float(s16(u16(t & 0xFFFF))), gy = float((t >> 16) & 0x7FFF);
    const float cx = vp->Pos.x + vp->Size.x * 0.5f + (gx - 320.0f) * scale;
    const float cy = vp->Pos.y + vp->Size.y * 0.5f + (gy - 240.0f) * scale;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float size = ImGui::GetFontSize() * 1.8f;
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, name.c_str());
    const ImVec2 p(cx - ts.x * 0.5f, cy - ts.y);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 2), ImVec2(p.x + ts.x + 6, p.y + ts.y + 2), IM_COL32(0, 0, 0, 120), 4.0f);
    dl->AddText(ImGui::GetFont(), size, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 255), name.c_str());
    dl->AddText(ImGui::GetFont(), size, p, IM_COL32(255, 255, 255, 255), name.c_str());
}

void pad(PADStatus* st) {
    const int li = localIndex();
    if (li < 0) return;
    const PADStatus mine = st[0];
    sLocalState.pad = {mine.button, mine.stickX, mine.stickY, mine.substickX, mine.substickY,
                       mine.triggerLeft, mine.triggerRight};
    // A joining player never operates the title/file select, or the host's port-1 menus.
    if (li == 1 && loading()) {
        st[0] = st[1] = {};
        st[0].err = st[1].err = PAD_ERR_NONE;
        sLocalState.pad = {};
        return;
    }
    st[li] = mine;
    PADStatus& other = st[1 - li];
    other = PADStatus{};
    if (const PlayerState* r = remote(); r && sHaveLocal && r->world && r->world == sLocalState.world) {
        const PadState& p = r->pad;
        other.button = p.buttons, other.stickX = p.stickX, other.stickY = p.stickY;
        other.substickX = p.substickX, other.substickY = p.substickY;
        other.triggerLeft = p.triggerL, other.triggerRight = p.triggerR;
        other.analogA = other.analogB = 0;
    }
    st[0].err = st[1].err = PAD_ERR_NONE;
    events::pad(st, mine);
}

const char* status() {
    static thread_local std::string sCopy;
    std::lock_guard lk(sStatusM);
    sCopy = sStatus;
    return sCopy.c_str();
}

void frame(Ctx& ctx) {
    const Config c = config();
    if (c.mode != Mode::Off && !gameSha1()) {
        setStatus("error: online co-op needs the USA game (GLME01)");
        return;
    }
    if (!(c == sActive) || (c.mode != Mode::Off && !sNet && !sRejected && sStatus.rfind("error", 0) != 0)) {
        if (!(c == sActive)) closeSocket();
        sActive = c;
        sLocal = c.mode == Mode::Host ? 0 : c.mode == Mode::Join ? 1 : -1;
        sJoinStage = c.mode == Mode::Join ? JoinStage::Connecting : JoinStage::Off;
        if (c.mode == Mode::Off) return setStatus("off");
        if (!openSocket(c)) return;
    }
    if (!sNet || sRejected) return;
    // The internet transport negotiates in the background; its progress and final failures show here.
    const NetStatus net = sNet->status();
    if (net.failed && !sConnected) {
        setStatus(net.text);  // "error: ..." (the session does not retry by itself)
        sNet.reset();
        return;
    }
    if (!sConnected && net.text != sLastNetText) setStatus(net.text), sLastNetText = net.text;
    {
        std::lock_guard lk(sStatusM);
        sInvite = net.inviteCode;
    }
    sRemoteAge++;
    receive();
    if (sRejected) return;
    const auto now = Clock::now();
    const bool wasConnected = sConnected.load();
    if (wasConnected && std::chrono::duration<double>(now - sLastRecv).count() > kTimeout) {
        sConnected = false, sHaveRemote = false;
        sHaveGame = false;
        events::reset();
        if (sActive.mode == Mode::Host) sHavePeer = false;  // a new client may join
        setStatus("disconnected (no packets for 5 s)");
        return;
    }
    if (sConnected) setStatus("connected to " + sNet->describe(sPeer));
    events::frame(ctx);
    events::fill(sLocalState.event);
    sLocalState.scene = u8(rd32(kSceneId));
    sLocalState.sceneDone = sOpeningDone;
    const u32 scene = rd32(kSceneId);
    const bool opening = settled() && scene >= 4 && scene <= 7 && (scene != 4 || rd32(0x804CFFAC) == 2);
    if (opening) {
        sHaveLocal = true;
        sLocalState.world = world();
        sLocalState.map = 2;
    }
    if (!sConnected && sActive.mode == Mode::Join &&
        std::chrono::duration<double>(now - sLastHello).count() > kHelloEvery) {
        sendHello();
        sLastHello = now;
    }
    if (sConnected && ++sFrames % kStateEvery == 0) {
        u8 body[kStateWireSize];
        // Before setLocal has a player, the state carries only the pad (filled by pad()).
        send(Kind::State, body, encodeState(sLocalState, body, sizeof(body)), sPeer);
    }

    const bool inGame = settled() && rd32(kSceneId) == 2;
    if (!inGame && !opening) sHaveLocal = false, sLocalState.world = 0;
    if (sActive.mode == Mode::Host) {
        if (sNeedGame && sConnected && (opening || (inGame && (events::hostGame() || rd32(kEvent + 0xC) == 0))) && rd32(0x804D80C4) == 0 &&
            std::chrono::duration<double>(now - sLastGame).count() > kHelloEvery) {
            GameState game{};
            if (const auto* beforeEvent = events::hostGame()) game = *beforeEvent;
            else if (!captureGame(ctx, game)) return;
            u8 body[kGameWireSize];
            send(Kind::Game, body, encodeGame(game, body, sizeof(body)), sPeer);
            sNeedGame = false, sLastGame = now;
            log("[online] sent host game: world %u map %u room %u", game.world, game.map, game.room);
        }
        return;
    }
    if (!sConnected) {
        sJoinStage = JoinStage::Connecting;
        sJoinedWorld = 0;
        return;
    }
    // A shared event that ends in a map change (the lab's "Where will you go?") never reaches its end on the guest, so it stays
    // "busy"; a snapshot of a newer world means the host has already left it, and the guest follows.
    if (sHaveGame && !sLoadRequested && events::eventWorld() && sPendingGame.world != events::eventWorld()) events::reset();  // also stops its hold on the scene change
    const bool eventHoldsLoad = events::busy() && sPendingGame.world == events::eventWorld();
    if (sHaveGame && !sLoadRequested && !eventHoldsLoad && settled() && rd32(kSceneId) != 0) {
        sLoadingGame = sPendingGame;
        sHaveGame = false, sLoadRequested = true;
        sJoinedWorld = 0;
        sJoinStage = JoinStage::Loading;
        wr32(0x804D872C, 0);
        wr32(kNextMap, sLoadingGame.map);  // teardown preserves the old current map until its resources are freed
        wr32(kSceneControl + 4, sLoadingGame.scene);
        wr32(kSceneId, 0);  // normal scene teardown and reload, including main-game -> main-game
        log("[online] loading host game: world %u map %u room %u (no file select)",
            sLoadingGame.world, sLoadingGame.map, sLoadingGame.room);
        return;
    }
    if (sLoadRequested && ((sLoadingGame.scene == scene && opening) || (sLoadingGame.scene == 2 && inGame && coop::player(ctx, 0) && coop::player(ctx, 1)))) {
        sJoinedWorld = sLoadingGame.world;
        sLoadRequested = false;
        sJoinStage = JoinStage::Playing;
        log("[online] entered player 1's game: world %u map %u room %u", sJoinedWorld, sLoadingGame.map, sLoadingGame.room);
        if (sLoadingGame.scene == 4) log("[online] guest watching the opening cutscene");
    }
    if (!sLoadRequested && (!sJoinedWorld || !sHaveRemote || sRemote.world != sJoinedWorld || (!inGame && !opening))) {
        sJoinStage = JoinStage::Waiting;
        if (std::chrono::duration<double>(now - sLastGame).count() > kHelloEvery) {
            send(Kind::GameRequest, nullptr, 0, sPeer);
            sLastGame = now;
        }
    } else if (!sLoadRequested) {
        sJoinStage = JoinStage::Playing;
    }
}

void beforeMapSetup(Ctx& ctx) {
    if (!joiningMap()) return;
    events::mapSetup();
    static const u32 buffer = hleAlloc(kSaveSize);
    std::memcpy(hostp(buffer), sLoadingGame.save, kSaveSize);
    gu::call(ctx, GU_SYM("fn_80067050"), {buffer});
    // Player 2 shares progress bits and equipment; starting with player 1's treasure counters
    // would count all the host's existing money/captures twice when mergeProgress runs.
    constexpr u32 rec0 = 0x803D5CD0, rec1 = rec0 + 0x16C;
    gu::call(ctx, GU_SYM("fn_80072B84"), {rec1});
    wr32(rec1, rd32(rec0));
    std::memcpy(hostp(rec1 + 0xD4), hostp(rec0 + 0xD4), 0x16C - 0xD4);
    wr32(0x804D80C0, sLoadingGame.hidden);
    wr32(kMap, sLoadingGame.map);
}

constexpr float kSpawnGap = 70.0f;  // game units between the two players at a join

void onMapSetup(Ctx& ctx) {
    if (ver::pal()) return;
    sOpeningDone = false;
    if (localIndex() == 0) {
        events::mapSetup();
        ++sWorld;
        sHaveLocal = false, sLocalState.world = 0;
    } else if (joiningMap()) {
        std::memcpy(hostp(kFlags), sLoadingGame.flags, sizeof(sLoadingGame.flags));
        for (int i = 0; i < 2; i++) {
            if (const u32 p = coop::player(ctx, i)) {
                for (int k = 0; k < 3; k++) gu::wrf(p + 0x44 + 4 * k, sLoadingGame.pos[k]);
                // Player 2 stands beside the host's player instead of inside it.
                if (i == 1) gu::wrf(p + 0x44, sLoadingGame.pos[0] + std::cos(sLoadingGame.rotY) * kSpawnGap),
                            gu::wrf(p + 0x4C, sLoadingGame.pos[2] - std::sin(sLoadingGame.rotY) * kSpawnGap);
                gu::wrf(p + 0xF48, std::sin(sLoadingGame.rotY));
                gu::wrf(p + 0xF50, std::cos(sLoadingGame.rotY));
            }
        }
        sSpawnTool = 0, sSpawnEntry = -1;
    }
}

bool captureGame(Ctx& ctx, GameState& game) {
    const u32 scene = rd32(kSceneId);
    if (!sWorld || (scene != 2 && (scene < 4 || scene > 7))) return false;
    const u32 p = scene == 2 ? coop::player(ctx, 0) : 0;
    if (scene == 2 && !p) return false;
    game = {};
    game.world = sWorld, game.scene = u8(scene), game.map = scene == 2 ? u16(rd32(kMap)) : 2;
    game.rank = scene == 6 ? u8(rd32(0x804D8100)) : 0;
    game.room = p ? u8(rd32(p + 0xB4)) : 0;
    game.hidden = rd32(0x804D80C0) != 0;
    if (p) {
        for (int k = 0; k < 3; k++) game.pos[k] = gu::rdf(p + 0x44 + 4 * k);
        game.rotY = std::atan2(gu::rdf(p + 0xF48), gu::rdf(p + 0xF50));
    }
    static const u32 buffer = hleAlloc(kSaveSize);
    std::memset(hostp(buffer), 0, kSaveSize);
    if (p) gu::call(ctx, GU_SYM("fn_800AEEA8"), {p, 59, 0});
    gu::call(ctx, GU_SYM("fn_80067108"), {buffer});
    std::memcpy(game.save, hostp(buffer), kSaveSize);
    std::memcpy(game.flags, hostp(kFlags), sizeof(game.flags));
    return true;
}

void sendEvent(const SceneEvent& event) {
    if (!sNet || !sConnected || sActive.mode != Mode::Host) return;
    u8 body[kEventWireSize];
    send(Kind::Event, body, encodeEvent(event, body, sizeof(body)), sPeer);
}

void noteSpawnEntry(u32 tool, int entry) {
    if (joiningMap()) sSpawnTool = tool, sSpawnEntry = entry;
}
void spawnValue(u32 tool, int entry, const char* field, u32 out) {
    if (!joiningMap() || tool != sSpawnTool || entry != sSpawnEntry) return;
    const int axis = !std::strcmp(field, "pos_x") ? 0 : !std::strcmp(field, "pos_y") ? 1 : !std::strcmp(field, "pos_z") ? 2 : -1;
    if (axis >= 0) gu::wrf(out, sLoadingGame.pos[axis]);
}

HLE(fn_8000BA4C) {
    if (guest() && loading()) { c.r[3] = 2; return; }
    // A guest's save belongs to the host; never open a local save prompt.
    if (guest() && rd32(0x804D80C4) == 5) wr32(0x804D80C4, 0);
    callOriginal(c, "fn_8000BA4C");
}

static void sceneSetup(Ctx& c, u32 scene, const char* symbol) {
    if (wanted()) {
        sOpeningDone = false;
        setNameTag(false, 0, 0);
        if (guest() && joiningMap()) {
            beforeMapSetup(c);
            std::memcpy(hostp(kFlags), sLoadingGame.flags, sizeof(sLoadingGame.flags));
            if (scene == 4) wr32(0x804CFFAC, 2);
            if (scene == 6) wr32(0x804D8100, sLoadingGame.rank);
        } else if (localIndex() == 0 && (scene != 4 || rd32(0x804CFFAC) == 2)) {
            events::mapSetup();
            ++sWorld;
            sHaveLocal = false, sLocalState.world = 0;
        }
    }
    callOriginal(c, symbol);
}

static void sceneUpdate(Ctx& c, u32 scene, const char* symbol) {
    if (!wanted() || (scene == 4 && rd32(0x804CFFAC) != 2)) return callOriginal(c, symbol);
    const auto* p = remote();
    if (connected() && (!p || p->scene != scene || p->world != world())) { c.r[3] = scene; return; }
    if (!sOpeningDone) {
        callOriginal(c, symbol);
        if (c.r[3] != scene) sOpeningDone = true, sOpeningTarget = c.r[3];
    }
    if (sOpeningDone) {
        // The guest leaves through the next host snapshot, so the opening is never torn down early.
        c.r[3] = guest() || (connected() && (!p || !p->sceneDone)) ? scene : sOpeningTarget;
    }
}

HLE(fn_8000CE58) { sceneSetup(c, 4, "fn_8000CE58"); }
HLE(fn_8000CEF0) { sceneUpdate(c, 4, "fn_8000CEF0"); }
HLE(fn_8000CFC8) { sceneSetup(c, 5, "fn_8000CFC8"); }
HLE(fn_8000CFE8) { sceneUpdate(c, 5, "fn_8000CFE8"); }
HLE(fn_8000D1CC) { sceneSetup(c, 6, "fn_8000D1CC"); }
HLE(fn_8000D624) { sceneUpdate(c, 6, "fn_8000D624"); }
HLE(fn_8000EF58) { sceneSetup(c, 7, "fn_8000EF58"); }
HLE(fn_8000F020) { sceneUpdate(c, 7, "fn_8000F020"); }

void drawLoadingScreen() {
    if (!loading()) return;
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    ImGui::Begin("Joining player 1##online-loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto* dl = ImGui::GetWindowDrawList();
#ifndef LM_STATIC_MODULE
    int w = 0, h = 0;
    if (const ImTextureID texture = setup::loadingTexture(&w, &h); texture && w && h) {
        const float scale = std::min(vp->Size.x / w, (vp->Size.y - 56) / h);
        const ImVec2 a(vp->Pos.x + (vp->Size.x - w * scale) * 0.5f, vp->Pos.y + (vp->Size.y - 56 - h * scale) * 0.5f);
        dl->AddImage(texture, a, ImVec2(a.x + w * scale, a.y + h * scale));
    }
#endif
    const auto stage = sJoinStage.load();
    const char* message = stage == JoinStage::Loading ? "Loading player 1's game..." : stage == JoinStage::Waiting ?
        "Waiting for player 1 to enter the game..." : "Connecting to player 1...";
    ImGui::SetCursorPos(ImVec2(12, std::max(12.0f, vp->Size.y - 54)));
    ImGui::TextUnformatted(message);
    ImGui::TextDisabled("%s", status());
    ImGui::SetCursorPos(ImVec2(std::max(12.0f, vp->Size.x - 140), std::max(12.0f, vp->Size.y - 46)));
    if (menu::Button("Back to start")) port::relaunch();
    ImGui::End();
    ImGui::PopStyleColor();
}

void setSessionMode(int mode) {
    sSessionMode = mode;
    sJoinStage = mode == 2 ? JoinStage::Connecting : JoinStage::Off;
}

bool readyToStart() {
    std::lock_guard lk(sCfgM);
    if (sCfg.mode == Mode::Off) return false;
    if (sCfg.internet)
        return (sCfg.steam || !sCfg.signalUrl.empty()) && (sCfg.mode == Mode::Host || !sCfg.inviteCode.empty() || !sCfg.lobbyId.empty());
    return !sCfg.address.empty() && sCfg.port > 0 && sCfg.port <= 65535;
}

Config savedConfig() {
    std::lock_guard lk(sCfgM);
    return sCfg;
}
void setSavedConfig(const Config& c) {
    std::lock_guard lk(sCfgM);
    sCfg = c;
}
std::string inviteCode() {
    std::lock_guard lk(sStatusM);
    return sInvite;
}

int configuredMode() {
    std::lock_guard lk(sCfgM);
    return int(sCfg.mode);
}

Config sessionConfig() { return config(); }
std::unique_ptr<Transport> makeTransport(const Config& c) {
    return !c.internet ? makeUdpTransport(netConfig(c)) : c.steam ? makeSteamTransport(netConfig(c)) : makeRtcTransport(netConfig(c));
}
void adoptTransport(std::unique_ptr<Transport> t) {
    std::lock_guard lk(sAdoptM);
    sAdopted = std::move(t);
}

bool drawHostJoin(bool withOff) {
    bool changed = false;
    Config cfg;
    {
        std::lock_guard lk(sCfgM);
        cfg = sCfg;
    }
    int mode = int(cfg.mode);
    if (withOff) {
        changed |= ImGui::RadioButton("Off", &mode, 0);
        ImGui::SameLine();
    }
    changed |= ImGui::RadioButton("Host (play as Luigi)", &mode, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Join (play as Mario)", &mode, 2);
    cfg.mode = Mode(mode);
    static char sAddr[64], sPw[64];
    static bool sInit = false;
    if (!sInit) {
        std::snprintf(sAddr, sizeof(sAddr), "%s", cfg.address.c_str());
        std::snprintf(sPw, sizeof(sPw), "%s", cfg.password.c_str());
        sInit = true;
    }
    static char sName[kNameSize];
    static bool sNameInit = false;
    if (!sNameInit) std::snprintf(sName, sizeof(sName), "%s", cfg.name.c_str()), sNameInit = true;
    ImGui::SetNextItemWidth(220.0f * ImGui::GetFontSize() / 13.0f);
    if (ImGui::InputText("Your name", sName, sizeof(sName)))
        cfg.name = sName, changed = true;
    ImGui::SetNextItemWidth(220.0f * ImGui::GetFontSize() / 13.0f);
    if (ImGui::InputText(cfg.mode == Mode::Join ? "Host's address" : "This PC's address", sAddr, sizeof(sAddr)) ||
        ImGui::IsItemDeactivatedAfterEdit())
        cfg.address = sAddr, changed = true;
    ImGui::SetNextItemWidth(120.0f * ImGui::GetFontSize() / 13.0f);
    changed |= ImGui::InputInt("Port", &cfg.port);
    ImGui::SetNextItemWidth(220.0f * ImGui::GetFontSize() / 13.0f);
    if (ImGui::InputText("Password", sPw, sizeof(sPw), ImGuiInputTextFlags_Password))
        cfg.password = sPw, changed = true;
    if (changed) {
        std::lock_guard lk(sCfgM);
        sCfg = cfg;
    }
    return changed;
}

// The lobby while a session is configured: invite code, who is in, host, leave.
void drawLobbyPanel() {
    const Config c = config();
    if (c.mode == Mode::Off) return;
    ImGui::SeparatorText(c.internet ? "Online lobby" : "Direct connection");
    if (c.internet && c.mode == Mode::Host) {
        const std::string code = inviteCode();
        if (code.empty()) ImGui::TextDisabled("Invite code: (registering with the matchmaking server...)");
        else {
            ImGui::Text("Invite code: %s", code.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(code.c_str());
            ImGui::TextDisabled("Your friend picks Join by Code and types it. %s lobby.", c.publicLobby ? "Public" : "Private");
        }
    }
    ImGui::Text("Host: %s", c.mode == Mode::Host ? (c.name + " (you)").c_str() : remoteName()[0] ? remoteName() : "(connecting)");
    ImGui::Text("Players (%d/2):", connected() ? 2 : 1);
    ImGui::BulletText("%s (you, %s)", c.name.c_str(), c.mode == Mode::Host ? "Luigi" : "Mario");
    if (connected()) ImGui::BulletText("%s (%s)", remoteName()[0] ? remoteName() : "partner", c.mode == Mode::Host ? "Mario" : "Luigi");
    if (menu::Button("Leave lobby")) port::relaunch();  // back to the start menu
}

bool drawTab() {
    ImGui::TextWrapped("Play the 2-player mode with a friend on another PC: you are Luigi when you host, Mario "
                       "when you join. Both PCs need their own copy of the USA game. Pick Multiplayer > Host Online "
                       "or Join by Code on the start menu to play over the internet without a VPN, or use the "
                       "address fields below (Direct Connect) on a LAN or a Radmin VPN. The host "
                       "chooses a save. Joining loads that game directly; your own saves stay read-only. "
                       "Both PCs need the same current build. Hosting enables two players at the next map load.");
    ImGui::Spacing();
    bool changed = false;
    if (!savedConfig().internet) changed = drawHostJoin(true);
    ImGui::Text("Status: %s", status());
    drawLobbyPanel();
    return changed;
}

}  // namespace recomp::online
