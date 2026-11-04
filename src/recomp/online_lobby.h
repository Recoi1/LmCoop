#pragma once
// Lobby browser data for the Browse Lobbies screen (online_rtc.cpp). The fetch runs on library
// threads; the UI only reads snapshots.
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::online {

struct LobbyInfo {
    std::string id, name, build;
    int players = 0, max = 2;
    bool password = false;
};
struct LobbyList {
    bool busy = false;
    bool loaded = false;     // a list (possibly empty) has arrived at least once
    std::string error;       // set when the last refresh failed
    std::vector<LobbyInfo> lobbies;
};

void refreshLobbies(const std::string& signalUrl);  // asks the matchmaking server; returns at once
LobbyList lobbyList();                               // newest snapshot

// The same for Steam lobbies (online_steam.cpp).
void steamRefreshLobbies();
LobbyList steamLobbyList();
// A Steam user's display name and avatar texture (an ImTextureID as an integer; 0 while it loads or without
// Steam). id 0 = this PC's own user.
std::string steamName(std::uint64_t id);
std::uint64_t steamAvatar(std::uint64_t id);

}  // namespace recomp::online
