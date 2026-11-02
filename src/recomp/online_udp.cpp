// UDP transport ("Direct Connect"): the original LAN / Radmin VPN path, moved out of online.cpp
// unchanged in behavior. The host binds only the address the user entered.
#include "recomp/online_transport.h"

#include <cstring>

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

namespace recomp::online {
namespace {

PeerId pack(const sockaddr_in& a) { return (PeerId(ntohl(a.sin_addr.s_addr)) << 16) | ntohs(a.sin_port); }
sockaddr_in unpack(PeerId id) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(u32(id >> 16));
    a.sin_port = htons(u16(id & 0xFFFF));
    return a;
}

class UdpTransport final : public Transport {
public:
    explicit UdpTransport(const NetConfig& c) {
#ifdef _WIN32
        static const bool sWsa = [] {
            WSADATA d;
            return WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        if (!sWsa) { fail("error: Winsock unavailable"); return; }
#endif
        if (c.port <= 0 || c.port > 65535) { fail("error: port must be 1-65535"); return; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        if (c.host) {
            // The host listens on the chosen address only (never every interface).
            if (c.address.empty() || inet_pton(AF_INET, c.address.c_str(), &a.sin_addr) != 1)
                { fail("error: host needs this PC's address (e.g. its Radmin VPN 26.x.x.x)"); return; }
            a.sin_port = htons(u16(c.port));
        } else {
            sockaddr_in peer{};
            if (c.address.empty() || inet_pton(AF_INET, c.address.c_str(), &peer.sin_addr) != 1)
                { fail("error: enter the host's address"); return; }
            peer.sin_family = AF_INET, peer.sin_port = htons(u16(c.port));
            peer_ = pack(peer);
        }
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == kNoSocket) { fail("error: no UDP socket"); return; }
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sock_, FIONBIO, &nb);
#else
        fcntl(sock_, F_SETFL, fcntl(sock_, F_GETFL) | O_NONBLOCK);
#endif
        if (bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            close();
            { fail("error: cannot listen on " + c.address + ":" + std::to_string(c.port)); return; }
        }
        status_.text = c.host ? "waiting on " + c.address + ":" + std::to_string(c.port)
                              : "waiting: joining " + c.address + ":" + std::to_string(c.port);
        status_.linked = true;  // a UDP "path" exists as soon as the socket does
    }
    ~UdpTransport() override { close(); }

    void send(PeerId to, const u8* data, std::size_t n, bool) override {
        if (sock_ == kNoSocket) return;
        const sockaddr_in a = unpack(to);
        sendto(sock_, reinterpret_cast<const char*>(data), int(n), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    }
    std::size_t receive(PeerId& from, u8* out, std::size_t cap) override {
        if (sock_ == kNoSocket) return 0;
        sockaddr_in a{};
        socklen_t al = sizeof(a);
        const int n = int(recvfrom(sock_, reinterpret_cast<char*>(out), int(cap), 0, reinterpret_cast<sockaddr*>(&a), &al));
        if (n <= 0) return 0;
        from = pack(a);
        return std::size_t(n);
    }
    NetStatus status() override { return status_; }
    std::string describe(PeerId peer) const override {
        const sockaddr_in a = unpack(peer);
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
    }
    PeerId initialPeer() const override { return peer_; }

private:
    void fail(std::string why) { status_.text = std::move(why), status_.failed = true; }
    void close() {
        if (sock_ == kNoSocket) return;
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = kNoSocket;
    }
    SocketHandle sock_ = kNoSocket;
    PeerId peer_ = 0;
    NetStatus status_;
};

}  // namespace

std::unique_ptr<Transport> makeUdpTransport(const NetConfig& c) { return std::make_unique<UdpTransport>(c); }

}  // namespace recomp::online
