'use strict';
// End-to-end test of the internet path without the game: two lm_rtc_test processes negotiate a
// WebRTC data channel through the real server on loopback and exchange protocol packets.
// Build the helper first:  tools/build.ps1 -Target lm_rtc_test   (skipped when it is missing).
// LM_RTC_TEST=<path to lm_rtc_test.exe> overrides the default location.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { createServer } = require('../src/server');

const exe = process.env.LM_RTC_TEST ||
  path.join(__dirname, '..', '..', 'build', 'windows-clang', process.platform === 'win32' ? 'lm_rtc_test.exe' : 'lm_rtc_test');
const skip = fs.existsSync(exe) ? false : `build lm_rtc_test first (${exe})`;

function run(args) {
  const child = spawn(exe, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  const p = { child, out: '', waiters: [] };
  child.stdout.on('data', (d) => { p.out += d; p.waiters = p.waiters.filter((w) => !w()); });
  child.stderr.on('data', () => {});
  p.until = (re) => new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`timeout waiting for ${re}; output so far: ${p.out}`)), 60000);
    const check = () => { const m = p.out.match(re); if (m) { clearTimeout(t); resolve(m); return true; } return false; };
    if (!check()) p.waiters.push(check);
  });
  p.exited = new Promise((resolve) => child.on('exit', (code) => resolve(code)));
  return p;
}

async function withServer(fn) {
  const srv = createServer({ port: 0, host: '127.0.0.1', games: ['GLME01'] });
  const port = await srv.listen();
  try { await fn(`ws://127.0.0.1:${port}`, srv); } finally { await srv.close(); }
}

test('host and guest connect through the server and exchange packets', { skip, timeout: 120000 }, async () => {
  await withServer(async (url, srv) => {
    const host = run(['host', url, 'E2E lobby']);
    const code = (await host.until(/CODE ([A-Z2-9]{6})/))[1];
    assert.equal(srv.stats().lobbies, 1);
    const guest = run(['join', url, code]);
    await Promise.all([guest.until(/RESULT ok/), host.until(/RESULT ok/)]);
    assert.equal(await guest.exited, 0);
    assert.equal(await host.exited, 0);
  });
});

test('joining with an unknown code fails with a clear message', { skip, timeout: 60000 }, async () => {
  await withServer(async (url) => {
    const guest = run(['join', url, 'ZZZZZZ']);
    await guest.until(/RESULT error: no lobby with that code/);
    assert.equal(await guest.exited, 1);
  });
});

test('a different build is refused', { skip, timeout: 60000 }, async () => {
  await withServer(async (url) => {
    const host = run(['host', url, 'E2E lobby']);
    const code = (await host.until(/CODE ([A-Z2-9]{6})/))[1];
    const guest = run(['join', url, code, 'other-build']);
    await guest.until(/RESULT error: the host runs a different LmCoop version/);
    host.child.kill();
  });
});

test('an unreachable server is reported, not hung', { skip, timeout: 60000 }, async () => {
  const guest = run(['join', 'ws://127.0.0.1:9', 'ABCDEF']);
  await guest.until(/RESULT error: (cannot reach|lost)/);
  assert.equal(await guest.exited, 1);
});
