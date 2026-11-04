// The pre-game lobby. Both PCs send a small Kind::Lobby packet twice a second (name, and for the host
// whether the game has started) over the transport that the session will keep using: Steam lobbies and
// relay, or the WebRTC channel. Names and avatars come from Steam when it is the transport.
#include "recomp/online_room.h"

#include "recomp/online_lobby.h"
#include "recomp/online_protocol.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::online::room {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kSendEvery = std::chrono::milliseconds(500);
constexpr auto kGoneAfter = std::chrono::seconds(4);

std::unique_ptr<Transport> sT;
Config sCfg;
State sS;
std::string sOtherName;
Clock::time_point sLastSend{}, sOtherSeen{}, sAutoSince{};
u32 sSeq = 0;
PeerId sPeer = 0;
bool sSeen = false;

void sendLobby(bool started) {
    if (!sT) return;
    LobbyMsg m{};
    std::snprintf(m.name, kNameSize, "%s", sCfg.name.c_str());
    m.started = started;
    u8 body[kLobbyWireSize], buf[kMaxPacket];
    const size_t bn = encodeLobby(m, body, sizeof(body));
    const size_t n = encode(Header{kMagic, Kind::Lobby, ++sSeq}, body, bn, buf, sizeof(buf));
    if (n) sT->send(sPeer, buf, n, true);
}
}  // namespace

bool open(const Config& c) {
    leave();
    sCfg = c;
    sS = State{};
    sS.open = true, sS.host = c.mode == Mode::Host;
    sS.status = "opening the lobby...";
    sT = makeTransport(c);
    sPeer = sT->initialPeer();
    sSeen = false, sOtherName.clear();
    sLastSend = sOtherSeen = Clock::now();
    return true;
}

State update() {
    if (!sT) return sS;
    const auto now = Clock::now();
    const NetStatus n = sT->status();
    if (n.text != sS.status) log("[lobby] %s", n.text.c_str());
    sS.status = n.text, sS.code = n.inviteCode, sS.failed = n.failed;
    u8 buf[kMaxPacket];
    for (PeerId from; const size_t len = sT->receive(from, buf, sizeof(buf));) {
        Header h;
        const u8* body;
        size_t bn;
        LobbyMsg m;
        if (!decode(buf, len, h, body, bn) || h.kind != Kind::Lobby || !decodeLobby(body, bn, m)) continue;
        sPeer = from, sOtherSeen = now, sSeen = true;
        sOtherName = m.name;
        if (!sS.host && m.started) sS.started = true;
    }
    if (n.linked && !n.failed && now - sLastSend >= kSendEvery) sendLobby(sS.started && sS.host), sLastSend = now;

    const bool steam = sCfg.steam && sCfg.internet;
    sS.other.present = sSeen && now - sOtherSeen < kGoneAfter;
    sS.me.present = !n.failed;
    sS.me.name = steam ? steamName(0) : sCfg.name;
    if (sS.me.name.empty()) sS.me.name = sCfg.name;
    sS.me.avatar = steam ? steamAvatar(0) : 0;
    if (sS.other.present) {
        sS.other.name = steam ? steamName(sT->peerSteamId()) : std::string();
        if (sS.other.name.empty()) sS.other.name = sOtherName;
        sS.other.avatar = steam ? steamAvatar(sT->peerSteamId()) : 0;
    } else {
        sS.other = Player{};
    }
    return sS;
}

void start() {
    if (!sT || !sS.host) return;
    sS.started = true;
    for (int i = 0; i < 8; i++) sendLobby(true);  // a burst: UDP may drop some, and the host stops listening now
    log("[lobby] host started the game");
}

void finish() {
    if (!sT) return;
    log("[lobby] game starting (%s)", sS.host ? "host" : "guest");
    adoptTransport(std::move(sT));
    sS = State{};
}

void leave() {
    sT.reset();
    sS = State{};
}

bool autopilot() {
    const char* v = std::getenv("LM_LOBBY_AUTO");
    if (!v || !*v) return false;
    static bool opened = false;
    if (!opened) {
        opened = true;
        Config c = sessionConfig();
        c.mode = !std::strcmp(v, "host") ? Mode::Host : Mode::Join;  // internet / steam / direct as LM_ONLINE_NET says
        setSavedConfig(c);
        open(c);
        log("[lobby] auto %s", v);
        return true;
    }
    if (sS.host && sS.other.present && !std::getenv("LM_LOBBY_NOSTART")) {
        if (sAutoSince == Clock::time_point{}) sAutoSince = Clock::now();
        if (Clock::now() - sAutoSince > std::chrono::seconds(3) && !sS.started) start();
    }
    return false;
}

}  // namespace recomp::online::room
