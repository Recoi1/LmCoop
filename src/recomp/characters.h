#pragma once
// Alternative player characters ("character packs"). A pack is a folder <dataDir>/characters/<id>/ with a
// pack.ini (name=...) and replacement files taken from a mod disc by tools/make_character_pack.py:
//   luige.mdl                    the playable Luigi's model, put into Game/game_usa.szp (data/model/luige.arc)
//   LuiSec0_0.aw LuiSec1_0.aw LuiSec2_0.aw JaiInit.aaf   the voice banks, served in place of the disc's
// The choice (the lobby, or the singleplayer lobby) is made before the game boots; the disc layer
// (hle_dvd.cpp) then serves the replaced files. Nothing of a pack is part of LmCoop or its releases.
#include "recomp/ctx.h"

#include <string>
#include <vector>

namespace recomp::characters {

struct Info {
    std::string id, name;
};
std::vector<Info> available();       // "luigi" (the original game) first, then every installed pack
std::string selected();              // the saved choice; "luigi" when none or when its pack is gone
void select(const std::string& id);  // saved with the settings
bool drawSelect(float scale);        // ImGui: one button per character. True when the choice changed.

// Host-served disc files while a pack is selected (hle_dvd.cpp).
void afterDecompress(u32 dest, u32 size);  // hle_dvd.cpp: after the game decompressed a file to guest memory
s32 overlayEntry(const char* guestPath);  // -1 when not redirected
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset);

}  // namespace recomp::characters
