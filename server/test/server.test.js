'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const WebSocket = require('ws');
const { createServer, PROTOCOL, CODE_ALPHABET } = require('../src/server');

async function start(opts = {}) {
  const srv = createServer({ port: 0, host: '127.0.0.1', ...opts });
  const port = await srv.listen();
  const open = () => new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);
    ws.queue = [], ws.waiting = [];
    ws.on('message', (d) => {
      const m = JSON.parse(d);
      const w = ws.waiting.shift();
      w ? w(m) : ws.queue.push(m);
    });
    ws.on('open', () => resolve(ws));
    ws.on('error', reject);
  });
  return { srv, port, open };
}
const next = (ws) => ws.queue.length ? Promise.resolve(ws.queue.shift()) : new Promise((r) => ws.waiting.push(r));
const call = async (ws, msg) => { ws.send(JSON.stringify(msg)); return next(ws); };
const hostMsg = (extra = {}) => ({ t: 'host', proto: PROTOCOL, game: 'GLME01', name: 'Mansion', build: '1.3.0', public: true, ...extra });

test('host, list, join by code and by id, signaling both ways', async () => {
  const { srv, open } = await start();
  const host = await open(), guest = await open(), browser = await open();
  const hosted = await call(host, hostMsg());
  assert.equal(hosted.t, 'hosted');
  assert.match(hosted.code, new RegExp(`^[${CODE_ALPHABET}]{6}$`));

  const listed = await call(browser, { t: 'list' });
  assert.equal(listed.lobbies.length, 1);
  assert.deepEqual(Object.keys(listed.lobbies[0]).sort(), ['build', 'id', 'max', 'name', 'password', 'players', 'proto']);
  assert.equal(listed.lobbies[0].players, 1);
  assert.doesNotMatch(JSON.stringify(listed), /127\.0\.0\.1/);

  const joined = await call(guest, { t: 'join', proto: PROTOCOL, game: 'GLME01', build: '1.3.0', code: hosted.code.toLowerCase() });
  assert.equal(joined.t, 'joined');
  const peer = await next(host);
  assert.equal(peer.t, 'peer-joined');
  assert.equal(peer.token, joined.token);

  guest.send(JSON.stringify({ t: 'signal', data: { type: 'offer', sdp: 'x' } }));
  assert.deepEqual((await next(host)).data, { type: 'offer', sdp: 'x' });
  host.send(JSON.stringify({ t: 'signal', data: { type: 'answer', sdp: 'y' } }));
  assert.deepEqual((await next(guest)).data, { type: 'answer', sdp: 'y' });

  assert.equal((await call(browser, { t: 'list' })).lobbies[0].players, 2);
  for (const ws of [host, guest, browser]) ws.close();
  await srv.close();
});

test('capacity, private lobbies, version checks', async () => {
  const { srv, open } = await start();
  const [host, a, b, c, d] = await Promise.all([open(), open(), open(), open(), open()]);
  const hosted = await call(host, hostMsg({ public: false }));
  assert.equal((await call(a, { t: 'list' })).lobbies.length, 0, 'private lobby is not listed');
  assert.equal((await call(a, { t: 'join', proto: PROTOCOL, game: 'GLME01', build: '1.3.0', id: hosted.id })).error, 'lobby-not-found');
  const ok = { t: 'join', proto: PROTOCOL, game: 'GLME01', build: '1.3.0', code: hosted.code };
  assert.equal((await call(b, { ...ok, proto: 99 })).error, 'protocol-mismatch');
  assert.equal((await call(b, { ...ok, build: '0.0.1' })).error, 'build-mismatch');
  assert.equal((await call(b, { ...ok, game: 'GLMP01' })).error, 'unsupported-game');
  assert.equal((await call(c, ok)).t, 'joined');
  assert.equal((await call(d, ok)).error, 'lobby-full');
  assert.equal((await call(d, { ...ok, code: 'ZZZZZZ' })).error, 'lobby-not-found');
  await srv.close();
});

test('disconnects free the slot or remove the lobby', async () => {
  const { srv, open } = await start();
  const host = await open(), g1 = await open(), g2 = await open();
  const hosted = await call(host, hostMsg());
  const join = { t: 'join', proto: PROTOCOL, game: 'GLME01', build: '1.3.0', code: hosted.code };
  assert.equal((await call(g1, join)).t, 'joined');
  await next(host); // peer-joined
  g1.close();
  assert.equal((await next(host)).t, 'peer-left');
  assert.equal((await call(g2, join)).t, 'joined');
  await next(host);
  host.close();
  assert.equal((await next(g2)).t, 'closed');
  await new Promise((r) => setTimeout(r, 50));
  assert.equal(srv.stats().lobbies, 0);
  await srv.close();
});

test('validation: garbage, oversize, unknown types, signaling outside a lobby', async () => {
  const { srv, open } = await start();
  const ws = await open();
  ws.send('not json');
  assert.equal((await next(ws)).error, 'bad-request');
  assert.equal((await call(ws, { t: 'constructor' })).error, 'bad-request');
  assert.equal((await call(ws, { t: 'signal', data: {} })).error, 'not-in-lobby');
  assert.equal((await call(ws, hostMsg({ name: '   ' }))).error, 'bad-request');
  const closed = new Promise((r) => ws.on('close', (code) => r(code)));
  ws.send('x'.repeat(20000));
  assert.equal(await closed, 1009);
  await srv.close();
});

test('rate limiting closes a flooding connection', async () => {
  const { srv, open } = await start({ rateBurst: 5, ratePerSecond: 1 });
  const ws = await open();
  const closed = new Promise((r) => ws.on('close', (code) => r(code)));
  for (let i = 0; i < 50; i++) ws.send(JSON.stringify({ t: 'ping' }));
  assert.equal(await closed, 1008);
  await srv.close();
});

test('per-IP connection cap and lobby cap', async () => {
  const { srv, open, port } = await start({ maxConnectionsPerIp: 2, maxLobbies: 1 });
  const a = await open(), b = await open();
  const c = new WebSocket(`ws://127.0.0.1:${port}`);
  const code = await new Promise((r) => { c.on('close', r); c.on('error', () => {}); });
  assert.equal(code, 1008);
  assert.equal((await call(a, hostMsg())).t, 'hosted');
  assert.equal((await call(b, hostMsg())).error, 'server-full');
  await srv.close();
});

test('expired lobbies are swept', async () => {
  const { srv, open } = await start({ lobbyIdleMs: 1, heartbeatMs: 3600000 });
  const host = await open();
  await call(host, hostMsg());
  await new Promise((r) => setTimeout(r, 20));
  srv.sweepNow();
  assert.equal((await next(host)).reason, 'expired');
  assert.equal(srv.stats().lobbies, 0);
  await srv.close();
});
