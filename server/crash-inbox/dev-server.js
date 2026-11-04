// Local stand-in for the deployed Worker, for testing the game's upload: node dev-server.js [port]
// Same code (worker.js), an in-memory store, no GitHub. Prints each report it stores.
import http from 'node:http';
import fs from 'node:fs';
import { handle } from './worker.js';

const store = new Map();
const env = { REPORTS: { get: async (k) => store.get(k) ?? null, put: async (k, v) => void store.set(k, v) } };
const port = Number(process.argv[2] || 8790);
const dump = process.env.DUMP_DIR;  // optional: also write each stored report here as JSON
if (dump) fs.mkdirSync(dump, { recursive: true });

http
  .createServer(async (req, res) => {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const request = new Request(`http://127.0.0.1:${port}${req.url}`, {
      method: req.method,
      headers: { ...req.headers, 'cf-connecting-ip': req.socket.remoteAddress },
      body: ['GET', 'HEAD'].includes(req.method) ? undefined : Buffer.concat(chunks),
    });
    const out = await handle(request, env);
    res.writeHead(out.status, { 'content-type': 'application/json' });
    res.end(await out.text());
    const last = [...store.keys()].filter((k) => k.startsWith('report:')).pop();
    if (req.method === 'POST' && last) {
      const r = JSON.parse(store.get(last));
      if (dump) fs.writeFileSync(`${dump}/${last.replace(':', '-')}.json`, store.get(last));
      console.log(`stored ${last}: version ${r.version}, ${r.mode}/${r.net}, mods ${r.mods.length}, log ${r.log.length} chars`);
    }
  })
  .listen(port, '127.0.0.1', () => console.log(`crash inbox (dev) on http://127.0.0.1:${port}`));
