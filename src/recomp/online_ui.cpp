// LmCoop's multiplayer screens on the start menu: Host Online, Browse Lobbies, Join by Code,
// Direct Connect and Settings (setup.cpp draws drawMultiplayerMenu in place of the old host/join form).
// The screens only edit the saved online settings; the session (online.cpp) acts on them once the
// game starts. The lobby itself (invite code, players, leave) is in the F1 > Controls > Online tab.
#include "recomp/online.h"
#include "recomp/online_config.h"
#include "recomp/online_lobby.h"

#include "port/update.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace recomp::online {
namespace {

enum class Page { Menu, Host, Browse, Code, Direct, Settings };
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
    if (c.signalUrl.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1), "No matchmaking server set. Open Settings and enter one (docs/ONLINE_SERVER.md).");
        if (ImGui::Button("Open Settings")) go(Page::Settings);
    }
}

void startInternet(Config& c, Mode mode) {
    c.mode = mode, c.internet = true;
    setSavedConfig(c);
}

}  // namespace

MenuResult drawMultiplayerMenu(float scale) {
    Config c = savedConfig();
    bool dirty = false;
    MenuResult result = MenuResult::Stay;
    const ImVec2 big(ImGui::GetWindowWidth() * 0.25f, 44 * scale);
    auto bigButton = [&](const char* label) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - big.x) * 0.5f);
        const bool hit = ImGui::Button(label, big);
        ImGui::Dummy(ImVec2(0, 6 * scale));
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
    if (sPage != Page::Menu) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
        ImGui::BeginGroup();
    }
    const bool entered = sPageEntered;
    sPageEntered = false;

    switch (sPage) {
    case Page::Menu:
        if (bigButton("Host Online")) go(Page::Host);
        if (bigButton("Browse Lobbies")) go(Page::Browse);
        if (bigButton("Join by Code")) go(Page::Code);
        if (bigButton("Direct Connect")) go(Page::Direct);
        if (bigButton("Settings")) go(Page::Settings);
        back();
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
        ImGui::BeginDisabled(c.signalUrl.empty() || c.lobbyName.empty());
        if (ImGui::Button("Create lobby", ImVec2(160 * scale, 36 * scale))) {
            startInternet(c, Mode::Host);
            result = MenuResult::Start;
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
        const bool haveServer = !c.signalUrl.empty();
        const LobbyList list = lobbyList();
        const bool pressed = ImGui::Button("Refresh");
        if ((entered || pressed) && haveServer) refreshLobbies(c.signalUrl);
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
                    result = MenuResult::Start;
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
        ImGui::BeginDisabled(c.signalUrl.empty() || c.inviteCode.size() < 6);
        if (ImGui::Button("Join", ImVec2(160 * scale, 36 * scale))) {
            c.lobbyId.clear();
            startInternet(c, Mode::Join);
            result = MenuResult::Start;
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
        if (ImGui::Button("Start", ImVec2(160 * scale, 36 * scale))) {
            Config d = savedConfig();
            d.internet = false;
            setSavedConfig(d);
            result = MenuResult::Start;
        }
        ImGui::EndDisabled();
        back();
        break;

    case Page::Settings: {
        nameField(c, dirty);
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

    if (sPage != Page::Menu) ImGui::EndGroup();
    if (dirty) setSavedConfig(c);
    if (result == MenuResult::Back || result == MenuResult::Start) go(Page::Menu);
    return result;
}

}  // namespace recomp::online
