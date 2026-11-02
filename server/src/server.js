// LmCoop matchmaking + signaling server. It lists lobbies and relays WebRTC negotiation messages
// between the two peers of a lobby. It never sees game traffic and never lists IP addresses.
'use strict';
const crypto = require('node:crypto');
const http = require('node:http');
const { WebSocketServer } = require('ws');

const PROTOCOL = 1; // signaling protocol, bumped when messages change
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // no I, O, 0, 1
const CODE_LENGTH = 6;

const DEFAULTS = {
  port: 8787,
  host: '0.0.0.0',
  maxLobbies: 1000,
  maxConnectionsPerIp: 8,
  maxMessageBytes: 8192, // an SDP offer with candidates stays far below this
  heartbeatMs: 20000,
  lobbyIdleMs: 6 * 3600 * 1000, // a host left connected for this long without a joiner is dropped
  rateBurst: 30,
  ratePerSecond: 10,
  trustProxy: false, // true behind a reverse proxy that sets X-Forwarded-For
  games: ['GLME01'],
  maxPlayers: 2, // the game supports exactly two players
};

const cleanText = (s, max) =>
  typeof s === 'string' ? s.replace(/[^\x20-\x7E]/g, '').trim().slice(0, max) : '';

function createServer(options = {}) {
  const cfg = { ...DEFAULTS, ...options };
  const lobbies = new Map(); // id -> lobby
  const codes = new Map(); // code -> lobby
  const connectionsByIp = new Map();
  const clients = new Set();

  const httpServer = http.createServer((req, res) => {
    if (req.url === '/health') {
      res.writeHead(200, { 'content-type': 'application/json' });
      return res.end(JSON.stringify({ ok: true, protocol: PROTOCOL, lobbies: lobbies.size }));
    }
    res.writeHead(404).end();
  });
  const wss = new WebSocketServer({ server: httpServer, maxPayload: cfg.maxMessageBytes });

  function makeCode() {
    for (let tries = 0; tries < 50; tries++) {
      let code = '';
      for (let i = 0; i < CODE_LENGTH; i++) code += CODE_ALPHABET[crypto.randomInt(CODE_ALPHABET.length)];
      if (!codes.has(code)) return code;
    }
    return null;
  }

  const send = (ws, msg) => { if (ws.readyState === 1) ws.send(JSON.stringify(msg)); };
  const fail = (ws, error, extra = {}) => send(ws, { t: 'error', error, ...extra });

  function publicView(l) {
    // No addresses here: only what a browser screen needs.
    return { id: l.id, name: l.name, players: 1 + (l.guest ? 1 : 0), max: cfg.maxPlayers,
             password: l.password, build: l.build, proto: l.proto };
  }

  function removeLobby(l, reason) {
    if (!lobbies.delete(l.id)) return;
    codes.delete(l.code);
    if (l.guest) { send(l.guest, { t: 'closed', reason }); l.guest.lobby = null; l.guest.role = null; }
    send(l.host, { t: 'closed', reason });
    l.host.lobby = null; l.host.role = null;
  }

  function leave(ws, reason) {
    const l = ws.lobby;
    if (!l) return;
    if (ws.role === 'host') return removeLobby(l, reason);
    if (l.guest === ws) {
      l.guest = null, l.token = null;
      send(l.host, { t: 'peer-left' });
    }
    ws.lobby = null, ws.role = null;
  }

  const handlers = {
    host(ws, m) {
      if (ws.lobby) return fail(ws, 'already-in-lobby');
      if (m.proto !== PROTOCOL) return fail(ws, 'protocol-mismatch', { expected: PROTOCOL });
      if (!cfg.games.includes(m.game)) return fail(ws, 'unsupported-game');
      if (lobbies.size >= cfg.maxLobbies) return fail(ws, 'server-full');
      const name = cleanText(m.name, 24);
      const build = cleanText(m.build, 16);
      if (!name || !build) return fail(ws, 'bad-request');
      const code = makeCode();
      if (!code) return fail(ws, 'server-full');
      const l = { id: crypto.randomUUID(), code, name, build, proto: m.proto, game: m.game,
                  public: m.public === true, password: m.password === true,
                  host: ws, guest: null, token: null, created: Date.now() };
      lobbies.set(l.id, l), codes.set(code, l);
      ws.lobby = l, ws.role = 'host';
      send(ws, { t: 'hosted', id: l.id, code });
    },

    list(ws) {
      const out = [];
      for (const l of lobbies.values()) if (l.public) out.push(publicView(l));
      send(ws, { t: 'lobbies', lobbies: out });
    },

    join(ws, m) {
      if (ws.lobby) return fail(ws, 'already-in-lobby');
      if (m.proto !== PROTOCOL) return fail(ws, 'protocol-mismatch', { expected: PROTOCOL });
      let l = null;
      if (typeof m.code === 'string') l = codes.get(m.code.toUpperCase().replace(/[^A-Z0-9]/g, ''));
      else if (typeof m.id === 'string' && lobbies.get(m.id)?.public) l = lobbies.get(m.id);
      if (!l) return fail(ws, 'lobby-not-found');
      if (l.game !== m.game) return fail(ws, 'unsupported-game');
      if (m.build !== l.build) return fail(ws, 'build-mismatch', { expected: l.build });
      if (l.guest) return fail(ws, 'lobby-full');
      l.guest = ws, l.token = crypto.randomBytes(16).toString('hex'); // channel label the host expects
      ws.lobby = l, ws.role = 'guest';
      send(ws, { t: 'joined', id: l.id, name: l.name, password: l.password, token: l.token });
      send(l.host, { t: 'peer-joined', token: l.token });
    },

    // Negotiation messages go only to the other member of the same lobby.
    signal(ws, m) {
      const l = ws.lobby;
      if (!l) return fail(ws, 'not-in-lobby');
      const to = ws.role === 'host' ? l.guest : l.host;
      if (!to) return fail(ws, 'peer-missing');
      if (typeof m.data !== 'object' || m.data === null || JSON.stringify(m.data).length > 6000)
        return fail(ws, 'bad-request');
      send(to, { t: 'signal', data: m.data });
    },

    leave(ws) { leave(ws, 'left'); send(ws, { t: 'left' }); },
    ping(ws) { send(ws, { t: 'pong' }); },
  };

  const clientIp = (req) =>
    (cfg.trustProxy && req.headers['x-forwarded-for']?.split(',')[0].trim()) || req.socket.remoteAddress;

  wss.on('connection', (ws, req) => {
    const ip = clientIp(req);
    const n = connectionsByIp.get(ip) || 0;
    if (n >= cfg.maxConnectionsPerIp) return ws.close(1008, 'too-many-connections');
    connectionsByIp.set(ip, n + 1);
    clients.add(ws);
    ws.alive = true, ws.lobby = null, ws.role = null;
    ws.tokens = cfg.rateBurst, ws.refill = Date.now();

    ws.on('pong', () => { ws.alive = true; });
    ws.on('message', (data, isBinary) => {
      const now = Date.now();
      ws.tokens = Math.min(cfg.rateBurst, ws.tokens + (now - ws.refill) / 1000 * cfg.ratePerSecond);
      ws.refill = now;
      if (ws.tokens < 1) return ws.close(1008, 'rate-limited');
      ws.tokens -= 1;
      if (isBinary) return fail(ws, 'bad-request');
      let m;
      try { m = JSON.parse(data.toString('utf8')); } catch { return fail(ws, 'bad-request'); }
      if (typeof m !== 'object' || m === null || typeof m.t !== 'string' ||
          !Object.hasOwn(handlers, m.t)) return fail(ws, 'bad-request');
      try { handlers[m.t](ws, m); } catch { fail(ws, 'internal'); }
    });
    ws.on('close', () => {
      leave(ws, 'disconnected');
      clients.delete(ws);
      const left = (connectionsByIp.get(ip) || 1) - 1;
      if (left > 0) connectionsByIp.set(ip, left); else connectionsByIp.delete(ip);
    });
    ws.on('error', () => ws.terminate());
  });

  // Dead sockets (no pong) and lobbies left open for too long are removed.
  const sweep = setInterval(() => {
    const now = Date.now();
    for (const ws of clients) {
      if (!ws.alive) { ws.terminate(); continue; }
      ws.alive = false;
      ws.ping();
    }
    for (const l of [...lobbies.values()])
      if (!l.guest && now - l.created > cfg.lobbyIdleMs) removeLobby(l, 'expired');
  }, cfg.heartbeatMs);
  sweep.unref();

  return {
    config: cfg,
    stats: () => ({ lobbies: lobbies.size, clients: clients.size }),
    listen: () => new Promise((resolve) => httpServer.listen(cfg.port, cfg.host, () => resolve(httpServer.address().port))),
    close: () => new Promise((resolve) => {
      clearInterval(sweep);
      for (const ws of clients) ws.terminate();
      wss.close(() => httpServer.close(() => resolve()));
    }),
    sweepNow: () => sweep._onTimeout(),
  };
}

module.exports = { createServer, PROTOCOL, CODE_ALPHABET, CODE_LENGTH };
