// Online co-op session: docs/UNUSED_CONTENT.md "2-player mode", design in the C:\LM spec
// (2026-11-01-online-coop-design.md). The host binds the address the user chose (a Radmin VPN
// 26.x.x.x one, or 127.0.0.1 for two copies on one PC) and accepts one client that knows the
// password and runs the same game (GLME01). Both sides then exchange their player's state
// (online_protocol.h) at 30 Hz; 5 s without a packet ends the session.
#include "recomp/online.h"

#include "port/dol.h"
#include "port/settings.h"
#include "recomp/menu.h"

#include <dolphin/pad.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

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

// ---- Settings ([online]); LM_ONLINE, LM_ONLINE_ADDR, LM_ONLINE_PORT, LM_ONLINE_PASSWORD override ----
enum class Mode : int { Off, Host, Join };
struct Config {
    Mode mode = Mode::Off;
    std::string address;  // host: the address to listen on; join: the host's address
    int port = 28555;
    std::string password;
    bool operator==(const Config&) const = default;
};
std::mutex sCfgM;
Config sCfg;

void resetConfig() {
    std::lock_guard lk(sCfgM);
    sCfg = Config{};
}
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "online") return;
    std::lock_guard lk(sCfgM);
    if (key == "mode") sCfg.mode = value == "host" ? Mode::Host : value == "join" ? Mode::Join : Mode::Off;
    else if (key == "address") sCfg.address = value;
    else if (key == "port") sCfg.port = std::atoi(value.c_str());
    else if (key == "password") sCfg.password = value;
}
void saveConfig(std::ostream& out) {
    std::lock_guard lk(sCfgM);
    out << "[online]\nmode=" << (sCfg.mode == Mode::Host ? "host" : sCfg.mode == Mode::Join ? "join" : "off")
        << "\naddress=" << sCfg.address << "\nport=" << sCfg.port << "\npassword=" << sCfg.password << '\n';
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
    return c;
}

// ---- Session (guest thread; status read by the menu) ------------------------------------
struct Peer {
    sockaddr_in addr{};
    bool operator==(const Peer& o) const {
        return addr.sin_addr.s_addr == o.addr.sin_addr.s_addr && addr.sin_port == o.addr.sin_port;
    }
};
std::mutex sStatusM;
std::string sStatus = "off";
std::atomic<int> sLocal{-1};
std::atomic<bool> sConnected{false};

SocketHandle sSock = kNoSocket;
Config sActive;  // the config the socket was opened with
bool sRejected = false;
Peer sPeer;
bool sHavePeer = false;
Clock::time_point sLastRecv, sLastHello;
u32 sSeq = 0, sFrames = 0;
PlayerState sLocalState{}, sRemote{};
bool sHaveLocal = false, sHaveRemote = false;
u32 sRemoteAge = 0;

void setStatus(const std::string& s) {
    std::lock_guard lk(sStatusM);
    if (s == sStatus) return;
    sStatus = s;
    log("[online] %s", s.c_str());
}

void closeSocket() {
    if (sSock != kNoSocket) {
#ifdef _WIN32
        closesocket(sSock);
#else
        ::close(sSock);
#endif
    }
    sSock = kNoSocket;
    sHavePeer = sHaveRemote = sRejected = false;
    sConnected = false;
}

std::array<u8, 32> passwordHash(const std::string& pw) {
    const std::string salted = "lmco-v1:" + pw;
    const auto h = port::dol::sha1(reinterpret_cast<const u8*>(salted.data()), salted.size());
    std::array<u8, 32> out{};
    std::memcpy(out.data(), h.data(), h.size());
    return out;
}

bool openSocket(const Config& c) {
#ifdef _WIN32
    static const bool sWsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!sWsa) return setStatus("error: Winsock unavailable"), false;
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    if (c.port <= 0 || c.port > 65535) return setStatus("error: port must be 1-65535"), false;
    if (c.mode == Mode::Host) {
        // The host listens on the chosen address only (never every interface).
        if (c.address.empty() || inet_pton(AF_INET, c.address.c_str(), &a.sin_addr) != 1)
            return setStatus("error: host needs this PC's address (e.g. its Radmin VPN 26.x.x.x)"), false;
        a.sin_port = htons(u16(c.port));
    } else {
        if (c.address.empty() || inet_pton(AF_INET, c.address.c_str(), &sPeer.addr.sin_addr) != 1)
            return setStatus("error: enter the host's address"), false;
        sPeer.addr.sin_family = AF_INET;
        sPeer.addr.sin_port = htons(u16(c.port));
        sHavePeer = true;
    }
    sSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSock == kNoSocket) return setStatus("error: no UDP socket"), false;
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(sSock, FIONBIO, &nb);
#else
    fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL) | O_NONBLOCK);
#endif
    if (bind(sSock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closeSocket();
        return setStatus("error: cannot listen on " + c.address + ":" + std::to_string(c.port)), false;
    }
    sLastRecv = sLastHello = Clock::now() - std::chrono::seconds(1);
    setStatus(c.mode == Mode::Host ? "waiting on " + c.address + ":" + std::to_string(c.port)
                                   : "waiting: joining " + c.address + ":" + std::to_string(c.port));
    return true;
}

void send(Kind kind, const u8* body, size_t n, const Peer& to) {
    u8 buf[kMaxPacket];
    const size_t len = encode(Header{kMagic, kind, sSeq++}, body, n, buf, sizeof(buf));
    if (len) sendto(sSock, reinterpret_cast<const char*>(buf), int(len), 0,
                    reinterpret_cast<const sockaddr*>(&to.addr), sizeof(to.addr));
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
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        const int n = int(recvfrom(sSock, reinterpret_cast<char*>(buf), int(sizeof(buf)), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fl));
        if (n <= 0) return;
        Header h{};
        const u8* body = nullptr;
        size_t bn = 0;
        if (!decode(buf, size_t(n), h, body, bn)) continue;
        const Peer p{from};
        if (sActive.mode == Mode::Host) {
            if (h.kind == Kind::Hello && (!sHavePeer || p == sPeer)) {
                if (const std::string why = checkHello(body, bn); !why.empty()) {
                    send(Kind::Reject, reinterpret_cast<const u8*>(why.data()), why.size(), p);
                    static u32 sRefused = 0;  // junk must not flood the log: the first 5, then every 1000th
                    if (++sRefused <= 5 || sRefused % 1000 == 0) log("[online] refused a join (%u): %s", sRefused, why.c_str());
                    continue;
                }
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
        if (h.kind == Kind::State && decodeState(body, bn, sRemote)) sHaveRemote = true, sConnected = true, sRemoteAge = 0;
    }
}

}  // namespace

bool wanted() { return config().mode != Mode::Off; }
int localIndex() { return sLocal.load(); }
bool connected() { return sConnected.load(); }
const PlayerState* remote() { return sHaveRemote && sConnected ? &sRemote : nullptr; }
u32 remoteAge() { return sRemoteAge; }
void setLocal(const PlayerState& s) {
    const PadState keep = sLocalState.pad;  // the pad comes from pad()
    sLocalState = s, sLocalState.pad = keep, sHaveLocal = true;
}

void pad(PADStatus* st) {
    const int li = localIndex();
    if (li < 0) return;
    const PADStatus mine = st[0];
    sLocalState.pad = {mine.button, mine.stickX, mine.stickY, mine.substickX, mine.substickY,
                       mine.triggerLeft, mine.triggerRight};
    st[li] = mine;
    PADStatus& other = st[1 - li];
    other = PADStatus{};
    if (const PlayerState* r = remote()) {
        const PadState& p = r->pad;
        other.button = p.buttons, other.stickX = p.stickX, other.stickY = p.stickY;
        other.substickX = p.substickX, other.substickY = p.substickY;
        other.triggerLeft = p.triggerL, other.triggerRight = p.triggerR;
        other.analogA = other.analogB = 0;
    }
    st[0].err = st[1].err = PAD_ERR_NONE;
}

const char* status() {
    static thread_local std::string sCopy;
    std::lock_guard lk(sStatusM);
    sCopy = sStatus;
    return sCopy.c_str();
}

void frame() {
    const Config c = config();
    if (!(c == sActive) || (c.mode != Mode::Off && sSock == kNoSocket && !sRejected && sStatus.rfind("error", 0) != 0)) {
        if (!(c == sActive)) closeSocket();
        sActive = c;
        sLocal = c.mode == Mode::Host ? 0 : c.mode == Mode::Join ? 1 : -1;
        if (c.mode == Mode::Off) return setStatus("off");
        if (!openSocket(c)) return;
    }
    if (sSock == kNoSocket || sRejected) return;
    sRemoteAge++;
    receive();
    if (sRejected) return;
    const auto now = Clock::now();
    const bool wasConnected = sConnected.load();
    if (wasConnected && std::chrono::duration<double>(now - sLastRecv).count() > kTimeout) {
        sConnected = false, sHaveRemote = false;
        if (sActive.mode == Mode::Host) sHavePeer = false;  // a new client may join
        setStatus("disconnected (no packets for 5 s)");
        return;
    }
    if (sConnected) setStatus("connected to " + std::string(inet_ntoa(sPeer.addr.sin_addr)) + ":" +
                              std::to_string(ntohs(sPeer.addr.sin_port)));
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
}

void setSessionMode(int mode) { sSessionMode = mode; }

bool readyToStart() {
    std::lock_guard lk(sCfgM);
    return sCfg.mode != Mode::Off && !sCfg.address.empty() && sCfg.port > 0 && sCfg.port <= 65535;
}

int configuredMode() {
    std::lock_guard lk(sCfgM);
    return int(sCfg.mode);
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

bool drawTab() {
    ImGui::TextWrapped("Play the 2-player mode with a friend on another PC: you are Luigi when you host, Mario "
                       "when you join. Both PCs need their own copy of the USA game. Over the internet, put both "
                       "PCs in one Radmin VPN network and use the host's Radmin address (26.x.x.x). Changes take "
                       "effect at once; the 2-player mode itself starts at the next map load.");
    ImGui::Spacing();
    const bool changed = drawHostJoin(true);
    ImGui::Text("Status: %s", status());
    return changed;
}

}  // namespace recomp::online
