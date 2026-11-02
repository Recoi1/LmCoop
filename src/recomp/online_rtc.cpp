// Internet transport: a WebRTC data channel (libdatachannel: ICE with STUN/TURN, DTLS) negotiated
// through the matchmaking server in server/. The server only relays the negotiation; game traffic
// goes peer to peer (or through the user's configured TURN relay) and is encrypted by DTLS.
//
// Channels: "lmcoop-ctl-<token>" is reliable and ordered (handshake, game snapshot, cutscene events);
// "lmcoop-st-<token>" is unordered with no retransmits (30 Hz player state). <token> is chosen by
// the server per join; the host drops any channel without it.
//
// Threads: libdatachannel calls back on its own threads. Callbacks only touch `Core` under its
// mutex; the game thread reads through receive()/status() and never waits on the network.
#include "recomp/online_lobby.h"
#include "recomp/online_transport.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace recomp { void log(const char* fmt, ...); }  // runtime.cpp

namespace recomp::online {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int kSignalProtocol = 1;  // must match PROTOCOL in server/src/server.js
constexpr char kDefaultIce[] = "stun:stun.l.google.com:19302;stun:stun.cloudflare.com:3478";
constexpr auto kNegotiateTimeout = std::chrono::seconds(30);
constexpr std::size_t kMaxInbox = 512;

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s + ";") {
        if (ch == ';' || ch == '\n' || ch == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur += ch;
    }
    return out;
}

// wss:// needs trusted roots; neither MbedTLS nor OpenSSL read the Windows store themselves.
std::string caBundleFile() {
    static const std::string path = [] {
        std::string out;
#ifdef _WIN32
        const char* tmp = std::getenv("TEMP");
        out = std::string(tmp ? tmp : ".") + "\\lmcoop-roots.pem";
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT")) {
            for (PCCERT_CONTEXT c = CertEnumCertificatesInStore(store, nullptr); c; c = CertEnumCertificatesInStore(store, c)) {
                DWORD n = 0;
                if (!CryptBinaryToStringA(c->pbCertEncoded, c->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &n)) continue;
                std::string pem(n, '\0');
                if (CryptBinaryToStringA(c->pbCertEncoded, c->cbCertEncoded, CRYPT_STRING_BASE64HEADER, pem.data(), &n))
                    f.write(pem.data(), std::streamsize(n));
            }
            CertCloseStore(store, 0);
        }
#else
        for (const char* p : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"})
            if (std::ifstream(p).good()) { out = p; break; }
#endif
        return out;
    }();
    return path;
}

std::shared_ptr<rtc::WebSocket> openSignal(const std::string& url) {
    rtc::WebSocket::Configuration wc;
    if (url.rfind("wss://", 0) == 0) wc.caCertificatePemFile = caBundleFile();
    auto ws = std::make_shared<rtc::WebSocket>(wc);
    ws->open(url);  // asynchronous
    return ws;
}

void initLoggerOnce() {
    static const bool once = [] {
        rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel, std::string m) { log("[rtc] %s", m.c_str()); });
        return true;
    }();
    (void)once;
}

std::string explain(const std::string& error, const json& m) {
    if (error == "lobby-not-found") return "error: no lobby with that code (or it is private)";
    if (error == "lobby-full") return "error: the lobby is full";
    if (error == "build-mismatch") return "error: the host runs a different LmCoop version (" + m.value("expected", "?") + ")";
    if (error == "protocol-mismatch") return "error: this LmCoop version cannot talk to that matchmaking server";
    if (error == "unsupported-game") return "error: the server does not support this game version";
    if (error == "server-full") return "error: the matchmaking server is full, try again later";
    return "error: matchmaking server said " + error;
}

// ---------------------------------------------------------------------------------------------
struct Core : std::enable_shared_from_this<Core> {
    NetConfig cfg;
    std::mutex m;
    NetStatus st;
    std::deque<std::vector<u8>> inbox;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> ctl, state;
    std::string token;
    bool negotiating = false;
    Clock::time_point deadline;

    void setText(std::string t) { std::lock_guard lk(m); st.text = std::move(t); }
    void fail(std::string why) {
        std::lock_guard lk(m);
        if (st.failed) return;
        st.failed = true, st.linked = false, st.text = std::move(why);
        log("[online] %s", st.text.c_str());
    }

    void sendSignal(const json& body) {
        std::shared_ptr<rtc::WebSocket> w;
        { std::lock_guard lk(m); w = ws; }
        try { if (w && w->isOpen()) w->send(body.dump()); } catch (const std::exception&) {}
    }

    void start() {
        initLoggerOnce();
        if (cfg.signalUrl.rfind("ws://", 0) && cfg.signalUrl.rfind("wss://", 0))
            return fail("error: the matchmaking server address must start with ws:// or wss://");
        const bool host = cfg.host;
        if (!host && cfg.inviteCode.empty() && cfg.lobbyId.empty()) return fail("error: enter an invite code or pick a lobby");
        setText("contacting the matchmaking server...");
        auto self = weak_from_this();
        try {
            auto w = openSignal(cfg.signalUrl);
            w->onOpen([self] {
                if (auto c = self.lock()) c->onSignalOpen();
            });
            w->onMessage([self](std::variant<rtc::binary, std::string> msg) {
                if (auto c = self.lock(); c && std::holds_alternative<std::string>(msg)) c->onSignalMessage(std::get<std::string>(msg));
            });
            w->onError([self](std::string e) {
                if (auto c = self.lock()) c->fail("error: cannot reach the matchmaking server (" + e + ")");
            });
            w->onClosed([self] {
                auto c = self.lock();
                if (!c) return;
                std::lock_guard lk(c->m);
                // After the peer link is up the session continues without the server.
                if (!c->st.linked && !c->st.failed) c->st.failed = true, c->st.text = "error: lost the matchmaking server";
            });
            std::lock_guard lk(m);
            ws = w;
        } catch (const std::exception& e) {
            fail(std::string("error: bad matchmaking server address (") + e.what() + ")");
        }
    }

    void onSignalOpen() {
        if (cfg.host) {
            sendSignal({{"t", "host"}, {"proto", kSignalProtocol}, {"game", "GLME01"}, {"name", cfg.lobbyName},
                        {"build", cfg.build}, {"public", cfg.publicLobby}, {"password", cfg.hasPassword}});
        } else {
            json j = {{"t", "join"}, {"proto", kSignalProtocol}, {"game", "GLME01"}, {"build", cfg.build}};
            if (!cfg.inviteCode.empty()) j["code"] = cfg.inviteCode; else j["id"] = cfg.lobbyId;
            sendSignal(j);
        }
        setText(cfg.host ? "registering the lobby..." : "joining the lobby...");
    }

    void onSignalMessage(const std::string& text) {
        json m = json::parse(text, nullptr, false);
        if (!m.is_object() || !m.contains("t") || !m["t"].is_string()) return;
        const std::string t = m["t"];
        try {
            if (t == "hosted") {
                std::lock_guard lk(this->m);
                st.inviteCode = m.at("code").get<std::string>();
                st.text = "lobby " + st.inviteCode + ": waiting for a player";
            } else if (t == "joined" || t == "peer-joined") {
                beginPeer(m.at("token").get<std::string>(), t == "joined");
            } else if (t == "peer-left") {
                resetPeer();
                std::lock_guard lk(this->m);
                st.text = "lobby " + st.inviteCode + ": waiting for a player";
            } else if (t == "signal") {
                onRemoteSignal(m.at("data"));
            } else if (t == "error") {
                fail(explain(m.value("error", "?"), m));
            } else if (t == "closed") {
                const std::string why = m.value("reason", "");
                if (cfg.host) { if (why == "expired") fail("error: the lobby expired"); }
                else { std::lock_guard lk(this->m); if (!st.linked) { st.failed = true, st.text = "error: the host closed the lobby"; } }
            }
        } catch (const std::exception&) {
            // A malformed server message is ignored.
        }
    }

    // ---- peer connection ----
    void resetPeer() {
        std::shared_ptr<rtc::PeerConnection> p;
        std::shared_ptr<rtc::DataChannel> a, b;
        {
            std::lock_guard lk(m);
            p = std::move(pc), a = std::move(ctl), b = std::move(state);
            st.linked = false, negotiating = false, token.clear();
            inbox.clear();
        }
        // Closing can wait for library threads; never do it on the game thread's path.
        std::thread([p, a, b]() mutable {
            try { if (p) p->resetCallbacks(); if (a) a->resetCallbacks(); if (b) b->resetCallbacks(); if (p) p->close(); } catch (...) {}
        }).detach();
    }

    void checkOpen() {
        std::lock_guard lk(m);
        if (ctl && state && ctl->isOpen() && state->isOpen() && !st.linked && !st.failed) {
            st.linked = true, negotiating = false;
            st.text = "connected (encrypted peer-to-peer link)";
            log("[online] peer link open");
        }
    }

    void attach(const std::shared_ptr<rtc::DataChannel>& dc, bool reliable) {
        auto self = weak_from_this();
        dc->onOpen([self] { if (auto c = self.lock()) c->checkOpen(); });
        dc->onClosed([self] {
            if (auto c = self.lock()) { std::lock_guard lk(c->m); if (c->st.linked) c->st.linked = false; }
        });
        dc->onMessage([self, reliable](std::variant<rtc::binary, std::string> msg) {
            auto c = self.lock();
            if (!c || !std::holds_alternative<rtc::binary>(msg)) return;
            const auto& b = std::get<rtc::binary>(msg);
            if (b.empty() || b.size() > kMaxPacket) return;  // size validation before it reaches the game code
            std::lock_guard lk(c->m);
            if (c->inbox.size() >= kMaxInbox) c->inbox.pop_front();
            c->inbox.emplace_back(reinterpret_cast<const u8*>(b.data()), reinterpret_cast<const u8*>(b.data()) + b.size());
            (void)reliable;
        });
    }

    void beginPeer(const std::string& newToken, bool offerer) {
        if (newToken.size() != 32) return;
        resetPeer();
        rtc::Configuration rc;
        // "-" = no ICE servers at all (host candidates only; the automated tests use it).
        for (const std::string& url : splitList(cfg.iceServers.empty() ? kDefaultIce : cfg.iceServers == "-" ? "" : cfg.iceServers)) {
            try { rc.iceServers.emplace_back(url); } catch (const std::exception&) { log("[online] ignoring bad ICE server '%s'", url.c_str()); }
        }
        auto p = std::make_shared<rtc::PeerConnection>(rc);
        auto self = weak_from_this();
        p->onLocalDescription([self](rtc::Description d) {
            if (auto c = self.lock()) c->sendSignal({{"t", "signal"}, {"data", {{"type", "description"}, {"sdpType", d.typeString()}, {"sdp", std::string(d)}}}});
        });
        p->onLocalCandidate([self](rtc::Candidate cand) {
            if (auto c = self.lock()) c->sendSignal({{"t", "signal"}, {"data", {{"type", "candidate"}, {"candidate", std::string(cand)}, {"mid", cand.mid()}}}});
        });
        p->onStateChange([self](rtc::PeerConnection::State s) {
            auto c = self.lock();
            if (!c) return;
            if (s == rtc::PeerConnection::State::Failed) {
                const bool turn = c->cfg.iceServers.find("turn:") != std::string::npos || c->cfg.iceServers.find("turns:") != std::string::npos;
                c->fail(turn ? "error: no connection path even through the TURN relay"
                             : "error: could not connect directly (NAT/firewall); ask the host to try Direct Connect or configure a TURN relay");
            } else if (s == rtc::PeerConnection::State::Disconnected || s == rtc::PeerConnection::State::Closed) {
                std::lock_guard lk(c->m);
                c->st.linked = false;
            }
        });
        std::shared_ptr<rtc::DataChannel> a, b;
        if (offerer) {
            rtc::DataChannelInit unreliable;
            unreliable.reliability.unordered = true;
            unreliable.reliability.maxRetransmits = 0;
            a = p->createDataChannel("lmcoop-ctl-" + newToken);
            b = p->createDataChannel("lmcoop-st-" + newToken, unreliable);
            attach(a, true), attach(b, false);
        } else {
            const std::string want = newToken;
            p->onDataChannel([self, want](std::shared_ptr<rtc::DataChannel> dc) {
                auto c = self.lock();
                if (!c) return;
                const std::string label = dc->label();
                const bool isCtl = label == "lmcoop-ctl-" + want, isSt = label == "lmcoop-st-" + want;
                if (!isCtl && !isSt) { dc->close(); return; }  // not the player the server admitted
                c->attach(dc, isCtl);
                { std::lock_guard lk(c->m); (isCtl ? c->ctl : c->state) = dc; }
                c->checkOpen();
            });
        }
        {
            std::lock_guard lk(m);
            pc = p, ctl = a, state = b, token = newToken;
            negotiating = true, deadline = Clock::now() + kNegotiateTimeout;
            st.text = "negotiating a peer-to-peer connection...";
        }
    }

    void onRemoteSignal(const json& d) {
        std::shared_ptr<rtc::PeerConnection> p;
        { std::lock_guard lk(m); p = pc; }
        if (!p) return;
        const std::string type = d.value("type", "");
        if (type == "description") p->setRemoteDescription(rtc::Description(d.at("sdp").get<std::string>(), d.at("sdpType").get<std::string>()));
        else if (type == "candidate") p->addRemoteCandidate(rtc::Candidate(d.at("candidate").get<std::string>(), d.value("mid", "")));
    }
};

class RtcTransport final : public Transport {
public:
    explicit RtcTransport(const NetConfig& c) : core_(std::make_shared<Core>()) {
        core_->cfg = c;
        core_->start();
    }
    ~RtcTransport() override {
        // Tear down off the game thread: closing a peer connection can wait for its threads.
        std::thread([core = core_] {
            std::shared_ptr<rtc::WebSocket> ws;
            { std::lock_guard lk(core->m); ws = std::move(core->ws); core->st.failed = true; }
            try { if (ws) { ws->resetCallbacks(); ws->close(); } } catch (...) {}
            core->resetPeer();
        }).detach();
    }

    void send(PeerId, const u8* data, std::size_t n, bool reliable) override {
        std::shared_ptr<rtc::DataChannel> ch;
        {
            std::lock_guard lk(core_->m);
            ch = reliable ? core_->ctl : core_->state;
        }
        if (!ch || n == 0 || n > kMaxPacket) return;
        try { if (ch->isOpen()) ch->send(reinterpret_cast<const std::byte*>(data), n); } catch (const std::exception&) {}
    }
    std::size_t receive(PeerId& from, u8* out, std::size_t cap) override {
        std::lock_guard lk(core_->m);
        if (core_->inbox.empty()) return 0;
        const auto& pkt = core_->inbox.front();
        const std::size_t n = pkt.size() <= cap ? pkt.size() : 0;
        if (n) std::memcpy(out, pkt.data(), n);
        core_->inbox.pop_front();
        from = 1;
        return n;
    }
    NetStatus status() override {
        std::lock_guard lk(core_->m);
        if (core_->negotiating && !core_->st.linked && !core_->st.failed && Clock::now() > core_->deadline) {
            core_->st.failed = true;
            core_->st.text = "error: timed out negotiating the connection (blocked by a firewall or NAT?)";
        }
        return core_->st;
    }
    std::string describe(PeerId) const override { return "internet peer (DTLS)"; }
    PeerId initialPeer() const override { return core_->cfg.host ? 0 : 1; }

private:
    std::shared_ptr<Core> core_;
};

// ---- lobby browser ----
std::mutex sListM;
LobbyList sList;
std::shared_ptr<rtc::WebSocket> sListSocket;

}  // namespace

std::unique_ptr<Transport> makeRtcTransport(const NetConfig& c) { return std::make_unique<RtcTransport>(c); }

void refreshLobbies(const std::string& url) {
    initLoggerOnce();
    {
        std::lock_guard lk(sListM);
        if (sList.busy) return;
        sList.busy = true, sList.error.clear();
    }
    auto finish = [](std::string error, std::vector<LobbyInfo> lobbies, bool ok) {
        std::shared_ptr<rtc::WebSocket> w;
        {
            std::lock_guard lk(sListM);
            sList.busy = false;
            if (ok) sList.loaded = true, sList.lobbies = std::move(lobbies); else sList.error = std::move(error);
            w = std::move(sListSocket);
        }
        if (w) std::thread([w] { try { w->resetCallbacks(); w->close(); } catch (...) {} }).detach();
    };
    if (url.rfind("ws://", 0) && url.rfind("wss://", 0)) return finish("the matchmaking server address must start with ws:// or wss://", {}, false);
    try {
        auto w = openSignal(url);
        w->onOpen([weak = std::weak_ptr<rtc::WebSocket>(w)] {
            if (auto s = weak.lock()) s->send(json{{"t", "list"}}.dump());
        });
        w->onMessage([finish](std::variant<rtc::binary, std::string> msg) {
            if (!std::holds_alternative<std::string>(msg)) return;
            json m = json::parse(std::get<std::string>(msg), nullptr, false);
            if (!m.is_object() || m.value("t", "") != "lobbies" || !m.contains("lobbies") || !m["lobbies"].is_array()) return;
            std::vector<LobbyInfo> out;
            for (const json& e : m["lobbies"]) {
                if (!e.is_object() || out.size() >= 200) continue;
                LobbyInfo l;
                l.id = e.value("id", ""), l.name = e.value("name", ""), l.build = e.value("build", "");
                l.players = e.value("players", 0), l.max = e.value("max", 2), l.password = e.value("password", false);
                if (!l.id.empty()) out.push_back(std::move(l));
            }
            finish("", std::move(out), true);
        });
        w->onError([finish](std::string e) { finish("cannot reach the matchmaking server (" + e + ")", {}, false); });
        w->onClosed([finish] {
            std::lock_guard lk(sListM);
            if (sList.busy) { sList.busy = false; sList.error = "the matchmaking server closed the connection"; }
        });
        std::lock_guard lk(sListM);
        sListSocket = w;
    } catch (const std::exception& e) {
        finish(std::string("bad matchmaking server address (") + e.what() + ")", {}, false);
    }
}

LobbyList lobbyList() {
    std::lock_guard lk(sListM);
    return sList;
}

}  // namespace recomp::online
