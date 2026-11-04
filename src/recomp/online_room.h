#pragma once
// The pre-game lobby (online_room.cpp): after Create lobby / Join, both players wait here, before the
// game boots. The transport is opened here and handed to the session when the game starts. Only the
// host can start. Called from the start menu's thread, one thread at a time.
#include "recomp/online.h"

#include <cstdint>
#include <string>

namespace recomp::online::room {

struct Player {
    std::string name;
    std::uint64_t avatar = 0;  // an ImTextureID (Steam only); 0 = none
    bool present = false;
};
struct State {
    bool open = false;
    bool failed = false;       // `status` says why
    bool started = false;      // the host started the game: leave the lobby for the game
    bool host = false;
    std::string status;        // one line
    std::string code;          // the invite code (host)
    Player me, other;
};

bool open(const Config& c);    // starts opening the transport; the result shows in update()
State update();                // every frame while the lobby is on screen
void start();                  // host: tell the guest and begin
void finish();                 // the game is starting: hand the transport to the session
void leave();                  // close the transport
bool autopilot();              // test aid: LM_LOBBY_AUTO=host|join opens (and the host starts) by itself
}  // namespace recomp::online::room
