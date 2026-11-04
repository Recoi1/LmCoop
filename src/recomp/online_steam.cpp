// Steam transport for online co-op (optional: built only with -DLM_STEAM_SDK_DIR=<steamworks sdk folder>).
// Lobbies, invite codes and the relay all come from Steam; there is no matchmaking server. The game runs
// as Steam app 480 ("Spacewar", Valve's public test app), so every LmCoop lobby carries lmcoop=1 and the
// game build, and searches filter on those. Datagrams travel as ISteamNetworkingMessages (Steam's relay
// network hides both players' addresses). Everything is polled from the guest thread (the transport
// contract): Steam callbacks are pumped from status(), which the session calls every frame.
#include "recomp/online_config.h"
#include "recomp/online_lobby.h"
#include "recomp/online_transport.h"

#include <string>

namespace recomp::online {

#if defined(LM_STEAM) && defined(_WIN32)

}  // namespace recomp::online

#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace recomp::online {
namespace {

constexpr uint32 kAppId = 480;  // Valve's "Spacewar" test app

bool sInit = false;
std::string sInitError;

// steam_appid.txt next to the exe tells Steam which app this is when the game is not started from Steam.
bool ensureSteam(std::string& err) {
    if (sInit) return true;
    if (!sInitError.empty()) return err = sInitError, false;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto idFile = std::filesystem::path(exe).parent_path() / "steam_appid.txt";
    std::error_code ec;
    if (!std::filesystem::exists(idFile, ec)) std::ofstream(idFile) << kAppId << '\n';
    SteamErrMsg msg{};
    if (SteamAPI_InitEx(&msg) != k_ESteamAPIInitResult_OK) {
        sInitError = std::string("error: could not start Steam (") + msg + "). Is Steam running and logged in?";
        return err = sInitError, false;
    }
    sInit = true;
    return true;
}

std::string newCode() {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no I O 0 1, like the matchmaking server
    std::random_device rd;
    std::string code;
    for (int i = 0; i < 6; i++) code += alphabet[rd() % (sizeof(alphabet) - 1)];
    return code;
}

// ---- Lobby browser (Browse Lobbies) ------------------------------------------------------------
class Browser {
public:
    void refresh() {
        std::string err;
        if (!ensureSteam(err)) {
            list_.error = err.substr(7), list_.busy = false, list_.loaded = true;
            return;
        }
        list_.busy = true, list_.error.clear();
        SteamMatchmaking()->AddRequestLobbyListStringFilter("lmcoop", "1", k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListStringFilter("listed", "1", k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        SteamMatchmaking()->AddRequestLobbyListResultCountFilter(50);
        call_.Set(SteamMatchmaking()->RequestLobbyList(), this, &Browser::onList);
    }
    LobbyList snapshot() {
        if (sInit) SteamAPI_RunCallbacks();
        return list_;
    }

private:
    void onList(LobbyMatchList_t* r, bool ioFailure) {
        list_.busy = false, list_.loaded = true, list_.lobbies.clear();
        if (ioFailure) return void(list_.error = "Steam did not answer");
        for (uint32 i = 0; i < r->m_nLobbiesMatching; i++) {
            const CSteamID id = SteamMatchmaking()->GetLobbyByIndex(int(i));
            LobbyInfo l;
            l.id = std::to_string(id.ConvertToUint64());
            l.name = SteamMatchmaking()->GetLobbyData(id, "name");
            l.build = SteamMatchmaking()->GetLobbyData(id, "build");
            l.players = SteamMatchmaking()->GetNumLobbyMembers(id), l.max = 2;
            l.password = std::string(SteamMatchmaking()->GetLobbyData(id, "pw")) == "1";
            list_.lobbies.push_back(std::move(l));
        }
    }
    LobbyList list_;
    CCallResult<Browser, LobbyMatchList_t> call_;
};
Browser& browser() {
    static Browser b;
    return b;
}

// ---- The session transport ---------------------------------------------------------------------
class SteamTransport final : public Transport {
public:
    explicit SteamTransport(const NetConfig& c) : cfg_(c) {
        std::string err;
        if (!ensureSteam(err)) {
            fail(err);
            return;
        }
        me_ = SteamUser()->GetSteamID();
        if (cfg_.host) {
            text_ = "Steam: creating the lobby...";
            created_.Set(SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 2), this, &SteamTransport::onCreated);
        } else if (!cfg_.lobbyId.empty()) {
            text_ = "Steam: joining the lobby...";
            const uint64 id = std::strtoull(cfg_.lobbyId.c_str(), nullptr, 10);
            if (!id) {
                fail("error: bad lobby id");
                return;
            }
            joinLobby(CSteamID(id));
        } else {
            text_ = "Steam: looking for the lobby...";
            SteamMatchmaking()->AddRequestLobbyListStringFilter("lmcoop", "1", k_ELobbyComparisonEqual);
            SteamMatchmaking()->AddRequestLobbyListStringFilter("code", cfg_.inviteCode.c_str(), k_ELobbyComparisonEqual);
            SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
            found_.Set(SteamMatchmaking()->RequestLobbyList(), this, &SteamTransport::onFound);
        }
    }
    ~SteamTransport() override {
        if (!sInit) return;
        if (peer_.IsValid()) {
            SteamNetworkingIdentity who;
            who.SetSteamID(peer_);
            SteamNetworkingMessages()->CloseSessionWithUser(who);
        }
        if (lobby_.IsValid()) SteamMatchmaking()->LeaveLobby(lobby_);
    }

    void send(PeerId, const u8* data, std::size_t n, bool reliable) override {
        if (!linked_ || !peer_.IsValid() || n == 0 || n > kMaxPacket) return;
        SteamNetworkingIdentity who;
        who.SetSteamID(peer_);
        const int flags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable) |
                          k_nSteamNetworkingSend_NoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession;
        SteamNetworkingMessages()->SendMessageToUser(who, data, uint32(n), flags, 0);
    }

    std::size_t receive(PeerId& from, u8* out, std::size_t cap) override {
        if (!sInit || !linked_) return 0;
        if (inbox_.empty()) {
            SteamNetworkingMessage_t* msgs[16];
            const int got = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, msgs, 16);
            for (int i = 0; i < got; i++) {
                const CSteamID sender = msgs[i]->m_identityPeer.GetSteamID();
                const size_t n = size_t(msgs[i]->m_cbSize);
                if (sender == peer_ && n > 0 && n <= kMaxPacket && inbox_.size() < 512)
                    inbox_.emplace_back(static_cast<const u8*>(msgs[i]->m_pData), static_cast<const u8*>(msgs[i]->m_pData) + n);
                msgs[i]->Release();
            }
        }
        if (inbox_.empty()) return 0;
        const std::vector<u8> d = std::move(inbox_.front());
        inbox_.pop_front();
        if (d.size() > cap) return 0;
        std::copy(d.begin(), d.end(), out);
        from = 1;
        return d.size();
    }

    NetStatus status() override {
        if (sInit) {
            SteamAPI_RunCallbacks();
            if (cfg_.host && lobby_.IsValid() && !failed_) pollGuest();
        }
        NetStatus s;
        s.text = text_, s.inviteCode = code_, s.failed = failed_, s.linked = linked_;
        return s;
    }
    std::string describe(PeerId) const override { return "the other player over Steam"; }
    PeerId initialPeer() const override { return cfg_.host ? 0 : 1; }

private:
    void fail(const std::string& why) {
        failed_ = true, text_ = why.rfind("error", 0) == 0 ? why : "error: " + why;
    }

    // Host: the lobby exists; publish what a joiner searches on.
    void onCreated(LobbyCreated_t* r, bool ioFailure) {
        if (ioFailure || r->m_eResult != k_EResultOK) return fail("Steam could not create the lobby");
        lobby_ = CSteamID(r->m_ulSteamIDLobby);
        code_ = newCode();
        auto* mm = SteamMatchmaking();
        mm->SetLobbyData(lobby_, "lmcoop", "1");
        mm->SetLobbyData(lobby_, "code", code_.c_str());
        mm->SetLobbyData(lobby_, "build", cfg_.build.c_str());
        mm->SetLobbyData(lobby_, "name", cfg_.lobbyName.c_str());
        mm->SetLobbyData(lobby_, "listed", cfg_.publicLobby ? "1" : "0");
        mm->SetLobbyData(lobby_, "pw", cfg_.hasPassword ? "1" : "0");
        text_ = "Steam lobby " + code_ + ": waiting for a player";
    }
    // Host: the other lobby member is the only one allowed to open a session with us.
    void pollGuest() {
        const int n = SteamMatchmaking()->GetNumLobbyMembers(lobby_);
        CSteamID other;
        for (int i = 0; i < n; i++)
            if (const CSteamID m = SteamMatchmaking()->GetLobbyMemberByIndex(lobby_, i); m != me_) other = m;
        if (other.IsValid() && other != peer_) {
            peer_ = other;
            SteamNetworkingIdentity who;
            who.SetSteamID(peer_);
            SteamNetworkingMessages()->AcceptSessionWithUser(who);
            linked_ = true, text_ = "Steam lobby " + code_ + ": a player joined";
        } else if (!other.IsValid() && peer_.IsValid()) {
            peer_ = CSteamID(), linked_ = false, text_ = "Steam lobby " + code_ + ": waiting for a player";
        }
    }

    // Guest: the search by invite code came back.
    void onFound(LobbyMatchList_t* r, bool ioFailure) {
        if (ioFailure) return fail("Steam did not answer");
        if (r->m_nLobbiesMatching == 0) return fail("no lobby with that code");
        joinLobby(SteamMatchmaking()->GetLobbyByIndex(0));
    }
    void joinLobby(CSteamID id) {
        entered_.Set(SteamMatchmaking()->JoinLobby(id), this, &SteamTransport::onEntered);
    }
    void onEntered(LobbyEnter_t* r, bool ioFailure) {
        if (ioFailure || r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
            return fail(r && r->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseFull ? "the lobby is full" : "could not join the Steam lobby");
        lobby_ = CSteamID(r->m_ulSteamIDLobby);
        auto* mm = SteamMatchmaking();
        if (std::string(mm->GetLobbyData(lobby_, "lmcoop")) != "1") return leaveWith("that is not an LmCoop lobby");
        if (std::string(mm->GetLobbyData(lobby_, "build")) != cfg_.build) return leaveWith("the host runs a different LmCoop version");
        peer_ = mm->GetLobbyOwner(lobby_);
        if (peer_ == me_) return leaveWith("that lobby is yours: join with another Steam account");
        linked_ = true, text_ = "Steam: joined the lobby, connecting...";
    }
    void leaveWith(const std::string& why) {
        SteamMatchmaking()->LeaveLobby(lobby_);
        lobby_ = CSteamID();
        fail(why);
    }

    NetConfig cfg_;
    CSteamID me_, lobby_, peer_;
    std::string text_, code_;
    bool failed_ = false, linked_ = false;
    std::deque<std::vector<u8>> inbox_;
    CCallResult<SteamTransport, LobbyCreated_t> created_;
    CCallResult<SteamTransport, LobbyMatchList_t> found_;
    CCallResult<SteamTransport, LobbyEnter_t> entered_;
};

}  // namespace

bool steamAvailable() { return true; }
std::unique_ptr<Transport> makeSteamTransport(const NetConfig& c) { return std::make_unique<SteamTransport>(c); }
void steamRefreshLobbies() { browser().refresh(); }
LobbyList steamLobbyList() { return browser().snapshot(); }

#else  // built without the Steamworks SDK

namespace {
class NoSteam final : public Transport {
public:
    void send(PeerId, const u8*, std::size_t, bool) override {}
    std::size_t receive(PeerId&, u8*, std::size_t) override { return 0; }
    NetStatus status() override {
        NetStatus s;
        s.text = "error: this LmCoop build has no Steam support", s.failed = true;
        return s;
    }
    std::string describe(PeerId) const override { return ""; }
};
}  // namespace

bool steamAvailable() { return false; }
std::unique_ptr<Transport> makeSteamTransport(const NetConfig&) { return std::make_unique<NoSteam>(); }
void steamRefreshLobbies() {}
LobbyList steamLobbyList() {
    LobbyList l;
    l.loaded = true, l.error = "this LmCoop build has no Steam support";
    return l;
}

#endif

}  // namespace recomp::online
