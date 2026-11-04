# Steam transport (optional)

Online co-op over Steam lobbies and Steam's relay network: no matchmaking server, no port forwarding,
and neither player sees the other's address. It is a third transport next to Direct Connect and the
WebRTC one (`src/recomp/online_steam.cpp`). Datagrams, passwords and the game sync are unchanged.

## How it works

* The game runs as Steam app **480 ("Spacewar")**, Valve's public test app. `steam_appid.txt` (containing
  480) is written next to the exe on first use. Steam must be running and logged in.
* The host creates a 2-slot lobby carrying `lmcoop=1`, the game build, a 6-character invite code, the
  lobby name and whether it is listed. Join by Code searches for `lmcoop=1` and the code; Browse Lobbies
  lists `lmcoop=1` + `listed=1`. Everybody using app 480 shares Steam's lobby pool, so those filters
  matter. A different `build` is refused with "the host runs a different LmCoop version".
* Game datagrams are `ISteamNetworkingMessages` (reliable for everything except the 30 Hz state), and
  only the other lobby member is accepted.

## Building it

The official Windows releases include it: the build downloads the SDK's headers and `steam_api64.*` from a
draft release in this repository (collaborators only; `.github/workflows/build.yml`), and without that access
(forks) it builds without Steam. To build it yourself (the SDK is Valve's and is not in this repository),
download the Steamworks SDK, then configure with its `sdk` folder:

    cmake -DLM_STEAM_SDK_DIR=C:/path/to/steamworks_sdk/sdk build/windows-clang

Windows only. The build links `steam_api64.lib`, copies `steam_api64.dll` beside the exe and installs it
into the package. Without the option the Steam checkbox is hidden and choosing Steam reports that the
build has no Steam support.

## Using it

Multiplayer > Settings > "Use Steam instead of a matchmaking server", then Host Online / Join by Code /
Browse Lobbies as usual. The test aid is `LM_ONLINE_NET=steam`.

## Tested / not tested

Tested on one PC: Steam initialisation, lobby creation with an invite code, the Settings and Host pages,
finding a lobby by code and joining it, the version check, and the refusal to join your own lobby. **Not
tested: a real session between two Steam accounts** (the data path, the relay, Browse Lobbies listing
another player's lobby): Steam lets one account join its own lobby but there is nobody on the other end.

## Terms

Valve intends app 480 for testing. Whether shipping a public release under it is allowed depends on
Valve's Steamworks agreements, which are not in the SDK download; read them before distributing a Steam
build.
