#pragma once
#include "recomp/online_protocol.h"
struct PADStatus;
namespace recomp { struct Ctx; }
namespace recomp::online::events {
void reset();
void mapSetup();
void receive(const SceneEvent& event);
void frame(Ctx& c);
void fill(EventProgress& progress);
const GameState* hostGame();
bool condition(Ctx& c, u32 trigger, bool eligible);
bool watching();
bool busy();
// The world the current shared event belongs to (0 when none).
u32 eventWorld();
bool holdPlayers();
void pad(PADStatus* pads, const PADStatus& mine);
}  // namespace recomp::online::events
