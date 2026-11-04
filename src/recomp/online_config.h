#pragma once
// The online settings ([online] in settings.ini and the LM_ONLINE* test overrides), shared by the
// session (online.cpp) and the multiplayer screens (online_ui.cpp).
#include <string>

namespace recomp::online {

enum class Mode : int { Off, Host, Join };

struct Config {
    Mode mode = Mode::Off;
    bool internet = false;            // false: Direct Connect (UDP address, LAN / Radmin)
    bool steam = false;               // internet through Steam lobbies and relay (no matchmaking server)
    std::string address;              // direct: the address to listen on (host) or the host's address (join)
    int port = 28555;
    std::string password;
    std::string name = "Player";      // shown above this PC's player on the other PC
    std::string signalUrl;            // internet: ws:// / wss:// matchmaking server (see docs/ONLINE_SERVER.md)
    std::string iceServers;           // internet: ';'-separated stun:/turn: URLs, empty = public STUN
    std::string lobbyName = "Luigi's Mansion";
    bool publicLobby = false;
    std::string inviteCode;           // internet join by code
    std::string lobbyId;              // internet join from the browser
    bool operator==(const Config&) const = default;
};

Config savedConfig();                 // thread-safe copy of the saved settings
void setSavedConfig(const Config& c);  // replaces the saved settings (written by port::saveSettings)

}  // namespace recomp::online
