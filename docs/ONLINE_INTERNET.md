# LmCoop internet play (developer documentation)

Internet play lets two PCs connect without a VPN or port forwarding when NAT traversal succeeds. It
adds a transport under the existing co-op session and a small matchmaking server. **Game
synchronization is unchanged**: the datagram protocol in `src/recomp/online_protocol.h` is the same
for LAN, Radmin and internet play.

Related: [ONLINE_SERVER.md](ONLINE_SERVER.md) (hosting the server), [ONLINE_PLAYERS.md](ONLINE_PLAYERS.md)
(player setup), `server/` (matchmaking server source).

## 1. Audit of the networking before this work

Verified from source (commit `38ac3b6` and the working tree at that time), not assumed:

| Question | Answer |
|---|---|
| Library | None. Raw sockets (Winsock / BSD) in `src/recomp/online.cpp`. No ENet, no TCP. |
| Socket code | One non-blocking `SOCK_DGRAM` socket: `openSocket` (create, bind), `send` (`sendto`), `receive` (`recvfrom`, at most 64 datagrams per frame), `closeSocket`. |
| Topology | Host binds the address the user typed (never every interface) and accepts one client; a client sends `Hello` to the host's address and port (default 28555). |
| When it runs | `online::frame(Ctx&)`, called once per guest frame from `dev_tools.cpp`. No network thread. |
| Serialization | `online_protocol.h`: little-endian, field by field, header = magic `LMCO`, kind, sequence. Kinds: Hello, Welcome, Reject, State, Bye, GameRequest, Game, Event. Every decoder validates sizes, ranges and floats. |
| Reliability | None. State is sent at 30 Hz and tolerates loss; the client repeats `Hello` and `GameRequest` every 0.5 s. `Event` (cutscene starts) was sent once. 5 s without a packet ends the session. |
| Authentication | `Hello` carries the protocol version, the SHA-1 of `main.dol` (must be GLME01) and a salted SHA-1 of the password (legacy, not a secure password hash). |
| Encryption | None. |
| UI / settings | `online::drawHostJoin`, `drawTab` (F1 > Controls > Online), the start menu in `src/setup/setup.cpp`; settings in `[online]` of `settings.ini` via `port::addSettingsExtension`; `LM_ONLINE*` environment overrides. |
| Build | CMake (Ninja, clang-cl on Windows), `src/recomp/*.cpp` globbed into both executables; Aurora brings SDL3, Dawn, nod. |
| Capacity | Exactly two players (host = Luigi, client = Mario). |

## 2. Design

```
          Host PC                                            Guest PC
   online.cpp (session)                               online.cpp (session)
        |  Transport                                        |  Transport
   RtcTransport  <--- DTLS data channels (ICE) --->  RtcTransport
        |                                                   |
        +---- WebSocket ---> matchmaking server <--- WebSocket ----+
                (lobbies, codes, offer/answer/candidate relay only)
```

* `online_transport.h` defines `Transport` (`send`, `receive`, `status`, `describe`). `online.cpp`
  holds a `std::unique_ptr<Transport>`; nothing else in the game touches sockets.
* `online_udp.cpp`: the original UDP path ("Direct Connect"), same behavior as before.
* `online_rtc.cpp`: libdatachannel 0.24.6 (WebRTC data channels, libjuice ICE with STUN/TURN, DTLS)
  on MbedTLS 3.6.7; nlohmann/json for the signaling messages. All built from source, linked
  statically.
* Two channels per connection. `lmcoop-ctl-<token>`: reliable, ordered (handshake, game snapshot,
  cutscene events). `lmcoop-st-<token>`: unordered, no retransmits (30 Hz player state).
  `<token>` is a random value the server issues per join; the host closes any channel without it.
* `server/`: Node.js + `ws`. Registers lobbies, generates ids and 6-character invite codes
  (alphabet without `I O 0 1`), lists public lobbies, pairs a joiner with the lobby, relays
  negotiation messages **only** between the two members of one lobby. It carries no game traffic and
  runs no game logic.
* Connection order: host registers, guest joins (by code or from the browser), the guest creates the
  offer, both exchange candidates, a direct path is tried, DTLS comes up, then the existing
  `Hello`/password/game-hash handshake runs over the encrypted channel. If ICE fails, the status
  line says so and names the TURN option. A TURN relay is used only if the user configured one.
* Threads: libdatachannel callbacks run on library threads and only touch a mutex-protected queue
  and status. The guest thread calls `receive()` (pops a queue) and `send()` (hands a buffer to the
  library). Opening and closing peer connections is asynchronous; teardown runs on a detached
  thread.

### Files

| File | Role |
|---|---|
| `src/recomp/online.cpp` / `online.h` | Session; now over `Transport`. Settings keys, `LM_ONLINE_*` aids, lobby panel. |
| `src/recomp/online_transport.h` | Transport interface and `NetConfig` / `NetStatus`. |
| `src/recomp/online_udp.cpp` | UDP transport. |
| `src/recomp/online_rtc.cpp` | WebRTC transport, signaling client, lobby list fetch. |
| `src/recomp/online_lobby.h` | Lobby list types for the browser. |
| `src/recomp/online_config.h` | `Config` shared by the session and the screens. |
| `src/recomp/online_ui.cpp` | Host Online, Browse Lobbies, Join by Code, Direct Connect, Settings. |
| `src/setup/setup.cpp` | Start menu hooks the screens in. |
| `server/src/server.js`, `main.js` | Matchmaking server. |
| `tools/recomp/online_rtc_test.cpp` | Transport test program (`lm_rtc_test`). |
| `packaging/mbedtls/lmcoop_config.h` | Enables DTLS-SRTP in MbedTLS (libdatachannel needs it). |

### Signaling protocol (JSON over WebSocket, version 1)

Client to server: `host {proto, game, name, build, public, password}`, `list`,
`join {proto, game, build, code | id}`, `signal {data}`, `leave`, `ping`.
Server to client: `hosted {id, code}`, `lobbies {lobbies[]}`, `joined {token, ...}`,
`peer-joined {token}`, `peer-left`, `signal {data}`, `closed {reason}`, `error {error}`.
Lobby listings carry id, name, players, max, password flag, build, proto, and no addresses.

## 3. Security and privacy

* Transport is DTLS (from WebRTC). The password never reaches the server: it is checked in the game
  handshake over the encrypted channel (as a hash; see limits).
* Trust model: DTLS fingerprints travel through the matchmaking server, so a malicious server could
  mount a man-in-the-middle. Players should only use servers they trust (ideally their own).
* The server validates every field, caps message size (8 KB), rate limits each connection (burst 30,
  10/s), caps connections per IP and total lobbies, drops dead sockets with a ping/pong sweep and
  expires idle lobbies. Lobbies disappear when their host disconnects.
* Packet sizes and kinds are validated before game code sees them (`decode`, `decodeState`, ...);
  the data-channel receive path also drops empty and oversized messages and bounds its queue.
* IP addresses: lobby lists never show them. **A peer-to-peer connection still reveals each player's
  public IP address to the other player**, and STUN servers see it while the link is set up. A TURN
  relay hides the address from the other player but sees the (encrypted) traffic.
* Default STUN servers are public ones (Google, Cloudflare). Change them in Multiplayer > Settings.

## 4. Testing

Run: `server/`: `npm test` (7 server tests, plus 4 end-to-end tests when `lm_rtc_test` is built).
Game: `Run-Crash-Tests.cmd --exe <lmrecomp.exe> --only online-` (two game instances; the
`online-internet-*` cases start a local matchmaking server themselves).

Results of 2026-11-02 (Windows 11, one PC, loopback). See section 6 for what this does **not** cover.
Other LAN cases on the working tree (which also holds unfinished, uncommitted cutscene-sync work by another assistant): `online-handshake`, `online-bad-password`, `online-names`, `online-client-drop` passed; `online-puppet` failed there with `client:setup_failed`, but passes on the committed code alone, so that failure came from the unfinished cutscene work. `online-map-mismatch` fails (`host:assertion_failed`) on the untouched 1.2.3 baseline too (2 of 2 runs), so it predates this work and was not investigated further. `online-internet-handshake` and `online-internet-bad-password` passed. Internet cases give the host a longer run than the LAN ones because the client starts after the invite code appears.

| # | Requirement | Result |
|---|---|---|
| 1 | LAN multiplayer still works | `online-handshake` (UDP, two instances) passed on the new transport layer. |
| 2 | Radmin still works | Same UDP code path; **not tested on Radmin itself**. |
| 3 | Two networks, no VPN | **Not tested.** Needs two real networks; see section 5. |
| 4 | Private invite codes | Server test (private lobby unlisted, joined by code); `online-internet-handshake` joins by code in the real game. |
| 5 | Public lobby discovery | Server test (listing contents); the Browse screen is **not exercised in a running game**. |
| 6 | Disconnects do not crash | `online-internet-client-drop` passed (client killed mid-game; host logs the disconnect and hides player 2). LAN `online-client-drop` passed. |
| 7 | Synchronization as before | `online-internet-puppet` passed (both PCs agree on both players' positions after a teleport). `online-internet-input` (client pad moves player 2 on the host) **failed**, but the same LAN test `online-input` is timing-flaky on the untouched 1.2.3 build too (passed 1 of 4 runs there, 1 of 3 on this work without the other pending changes), so it proves nothing either way. |
| 8 | Room transitions | Not re-tested separately; the session code is unchanged. |
| 9 | Failed NAT traversal handled | Unreachable server, unknown code and version mismatch are tested and produce clear messages. ICE failure itself is **not reproduced**; the message is untested. |
| 10 | No frame-time stalls | Network work is off the guest thread by construction. **Frame times were not measured.** |

## 5. Tests that need real networks

1. Two PCs on different home networks, no VPN, default STUN, a server on a VPS: host, join by code.
2. Symmetric NAT or CGNAT on one side, no TURN: expect the "could not connect directly" message.
3. The same with a coturn relay configured in Settings: expect a link and the game running.
4. `wss://` through a reverse proxy (certificate verification uses the Windows root store, exported to
   `%TEMP%\lmcoop-roots.pem`; Linux and macOS read the system bundle). Only `ws://` was tested.
5. Long session (30+ minutes) for reconnect and timeout behavior.

## 6. Known limitations

* No free public matchmaking server is provided or assumed. Players need a server address
  (ONLINE_SERVER.md). Nothing is hosted for you.
* No relay unless you run or rent a TURN server. Some connections (symmetric NAT,
  CGNAT, strict firewalls) cannot connect peer to peer; how many depends on your players' ISPs.
* Two players only, because the game supports two.
* The browser's "status" is availability (open/full/different version/password), not connection
  quality; the real result appears after joining.
* The game password check still uses the legacy salted SHA-1, and `settings.ini` stores the
  password in plain text. Both predate this work.
* After the link drops, the player returns to the start menu; there is no automatic reconnect.
* Built and tested on Windows only. The code is portable (libdatachannel, MbedTLS, POSIX paths in
  the CA-bundle lookup), but Linux and macOS builds were not run.
* `wss://` certificate checking is implemented but untested against a real certificate.
* SM64CoopDX's CoopNet is AGPL-3.0 (`coop-deluxe/coopnet`); none of its code is used here.

## 7. Build

CMake fetches libdatachannel (`v0.24.6`, submodules libjuice, plog, usrsctp only), MbedTLS
(`mbedtls-3.6.7`) and nlohmann/json (3.11.3, hash-checked). Configure once with network access.
Test helper: `powershell -File tools/build.ps1 -Target lm_rtc_test`.

## 8. Cutscenes, aim and spawn (protocol 4)

* The host chooses when a cinematic event starts (`online_events.cpp`); both PCs then run the game's own script and camera, dialogue is read independently, and choices and leaving the scene wait for both. The guest ignores event packets until it has joined the host's game (the host repeats them every 150 ms).
* Mouse aim and mouse turning are not pad input, so the state packet also carries the player's facing (actor +0x88) and flashlight/vacuum aim; each PC applies them to its copy of the other player (`mouselook::setRemote`).
* A joining player is placed 70 units beside the host's player, not inside it.
* Tests added: `online-aim` (both PCs show the same aim and facing), `online-spawn` (host in another room, both players start there), `tools/recomp/online_protocol_test.cpp`. Not covered: dialogue choices, the opening and ending scenes, a spawn offset that lands in a wall.
