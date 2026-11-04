// LmCoop crash inbox: a small Cloudflare Worker. The game POSTs one crash report (JSON, already stripped of
// passwords, addresses and personal paths by the game); this service stores it, opens an issue in a PRIVATE GitHub
// repository for the developer to review, and answers with a report number. Players need no GitHub account.
//
// The GitHub token exists only here, as the Worker secret GITHUB_TOKEN (see README.md); the game never holds a
// credential. Nothing in a report is trusted: sizes and field shapes are checked, requests are rate limited per
// address, and the report is only ever written into a fenced code block.

const MAX_BODY = 300_000;     // bytes accepted
const MAX_LOG = 250_000;      // characters of log kept
const ISSUE_LOG = 45_000;     // characters of log put into the issue (GitHub allows 65,536 for the whole body)
const PER_HOUR = 8;           // reports per address per hour
const KEEP_SECONDS = 90 * 24 * 3600;
const MODES = ['off', 'host', 'join', 'unknown'];
const NETS = ['direct', 'internet', 'steam', 'unknown'];

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });

const str = (v, max) => (typeof v === 'string' ? v.slice(0, max) : '');
const oneLine = (s, max) => str(s, max * 4).replace(/[\r\n\t]+/g, ' ').replace(/[`|<>]/g, "'").trim().slice(0, max);

// What the game is allowed to send. Anything else is dropped; wrong shapes are refused.
export function validate(body) {
  if (!body || typeof body !== 'object' || body.schema !== 1) return { error: 'unsupported report' };
  if (typeof body.id !== 'string' || !/^[0-9a-f]{8,32}$/.test(body.id)) return { error: 'bad report id' };
  if (typeof body.log !== 'string' || body.log.length === 0) return { error: 'no log' };
  const mods = Array.isArray(body.mods) ? body.mods.slice(0, 50) : [];
  return {
    report: {
      id: body.id,
      version: oneLine(body.version, 40),
      build: oneLine(body.build, 80),
      os: oneLine(body.os, 200),
      gpu: oneLine(body.gpu, 300),
      mode: MODES.includes(body.mode) ? body.mode : 'unknown',
      net: NETS.includes(body.net) ? body.net : 'unknown',
      mods: mods.map((m) => ({ name: oneLine(m && m.name, 80), version: oneLine(m && m.version, 30), kind: oneLine(m && m.kind, 20) })),
      reason: oneLine(body.reason, 300),
      description: str(body.description, 2000),
      log: str(body.log, MAX_LOG),
    },
  };
}

// A fence longer than any run of backticks in the text, so a log cannot break out of its code block.
function fence(text) {
  const runs = text.match(/`+/g) || [];
  return '`'.repeat(Math.max(3, ...runs.map((r) => r.length + 1)));
}

export function issueBody(r, seq) {
  const tail = r.log.length > ISSUE_LOG ? '[...earlier log in the stored report...]\n' + r.log.slice(-ISSUE_LOG) : r.log;
  const f = fence(tail);
  const mods = r.mods.length ? r.mods.map((m) => `- ${m.name}${m.version ? ' ' + m.version : ''}${m.kind ? ' (' + m.kind + ')' : ''}`).join('\n') : 'none';
  return [
    `Report #${seq} (id \`${r.id}\`)`,
    '',
    `| | |`,
    `|---|---|`,
    `| Version | ${r.version} |`,
    `| Build | ${r.build} |`,
    `| Windows | ${r.os} |`,
    `| Graphics | ${r.gpu} |`,
    `| Session | ${r.mode}, ${r.net} |`,
    `| Error | ${r.reason} |`,
    '',
    '### Active mods',
    mods,
    '',
    '### What the player said',
    r.description ? `${fence(r.description)}\n${r.description}\n${fence(r.description)}` : '_nothing_',
    '',
    '### Log (end)',
    `${f}text\n${tail}\n${f}`,
  ].join('\n');
}

async function rateLimited(env, ip) {
  const key = `rl:${ip}:${Math.floor(Date.now() / 3_600_000)}`;
  const n = parseInt((await env.REPORTS.get(key)) || '0', 10);
  if (n >= PER_HOUR) return true;
  await env.REPORTS.put(key, String(n + 1), { expirationTtl: 7200 });
  return false;
}

export async function handle(request, env, fetchFn = fetch) {
  const url = new URL(request.url);
  if (request.method === 'GET' && url.pathname === '/health') return json({ ok: true });
  if (request.method !== 'POST' || url.pathname !== '/report') return json({ error: 'not found' }, 404);

  const declared = parseInt(request.headers.get('content-length') || '0', 10);
  if (declared > MAX_BODY) return json({ error: 'report too large' }, 413);
  const text = await request.text();
  if (text.length > MAX_BODY) return json({ error: 'report too large' }, 413);
  let body;
  try {
    body = JSON.parse(text);
  } catch {
    return json({ error: 'not json' }, 400);
  }
  const v = validate(body);
  if (v.error) return json({ error: v.error }, 400);
  const r = v.report;

  // A retry of a report already stored gets the same number back, and nothing is created twice.
  const known = await env.REPORTS.get(`id:${r.id}`);
  if (known) return json({ ok: true, number: parseInt(known, 10), duplicate: true });

  const ip = request.headers.get('cf-connecting-ip') || 'unknown';
  if (await rateLimited(env, ip)) return json({ error: 'too many reports, try again later' }, 429);

  const seq = parseInt((await env.REPORTS.get('counter')) || '0', 10) + 1;
  await env.REPORTS.put('counter', String(seq));
  await env.REPORTS.put(`report:${seq}`, JSON.stringify(r), { expirationTtl: KEEP_SECONDS });

  let number = seq;
  let issue = false;
  if (env.GITHUB_TOKEN && env.GITHUB_REPO) {
    try {
      const reason = r.reason || 'crash';
      const res = await fetchFn(`https://api.github.com/repos/${env.GITHUB_REPO}/issues`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${env.GITHUB_TOKEN}`,
          accept: 'application/vnd.github+json',
          'x-github-api-version': '2022-11-28',
          'user-agent': 'lmcoop-crash-inbox',
          'content-type': 'application/json',
        },
        body: JSON.stringify({ title: `Crash #${seq} (${r.version}): ${reason}`.slice(0, 120), body: issueBody(r, seq), labels: ['crash'] }),
      });
      if (res.ok) {
        number = (await res.json()).number || seq;
        issue = true;
      }
    } catch {
      // the report is stored; the issue can be made from it later
    }
  }
  await env.REPORTS.put(`id:${r.id}`, String(number), { expirationTtl: KEEP_SECONDS });
  return json({ ok: true, number, issue });
}

export default { fetch: (request, env) => handle(request, env) };
