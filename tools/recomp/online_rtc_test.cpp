// Transport-level end-to-end test for the internet path (no game): two copies of this program
// negotiate through a running matchmaking server and exchange protocol packets over the data
// channels. Driven by server/test/e2e.test.js; usage:
//   lm_rtc_test host <ws-url> <lobby-name> [public]     prints "CODE <invite code>" once registered
//   lm_rtc_test join <ws-url> <invite code> [build]
// Both print "RESULT ok" after 50 reliable packets (and at least 25 of 50 unreliable ones) made the round trip, or
// "RESULT <error text>" and exit 1.
#include "recomp/online_transport.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace recomp {
void log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    std::fputc('\n', stderr);
    va_end(a);
}
}  // namespace recomp

using namespace recomp::online;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const bool host = !std::strcmp(argv[1], "host");
    NetConfig c;
    c.internet = true, c.host = host, c.signalUrl = argv[2], c.build = argc > 4 && !host ? argv[4] : "test", c.iceServers = "-";
    if (host) c.lobbyName = argv[3], c.publicLobby = argc > 4;
    else c.inviteCode = argv[3];
    auto net = makeRtcTransport(c);
    const auto start = Clock::now();
    bool printedCode = false;
    int sentR = 0, sentU = 0, relGot = 0, unrelGot = 0;
    PeerId peer = net->initialPeer();
    u8 buf[kMaxPacket + 1];
    while (Clock::now() - start < std::chrono::seconds(45)) {
        const NetStatus s = net->status();
        if (s.failed) { std::printf("RESULT %s\n", s.text.c_str()); return 1; }
        if (host && !printedCode && !s.inviteCode.empty()) std::printf("CODE %s\n", s.inviteCode.c_str()), std::fflush(stdout), printedCode = true;
        PeerId from = 0;
        while (const size_t n = net->receive(from, buf, sizeof(buf))) {
            Header h{};
            const u8* body = nullptr;
            size_t bn = 0;
            if (!decode(buf, n, h, body, bn)) { std::printf("RESULT malformed packet\n"); return 1; }
            peer = from;
            (h.kind == Kind::State ? unrelGot : relGot)++;
            // The host echoes everything back on the class it came in on.
            if (host) net->send(peer, buf, n, h.kind != Kind::State);
        }
        if (!host && s.linked && sentR < 50) {
            u8 pkt[kMaxPacket];
            const u8 body[4] = {1, 2, 3, 4};
            for (int i = 0; i < 5 && sentR < 50; i++, sentR++)
                net->send(peer, pkt, encode(Header{kMagic, Kind::Hello, u32(sentR)}, body, sizeof(body), pkt, sizeof(pkt)), true);
            for (int i = 0; i < 5 && sentU < 50; i++, sentU++)
                net->send(peer, pkt, encode(Header{kMagic, Kind::State, u32(sentU)}, body, sizeof(body), pkt, sizeof(pkt)), false);
        }
        // Every reliable packet must make the round trip; unreliable ones may be lost, so 25 of 50 will do.
        if (!host && relGot >= 50 && unrelGot >= 25) { std::printf("RESULT ok\n"); return 0; }
        if (host && relGot >= 50) {
            std::this_thread::sleep_for(std::chrono::seconds(1));  // let the last echoes leave
            std::printf("RESULT ok\n");
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("RESULT timeout (status: %s)\n", net->status().text.c_str());
    return 1;
}
