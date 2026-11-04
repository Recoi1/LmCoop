#pragma once
// Lobby browser data for the Browse Lobbies screen (online_rtc.cpp). The fetch runs on library
// threads; the UI only reads snapshots.
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

}  // namespace recomp::online
