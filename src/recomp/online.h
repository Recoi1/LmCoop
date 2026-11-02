#pragma once
// Online co-op session (online.cpp): the 2-player mode across two PCs over UDP, e.g. a Radmin VPN
// address. Host = player 1 (Luigi), client = player 2 (Mario). F1 > Controls > Online, or the
// LM_ONLINE* test aids. Off by default; nothing is opened while off.
#include "recomp/online_protocol.h"

#include <string>

struct PADStatus;
namespace recomp { struct Ctx; }

namespace recomp::online {

bool wanted();         // host or join configured
int localIndex();      // 0 host, 1 client, -1 off
bool connected();
const PlayerState* remote();      // the newest state from the other PC, null before the first
u32 remoteAge();       // game frames since that state arrived
const char* remoteName();  // the other player's chosen name ("" before the first state)
// Name tag above the other player (coop_fixes.cpp projects the head each game frame; x/y in the
// game's 640x480 picture) and its drawing on the host (dev_tools.cpp overlays).
void setNameTag(bool shown, int x, int y);
void drawNameTag();
void setLocal(const PlayerState& s);  // this PC's player, sent at 30 Hz
// PADRead (hle_pad.cpp), after the script and skip filters: this PC's pad (port 1) drives the
// local player's port, the other PC's last pad the remote player's; both read as connected.
void pad(PADStatus* st);
void frame(Ctx& c);    // scene manager: also runs before a game/save has been selected
void beforeMapSetup(Ctx& c);  // apply the host's record after the old actors have been destroyed
void onMapSetup(Ctx& c);  // after the map's actors exist
bool guest();          // joining this session: local saves are read-only
bool joiningMap();     // a host snapshot is being loaded
bool loading();
void drawLoadingScreen();  // host thread, over the guest's boot/title/loading frames
void noteSpawnEntry(u32 tool, int entry);
void spawnValue(u32 tool, int entry, const char* field, u32 out);
const char* status();  // "off", "waiting", "connected", "rejected: <why>", "disconnected"

bool drawTab();        // Controls tab, "Online" sub-tab. True when a setting changed.
// Host/Join, address, port and password (the Online tab and the LmCoop start menu). True when changed.
bool drawHostJoin(bool withOff);
bool readyToStart();   // Host or Join chosen and an address entered
int configuredMode();  // the saved mode: 0 off, 1 host, 2 join
// The LmCoop start menu's choice for this session (-1 = the saved settings, 0 off, 1 host, 2 join).
void setSessionMode(int mode);

}  // namespace recomp::online
