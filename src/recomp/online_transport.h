#pragma once
// Transport under the online co-op session (online.cpp). The session speaks the datagram protocol
// of online_protocol.h; a transport only moves those datagrams between the two PCs:
//   Udp:      one raw UDP socket on a LAN / Radmin / loopback address ("Direct Connect").
//   Internet: a WebRTC data channel (ICE + STUN, optional TURN, DTLS) negotiated through the
//             matchmaking server in server/ (online_rtc.cpp).
// Nothing here knows about the game. All methods are called from the guest thread and must not
// block; the internet transport keeps its own network threads behind a mutex-protected queue.
#include "recomp/online_protocol.h"

#include <cstdint>
#include <memory>
#include <string>

namespace recomp::online {

// Identifies the sender of a datagram: the address for UDP, always 1 for the single WebRTC peer.
using PeerId = std::uint64_t;

// Everything a transport needs from the user's settings.
struct NetConfig {
    bool internet = false;          // false: UDP to/from `address:port`
    bool host = false;
    std::string address;            // UDP: the address to listen on (host) or the host's address (join)
    int port = 28555;
    std::string signalUrl;          // internet: ws:// or wss:// matchmaking server
    std::string iceServers;         // internet: ';'-separated stun:/turn: URLs; empty = built-in public STUN
    std::string lobbyName;          // internet host
    bool publicLobby = false;       // internet host: listed in the browser
    bool hasPassword = false;       // internet host: shown in the browser (checked in the game handshake)
    std::string inviteCode;         // internet join: invite code ...
    std::string lobbyId;            // ... or the id of a browsed public lobby
    std::string build;              // game build; the server only pairs identical builds
};

struct NetStatus {
    std::string text;               // one line for the status display
    std::string inviteCode;         // internet host: the code to share ("" until registered)
    bool failed = false;            // final failure: `text` says why; the session will not recover
    bool linked = false;            // a datagram path to the other PC is open
};

class Transport {
public:
    virtual ~Transport() = default;
    // One datagram (<= kMaxPacket) to the peer. `reliable` datagrams are delivered in order and
    // retransmitted (internet); unreliable ones may be lost. UDP is always best effort.
    virtual void send(PeerId to, const u8* data, std::size_t n, bool reliable) = 0;
    // Next received datagram, or 0 when none is waiting. Never blocks.
    virtual std::size_t receive(PeerId& from, u8* out, std::size_t cap) = 0;
    virtual NetStatus status() = 0;
    virtual std::string describe(PeerId peer) const = 0;  // for the connected status line
    // Who a joining session sends to before it has heard from anyone; 0 = nobody yet (the host).
    virtual PeerId initialPeer() const { return 0; }
};

// Never null: on failure the returned transport reports status().failed with the reason.
std::unique_ptr<Transport> makeUdpTransport(const NetConfig& c);
std::unique_ptr<Transport> makeRtcTransport(const NetConfig& c);  // online_rtc.cpp

}  // namespace recomp::online
