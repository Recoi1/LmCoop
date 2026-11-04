// LmCoop's multiplayer screens on the start menu: Host Online, Browse Lobbies, Join by Code,
// Direct Connect and Settings (setup.cpp draws drawMultiplayerMenu in place of the old host/join form).
// The screens only edit the saved online settings; the session (online.cpp) acts on them once the
// game starts. The lobby itself (invite code, players, leave) is in the F1 > Controls > Online tab.
#include "recomp/online.h"
#include "recomp/online_config.h"
#include "recomp/online_lobby.h"
#include "recomp/characters.h"
#include "recomp/online_room.h"
#include "recomp/online_transport.h"

#include "port/update.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace recomp::online {
namespace {

enum class Page { Menu, Host, Browse, Code, Direct, Settings, Lobby };
Page sPage = Page::Menu;
bool sPageEntered = true;  // first frame of a page

void go(Page p) { sPage = p, sPageEntered = true; }

// Edits a std::string through ImGui's char buffer; true when it changed.
bool field(const char* label, std::string& value, size_t max, bool password = false, float width = 280.0f) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    ImGui::SetNextItemWidth(width * ImGui::GetFontSize() / 13.0f);
    const bool changed = ImGui::InputText(label, buf, std::min(sizeof(buf), max + 1), password ? ImGuiInputTextFlags_Password : 0);
    if (changed) value = buf;
    return changed;
}

void nameField(Config& c, bool& dirty) {
    std::string n = c.name;
    if (field("Your name", n, 15)) c.name = n, dirty = true;
}

void serverNotice(const Config& c) {
    if (c.signalUrl.empty() && !c.steam) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1), "No matchmaking server set. Open Settings and enter one (docs/ONLINE_SERVER.md).");
        if (ImGui::Button("Open Settings")) go(Page::Settings);
    }
}

void startInternet(Config& c, Mode mode) {
    c.mode = mode, c.internet = true;
    setSavedConfig(c);
}

}  // namespace

bool lobbyScreenOpen() { return sPage == Page::Lobby; }

MenuResult drawMultiplayerMenu(float scale) {
    Config c = savedConfig();
    bool dirty = false;
    MenuResult result = MenuResult::Stay;
    // Scrolls when a page is taller than the room under the logo; the menu itself fits at 1080p.
    ImGui::BeginChild("##multiplayer", ImVec2(0, 0), false);
    const ImVec2 big(ImGui::GetWindowWidth() * 0.25f, std::min(44 * scale, ImGui::GetWindowHeight() * 0.12f));
    auto bigButton = [&](const char* label) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - big.x) * 0.5f);
        const bool hit = ImGui::Button(label, big);
        ImGui::Dummy(ImVec2(0, 2 * scale));
        return hit;
    };
    auto back = [&] {
        ImGui::Dummy(ImVec2(0, 8 * scale));
        if (ImGui::Button("Back", ImVec2(120 * scale, 34 * scale))) {
            if (sPage == Page::Menu) result = MenuResult::Back; else go(Page::Menu);
        }
    };
    // Pages other than the menu are a centered column.
    const float w = ImGui::GetWindowWidth() * 0.6f;
    const bool grouped = sPage != Page::Menu;  // a button below changes sPage: close what was opened
    if (grouped) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
        ImGui::BeginGroup();
    }
    const bool entered = sPageEntered;
    sPageEntered = false;
    if (room::autopilot()) go(Page::Lobby);  // test aid (LM_LOBBY_AUTO)

    switch (sPage) {
    case Page::Menu:
        if (bigButton("Host Online")) go(Page::Host);
        if (bigButton("Browse Lobbies")) go(Page::Browse);
        if (bigButton("Join by Code")) go(Page::Code);
        if (bigButton("Direct Connect")) go(Page::Direct);
        {
            const ImVec2 half((big.x - 8 * scale) * 0.5f, big.y);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - big.x) * 0.5f);
            if (ImGui::Button("Settings", half)) go(Page::Settings);
            ImGui::SameLine(0, 8 * scale);
            if (ImGui::Button("Back", half)) result = MenuResult::Back;
        }
        break;

    case Page::Host: {
        ImGui::TextWrapped("Host a game over the internet. You play as Luigi and choose the save; your friend joins as Mario. "
                           "This game supports exactly two players.");
        serverNotice(c);
        nameField(c, dirty);
        dirty |= field("Lobby name", c.lobbyName, 24);
        dirty |= ImGui::Checkbox("Public (listed in Browse Lobbies)", &c.publicLobby);
        dirty |= field("Password (optional)", c.password, 40, true);
        ImGui::TextDisabled("Private lobbies are joined with the invite code shown in the lobby.");
        ImGui::BeginDisabled((c.signalUrl.empty() && !c.steam) || c.lobbyName.empty());
        if (ImGui::Button("Create lobby", ImVec2(160 * scale, 36 * scale))) {
            startInternet(c, Mode::Host);
            room::open(c);
            go(Page::Lobby);
        }
        ImGui::EndDisabled();
        back();
        break;
    }

    case Page::Browse: {
        ImGui::TextWrapped("Public lobbies on your matchmaking server. 'Open' means the lobby has a free slot and the same "
                           "LmCoop version; whether a connection can be made is only known once you join.");
        serverNotice(c);
        nameField(c, dirty);
        dirty |= field("Password (if needed)", c.password, 40, true);
        const bool haveServer = c.steam || !c.signalUrl.empty();
        const LobbyList list = c.steam ? steamLobbyList() : lobbyList();
        const bool pressed = ImGui::Button("Refresh");
        if ((entered || pressed) && haveServer) c.steam ? steamRefreshLobbies() : refreshLobbies(c.signalUrl);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", list.busy ? "refreshing..." : !list.error.empty() ? list.error.c_str() : list.loaded ? "" : "not loaded yet");
        const std::string mine = port::update::buildVersion();
        if (ImGui::BeginTable("lobbies", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Lobby");
            ImGui::TableSetupColumn("Players");
            ImGui::TableSetupColumn("Version");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const LobbyInfo& l : list.lobbies) {
                const bool sameBuild = l.build == mine, full = l.players >= l.max;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(), ImGui::TextUnformatted(l.name.c_str());
                ImGui::TableNextColumn(), ImGui::Text("%d/%d", l.players, l.max);
                ImGui::TableNextColumn(), ImGui::TextUnformatted(l.build.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(!sameBuild ? "Different version" : full ? "Full" : l.password ? "Open (password)" : "Open");
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!sameBuild || full);
                ImGui::PushID(l.id.c_str());
                if (ImGui::Button("Join")) {
                    c.inviteCode.clear(), c.lobbyId = l.id;
                    startInternet(c, Mode::Join);
                    room::open(c);
                    go(Page::Lobby);
                }
                ImGui::PopID();
                ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }
        if (list.loaded && list.lobbies.empty()) ImGui::TextDisabled("No public lobbies right now. Host one, or use Join by Code.");
        back();
        break;
    }

    case Page::Code: {
        ImGui::TextWrapped("Enter the invite code your friend shows in their lobby.");
        serverNotice(c);
        nameField(c, dirty);
        std::string code = c.inviteCode;
        if (field("Invite code", code, 8)) {
            code.erase(std::remove_if(code.begin(), code.end(), [](unsigned char ch) { return !std::isalnum(ch); }), code.end());
            std::transform(code.begin(), code.end(), code.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
            c.inviteCode = code, dirty = true;
        }
        dirty |= field("Password (if needed)", c.password, 40, true);
        ImGui::BeginDisabled((c.signalUrl.empty() && !c.steam) || c.inviteCode.size() < 6);
        if (ImGui::Button("Join", ImVec2(160 * scale, 36 * scale))) {
            c.lobbyId.clear();
            startInternet(c, Mode::Join);
            room::open(c);
            go(Page::Lobby);
        }
        ImGui::EndDisabled();
        back();
        break;
    }

    case Page::Direct:
        if (entered && c.internet) c.internet = false, dirty = true;
        ImGui::TextWrapped("Connect straight to an address, on a LAN or a VPN such as Radmin. Host: choose your save and play as Luigi. "
                           "Join: wait on the loading screen, then enter the host's game as Mario.");
        ImGui::Spacing();
        if (dirty) setSavedConfig(c), dirty = false;
        drawHostJoin(false);
        ImGui::BeginDisabled(!readyToStart());
        if (ImGui::Button("Open lobby", ImVec2(160 * scale, 36 * scale))) {
            Config d = savedConfig();
            d.internet = false;
            setSavedConfig(d);
            room::open(d);
            go(Page::Lobby);
        }
        ImGui::EndDisabled();
        back();
        break;

    case Page::Lobby: {
        const room::State st = room::update();
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextUnformatted("LOBBY");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", st.status.c_str());
        if (st.host && !st.code.empty()) {
            ImGui::Text("Invite code: %s", st.code.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Copy")) ImGui::SetClipboardText(st.code.c_str());
        }
        ImGui::Dummy(ImVec2(0, 8 * scale));
        if (!st.failed) {
            // Slot 1 is the host (Luigi), slot 2 the guest (Mario), whichever PC this is.
            const room::Player& hostP = st.host ? st.me : st.other;
            const room::Player& guestP = st.host ? st.other : st.me;
            const float face = 64 * scale;
            auto slot = [&](const room::Player& p, const char* role) {
                ImGui::PushID(role);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (p.avatar) {
                    ImGui::Image(ImTextureID(p.avatar), ImVec2(face, face));
                } else {
                    ImGui::Dummy(ImVec2(face, face));
                    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + face, at.y + face), p.present ? IM_COL32(60, 90, 150, 255) : IM_COL32(35, 40, 55, 255));
                }
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::Dummy(ImVec2(0, face * 0.12f));
                if (p.present) ImGui::Text("%s", p.name.c_str()); else ImGui::TextDisabled("Player slot available");
                ImGui::TextDisabled("%s", role);
                ImGui::EndGroup();
                ImGui::Dummy(ImVec2(0, 4 * scale));
                ImGui::PopID();
            };
            slot(hostP, "Luigi (host)");
            slot(guestP, "Mario");
            if (characters::available().size() > 1) {  // a character pack is installed: each player picks theirs
                ImGui::Dummy(ImVec2(0, 4 * scale));
                ImGui::TextDisabled("Your character");
                characters::drawSelect(scale);
            }
            ImGui::Dummy(ImVec2(0, 8 * scale));
            if (st.host) {
                if (ImGui::Button("Start the game", ImVec2(200 * scale, 36 * scale))) room::start();
                if (st.started) {  // the button or the test autopilot started it
                    room::finish();
                    result = MenuResult::Start;
                }
            } else {
                ImGui::TextDisabled("Waiting for the host to start the game...");
                if (st.started) {
                    room::finish();
                    result = MenuResult::Start;
                }
            }
        }
        ImGui::Dummy(ImVec2(0, 8 * scale));
        if (ImGui::Button("Leave lobby", ImVec2(160 * scale, 34 * scale))) {
            room::leave();
            go(Page::Menu);
        }
        break;
    }

    case Page::Settings: {
        nameField(c, dirty);
        if (steamAvailable()) {
            dirty |= ImGui::Checkbox("Use Steam instead of a matchmaking server", &c.steam);
            ImGui::TextDisabled("Steam must be running. Lobbies and the relay come from Steam; nobody has to host a server.");
        }
        dirty |= field("Matchmaking server", c.signalUrl, 200, false, 380.0f);
        ImGui::TextDisabled("ws://host:8787 for a test server, wss://your.domain for a real one. Nobody hosts one for you: docs/ONLINE_SERVER.md.");
        dirty |= field("STUN / TURN servers", c.iceServers, 400, false, 380.0f);
        ImGui::TextDisabled("Separate with ';'. Empty = public STUN only (stun:stun.l.google.com:19302;stun:stun.cloudflare.com:3478).");
        ImGui::TextDisabled("A TURN relay is only needed when a direct connection fails: turn:user:password@host:3478");
        ImGui::Spacing();
        ImGui::TextWrapped("Privacy: the lobby list never shows addresses. A peer-to-peer connection reveals each player's public "
                           "IP address to the other player, and STUN servers see yours while the connection is set up. "
                           "A TURN relay hides the address from the other player but sees the (encrypted) traffic.");
        back();
        break;
    }
    }

    if (grouped) ImGui::EndGroup();
    ImGui::EndChild();
    if (dirty) setSavedConfig(c);
    if (result == MenuResult::Back || result == MenuResult::Start) go(Page::Menu);
    return result;
}

}  // namespace recomp::online
