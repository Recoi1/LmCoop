# Mods

The start menu's **Mods** screen lists Luigi's Mansion mods from GameBanana (game 7120, its public API) with what
the LmCoop developers have tested: **Verified** (works), **Partial** (part of it works), **Broken**, **Untested**.
The test results are `data/mod_compat.json` in this repository, read from GitHub (so they can change between
releases). Nothing is hosted by LmCoop: downloads come straight from GameBanana, on your PC, when you press
Install.

## What can be installed from the game

| Kind | How it works |
|---|---|
| **Texture packs** (Dolphin style: `tex1_<w>x<h>_<hash>_<fmt>.png` / `.dds`) | Unpacked to `<dataDir>/texture_packs/gb<id>/` and registered with the renderer (Aurora's texture replacement); on and off at once from the Mods screen. |
| **Loose game files** (an archive with a `files/` folder of disc paths: `files/Map/map2.szp`, ...) | Same folder; while enabled, the disc layer serves those files instead of the disc's. The mod's `sys/` files (the game's program file) are not used. |
| **Character packs** (a mod's Luigi and voice) | Made with `tools/make_character_pack.py`, see CHARACTERS.md. |

Install = download (`.zip`, `.7z` or `.rar`, only from gamebanana.com), unpack with the system's `tar`, check that the
archive holds one of the kinds above (otherwise it is deleted again and the screen says why), enable.

## What cannot

Mods that change the game's **code** (full overhauls such as Premium Deluxe, the randomizer, Beta Restoration)
come as patches against a whole disc and need the game rebuilt from a changed program file; they show "Open page"
only. Save files and texture packs for other emulators are not installable either.

## Compatibility list

`data/mod_compat.json`, keyed by GameBanana mod id: `status` (verified / partial / broken) and a `note` saying what
was tested. A mod not in the file shows as Untested. Add entries after testing with a pull request.

## Limits

* Several enabled mods that replace the same file: the later one wins. A character pack and a loose-files mod that
  both replace `Game/game_usa.szp` conflict: the loose-files version is served and the pack's Luigi model is not
  written over it.
* `LM_TEXTURE_PACK_DIR=<folder>` loads any folder as a texture pack (testing).
* Windows is tested; on Linux and macOS the installer uses `unzip`/`tar` and has not been run.
