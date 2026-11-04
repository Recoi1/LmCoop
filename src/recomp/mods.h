#pragma once
// The in-game mod browser (Mods on the start menu): Luigi's Mansion mods listed on GameBanana (game 7120, its
// public API) and, for each, what the LmCoop developers have tested. The test results are a small JSON file in
// this repository (data/mod_compat.json), read from GitHub so it can change between releases. Nothing is hosted
// here and nothing is downloaded or run from GameBanana by this screen: "Open page" sends you to the mod's page.
#include <string>
#include <vector>

namespace recomp::mods {

struct Entry {
    int id = 0;                 // GameBanana mod id
    std::string name, author, category, url;
    int likes = 0;
    std::string status;         // "verified", "partial", "broken" from the developers' list; "" = untested
    std::string note;           // what was tested, and how it works with LmCoop
};
struct State {
    bool busy = false;
    bool loaded = false;        // a list (possibly empty) has arrived
    std::string error;          // GameBanana could not be reached
    bool haveTests = false;     // the developers' list arrived too
    std::vector<Entry> mods;
};

void refresh();                 // asks GameBanana and GitHub in the background; returns at once
State state();                  // newest snapshot
void openPage(const std::string& url);  // default browser; only https://gamebanana.com/ pages

}  // namespace recomp::mods
