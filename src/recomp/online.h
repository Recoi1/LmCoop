#pragma once
// Online co-op session (online.cpp): the 2-player mode across two PCs over UDP, e.g. a Radmin VPN
// address. Host = player 1 (Luigi), client = player 2 (Mario). F1 > Controls > Online, or the
// LM_ONLINE* test aids. Off by default; nothing is opened while off.
#include "recomp/online_protocol.h"

namespace recomp::online {

bool wanted();         // host or join configured
int localIndex();      // 0 host, 1 client, -1 off
bool connected();
const PlayerState* remote();      // the newest state from the other PC, null before the first
void setLocal(const PlayerState& s);  // this PC's player, sent at 30 Hz
void frame();          // once per game-scene frame (fn_800112F8 hook, coop_fixes.cpp)
const char* status();  // "off", "waiting", "connected", "rejected: <why>", "disconnected"

bool drawTab();        // Controls tab, "Online" sub-tab. True when a setting changed.

}  // namespace recomp::online
