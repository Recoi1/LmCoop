LmCoop 1.7.0 - Windows x64
=========================

Download/source: https://github.com/Recoi1/LmCoop
Based on rob646464/luigis-mansion-recompiled.

No retail game data is included. Supply your own USA Luigi's Mansion disc dump
(GLME01 revision 0). Setup builds the game module on your PC.

REQUIREMENTS AND INSTALLATION
- Windows 10/11, 64-bit, Direct3D 12 capable graphics card.
- About 2 GB free space for the module and caches, plus your own disc image.
- Extract the whole ZIP to a writable folder. Keep the DLLs and llvm folder.
- Run LmCoop.exe. Drag your USA ISO/RVZ onto the setup screen,
  or use Browse. Wait for module compilation and shader preparation.
- Choose Singleplayer or Multiplayer at the start menu.
- Settings on the start screen configures visuals, sound, keyboard/mouse and controllers.
- The bottom-right corner shows the version and checks for updates automatically.
  Click the update status for release notes and download/install controls.

MULTIPLAYER (EXPERIMENTAL)
Both PCs need the same build and their own USA disc dump. Choose Host (Luigi)
on one PC and Join (Mario) on the other. The host chooses a save; the joining
player waits on the loading screen and enters the host's current game directly.
Player 2 does not choose a local save, and their own saves remain read-only.
Both PCs must use version 1.2.0 or newer. Use the host's reachable address and
matching port/password. For internet play the UI recommends a shared Radmin VPN
network and the host's VPN address. Two-PC synchronization and a complete co-op
campaign have not been verified for this release; the underlying two-player
mode remains unfinished.

CONTROLS
F1 opens settings and configurable keyboard/mouse/controller bindings.
Controls opens a visual GameCube controller page: select a player and input device,
then click the binding beside a button or stick direction to remap it. Alternate
bindings, the full binding list, calibration and rumble remain available.
Enter is Start. Mouse selection works on the file selection screen.

DATA AND TROUBLESHOOTING
Settings, caches, logs and saves normally live in:
  %APPDATA%\Luigi's Mansion Recompiled
Create portable.txt beside the executable before first launch to keep them
in a portable folder here instead. Back up saves before experimental co-op.
If startup fails, check last_run.log in the data folder and the recovery UI.
To uninstall, delete this extracted folder; AppData saves remain separately.

CONTENTS
LmCoop.exe: setup, recompiler and runtime, without the retail game.
LuigisMansionRecomp.exe: the same program under its old name, kept so older versions can update.
*.dll: runtime libraries. pipelines.bin: shader precache list.
llvm/bin/clang.exe and lld-link.exe: first-run compiler/linker.
NOTICE.md and licenses/: third-party notices and licenses.
llvm/LLVM-LICENSE.TXT: Apache-2.0 WITH LLVM-exception.

Updates are checked from Recoi1/LmCoop on the start screen, before gameplay.
Older inherited technical documents
may refer to the original project's features and release pages.
