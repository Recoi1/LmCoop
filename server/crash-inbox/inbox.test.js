import test from 'node:test';
import assert from 'node:assert/strict';
import { handle, validate, issueBody, signatureOf } from './worker.js';

const kv = () => {
  const m = new Map();
  return { m, get: async (k) => m.get(k) ?? null, put: async (k, v) => void m.set(k, v) };
};
const good = (over = {}) => ({
  schema: 1, id: 'a1b2c3d4e5f60718', version: '1.7.0', build: 'abc1234', os: 'Windows 11', gpu: 'Some GPU', mode: 'host', net: 'steam',
  mods: [{ name: 'Test pack', version: '1', kind: 'textures' }], reason: 'exception 0xC0000005', description: 'it died', log: 'line 1\nline 2\n=== crash ===', ...over,
});
const post = (body, ip = '1.2.3.4', raw) =>
  new Request('https://x.test/report', { method: 'POST', headers: { 'cf-connecting-ip': ip }, body: raw ?? JSON.stringify(body) });
const env = (extra = {}) => ({ REPORTS: kv(), GITHUB_TOKEN: 'SECRET-TOKEN', GITHUB_REPO: 'me/private', ...extra });
// A fake GitHub: remembers issues by label, answers the signature search, creates issues, takes comments.
const github = (calls = [], existing = {}) => {
  const issues = { ...existing };  // sig label -> issue number
  let next = 77;
  return async (url, init = {}) => {
    calls.push({ url, init });
    if (url.includes('/search/issues')) {
      const label = decodeURIComponent(url).match(/label:(sig-[0-9a-f]+)/)?.[1];
      return new Response(JSON.stringify({ items: issues[label] ? [{ number: issues[label] }] : [] }), { status: 200 });
    }
    if (url.endsWith('/comments')) return new Response('{}', { status: 201 });
    const body = JSON.parse(init.body);
    const n = next++;
    for (const l of body.labels || []) if (l.startsWith('sig-')) issues[l] = n;
    return new Response(JSON.stringify({ number: n }), { status: 201 });
  };
};
const okIssue = (calls) => github(calls);
const created = (calls) => calls.filter((c) => c.init.method === 'POST' && c.url.endsWith('/issues'));

test('stores a report, opens a private issue, returns its number; the token never reaches the issue', async () => {
  const calls = [], e = env();
  const res = await handle(post(good()), e, okIssue(calls));
  const j = await res.json();
  assert.equal(res.status, 200);
  assert.deepEqual([j.ok, j.number, j.issue], [true, 77, true]);
  assert.equal(created(calls).length, 1);
  const c = created(calls)[0];
  assert.equal(c.url, 'https://api.github.com/repos/me/private/issues');
  assert.equal(c.init.headers.authorization, 'Bearer SECRET-TOKEN');
  assert.ok(!c.init.body.includes('SECRET-TOKEN'));
  assert.ok(JSON.parse(c.init.body).labels.some((l) => l.startsWith('sig-')));
  assert.ok(e.REPORTS.m.has('report:1'));
});

test('a retry of the same report gets the same number and makes nothing new', async () => {
  const calls = [], e = env();
  await handle(post(good()), e, okIssue(calls));
  const again = await (await handle(post(good()), e, okIssue(calls))).json();
  assert.deepEqual([again.number, again.duplicate], [77, true]);
  assert.equal(created(calls).length, 1);
});

test('GitHub down: the report is still stored and the player still gets a number', async () => {
  const e = env();
  const j = await (await handle(post(good()), e, async () => { throw new Error('offline'); })).json();
  assert.deepEqual([j.ok, j.number, j.issue], [true, 1, false]);
  assert.ok(e.REPORTS.m.has('report:1'));
});

test('refuses oversize, non-json, wrong schema, bad id, empty log', async () => {
  const e = env(), f = okIssue([]);
  assert.equal((await handle(post(null, '9.9.9.1', 'x'.repeat(400_000)), e, f)).status, 413);
  assert.equal((await handle(post(null, '9.9.9.2', 'not json'), e, f)).status, 400);
  assert.equal((await handle(post(good({ schema: 2 }), '9.9.9.3'), e, f)).status, 400);
  assert.equal((await handle(post(good({ id: '../../x' }), '9.9.9.4'), e, f)).status, 400);
  assert.equal((await handle(post(good({ log: '' }), '9.9.9.5'), e, f)).status, 400);
  assert.equal(e.REPORTS.m.size, 0);
});

test('rate limit: the 9th report from one address in an hour is refused', async () => {
  const e = env(), f = okIssue([]);
  for (let i = 0; i < 8; i++) assert.equal((await handle(post(good({ id: (i + 1).toString(16).padStart(8, '0') })), e, f)).status, 200);
  assert.equal((await handle(post(good({ id: 'ffffffff' })), e, f)).status, 429);
  assert.equal((await handle(post(good({ id: 'eeeeeeee' }), '5.6.7.8'), e, f)).status, 200);
});

test('unknown fields are dropped and enums are checked', () => {
  const { report } = validate({ ...good({ mode: 'weird', net: 'x' }), evil: '<script>', token: 'abc' });
  assert.equal(report.mode, 'unknown');
  assert.equal(report.net, 'unknown');
  assert.equal(report.evil, undefined);
});

test('a log cannot break out of its code block', () => {
  const body = issueBody(validate(good({ log: 'before\n```\n# injected heading\n```\nafter' })).report, 3);
  const fences = body.split('\n').filter((l) => /^`{3,}/.test(l));
  const longest = Math.max(...fences.map((l) => l.match(/^`+/)[0].length));
  assert.ok(longest >= 4);
});

test('no GitHub settings: stores only', async () => {
  const j = await (await handle(post(good()), { REPORTS: kv() }, async () => assert.fail('no call'))).json();
  assert.deepEqual([j.ok, j.number, j.issue], [true, 1, false]);
});

// The layout the game's crash handler writes: the exception line, the raw frames, then the same frames symbolized.
const crash = ({ at = 'LmCoop.exe+0x1EC88D3', handler = 4, names = [], rawExtra = [] } = {}) => [
  'boot',
  `=== exception 0xC0000005 at 00007FF62EE588D3 (${at}), rsp 00000000E56A4000 ===`,
  '=== crash: unhandled exception ===',
  '  thread 4242, raw frames:',
  ...Array.from({ length: handler }, (_, i) => `  #${String(i).padStart(2, '0')} LmCoop.exe+0x${(0x100 + i).toString(16)}`),
  ...rawExtra.map((o, i) => `  #${String(handler + i).padStart(2, '0')} LmCoop.exe+0x${o}`),
  ...['printStack', 'vectoredHandler', ...names].map((n, i) => `  #${String(i).padStart(2, '0')} ${n}+0x10 (${n}.cpp:${i + 1})`),
].join('\n');

test('signature: function names are stable across builds, the handler frames are ignored, a different crash differs', async () => {
  const sigOf = async (over) => signatureOf(validate(good(over)).report);
  const a = await sigOf({ version: '1.6.0', log: crash({ at: 'LmCoop.exe+0x1EC88D3', names: ['recomp::online::frame', 'main'] }) });
  const b = await sigOf({ version: '1.7.0', log: crash({ at: 'LmCoop.exe+0x2FFFF00', names: ['recomp::online::frame', 'main'] }) });
  const c = await sigOf({ log: crash({ names: ['recomp::mods::refresh'] }) });
  assert.equal(a.sig, b.sig);
  assert.notEqual(a.sig, c.sig);
  assert.deepEqual(a.frames, ['recomp::online::frame', 'main']);
});

test('signature: without symbols two different faults in one build do not collide, and the same one does', async () => {
  const sigOf = async (v, at) => (await signatureOf(validate(good({ version: v, log: crash({ at }) })).report)).sig;
  const x = await sigOf('1.7.0', 'LmCoop.exe+0x1EC88D3');
  assert.equal(x, await sigOf('1.7.0', 'LmCoop.exe+0x1EC88D3'));
  assert.notEqual(x, await sigOf('1.7.0', 'LmCoop.exe+0x00ABCDE'));
  assert.notEqual(x, await sigOf('1.8.0', 'LmCoop.exe+0x1EC88D3'));
});

test('signature: an abort() (no exception line) uses the raw frames after the handler, and a missing log tail still works', async () => {
  const abort = (extra) => ['x', '=== crash: abort() ===', '  thread 1, raw frames:', ...[0, 1, 2, 3].map((i) => `  #0${i} LmCoop.exe+0x${i}`), ...extra.map((o, i) => `  #0${4 + i} LmCoop.exe+0x${o}`)].join('\n');
  const s1 = (await signatureOf(validate(good({ log: abort(['a1', 'b2', 'c3']) })).report)).sig;
  const s2 = (await signatureOf(validate(good({ log: abort(['a1', 'b2', 'c3']) })).report)).sig;
  const s3 = (await signatureOf(validate(good({ log: abort(['d4', 'e5', 'f6']) })).report)).sig;
  assert.equal(s1, s2);
  assert.notEqual(s1, s3);
  assert.ok((await signatureOf(validate(good({ log: 'no crash marker here' })).report)).sig);
});

test('the same crash from two players: one issue, the second becomes a comment on it', async () => {
  const calls = [], e = env(), f = github(calls);
  const l = crash({ names: ['recomp::online::frame'] });
  const first = await (await handle(post(good({ id: '1111111111111111', log: l })), e, f)).json();
  const second = await (await handle(post(good({ id: '2222222222222222', log: l, description: 'joined a friend' }), '7.7.7.7'), e, f)).json();
  assert.equal(created(calls).length, 1);
  assert.equal(first.number, 77);
  assert.deepEqual([second.number, second.duplicate, second.issue], [77, true, true]);
  const comment = calls.find((c) => c.url.endsWith('/issues/77/comments'));
  assert.ok(comment && JSON.parse(comment.init.body).body.includes('joined a friend'));
});

test('two different crashes get two issues', async () => {
  const calls = [], e = env(), f = github(calls);
  await handle(post(good({ id: '3333333333333333', log: crash({ names: ['a::one'] }) })), e, f);
  await handle(post(good({ id: '4444444444444444', log: crash({ names: ['b::two'] }) }), '8.8.8.8'), e, f);
  assert.equal(created(calls).length, 2);
});

test('discord: the report is posted with mentions disabled and the log attached; a Discord outage changes nothing', async () => {
  const calls = [];
  const hook = 'https://discord.com/api/webhooks/1/SECRETHOOK';
  const ok = async (url, init) => { calls.push({ url, init }); return new Response(null, { status: 204 }); };
  const j = await (await handle(post(good({ description: '@everyone hi' })), { REPORTS: kv(), DISCORD_WEBHOOK: hook }, ok)).json();
  assert.deepEqual([j.ok, j.discord, calls.length, calls[0].url], [true, true, 1, hook]);
  const payload = JSON.parse(calls[0].init.body.get('payload_json'));
  assert.deepEqual(payload.allowed_mentions, { parse: [] });
  assert.ok(calls[0].init.body.get('files[0]'));
  const down = async () => { throw new Error('discord down'); };
  const j2 = await (await handle(post(good({ id: 'b1b2c3d4e5f60718' })), { REPORTS: kv(), DISCORD_WEBHOOK: hook }, down)).json();
  assert.deepEqual([j2.ok, j2.discord], [true, false]);
});
