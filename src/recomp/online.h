#pragma once
// Online co-op session (online.cpp): the 2-player mode across two PCs over UDP, e.g. a Radmin VPN
// address. Host = player 1 (Luigi), client = player 2 (Mario). F1 > Controls > Online, or the
// LM_ONLINE* test aids. Off by default; nothing is opened while off.
#include "recomp/online_protocol.h"

struct PADStatus;

namespace recomp::online {

bool wanted();         // host or join configured
int localIndex();      // 0 host, 1 client, -1 off
bool connected();
const PlayerState* remote();      // the newest state from the other PC, null before the first
u32 remoteAge();       // game frames since that state arrived
void setLocal(const PlayerState& s);  // this PC's player, sent at 30 Hz
// PADRead (hle_pad.cpp), after the script and skip filters: this PC's pad (port 1) drives the
// local player's port, the other PC's last pad the remote player's; both read as connected.
void pad(PADStatus* st);
void frame();          // once per game-scene frame (fn_800112F8 hook, coop_fixes.cpp)
const char* status();  // "off", "waiting", "connected", "rejected: <why>", "disconnected"

bool drawTab();        // Controls tab, "Online" sub-tab. True when a setting changed.

}  // namespace recomp::online
