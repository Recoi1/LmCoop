# Hosting the LmCoop matchmaking server

The server is a small Node.js program (`server/`). It lists lobbies, makes invite codes and relays the
WebRTC negotiation between the two players. **It never carries game traffic and never simulates the
game**, so it needs very little CPU and bandwidth. There is no free permanent hosting you can count on,
so nothing is hosted for you: run it on a PC at home, a cheap VPS, or any machine that stays on while
people play. Everything below is free and open source.

## Run it

Requirements: Node.js 20 or newer.

```
cd server
npm install --omit=dev
npm test                      # optional: 7 tests
PORT=8787 HOST=0.0.0.0 node src/main.js
```

Players then enter `ws://YOUR-ADDRESS:8787` under Multiplayer > Settings > Matchmaking server.

| Environment variable | Default | Meaning |
|---|---|---|
| `PORT` | 8787 | TCP port |
| `HOST` | 0.0.0.0 | Address to listen on (`127.0.0.1` behind a reverse proxy) |
| `TRUST_PROXY` | off | Set `1` behind a reverse proxy that sets `X-Forwarded-For`; the per-IP connection limit then uses the real client address |
| `MAX_LOBBIES` | 1000 | Total lobbies |

Built-in limits (edit `DEFAULTS` in `server/src/server.js`): 8 connections per IP, 8 KB messages,
30-message burst then 10 messages per second per connection, idle lobbies expire after 6 hours.
`GET /health` returns `{"ok":true,...}`.

Plain `ws://` is fine on a LAN or for testing, but it is unencrypted: the negotiation (including your
public IP in the connection candidates) travels in clear text. For the internet use `wss://`.

## wss:// with Caddy (free, automatic certificates)

You need a domain name that points at the server. `Caddyfile`:

```
lmcoop.example.com {
    reverse_proxy 127.0.0.1:8787
}
```

Run the server with `HOST=127.0.0.1 TRUST_PROXY=1`. Players enter `wss://lmcoop.example.com`.
(Caddy was not run in this project's tests; any reverse proxy that forwards WebSockets works.)

## Keeping it running (Linux, systemd)

`/etc/systemd/system/lmcoop-matchmaking.service`:

```
[Unit]
Description=LmCoop matchmaking server
After=network.target

[Service]
WorkingDirectory=/opt/lmcoop/server
Environment=PORT=8787 HOST=127.0.0.1 TRUST_PROXY=1
ExecStart=/usr/bin/node src/main.js
Restart=on-failure
DynamicUser=yes

[Install]
WantedBy=multi-user.target
```

## Optional: a TURN relay for connections that cannot go direct

Most connections go directly with STUN alone. When both players are behind strict NATs, a TURN relay is
the only way. A relay carries all game traffic, so it uses real bandwidth (a 2-player session is small,
but you pay for it), and anyone with the credentials can use it. Do not publish credentials you cannot
afford to share.

Free software: [coturn](https://github.com/coturn/coturn). A minimal `turnserver.conf`:

```
listening-port=3478
realm=lmcoop.example.com
lt-cred-mech
user=lmcoop:CHANGE-THIS-PASSWORD
fingerprint
no-multicast-peers
```

Open UDP 3478 and the relay port range (default 49152-65535, narrow it with `min-port`/`max-port`).
Players then add `turn:lmcoop:CHANGE-THIS-PASSWORD@lmcoop.example.com:3478` to Multiplayer > Settings >
STUN / TURN servers (separated from other entries by `;`). The relayed traffic is still DTLS encrypted.
This coturn setup was **not tested** in this project.

## What the server can see

Lobby names, build versions, invite codes, and the connection candidates players exchange (which include
their IP addresses) while they set up a connection. It does not see game data or passwords. A malicious
server operator could interfere with the negotiation, so only use servers you trust.
