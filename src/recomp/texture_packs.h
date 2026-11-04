#pragma once
// Texture packs: folders of replacement textures in Dolphin's naming ("tex1_<w>x<h>_<hash>_<fmt>.png", also
// .dds), as the texture packs on GameBanana are made. Aurora (the renderer) swaps them in when the game
// uploads a texture (extern/aurora/lib/gfx/texture_replacement.cpp). Packs live in
// <dataDir>/texture_packs/<id>/; the enabled ones are registered at startup and when changed. A pack that
// comes from GameBanana has the id gb<mod id> and is installed from the Mods screen (download, unpack, check
// that it holds textures). Nothing of any pack is part of LmCoop.
#include "recomp/ctx.h"

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::texpacks {

struct Info {
    std::string id, name;
    bool enabled = false;
    int textures = 0;     // texture replacements found (counted when installed)
    int files = 0;        // loose game files found
    std::string version;  // the mod's version on GameBanana when installed
};
std::vector<Info> installed();
void setEnabled(const std::string& id, bool on);  // saved with the settings; applied at once
bool remove(const std::string& id);               // deletes the pack's folder
void apply();                                     // (re)registers the enabled packs with the renderer
void pump();                                      // UI thread, every frame: applies a finished install
bool installedFromMod(int gbModId);

// Loose game files: a pack with a files/ folder (disc paths below it) replaces those disc files while enabled.
// Served by the disc layer (hle_dvd.cpp).
s32 fileOverlayEntry(const char* guestPath);  // -1 when not replaced
bool isFileOverlay(s32 entry);
u32 fileOverlayLength(s32 entry);
s32 fileOverlayRead(s32 entry, void* dst, s32 length, s32 offset);

enum class Stage { Idle, Resolving, Downloading, Unpacking, Done, Failed };
struct InstallState {
    Stage stage = Stage::Idle;
    int modId = 0;
    std::string name, message;   // Done: what was installed; Failed: why
    std::uint64_t got = 0;       // bytes downloaded so far
    std::uint64_t total = 0;     // the file's size (from GameBanana)
};
void install(int gbModId, const std::string& name);  // in the background; one at a time
InstallState installState();
void cancelInstall();

}  // namespace recomp::texpacks
