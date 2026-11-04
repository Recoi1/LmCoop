# Character packs

A character pack replaces the playable Luigi's model and voice with those of a mod, chosen before the game
starts. Nothing of any mod is part of LmCoop or its releases: you make a pack from **your own disc** and the
mod's `.xdelta` patch, on your own PC.

## Making a pack

Needs Python 3.

1. A plain ISO of your USA disc: `python tools/make_iso.py <extracted dump> GLME01.iso` (the extracted dump is
   the Dolphin "Extract Entire Disc" layout).
2. `python tools/make_character_pack.py --disc GLME01.iso --patch mod.xdelta --out "<dataDir>/characters/<id>" --name "Display name"`

`<dataDir>` is the folder the game prints at startup (`[install] data folder ...`), on Windows
`%APPDATA%\Luigi's Mansion Recompiled`. The tool applies the patch to a temporary copy (a few minutes, 1.4 GB)
and keeps only:

| File | What |
|---|---|
| `luige.mdl` | the playable Luigi's model (`luige/model/luige.mdl` inside `data/model/luige.arc` inside `Game/game_usa.szp`) |
| `LuiSec0_0.aw`, `LuiSec1_0.aw`, `LuiSec2_0.aw`, `JaiInit.aaf` | the voice banks |
| `pack.ini` | `name=...` |

It refuses a patch that does not change `luige.mdl`. The model must have the original's size (it is written over
the original's bytes), which holds for model/texture edits like Wega's Freaky Mansion v1.1.

The `.xdelta` is decoded by `tools/characters/vcdiff.py` (VCDIFF with xdelta3's LZMA secondary compression, every
window's adler32 checked), so no xdelta program is needed.

## Using it

With a pack installed the **singleplayer lobby** (Singleplayer on the start menu) and the **multiplayer lobby**
show a "Your character" row; the choice is saved (`[character] id=` in `settings.ini`) and applied when the game
boots. `LM_CHARACTER=<id>` overrides it, and `LM_CHARACTER_DIR=<folder>` offers a pack from any folder as id `env`.

How: the disc layer serves the pack's voice banks in place of the disc's, and after the game decompresses
`Game/game_usa.szp` the pack's `luige.mdl` is written over the original's bytes (`src/recomp/characters.cpp`).

## Limits

* The voice banks are the mod's whole banks (`LuiSec*` are the section banks), so other sounds the mod changed
  come with them.
* Cutscene models (`dluige01-03`, the `ADemo` demos) are not replaced yet: cutscenes still show the original Luigi.
* **Multiplayer:** the choice changes how the playable Luigi (the host's player) looks and sounds **on your own PC**.
  It is not sent to the other player, and the second player is still Mario. Playing the second player as a
  character, and showing each player's character to the other, is not done.
