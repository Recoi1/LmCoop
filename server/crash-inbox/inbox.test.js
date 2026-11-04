import test from 'node:test';
import assert from 'node:assert/strict';
import { handle, validate, issueBody } from './worker.js';

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
const okIssue = (calls) => async (url, init) => { calls.push({ url, init }); return new Response(JSON.stringify({ number: 77 }), { status: 201 }); };

test('stores a report, opens a private issue, returns its number; the token never reaches the issue', async () => {
  const calls = [], e = env();
  const res = await handle(post(good()), e, okIssue(calls));
  const j = await res.json();
  assert.equal(res.status, 200);
  assert.deepEqual([j.ok, j.number, j.issue], [true, 77, true]);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, 'https://api.github.com/repos/me/private/issues');
  assert.equal(calls[0].init.headers.authorization, 'Bearer SECRET-TOKEN');
  assert.ok(!calls[0].init.body.includes('SECRET-TOKEN'));
  assert.ok(e.REPORTS.m.has('report:1'));
});

test('a retry of the same report gets the same number and makes nothing new', async () => {
  const calls = [], e = env();
  await handle(post(good()), e, okIssue(calls));
  const again = await (await handle(post(good()), e, okIssue(calls))).json();
  assert.deepEqual([again.number, again.duplicate], [77, true]);
  assert.equal(calls.length, 1);
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
